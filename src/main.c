/* LotofBench — suite driver.
 *
 * Runs every implemented workload single-core and multi-core, in the
 * order of the Geekbench 6 CPU workload list (docs/benchmarks.md), and
 * prints one calibrated score per workload. The per-workload machinery
 * lives in workload_*.c; the shared-task threading and timing in
 * harness.c; this file owns the CLI and the output. */

#define _POSIX_C_SOURCE 200809L

#include "harness.h"
#include "lotofbench.h"
#include "workload_assetcompression.h"
#include "workload_clang.h"
#include "workload_compression.h"
#include "workload_horizon.h"
#include "workload_html5browser.h"
#include "workload_navigation.h"
#include "workload_pdfrender.h"
#include "workload_photolibrary.h"
#include "workload_textprocessing.h"
#include "workload_photofilter.h"
#include "workload_raytracer.h"
#include "workload_sfm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    unsigned corpus_mb, runs, seed, threads; /* requested thread count */
    int json;
} options;

static void usage(FILE *out)
{
    fprintf(out,
            "usage: lotofbench [--size-mb N] [--runs N] [--threads N] "
            "[--seed N] [--json]\n");
}

static options parse_args(int argc, char **argv)
{
    options o = { DEFAULT_CORPUS_MB, DEFAULT_RUNS, DEFAULT_SEED, 0, 0 };
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    if (nproc < 1)
        nproc = 1;
    o.threads = (unsigned)nproc;
    if (o.threads > DEFAULT_THREADS_CAP)
        o.threads = DEFAULT_THREADS_CAP;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) {
            o.corpus_mb = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--runs") && i + 1 < argc) {
            o.runs = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            o.threads = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            o.seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--json")) {
            o.json = 1;
        } else if (!strcmp(argv[i], "--help")) {
            usage(stdout);
            exit(0);
        } else {
            fprintf(stderr, "lotofbench: bad or incomplete option '%s'\n",
                    argv[i]);
            usage(stderr);
            exit(2);
        }
    }
    if (o.corpus_mb < 1)
        o.corpus_mb = 1;
    if (o.runs < 1 || o.runs > MAX_RUNS) {
        fprintf(stderr, "lotofbench: --runs must be between 1 and %u\n",
                MAX_RUNS);
        exit(2);
    }
    if (o.threads < 1)
        o.threads = 1;
    return o;
}

/* --- running the suite ------------------------------------------------------*/

static int run_fc(fc_ctx *fc, unsigned runs, unsigned threads_req,
                  lb_result *out)
{
    unsigned t = threads_req;

    if (fc_run(fc, runs, &t, &out->throughput) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_FILECOMPRESSION_MBPS);
    return 0;
}

static int run_nav(nav_ctx *nav, unsigned runs, unsigned threads_req,
                   lb_result *out, uint64_t *edges_relaxed)
{
    unsigned t = threads_req;

    if (nav_run(nav, runs, &t, &out->throughput, edges_relaxed) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_NAVIGATION_MEDGES);
    return 0;
}

static int run_h5(html5_ctx *h5, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *pages_rendered)
{
    unsigned t = threads_req;

    if (html5_run(h5, runs, &t, &out->throughput, pages_rendered) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_HTML5BROWSER_PPS);
    return 0;
}

static int run_pdf(pdf_ctx *pdf, unsigned runs, unsigned threads_req,
                   lb_result *out, uint64_t *pixels)
{
    unsigned t = threads_req;

    if (pdf_run(pdf, runs, &t, &out->throughput, pixels) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_PDFRENDER_MPIXELS);
    return 0;
}

static int run_pl(pl_ctx *pl, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *photos)
{
    unsigned t = threads_req;

    if (pl_run(pl, runs, &t, &out->throughput, photos) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_PHOTOLIBRARY_IPS);
    return 0;
}

static int run_cl(cl_ctx *cl, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *tokens)
{
    unsigned t = threads_req;

    if (cl_run(cl, runs, &t, &out->throughput, tokens) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_CLANG_KTOKENS);
    return 0;
}

static int run_tp(tp_ctx *tp, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *bytes)
{
    unsigned t = threads_req;

    if (tp_run(tp, runs, &t, &out->throughput, bytes) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_TEXTPROCESSING_MBPS);
    return 0;
}

static int run_ac(ac_ctx *ac, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *bytes)
{
    unsigned t = threads_req;

    if (ac_run(ac, runs, &t, &out->throughput, bytes) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_ASSETCOMPRESSION_MBPS);
    return 0;
}

static int run_pf(pf_ctx *pf, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *pixels)
{
    unsigned t = threads_req;

    if (pf_run(pf, runs, &t, &out->throughput, pixels) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_PHOTOFILTER_MPIXELS);
    return 0;
}

static int run_hd(hd_ctx *hd, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *pixels)
{
    unsigned t = threads_req;

    if (hd_run(hd, runs, &t, &out->throughput, pixels) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput,
                          BASELINE_HORIZONDETECTION_MPIXELS);
    return 0;
}

static int run_rt(rt_ctx *rt, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *rays)
{
    unsigned t = threads_req;

    if (rt_run(rt, runs, &t, &out->throughput, rays) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_RAYTRACER_MRAYS);
    return 0;
}

static int run_sfm(sfm_ctx *sfm, unsigned runs, unsigned threads_req,
                   lb_result *out, uint64_t *pairs)
{
    unsigned t = threads_req;

    if (sfm_run(sfm, runs, &t, &out->throughput, pairs) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_SFM_PAIRS);
    return 0;
}

/* --- output ------------------------------------------------------------------*/

static void print_block(const char *unit, double baseline,
                        const lb_result *single, const lb_result *multi)
{
    printf("Single-Core\n");
    printf("  throughput  %9.1f %s\n", single->throughput, unit);
    printf("  score       %9ld   (baseline %.1f %s = 1000)\n\n",
           single->score, baseline, unit);
    printf("Multi-Core (%u threads, shared task)\n", multi->threads);
    printf("  throughput  %9.1f %s\n", multi->throughput, unit);
    printf("  score       %9ld\n", multi->score);
}

static void print_json(const options *opt, const lb_result *fc_single,
                       const lb_result *fc_multi,
                       const lb_result *nav_single,
                       const lb_result *nav_multi, uint64_t nav_ops,
                       const lb_result *h5_single,
                       const lb_result *h5_multi, uint64_t h5_pages,
                       uint64_t h5_elements,
                       const lb_result *pdf_single,
                       const lb_result *pdf_multi, uint64_t pdf_pixels,
                       const lb_result *pl_single,
                       const lb_result *pl_multi, uint64_t pl_photos,
                       const lb_result *cl_single,
                       const lb_result *cl_multi, uint64_t cl_tokens,
                       const lb_result *tp_single,
                       const lb_result *tp_multi, uint64_t tp_bytes,
                       const lb_result *ac_single,
                       const lb_result *ac_multi, uint64_t ac_bytes,
                       const lb_result *hd_single,
                       const lb_result *hd_multi, uint64_t hd_pixels,
                       const lb_result *pf_single,
                       const lb_result *pf_multi, uint64_t pf_pixels,
                       const lb_result *rt_single,
                       const lb_result *rt_multi, uint64_t rt_rays,
                       unsigned rt_triangles,
                       const lb_result *sfm_single,
                       const lb_result *sfm_multi, uint64_t sfm_pairs)
{
    printf("{\n");
    printf("  \"benchmark\": \"lotofbench\",\n");
    printf("  \"version\": \"%s\",\n", LOTOFBENCH_VERSION);
    printf("  \"seed\": %u,\n", opt->seed);
    printf("  \"runs\": %u,\n", opt->runs);
    printf("  \"workloads\": [\n");
    printf("    {\"name\": \"%s\", \"corpus_mb\": %u, "
           "\"baseline_mbps\": %.1f,\n",
           FC_WORKLOAD_SLUG, opt->corpus_mb, BASELINE_FILECOMPRESSION_MBPS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld},\n",
           fc_single->threads, fc_single->throughput, fc_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld}},\n",
           fc_multi->threads, fc_multi->throughput, fc_multi->score);
    printf("    {\"name\": \"%s\", \"grid_side\": %u, \"routes\": %u, "
           "\"edges_relaxed\": %llu, \"baseline_medges\": %.1f,\n",
           NAV_WORKLOAD_SLUG, NAV_GRID_SIDE, NAV_ROUTES,
           (unsigned long long)nav_ops, BASELINE_NAVIGATION_MEDGES);
    printf("     \"single\": {\"threads\": %u, \"throughput_medges\": %.1f, "
           "\"score\": %ld},\n",
           nav_single->threads, nav_single->throughput, nav_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_medges\": %.1f, "
           "\"score\": %ld}},\n",
           nav_multi->threads, nav_multi->throughput, nav_multi->score);
    printf("    {\"name\": \"%s\", \"pages\": %u, \"view\": \"%ux%u\", "
           "\"elements\": %llu, \"pages_rendered\": %llu, "
           "\"baseline_pps\": %.1f,\n",
           HTML5_WORKLOAD_SLUG, HTML5_PAGES, HTML5_VIEW_W, HTML5_VIEW_H,
           (unsigned long long)h5_elements,
           (unsigned long long)h5_pages, BASELINE_HTML5BROWSER_PPS);
    printf("     \"single\": {\"threads\": %u, \"throughput_pps\": %.1f, "
           "\"score\": %ld},\n",
           h5_single->threads, h5_single->throughput, h5_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_pps\": %.1f, "
           "\"score\": %ld}},\n",
           h5_multi->threads, h5_multi->throughput, h5_multi->score);
    printf("    {\"name\": \"%s\", \"docs\": %u, \"pages_per_doc\": %u, "
           "\"pixels\": %llu, \"baseline_mpixels\": %.1f,\n",
           PDF_WORKLOAD_SLUG, PDF_DOCS, PDF_PAGES_PER_DOC,
           (unsigned long long)pdf_pixels, BASELINE_PDFRENDER_MPIXELS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mpixels\": %.1f, "
           "\"score\": %ld},\n",
           pdf_single->threads, pdf_single->throughput, pdf_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mpixels\": %.1f, "
           "\"score\": %ld}},\n",
           pdf_multi->threads, pdf_multi->throughput, pdf_multi->score);
    printf("    {\"name\": \"%s\", \"photos\": %u, \"view\": \"%ux%u\", "
           "\"classes\": %u, \"task_photos\": %llu, \"baseline_ips\": %.1f,\n",
           PL_WORKLOAD_SLUG, PL_PHOTOS, PL_PHOTO_W, PL_PHOTO_H,
           PL_CLASSES, (unsigned long long)pl_photos,
           BASELINE_PHOTOLIBRARY_IPS);
    printf("     \"single\": {\"threads\": %u, \"throughput_ips\": %.1f, "
           "\"score\": %ld},\n",
           pl_single->threads, pl_single->throughput, pl_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_ips\": %.1f, "
           "\"score\": %ld}},\n",
           pl_multi->threads, pl_multi->throughput, pl_multi->score);
    printf("    {\"name\": \"%s\", \"units\": %u, \"tokens\": %llu, "
           "\"baseline_ktokens\": %.1f,\n",
           CL_WORKLOAD_SLUG, CLANG_UNITS, (unsigned long long)cl_tokens,
           BASELINE_CLANG_KTOKENS);
    printf("     \"single\": {\"threads\": %u, \"throughput_ktokens\": %.1f, "
           "\"score\": %ld},\n",
           cl_single->threads, cl_single->throughput, cl_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_ktokens\": %.1f, "
           "\"score\": %ld}},\n",
           cl_multi->threads, cl_multi->throughput, cl_multi->score);
    printf("    {\"name\": \"%s\", \"docs\": %u, \"patterns_per_doc\": %u, "
           "\"task_bytes\": %llu, \"baseline_mbps\": %.1f,\n",
           TP_WORKLOAD_SLUG, TP_DOCS, TP_PATTERNS,
           (unsigned long long)tp_bytes, BASELINE_TEXTPROCESSING_MBPS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld},\n",
           tp_single->threads, tp_single->throughput, tp_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld}},\n",
           tp_multi->threads, tp_multi->throughput, tp_multi->score);
    printf("    {\"name\": \"%s\", \"assets\": %u, \"textures\": %u, "
           "\"meshes\": %u, \"task_bytes\": %llu, \"baseline_mbps\": %.1f,\n",
           AC_WORKLOAD_SLUG, AC_ASSETS, AC_TEXTURES + AC_ALPHA_TEXTURES,
           AC_MESHES, (unsigned long long)ac_bytes,
           BASELINE_ASSETCOMPRESSION_MBPS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld},\n",
           ac_single->threads, ac_single->throughput, ac_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld}},\n",
           ac_multi->threads, ac_multi->throughput, ac_multi->score);
    printf("    {\"name\": \"%s\", \"photos\": %u, \"view\": \"%ux%u\", "
           "\"task_pixels\": %llu, \"baseline_mpixels\": %.1f,\n",
           HD_WORKLOAD_SLUG, HD_PHOTOS, HD_PHOTO_W, HD_PHOTO_H,
           (unsigned long long)hd_pixels,
           BASELINE_HORIZONDETECTION_MPIXELS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mpixels\": %.1f, "
           "\"score\": %ld},\n",
           hd_single->threads, hd_single->throughput, hd_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mpixels\": %.1f, "
           "\"score\": %ld}},\n",
           hd_multi->threads, hd_multi->throughput, hd_multi->score);
    printf("    {\"name\": \"%s\", \"photos\": %u, \"view\": \"%ux%u\", "
           "\"task_pixels\": %llu, \"baseline_mpixels\": %.1f,\n",
           PF_WORKLOAD_SLUG, PF_PHOTOS, PF_PHOTO_W, PF_PHOTO_H,
           (unsigned long long)pf_pixels, BASELINE_PHOTOFILTER_MPIXELS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mpixels\": %.1f, "
           "\"score\": %ld},\n",
           pf_single->threads, pf_single->throughput, pf_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mpixels\": %.1f, "
           "\"score\": %ld}},\n",
           pf_multi->threads, pf_multi->throughput, pf_multi->score);
    printf("    {\"name\": \"%s\", \"views\": %u, \"view\": \"%ux%u\", "
           "\"triangles\": %u, \"task_rays\": %llu, \"baseline_mrays\": %.1f,\n",
           RT_WORKLOAD_SLUG, RT_VIEWS, RT_VIEW_W, RT_VIEW_H,
           rt_triangles, (unsigned long long)rt_rays,
           BASELINE_RAYTRACER_MRAYS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mrays\": %.1f, "
           "\"score\": %ld},\n",
           rt_single->threads, rt_single->throughput, rt_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mrays\": %.1f, "
           "\"score\": %ld}},\n",
           rt_multi->threads, rt_multi->throughput, rt_multi->score);
    printf("    {\"name\": \"%s\", \"pairs\": %u, \"view\": \"%ux%u\", "
           "\"scene_points\": %u, \"task_pairs\": %llu, "
           "\"baseline_pairs\": %.1f,\n",
           SFM_WORKLOAD_SLUG, SFM_PAIRS, SFM_VIEW_W, SFM_VIEW_H,
           SFM_SCENE_POINTS, (unsigned long long)sfm_pairs,
           BASELINE_SFM_PAIRS);
    printf("     \"single\": {\"threads\": %u, \"throughput_pairs\": %.1f, "
           "\"score\": %ld},\n",
           sfm_single->threads, sfm_single->throughput, sfm_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_pairs\": %.1f, "
           "\"score\": %ld}}\n",
           sfm_multi->threads, sfm_multi->throughput, sfm_multi->score);
    printf("  ]\n");
    printf("}\n");
}

int main(int argc, char **argv)
{
    options opt = parse_args(argc, argv);
    fc_ctx *fc = fc_prepare(opt.corpus_mb, opt.seed);
    nav_ctx *nav = nav_prepare(opt.seed);
    html5_ctx *h5 = html5_prepare(opt.seed);
    pdf_ctx *pdf = pdf_prepare(opt.seed);
    pl_ctx *pl = pl_prepare(opt.seed);
    cl_ctx *cl = cl_prepare(opt.seed);
    tp_ctx *tp = tp_prepare(opt.seed);
    ac_ctx *ac = ac_prepare(opt.seed);
    hd_ctx *hd = hd_prepare(opt.seed);
    pf_ctx *pf = pf_prepare(opt.seed);
    rt_ctx *rt = rt_prepare(opt.seed);
    sfm_ctx *sfm = sfm_prepare(opt.seed);

    if (!fc || !nav || !h5 || !pdf || !pl || !cl || !tp || !ac || !hd ||
        !pf || !rt || !sfm) {
        fprintf(stderr,
                "lotofbench: out of memory building the workload inputs\n");
        fc_free(fc);
        nav_free(nav);
        html5_free(h5);
        pdf_free(pdf);
        pl_free(pl);
        cl_free(cl);
        tp_free(tp);
        ac_free(ac);
        hd_free(hd);
        pf_free(pf);
        rt_free(rt);
        sfm_free(sfm);
        return 1;
    }

    lb_result fc_single = { 0, 0.0, 0 }, fc_multi = { 0, 0.0, 0 };
    lb_result nav_single = { 0, 0.0, 0 }, nav_multi = { 0, 0.0, 0 };
    lb_result h5_single = { 0, 0.0, 0 }, h5_multi = { 0, 0.0, 0 };
    lb_result pdf_single = { 0, 0.0, 0 }, pdf_multi = { 0, 0.0, 0 };
    lb_result pl_single = { 0, 0.0, 0 }, pl_multi = { 0, 0.0, 0 };
    lb_result cl_single = { 0, 0.0, 0 }, cl_multi = { 0, 0.0, 0 };
    lb_result tp_single = { 0, 0.0, 0 }, tp_multi = { 0, 0.0, 0 };
    lb_result ac_single = { 0, 0.0, 0 }, ac_multi = { 0, 0.0, 0 };
    lb_result hd_single = { 0, 0.0, 0 }, hd_multi = { 0, 0.0, 0 };
    lb_result pf_single = { 0, 0.0, 0 }, pf_multi = { 0, 0.0, 0 };
    lb_result rt_single = { 0, 0.0, 0 }, rt_multi = { 0, 0.0, 0 };
    lb_result sfm_single = { 0, 0.0, 0 }, sfm_multi = { 0, 0.0, 0 };
    uint64_t nav_ops = 0, h5_pages = 0, pdf_pixels = 0, pl_photos = 0,
             cl_tokens = 0, tp_bytes = 0, ac_bytes = 0, hd_pixels = 0,
             pf_pixels = 0, rt_rays = 0, sfm_pairs = 0;

    if (run_fc(fc, opt.runs, 1, &fc_single) != 0 ||
        run_fc(fc, opt.runs, opt.threads, &fc_multi) != 0 ||
        run_nav(nav, opt.runs, 1, &nav_single, &nav_ops) != 0 ||
        run_nav(nav, opt.runs, opt.threads, &nav_multi, &nav_ops) != 0 ||
        run_h5(h5, opt.runs, 1, &h5_single, &h5_pages) != 0 ||
        run_h5(h5, opt.runs, opt.threads, &h5_multi, &h5_pages) != 0 ||
        run_pdf(pdf, opt.runs, 1, &pdf_single, &pdf_pixels) != 0 ||
        run_pdf(pdf, opt.runs, opt.threads, &pdf_multi, &pdf_pixels) != 0 ||
        run_pl(pl, opt.runs, 1, &pl_single, &pl_photos) != 0 ||
        run_pl(pl, opt.runs, opt.threads, &pl_multi, &pl_photos) != 0 ||
        run_cl(cl, opt.runs, 1, &cl_single, &cl_tokens) != 0 ||
        run_cl(cl, opt.runs, opt.threads, &cl_multi, &cl_tokens) != 0 ||
        run_tp(tp, opt.runs, 1, &tp_single, &tp_bytes) != 0 ||
        run_tp(tp, opt.runs, opt.threads, &tp_multi, &tp_bytes) != 0 ||
        run_ac(ac, opt.runs, 1, &ac_single, &ac_bytes) != 0 ||
        run_ac(ac, opt.runs, opt.threads, &ac_multi, &ac_bytes) != 0 ||
        run_hd(hd, opt.runs, 1, &hd_single, &hd_pixels) != 0 ||
        run_hd(hd, opt.runs, opt.threads, &hd_multi, &hd_pixels) != 0 ||
        run_pf(pf, opt.runs, 1, &pf_single, &pf_pixels) != 0 ||
        run_pf(pf, opt.runs, opt.threads, &pf_multi, &pf_pixels) != 0 ||
        run_rt(rt, opt.runs, 1, &rt_single, &rt_rays) != 0 ||
        run_rt(rt, opt.runs, opt.threads, &rt_multi, &rt_rays) != 0 ||
        run_sfm(sfm, opt.runs, 1, &sfm_single, &sfm_pairs) != 0 ||
        run_sfm(sfm, opt.runs, opt.threads, &sfm_multi, &sfm_pairs) != 0) {
        fprintf(stderr,
                "lotofbench: workload failed — a result did not verify\n");
        fc_free(fc);
        nav_free(nav);
        html5_free(h5);
        pdf_free(pdf);
        pl_free(pl);
        cl_free(cl);
        tp_free(tp);
        ac_free(ac);
        hd_free(hd);
        pf_free(pf);
        rt_free(rt);
        sfm_free(sfm);
        return 1;
    }

    if (opt.json) {
        print_json(&opt, &fc_single, &fc_multi, &nav_single, &nav_multi,
                   nav_ops, &h5_single, &h5_multi, h5_pages,
                   html5_elements(h5), &pdf_single, &pdf_multi,
                   pdf_pixels, &pl_single, &pl_multi, pl_photos,
                   &cl_single, &cl_multi, cl_tokens,
                   &tp_single, &tp_multi, tp_bytes, &ac_single, &ac_multi,
                   ac_bytes, &hd_single, &hd_multi,
                   hd_pixels, &pf_single, &pf_multi, pf_pixels,
                   &rt_single, &rt_multi, rt_rays,
                   (unsigned)rt_tri_count(rt), &sfm_single, &sfm_multi,
                   sfm_pairs);
    } else {
        char fc_cfg[64], nav_cfg[64], h5_cfg[64], pdf_cfg[96], pl_cfg[96],
             cl_cfg[64], tp_cfg[64], ac_cfg[96], hd_cfg[96], pf_cfg[96],
             rt_cfg[96], sfm_cfg[96];

        snprintf(fc_cfg, sizeof(fc_cfg), "corpus %u MB synthetic",
                 opt.corpus_mb);
        snprintf(nav_cfg, sizeof(nav_cfg), "grid %ux%u, %u routes",
                 NAV_GRID_SIDE, NAV_GRID_SIDE, NAV_ROUTES);
        snprintf(h5_cfg, sizeof(h5_cfg), "%u pages, %llux%llu viewport",
                 HTML5_PAGES, (unsigned long long)HTML5_VIEW_W,
                 (unsigned long long)HTML5_VIEW_H);
        snprintf(pdf_cfg, sizeof(pdf_cfg),
                 "%u park maps, %u pages each, %ux%u px @2x", PDF_DOCS,
                 PDF_PAGES_PER_DOC, PDF_PAGE_W, PDF_PAGE_H);
        snprintf(pl_cfg, sizeof(pl_cfg),
                 "%u photos, %ux%u px, %u classes", PL_PHOTOS, PL_PHOTO_W,
                 PL_PHOTO_H, PL_CLASSES);
        snprintf(cl_cfg, sizeof(cl_cfg), "%u units, %llu source tokens",
                 CLANG_UNITS, (unsigned long long)cl_tokens);
        snprintf(tp_cfg, sizeof(tp_cfg),
                 "%u docs, %u patterns/doc", TP_DOCS, TP_PATTERNS);
        snprintf(ac_cfg, sizeof(ac_cfg),
                 "%u assets (%u textures, %u meshes)", AC_ASSETS,
                 AC_TEXTURES + AC_ALPHA_TEXTURES, AC_MESHES);
        snprintf(hd_cfg, sizeof(hd_cfg),
                 "%u photos, %ux%u px, 6 archetypes, Canny+Hough",
                 HD_PHOTOS, HD_PHOTO_W, HD_PHOTO_H);
        snprintf(pf_cfg, sizeof(pf_cfg),
                 "%u photos, %ux%u px, 6 archetypes", PF_PHOTOS,
                 PF_PHOTO_W, PF_PHOTO_H);
        snprintf(rt_cfg, sizeof(rt_cfg),
                 "%u views, %ux%u px, %u triangles, Whitted",
                 RT_VIEWS, RT_VIEW_W, RT_VIEW_H,
                 (unsigned)rt_tri_count(rt));
        snprintf(sfm_cfg, sizeof(sfm_cfg),
                 "%u pairs, %ux%u px, %u scene points, FAST+RANSAC",
                 SFM_PAIRS, SFM_VIEW_W, SFM_VIEW_H, SFM_SCENE_POINTS);

        printf("LotofBench %s\n", LOTOFBENCH_VERSION);
        printf("seed %u, %u runs, median\n\n", opt.seed, opt.runs);
        printf("%s (%s)\n", FC_WORKLOAD_NAME, fc_cfg);
        print_block("MB/s", BASELINE_FILECOMPRESSION_MBPS, &fc_single,
                    &fc_multi);
        printf("\n%s (%s)\n", NAV_WORKLOAD_NAME, nav_cfg);
        print_block("Medge/s", BASELINE_NAVIGATION_MEDGES, &nav_single,
                    &nav_multi);
        printf("\n%s (%s, %llu DOM elements)\n", HTML5_WORKLOAD_NAME,
               h5_cfg, (unsigned long long)html5_elements(h5));
        print_block("pages/s", BASELINE_HTML5BROWSER_PPS, &h5_single,
                    &h5_multi);
        printf("\n%s (%s)\n", PDF_WORKLOAD_NAME, pdf_cfg);
        print_block("Mpixel/s", BASELINE_PDFRENDER_MPIXELS, &pdf_single,
                    &pdf_multi);
        printf("\n%s (%s)\n", PL_WORKLOAD_NAME, pl_cfg);
        print_block("img/s", BASELINE_PHOTOLIBRARY_IPS, &pl_single,
                    &pl_multi);
        printf("\n%s (%s)\n", CL_WORKLOAD_NAME, cl_cfg);
        print_block("Ktokens/s", BASELINE_CLANG_KTOKENS, &cl_single,
                    &cl_multi);
        printf("\n%s (%s)\n", TP_WORKLOAD_NAME, tp_cfg);
        print_block("MB/s", BASELINE_TEXTPROCESSING_MBPS, &tp_single,
                    &tp_multi);
        printf("\n%s (%s)\n", AC_WORKLOAD_NAME, ac_cfg);
        print_block("MB/s", BASELINE_ASSETCOMPRESSION_MBPS, &ac_single,
                    &ac_multi);
        printf("\n%s (%s)\n", HD_WORKLOAD_NAME, hd_cfg);
        print_block("Mpixel/s", BASELINE_HORIZONDETECTION_MPIXELS,
                    &hd_single, &hd_multi);
        printf("\n%s (%s)\n", PF_WORKLOAD_NAME, pf_cfg);
        print_block("Mpixel/s", BASELINE_PHOTOFILTER_MPIXELS, &pf_single,
                    &pf_multi);
        printf("\n%s (%s)\n", RT_WORKLOAD_NAME, rt_cfg);
        print_block("Mray/s", BASELINE_RAYTRACER_MRAYS, &rt_single,
                    &rt_multi);
        printf("\n%s (%s)\n", SFM_WORKLOAD_NAME, sfm_cfg);
        print_block("pairs/s", BASELINE_SFM_PAIRS, &sfm_single, &sfm_multi);
    }

    fc_free(fc);
    nav_free(nav);
    html5_free(h5);
    pdf_free(pdf);
    pl_free(pl);
    cl_free(cl);
    tp_free(tp);
    ac_free(ac);
    hd_free(hd);
    pf_free(pf);
    rt_free(rt);
    sfm_free(sfm);
    return 0;
}
