/* Golden-vector generator for the LotofBench unit tests.
 *
 * The tests pin, as regression guards: the PRNG stream, the 1 MB seed-42
 * corpus (SHA-1 + head bytes), the LZ4 encoder output on fixed inputs
 * (length + SHA-1), and the default-seed edge-relaxation count. Those
 * pins live in tests/test_rng.c, tests/test_lz4.c,
 * tests/test_workload_compression.c and
 * tests/test_workload_navigation.c.
 *
 * After a DELIBERATE change to any pinned behaviour (never after an
 * accidental one), rebuild and run this tool and port the printed
 * values into the tests in the same commit:
 *
 *     make tools/gen_goldens && ./tools/gen_goldens
 */

#include "harness.h"
#include "imgops.h"
#include "lotofbench.h"
#include "lz4.h"
#include "rng.h"
#include "sha1.h"
#include "workload_assetcompression.h"
#include "workload_clang.h"
#include "workload_compression.h"
#include "workload_horizon.h"
#include "workload_html5browser.h"
#include "workload_navigation.h"
#include "workload_objectdetection.h"
#include "workload_photofilter.h"
#include "workload_photolibrary.h"
#include "workload_raytracer.h"
#include "workload_pdfrender.h"
#include "workload_sfm.h"
#include "workload_textprocessing.h"

#include <stdio.h>
#include <string.h>

/* scratch for the photo-library feature rebuild (constant sizes) */
#define PL_SCRATCH_BYTES \
    (PL_FEATURE_DIMS * PL_CLS_W * PL_CLS_H + \
     PL_FEATURE_DIMS * (PL_CLS_W / 2) * (PL_CLS_H / 2))

int main(void)
{
    char hex[41];

    /* rng stream, seed 42 */
    rng_t r;
    rng_init(&r, 42);
    printf("RNG42_FIRST3");
    for (int i = 0; i < 3; i++)
        printf(" %llu", (unsigned long long)rng_next(&r));
    rng_init(&r, 42);
    printf("\nRNG42_BELOW100_FIRST4");
    for (int i = 0; i < 4; i++)
        printf(" %u", rng_below(&r, 100));

    /* corpus (v0.1 compatibility pin), 1 MB seed 42 */
    fc_ctx *fc = fc_prepare(1, 42);
    sha1_hex(fc_corpus(fc), fc_len(fc), hex);
    printf("\nCORPUS_1MB_SEED42_SHA1 %s len=%zu", hex, fc_len(fc));
    printf("\nCORPUS_1MB_SEED42_HEAD");
    for (int i = 0; i < 8; i++)
        printf(" %02x", fc_corpus(fc)[i]);
    fc_free(fc);

    /* lz4 goldens: 4096 x 'a' */
    static uint8_t in[4096], out[16384];
    memset(in, 'a', sizeof in);
    for (int level = LZ4_LEVEL_FAST; level <= LZ4_LEVEL_HIGH; level++) {
        size_t clen = lz4_compress(in, sizeof in, out, level);
        sha1_hex(out, clen, hex);
        printf("\nLZ4_A4096_L%d clen=%zu sha1=%s", level, clen, hex);
    }

    /* lz4 goldens: 4096 rng bytes (seed 1234) */
    rng_init(&r, 1234);
    for (size_t i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)rng_next(&r);
    for (int level = LZ4_LEVEL_FAST; level <= LZ4_LEVEL_HIGH; level++) {
        size_t clen = lz4_compress(in, sizeof in, out, level);
        sha1_hex(out, clen, hex);
        printf("\nLZ4_RNG4096_L%d clen=%zu sha1=%s", level, clen, hex);
    }

    /* navigation default-seed edge count, single-threaded */
    nav_ctx *nav = nav_prepare(1);
    unsigned th = 1;
    double tp;
    uint64_t ops = 0;

    if (!nav || nav_run(nav, 1, &th, &tp, &ops) != 0)
        return 1;
    printf("\nNAV_SEED1_EDGES %llu threads_used=%u",
           (unsigned long long)ops, th);
    nav_free(nav);

    /* html5 goldens (seed 1): total DOM elements, page 0 (portal) and
     * page 5 (landing) sources and rendered pixmaps */
    html5_ctx *h5 = html5_prepare(1);
    static uint8_t pix[HTML5_VIEW_W * HTML5_VIEW_H * 4];
    static const unsigned HPAGES[] = { 0, 5 };

    if (!h5)
        return 1;
    printf("\nHTML5_SEED1_ELEMENTS %llu",
           (unsigned long long)html5_elements(h5));
    for (size_t i = 0; i < sizeof HPAGES / sizeof HPAGES[0]; i++) {
        unsigned p = HPAGES[i];
        uint64_t elts = html5_page_elements(h5, p);
        uint64_t recs = html5_page_records(h5, p);

        sha1_hex((const uint8_t *)html5_page_source(h5, p),
                 html5_page_source_len(h5, p), hex);
        printf("\nHTML5_SEED1_PAGE%u_SRC_SHA1 %s len=%zu elements=%llu "
               "records=%llu",
               p, hex, html5_page_source_len(h5, p),
               (unsigned long long)elts, (unsigned long long)recs);
        if (html5_render_page(h5, p, pix) != 0)
            return 1;
        sha1_hex(pix, sizeof pix, hex);
        printf("\nHTML5_SEED1_PAGE%u_PIX_SHA1 %s", p, hex);
    }
    html5_free(h5);

    /* pdf render: seed-1 document 0 digest + full-suite op count */
    pdf_ctx *pdf = pdf_prepare(1);

    if (!pdf)
        return 1;
    sha1_hex(pdf_doc(pdf, 0), pdf_doc_len(pdf, 0), hex);
    printf("\nPDF_SEED1_DOC0_SHA1 %s len=%zu", hex, pdf_doc_len(pdf, 0));
    uint64_t pdf_ops = 0;
    for (unsigned d = 0; d < PDF_DOCS; d++)
        pdf_ops += pdf_doc_ops(pdf, d);
    printf("\nPDF_SEED1_TOTAL_OPS %llu", (unsigned long long)pdf_ops);
    pdf_free(pdf);

    /* photo library: seed-1 photo 0 source digest, its three reference
     * thumbnail digests, its feature vector through the public kernels,
     * the class-0 centroid, and the task photo counts */
    {
        static uint8_t pl_t1[PL_THUMB1_W * PL_THUMB1_H * 3];
        static uint8_t pl_scr[PL_SCRATCH_BYTES];
        uint16_t feat[PL_FEATURE_DIMS];
        pl_ctx *pl = pl_prepare(1);

        if (!pl)
            return 1;
        sha1_hex(pl_photo(pl, 0), pl_photo_len(pl, 0), hex);
        printf("\nPL_SEED1_PHOTO0_SHA1 %s len=%zu", hex,
               pl_photo_len(pl, 0));
        for (unsigned t = 0; t < PL_THUMBS; t++) {
            const uint8_t *d = pl_reference_digest(pl, 0, t);

            printf("\nPL_SEED1_PHOTO0_THUMB%u", t);
            for (int i = 0; i < 20; i++)
                printf(" %02x", d[i]);
        }
        img_box_downscale(pl_t1, PL_CLS_W, PL_CLS_H, pl_photo(pl, 0),
                          PL_PHOTO_W, PL_PHOTO_H);
        pl_extract_features(pl_t1, PL_CLS_W, PL_CLS_H, feat, pl_scr);
        printf("\nPL_SEED1_PHOTO0_FEAT");
        for (unsigned k = 0; k < PL_FEATURE_DIMS; k++)
            printf(" %u", feat[k]);
        printf("\nPL_SEED1_CENTROID0");
        for (unsigned k = 0; k < PL_FEATURE_DIMS; k++)
            printf(" %u", pl_class_centroid(pl, 0)[k]);
        printf("\nPL_SEED1_TASK_PHOTOS %llu",
               (unsigned long long)pl_task_photos(0));
        printf("\nPL_SEED1_TASK_PHOTOS_MULTI %llu",
               (unsigned long long)pl_task_photos(1));
        pl_free(pl);
    }

    /* clang goldens (seed 1): unit 0 (call-graph archetype) source and
     * bytecode, and the corpus token total */
    cl_ctx *cl = cl_prepare(1);
    size_t blob_len = 0;

    if (!cl)
        return 1;
    sha1_hex((const uint8_t *)cl_unit_source(cl, 0),
             cl_unit_source_len(cl, 0), hex);
    printf("\nCLANG_SEED1_UNIT0_SRC_SHA1 %s len=%zu tokens=%llu nodes=%llu",
           hex, cl_unit_source_len(cl, 0),
           (unsigned long long)cl_unit_tokens(cl, 0),
           (unsigned long long)cl_unit_nodes(cl, 0));
    const uint8_t *blob = cl_unit_blob(cl, 0, &blob_len);

    sha1_hex(blob, blob_len, hex);
    printf("\nCLANG_SEED1_UNIT0_BLOB_SHA1 %s len=%zu", hex, blob_len);
    printf("\nCLANG_SEED1_TOTAL_TOKENS %llu",
           (unsigned long long)cl_tokens(cl));
    cl_free(cl);

    /* textprocessing goldens (seed 1): document 0 (API-doc archetype)
     * source and HTML, its first pattern and reference match count,
     * and the corpus task size */
    tp_ctx *tpw = tp_prepare(1);

    if (!tpw)
        return 1;
    sha1_hex((const uint8_t *)tp_doc_source(tpw, 0), tp_doc_len(tpw, 0),
             hex);
    printf("\nTP_SEED1_DOC0_SRC_SHA1 %s len=%llu", hex,
           (unsigned long long)tp_doc_len(tpw, 0));
    sha1_hex((const uint8_t *)tp_doc_html(tpw, 0),
             tp_doc_html_len(tpw, 0), hex);
    printf("\nTP_SEED1_DOC0_HTML_SHA1 %s len=%zu", hex,
           tp_doc_html_len(tpw, 0));
    printf("\nTP_SEED1_DOC0_P0 %s matches=%u",
           tp_doc_pattern(tpw, 0, 0), tp_doc_matches(tpw, 0, 0));
    printf("\nTP_SEED1_TASK_BYTES %llu",
           (unsigned long long)tp_bytes(tpw));
    tp_free(tpw);

    /* photo filter: seed-1 photo 0 source digest + filtered reference
     * digest + the exact task pixel counts */
    pf_ctx *pf = pf_prepare(1);

    if (!pf)
        return 1;
    sha1_hex(pf_photo(pf, 0), pf_photo_len(pf, 0), hex);
    printf("\nPF_SEED1_PHOTO0_SHA1 %s len=%zu", hex, pf_photo_len(pf, 0));
    {
        const uint8_t *ref = pf_reference_digest(pf, 0);

        printf("\nPF_SEED1_PHOTO0_REF");
        for (int i = 0; i < 20; i++)
            printf(" %02x", ref[i]);
    }
    printf("\nPF_SEED1_TASK_PIXELS %llu",
           (unsigned long long)pf_task_pixels(0));
    printf("\nPF_SEED1_TASK_PIXELS_MULTI %llu",
           (unsigned long long)pf_task_pixels(1));
    pf_free(pf);

    /* horizon detection: seed-1 photo 0 source digest, its reference
     * detection (edge digest + cell + votes) and the task pixel counts */
    hd_ctx *hd = hd_prepare(1);

    if (!hd)
        return 1;
    sha1_hex(hd_photo(hd, 0), hd_photo_len(hd, 0), hex);
    printf("\nHD_SEED1_PHOTO0_SHA1 %s len=%zu", hex, hd_photo_len(hd, 0));
    {
        const hd_result *r = hd_reference(hd, 0);

        printf("\nHD_SEED1_PHOTO0_REF");
        for (int i = 0; i < 20; i++)
            printf(" %02x", r->edge_digest[i]);
        printf("\nHD_SEED1_PHOTO0_CELL theta=%u rho=%d votes=%u",
               r->theta_bin, r->rho_bin, r->votes);
        printf("\nHD_SEED1_PHOTO0_PLANT cx=%d cy=%d slope=%d",
               hd_plant_cx(hd, 0), hd_plant_cy(hd, 0),
               hd_plant_slope(hd, 0));
    }
    printf("\nHD_SEED1_TASK_PIXELS %llu",
           (unsigned long long)hd_task_pixels(0));
    printf("\nHD_SEED1_TASK_PIXELS_MULTI %llu",
           (unsigned long long)hd_task_pixels(1));
    hd_free(hd);

    /* ray tracer: seed-1 view 0 reference (frame digest + exact ray
     * count), the triangle count and the task pixel counts */
    rt_ctx *rt = rt_prepare(1);

    if (!rt)
        return 1;
    {
        const rt_result *r = rt_reference(rt, 0);

        printf("\nRT_SEED1_VIEW0_REF");
        for (int i = 0; i < 20; i++)
            printf(" %02x", r->digest[i]);
        printf("\nRT_SEED1_VIEW0_RAYS %llu",
               (unsigned long long)r->rays);
    }
    printf("\nRT_SEED1_TRIANGLES %u", (unsigned)rt_tri_count(rt));
    printf("\nRT_SEED1_TASK_PIXELS %llu",
           (unsigned long long)rt_task_pixels(0));
    printf("\nRT_SEED1_TASK_PIXELS_MULTI %llu",
           (unsigned long long)rt_task_pixels(1));
    rt_free(rt);

    /* asset compression: seed-1 asset 0 (opaque texture) image digest +
     * both BC1 pass references, asset 8 (alpha texture) + its BC3
     * reference, asset 12 (mesh) shape + geometry stream references */
    {
        ac_ctx *ac = ac_prepare(1);

        if (!ac)
            return 1;
        const ac_image *im = ac_asset_image(ac, 0);

        sha1_hex(im->rgba, (size_t)im->w * im->h * 4, hex);
        printf("\nAC_SEED1_TEX0_SHA1 %s", hex);
        for (unsigned p = 0; p < 2; p++) {
            const ac_reference *ref = ac_asset_reference(ac, 0, p);

            printf("\nAC_SEED1_TEX0_P%u stream=%s decoded=%s err=%llu", p,
                   ref->stream_hex, ref->decoded_hex,
                   (unsigned long long)ref->error);
        }
        im = ac_asset_image(ac, 8);
        sha1_hex(im->rgba, (size_t)im->w * im->h * 4, hex);
        printf("\nAC_SEED1_ALPHA0_SHA1 %s", hex);
        {
            const ac_reference *ref = ac_asset_reference(ac, 8, 0);

            printf("\nAC_SEED1_ALPHA0_P0 stream=%s decoded=%s err=%llu",
                   ref->stream_hex, ref->decoded_hex,
                   (unsigned long long)ref->error);
        }
        {
            const ac_mesh *m = ac_asset_mesh(ac, 12);
            const ac_reference *ref = ac_asset_reference(ac, 12, 0);

            printf("\nAC_SEED1_MESH0 vc=%u ic=%u stream=%s decoded=%s aux=%s",
                   m->vertex_count, m->index_count, ref->stream_hex,
                   ref->decoded_hex, ref->aux_hex);
        }
        printf("\nAC_SEED1_TASK_BYTES %llu",
               (unsigned long long)ac_task_bytes(ac, 0));
        printf("\nAC_SEED1_TASK_BYTES_MULTI %llu",
               (unsigned long long)ac_task_bytes(ac, 1));
        ac_free(ac);
    }

    /* structure from motion: seed-1 pair 0 reference (reconstruction
     * digest + exact counters), the left view 0 frame digest, the
     * per-pair visible counts and the task pair counts */
    {
        sfm_ctx *sfm = sfm_prepare(1);

        if (!sfm)
            return 1;
        {
            const sfm_result *r = sfm_reference(sfm, 0);

            printf("\nSFM_SEED1_PAIR0_REF");
            for (int i = 0; i < 20; i++)
                printf(" %02x", r->digest[i]);
            printf("\nSFM_SEED1_PAIR0_COUNTS corners=%u/%u inliers=%u "
                   "tri=%u",
                   r->corners_left, r->corners_right, r->inliers,
                   r->triangulated);
        }
        {
            const uint8_t *v = sfm_view(sfm, 0, 0);

            sha1_hex(v, (size_t)SFM_VIEW_W * SFM_VIEW_H, hex);
            printf("\nSFM_SEED1_VIEW0_SHA1 %s", hex);
        }
        printf("\nSFM_SEED1_VISIBLE");
        for (unsigned p = 0; p < SFM_PAIRS; p++)
            printf(" %u", sfm_visible(sfm, p));
        printf("\nSFM_SEED1_TASK_PAIRS %llu",
               (unsigned long long)sfm_task_pairs(0));
        printf("\nSFM_SEED1_TASK_PAIRS_MULTI %llu",
               (unsigned long long)sfm_task_pairs(1));
        sfm_free(sfm);
    }

    /* object detection: the fixed net's digest, seed-1 photo 0 source
     * digest, its three reference digests, its per-class counts and
     * the task MAC/image counts */
    {
        od_ctx *od = od_prepare(1);

        if (!od)
            return 1;
        {
            uint8_t nd[20];

            memcpy(nd, od_net_digest(od), 20);
            sha1_hex(nd, 20, hex);
            printf("\nOD_SEED1_NET_SHA1 %s", hex);
        }
        sha1_hex(od_photo(od, 0), od_photo_len(od, 0), hex);
        printf("\nOD_SEED1_PHOTO0_SHA1 %s len=%zu", hex,
               od_photo_len(od, 0));
        {
            const od_result *r = od_reference(od, 0);

            printf("\nOD_SEED1_PHOTO0_FEAT");
            for (int i = 0; i < 20; i++)
                printf(" %02x", r->feature_digest[i]);
            printf("\nOD_SEED1_PHOTO0_TENS");
            for (int i = 0; i < 20; i++)
                printf(" %02x", r->tensor_digest[i]);
            printf("\nOD_SEED1_PHOTO0_DET");
            for (int i = 0; i < 20; i++)
                printf(" %02x", r->det_digest[i]);
            printf("\nOD_SEED1_PHOTO0_CLASSES");
            for (unsigned c = 0; c < OD_CLASSES; c++)
                printf(" %u", r->classes[c]);
        }
        printf("\nOD_SEED1_TASK_MACS %llu",
               (unsigned long long)od_task_macs());
        printf("\nOD_SEED1_TASK_IMAGES %llu",
               (unsigned long long)od_task_images(0));
        printf("\nOD_SEED1_TASK_IMAGES_MULTI %llu",
               (unsigned long long)od_task_images(1));
        od_free(od);
    }
    return 0;
}
