/* Unit tests — workload #16: Structure from Motion. */

#include "tests.h"

#include "sha1.h"
#include "workload_sfm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.12.0, seed 1). After a
 * DELIBERATE change to the scene generator or the reconstruction
 * pipeline, re-run the tool and port the printed values here in the
 * same commit. */
static const uint8_t SFM_GOLDEN_PAIR0_REF[20] = {
    0xcb, 0xe7, 0xb3, 0xdd, 0x6f, 0x22, 0x26, 0x4e, 0x67, 0x2e,
    0x52, 0x7a, 0xa0, 0xb2, 0xa3, 0x15, 0x8a, 0x3f, 0xd4, 0xd7
};
#define SFM_GOLDEN_VIEW0_SHA1_STR \
    "91a30df7a15e92d800c701f0f488b204e676abfd"
#define SFM_GOLDEN_PAIR0_CORNERS_L 1113u
#define SFM_GOLDEN_PAIR0_CORNERS_R 976u
#define SFM_GOLDEN_PAIR0_INLIERS   246u
#define SFM_GOLDEN_PAIR0_TRI       246u
static const unsigned SFM_GOLDEN_VISIBLE[SFM_PAIRS] = {
    282u, 288u, 219u, 269u, 240u, 258u,
    272u, 288u, 223u, 288u, 288u, 239u
};

/* Generation floor on the visible count (SFM_VIS_MIN in the workload;
 * the value is part of the documented input contract). */
#define SFM_TEST_VIS_MIN 64u

/* Number of planted mismatches of one pair. */
static unsigned outlier_count(const sfm_ctx *ctx, unsigned pair)
{
    unsigned cnt = 0;

    for (unsigned i = 0; i < sfm_visible(ctx, pair); i++)
        if (sfm_planted_of(ctx, pair, i)->is_outlier)
            cnt++;
    return cnt;
}

/* One shared prepare for the read-only tests (the scene is immutable;
 * the process exits right after the suite anyway). */
static sfm_ctx *shared_sfm(void)
{
    static sfm_ctx *ctx;

    if (!ctx)
        ctx = sfm_prepare(1);
    return ctx;
}

/* Max |R*R^T - I| over all entries — rotation orthonormality error. */
static float ortho_err(const float R[9])
{
    float worst = 0.0f;

    for (unsigned r = 0; r < 3u; r++)
        for (unsigned c = 0; c < 3u; c++) {
            float dot = R[r * 3] * R[c * 3] + R[r * 3 + 1] * R[c * 3 + 1] +
                        R[r * 3 + 2] * R[c * 3 + 2];
            float want = r == c ? 1.0f : 0.0f;
            float e = dot - want;

            if (e < 0.0f)
                e = -e;
            if (e > worst)
                worst = e;
        }
    return worst;
}

/* F = K^-T E K^-1 for the shared intrinsics (K = diag(f, f, 1) with
 * the principal point) — camera-space E to pixel space. */
static void ess_to_pixels(const float E[9], float F[9])
{
    const float f = SFM_FOCAL;
    const float ix = 1.0f / f;
    const float Kinv[9] = { ix, 0.0f, -SFM_CX * ix,
                            0.0f, ix, -SFM_CY * ix,
                            0.0f, 0.0f, 1.0f };
    float KT[9], EK[9];

    KT[0] = Kinv[0];
    KT[1] = Kinv[3];
    KT[2] = Kinv[6];
    KT[3] = Kinv[1];
    KT[4] = Kinv[4];
    KT[5] = Kinv[7];
    KT[6] = Kinv[2];
    KT[7] = Kinv[5];
    KT[8] = Kinv[8];
    for (unsigned r = 0; r < 3u; r++)
        for (unsigned c = 0; c < 3u; c++) {
            float s = 0.0f;

            for (unsigned k = 0; k < 3u; k++)
                s += KT[r * 3 + k] * E[k * 3 + c];
            EK[r * 3 + c] = s;
        }
    for (unsigned r = 0; r < 3u; r++)
        for (unsigned c = 0; c < 3u; c++) {
            float s = 0.0f;

            for (unsigned k = 0; k < 3u; k++)
                s += EK[r * 3 + k] * Kinv[k * 3 + c];
            F[r * 3 + c] = s;
        }
}

/* Projection of a camera-frame point through the shared intrinsics. */
static void project_px(const float xc[3], float *u, float *v)
{
    *u = SFM_FOCAL * xc[0] / xc[2] + SFM_CX;
    *v = SFM_FOCAL * xc[1] / xc[2] + SFM_CY;
}

void test_sfm_scene_deterministic(void)
{
    sfm_ctx *a = sfm_prepare(1), *b = sfm_prepare(1);

    CHECK(a && b);
    if (!a || !b) {
        sfm_free(a);
        sfm_free(b);
        return;
    }
    for (unsigned p = 0; p < SFM_PAIRS; p++) {
        CHECK_U64(sfm_visible(a, p), sfm_visible(b, p));
        CHECK(memcmp(sfm_reference(a, p), sfm_reference(b, p),
                     sizeof(sfm_result)) == 0);
        CHECK(memcmp(sfm_camera_left(a, p), sfm_camera_left(b, p),
                     sizeof(sfm_camera)) == 0);
        CHECK(memcmp(sfm_camera_right(a, p), sfm_camera_right(b, p),
                     sizeof(sfm_camera)) == 0);
        CHECK(memcmp(sfm_view(a, p, 0), sfm_view(b, p, 0),
                     (size_t)SFM_VIEW_W * SFM_VIEW_H) == 0);
    }
    sfm_free(a);
    sfm_free(b);
}

void test_sfm_input_invariants(void)
{
    sfm_ctx *ctx = shared_sfm();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned p = 0; p < SFM_PAIRS; p++) {
        const float *R = sfm_gt_R(ctx, p);
        const float *t = sfm_gt_t(ctx, p);
        float E[9], F[9];
        unsigned n = sfm_visible(ctx, p);

        CHECK(sfm_view(ctx, p, 0) && sfm_view(ctx, p, 1));
        CHECK(n >= SFM_TEST_VIS_MIN && n <= SFM_MAX_VISIBLE);
        CHECK(ortho_err(sfm_camera_left(ctx, p)->R) < 1e-3f);
        CHECK(ortho_err(sfm_camera_right(ctx, p)->R) < 1e-3f);
        CHECK(ortho_err(R) < 1e-3f);
        CHECK(sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]) > 0.1f);
        CHECK(sfm_essential_from_pose(R, t, E) == 0);
        ess_to_pixels(E, F);
        for (unsigned i = 0; i < n; i++) {
            const sfm_planted *pl = sfm_planted_of(ctx, p, i);
            sfm_corr c;
            float e2;

            CHECK(pl);
            if (!pl)
                continue;
            CHECK(pl->u1 >= 8.0f && pl->u1 <= (float)(SFM_VIEW_W - 9u));
            CHECK(pl->v1 >= 8.0f && pl->v1 <= (float)(SFM_VIEW_H - 9u));
            CHECK(pl->u2 >= 8.0f && pl->u2 <= (float)(SFM_VIEW_W - 9u));
            CHECK(pl->v2 >= 8.0f && pl->v2 <= (float)(SFM_VIEW_H - 9u));
            c.u1 = pl->u1;
            c.v1 = pl->v1;
            c.u2 = pl->u2;
            c.v2 = pl->v2;
            e2 = sfm_sampson2(F, &c);
            if (pl->is_outlier)
                /* planted mismatches sit far off their epipolar line:
                 * 10x the RANSAC threshold (the generator guarantee) */
                CHECK(e2 > 100.0f * SFM_RANSAC_PX * SFM_RANSAC_PX);
            else
                /* exact correspondences, quantized to pixels */
                CHECK(e2 <= SFM_RANSAC_PX * SFM_RANSAC_PX);
        }
        /* patch spacing: 9 px Chebyshev clearance between the stamped
         * disks. The left view holds every planted patch; the right
         * view too, except planted mismatches whose right coordinates
         * are another point's exact position by construction. */
        for (unsigned i = 0; i < n; i++) {
            const sfm_planted *pi = sfm_planted_of(ctx, p, i);

            for (unsigned j = i + 1u; j < n; j++) {
                const sfm_planted *pj = sfm_planted_of(ctx, p, j);
                float dxl = fabsf(pi->u1 - pj->u1);
                float dyl = fabsf(pi->v1 - pj->v1);
                float dxr = fabsf(pi->u2 - pj->u2);
                float dyr = fabsf(pi->v2 - pj->v2);
                float cl = dxl > dyl ? dxl : dyl;

                CHECK(cl >= 9.0f);
                if (pi->is_outlier || pj->is_outlier)
                    continue;
                cl = dxr > dyr ? dxr : dyr;
                CHECK(cl >= 9.0f);
            }
        }
    }
}

void test_sfm_reference_golden(void)
{
    sfm_ctx *ctx = shared_sfm();
    const sfm_result *r;
    const uint8_t *v;
    char hex[41];

    CHECK(ctx);
    if (!ctx)
        return;
    r = sfm_reference(ctx, 0);
    CHECK(r);
    if (!r)
        return;
    CHECK(memcmp(r->digest, SFM_GOLDEN_PAIR0_REF, 20) == 0);
    CHECK_U64(r->corners_left, SFM_GOLDEN_PAIR0_CORNERS_L);
    CHECK_U64(r->corners_right, SFM_GOLDEN_PAIR0_CORNERS_R);
    CHECK_U64(r->inliers, SFM_GOLDEN_PAIR0_INLIERS);
    CHECK_U64(r->triangulated, SFM_GOLDEN_PAIR0_TRI);
    CHECK_U64(r->inliers + outlier_count(ctx, 0),
              sfm_visible(ctx, 0));
    for (unsigned p = 0; p < SFM_PAIRS; p++)
        CHECK_U64(sfm_visible(ctx, p), SFM_GOLDEN_VISIBLE[p]);
    v = sfm_view(ctx, 0, 0);
    sha1_hex(v, (size_t)SFM_VIEW_W * SFM_VIEW_H, hex);
    CHECK_STR(hex, SFM_GOLDEN_VIEW0_SHA1_STR);
}

void test_sfm_essential_estimator(void)
{
    sfm_ctx *ctx = shared_sfm();

    CHECK(ctx);
    if (!ctx)
        return;
    for (unsigned p = 0; p < SFM_PAIRS; p++) {
        const float *R = sfm_gt_R(ctx, p);
        const float *t = sfm_gt_t(ctx, p);
        unsigned n = sfm_visible(ctx, p);
        sfm_corr c[SFM_MAX_VISIBLE];
        float E[9], Egt[9], Fgt[9];
        uint8_t mask[SFM_MAX_VISIBLE];
        unsigned n_in = 0;

        for (unsigned i = 0; i < n; i++) {
            const sfm_planted *pl = sfm_planted_of(ctx, p, i);

            c[i].u1 = pl->u1;
            c[i].v1 = pl->v1;
            c[i].u2 = pl->u2;
            c[i].v2 = pl->v2;
            c[i].id = pl->scene_id;
        }
        CHECK(sfm_essential_ransac(c, n, SFM_RANSAC_ITERS, 0x9e3779b9ull,
                                   E, mask, &n_in) == 0);
        CHECK_U64(n_in, n - outlier_count(ctx, p));
        for (unsigned i = 0; i < n; i++)
            /* mask[i] = 1 must mark exactly the planted inliers */
            CHECK_U64(!!mask[i], !sfm_planted_of(ctx, p, i)->is_outlier);
        /* the estimated pixel-space E must be proportional to the one
         * built independently from the planted pose */
        CHECK(sfm_essential_from_pose(R, t, Egt) == 0);
        ess_to_pixels(Egt, Fgt);
        {
            /* cos angle between E and Fgt, magnitude only (the scale
             * and the sign are free): |E . Fgt| / (|E| |Fgt|) ~ 1 */
            float de = 0.0f, dg = 0.0f, cross = 0.0f;

            for (unsigned k = 0; k < 9u; k++) {
                de += E[k] * E[k];
                dg += Fgt[k] * Fgt[k];
                cross += E[k] * Fgt[k];
            }
            cross = cross < 0.0f ? -cross : cross;
            CHECK(cross / sqrtf(de * dg) > 0.999f);
        }
    }
}

void test_sfm_fast9_synthetic(void)
{
    enum { W = 48, H = 48 };
    static uint8_t g[W * H];
    sfm_corner out[16];
    int got;

    memset(g, 100, sizeof(g));
    got = sfm_fast9_detect(g, W, H, SFM_FAST_THRESHOLD, 16, out);
    CHECK(got == 0); /* flat frame: nothing fires */

    /* one bright disk with a dark centre, like the workload patches */
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
            if (dx * dx + dy * dy <= 16)
                g[(size_t)(16 + dy) * W + (16 + dx)] = 200;
    g[(size_t)16 * W + 16] = 40;
    got = sfm_fast9_detect(g, W, H, SFM_FAST_THRESHOLD, 16, out);
    CHECK(got >= 1);
    if (got >= 1) {
        int centre = 0;

        for (int i = 0; i < got; i++)
            if (out[i].x == 16 && out[i].y == 16)
                centre = 1;
        CHECK(centre); /* the patch centre is always a local maximum */
    }

    /* NULL / degenerate arguments */
    CHECK(sfm_fast9_detect(NULL, W, H, 24, 16, out) == -1);
    CHECK(sfm_fast9_detect(g, 7u, H, 24, 16, out) == -1);
    CHECK(sfm_fast9_detect(g, W, H, 0, 16, out) == -1);
    CHECK(sfm_fast9_detect(g, W, H, 24, 0, out) == -1);
    CHECK(sfm_fast9_detect(g, W, H, 24, 16, NULL) == -1);
}

void test_sfm_jacobi3_eigen(void)
{
    {
        float m[9] = { 3.0f, 0.0f, 0.0f, 0.0f, 2.0f, 0.0f,
                       0.0f, 0.0f, 1.0f };
        float vals[3], vecs[9];

        CHECK(sfm_jacobi3(m, vals, vecs) == 0);
        CHECK_DBL(vals[0], 3.0);
        CHECK_DBL(vals[1], 2.0);
        CHECK_DBL(vals[2], 1.0);
    }
    {
        /* non-diagonal: eigenvalues 5, 3, 1 */
        float m[9] = { 2.0f, 1.0f, 0.0f, 1.0f, 2.0f, 0.0f,
                       0.0f, 0.0f, 5.0f };
        float orig[9], vals[3], vecs[9];

        memcpy(orig, m, sizeof(orig));
        CHECK(sfm_jacobi3(m, vals, vecs) == 0);
        CHECK(fabsf(vals[0] - 5.0f) < 1e-5f);
        CHECK(fabsf(vals[1] - 3.0f) < 1e-5f);
        CHECK(fabsf(vals[2] - 1.0f) < 1e-5f);
        for (unsigned c = 0; c < 3u; c++) {
            /* A v = lambda v for each eigenvector column */
            float v[3] = { vecs[c * 3], vecs[c * 3 + 1],
                           vecs[c * 3 + 2] };
            float Av[3] = {
                orig[0] * v[0] + orig[1] * v[1] + orig[2] * v[2],
                orig[3] * v[0] + orig[4] * v[1] + orig[5] * v[2],
                orig[6] * v[0] + orig[7] * v[1] + orig[8] * v[2]
            };

            CHECK(fabsf(Av[0] - vals[c] * v[0]) < 1e-4f);
            CHECK(fabsf(Av[1] - vals[c] * v[1]) < 1e-4f);
            CHECK(fabsf(Av[2] - vals[c] * v[2]) < 1e-4f);
        }
    }
    CHECK(sfm_jacobi3(NULL, NULL, NULL) == -1);
}

void test_sfm_jacobi9_nullvec(void)
{
    /* exact synthetic correspondences of a pure horizontal
     * translation: the 8-point AtA must be rank-8 and jacobi9_min must
     * return its (unit) null vector */
    const float b = 1.6f;
    float rows[8][9];
    float ata[81], vec[9];
    float norm = 0.0f;

    for (unsigned k = 0; k < 8u; k++) {
        float X = ((float)(k % 3) - 1.0f) * 1.3f;
        float Y = ((float)(k / 3 % 2) - 0.5f) * 1.1f;
        float Z = 4.0f + (float)(k / 6) * 1.7f;
        float u1 = SFM_FOCAL * X / Z + SFM_CX;
        float v1 = SFM_FOCAL * Y / Z + SFM_CY;
        float u2 = SFM_FOCAL * (X - b) / Z + SFM_CX;

        /* Hartley-style normalized rows (centroid/scale chosen exact:
         * the null-space test does not need sqrt(2) mean distance) */
        float a = u1 - 100.0f, q = v1 - 100.0f;
        float cc = u2 + 100.0f, d = v1 - 100.0f;

        rows[k][0] = cc * a;
        rows[k][1] = cc * q;
        rows[k][2] = cc;
        rows[k][3] = d * a;
        rows[k][4] = d * q;
        rows[k][5] = d;
        rows[k][6] = a;
        rows[k][7] = q;
        rows[k][8] = 1.0f;
    }
    for (unsigned i = 0; i < 81u; i++)
        ata[i] = 0.0f;
    for (unsigned k = 0; k < 8u; k++)
        for (unsigned i = 0; i < 9u; i++)
            for (unsigned j = 0; j < 9u; j++)
                ata[i * 9 + j] += rows[k][i] * rows[k][j];
    CHECK(sfm_jacobi9_min(ata, vec) == 0);
    for (unsigned k = 0; k < 9u; k++)
        norm += vec[k] * vec[k];
    CHECK(fabsf(norm - 1.0f) < 1e-4f); /* unit eigenvector */
    for (unsigned k = 0; k < 8u; k++) {
        float r = 0.0f;

        for (unsigned j = 0; j < 9u; j++)
            r += rows[k][j] * vec[j];
        CHECK(fabsf(r) < 1e-3f); /* the rows are annihilated */
    }
    CHECK(sfm_jacobi9_min(NULL, NULL) == -1);
}

void test_sfm_sampson_known(void)
{
    const float R[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                         0.0f, 0.0f, 1.0f };
    const float t[3] = { -1.6f, 0.0f, 0.0f };
    float E[9], F[9];
    sfm_corr c;

    CHECK(sfm_essential_from_pose(R, t, E) == 0);
    ess_to_pixels(E, F);
    /* exact correspondence of a point 6 m away, quantized to pixels */
    c.u1 = (float)(int)(SFM_FOCAL * 0.5f / 6.0f + SFM_CX + 0.5f);
    c.v1 = (float)(int)(SFM_FOCAL * 0.3f / 6.0f + SFM_CY + 0.5f);
    c.u2 = (float)(int)(SFM_FOCAL * (0.5f - 1.6f) / 6.0f + SFM_CX + 0.5f);
    c.v2 = c.v1; /* pure horizontal motion keeps the row */
    CHECK(sfm_sampson2(F, &c) < 1.0f);
    /* a 10 px row shift is far outside the epipolar band */
    c.v2 = c.v1 + 10.0f;
    CHECK(sfm_sampson2(F, &c) > 25.0f);
}

void test_sfm_decompose_pose(void)
{
    /* 30 deg about y then 20 deg about x, float-rounded */
    const float cy = 0.8660254f, sy = 0.5f;
    const float cx = 0.9396926f, sx = 0.3420201f;
    float Ry[9] = { cy,  0.0f, sy,
                    0.0f, 1.0f, 0.0f,
                    -sy, 0.0f, cy };
    float Rx[9] = { 1.0f, 0.0f,  0.0f,
                    0.0f, cx,  -sx,
                    0.0f, sx,  cx };
    float Rt[9], R[9], t[3] = { 0.3f, -1.7f, 0.9f };
    float E[9], R1[9], R2[9], tdir[3];
    float tn = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);

    for (unsigned r = 0; r < 3u; r++)
        for (unsigned c = 0; c < 3u; c++) {
            float s = 0.0f;

            for (unsigned k = 0; k < 3u; k++)
                s += Ry[r * 3 + k] * Rx[k * 3 + c];
            Rt[r * 3 + c] = s;
        }
    memcpy(R, Rt, sizeof(R));
    for (unsigned k = 0; k < 3u; k++)
        t[k] /= tn;
    /* the decomposition consumes an essential matrix, not a rotation */
    CHECK(sfm_essential_from_pose(R, t, E) == 0);
    CHECK(sfm_decompose_essential(E, R1, R2, tdir) == 0);
    /* one of the four (Ri, +/-t) candidates is the planted pose */
    {
        int ok = 0;

        for (int which = 0; which < 2 && !ok; which++) {
            const float *cand = which == 0 ? R1 : R2;

            for (int sign = 0; sign < 2 && !ok; sign++) {
                float s = sign == 0 ? 1.0f : -1.0f;
                float err = 0.0f;
                float dot = 0.0f;

                for (unsigned k = 0; k < 9u; k++) {
                    float e = cand[k] - R[k];

                    err += e * e;
                }
                for (unsigned k = 0; k < 3u; k++)
                    dot += tdir[k] * s * t[k];
                if (sqrtf(err) < 5e-3f && dot > 0.999f)
                    ok = 1;
            }
        }
        CHECK(ok);
    }
    CHECK(sfm_decompose_essential(NULL, NULL, NULL, NULL) == -1);
}

void test_sfm_triangulate_known(void)
{
    const float R[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                         0.0f, 0.0f, 1.0f };
    const float t[3] = { -1.6f, 0.0f, 0.0f };
    const float X[3] = { 0.5f, 0.3f, 6.0f };
    sfm_corr c;
    float Xh[3];

    project_px(X, &c.u1, &c.v1);
    {
        float xr[3] = { X[0] + t[0], X[1] + t[1], X[2] + t[2] };

        project_px(xr, &c.u2, &c.v2);
    }
    c.u1 = (float)(int)(c.u1 + 0.5f);
    c.v1 = (float)(int)(c.v1 + 0.5f);
    c.u2 = (float)(int)(c.u2 + 0.5f);
    c.v2 = (float)(int)(c.v2 + 0.5f);
    CHECK(sfm_triangulate_midpoint(R, t, &c, Xh) == 0);
    /* the reconstruction inherits the metric scale of t */
    CHECK(fabsf(Xh[0] - X[0]) < 0.2f);
    CHECK(fabsf(Xh[1] - X[1]) < 0.2f);
    CHECK(fabsf(Xh[2] - X[2]) < 0.2f);
    CHECK(sfm_triangulate_midpoint(NULL, NULL, NULL, NULL) == -1);
}

void test_sfm_seed_variation(void)
{
    sfm_ctx *a = sfm_prepare(1), *b = sfm_prepare(2);

    CHECK(a && b);
    if (!a || !b) {
        sfm_free(a);
        sfm_free(b);
        return;
    }
    CHECK(memcmp(sfm_reference(a, 0)->digest,
                 sfm_reference(b, 0)->digest, 20) != 0);
    sfm_free(a);
    sfm_free(b);
}

void test_sfm_run_sane(void)
{
    sfm_ctx *ctx = shared_sfm();
    unsigned threads = 1;
    double pairs_per_s = 0.0;
    uint64_t pairs = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(sfm_run(ctx, 1, &threads, &pairs_per_s, &pairs) == 0);
    CHECK_U64(pairs, sfm_task_pairs(0));
    CHECK_U64(pairs, SFM_PAIRS);
    CHECK(pairs_per_s > 0.0);
    CHECK_U64(threads, 1);
    threads = 2;
    CHECK(sfm_run(ctx, 1, &threads, &pairs_per_s, &pairs) == 0);
    CHECK_U64(pairs, sfm_task_pairs(1));
    CHECK_U64(pairs, SFM_PAIRS_MULTI);
    CHECK_U64(threads, 2);
}

void test_sfm_thread_clamp(void)
{
    sfm_ctx *ctx = shared_sfm();
    unsigned threads = 999;
    double pairs_per_s = 0.0;
    uint64_t pairs = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* a thread request above 1 selects the multi pass (48 pairs) and
     * clamps to its pair set: one pair instance per thread at least */
    CHECK(sfm_run(ctx, 1, &threads, &pairs_per_s, &pairs) == 0);
    CHECK_U64(pairs, SFM_PAIRS_MULTI);
    CHECK_U64(threads, SFM_PAIRS_MULTI);
}

void test_sfm_accessors_null_safe(void)
{
    CHECK(sfm_view(NULL, 0, 0) == NULL);
    CHECK(sfm_reference(NULL, 0) == NULL);
    CHECK(sfm_camera_left(NULL, 0) == NULL);
    CHECK(sfm_camera_right(NULL, 0) == NULL);
    CHECK(sfm_gt_R(NULL, 0) == NULL);
    CHECK(sfm_gt_t(NULL, 0) == NULL);
    CHECK_U64(sfm_visible(NULL, 0), 0);
    CHECK(sfm_planted_of(NULL, 0, 0) == NULL);

    sfm_ctx *ctx = shared_sfm();

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(sfm_view(ctx, SFM_PAIRS, 0) == NULL);
    CHECK(sfm_view(ctx, 0, 1) == sfm_view(ctx, 0, 2));
    CHECK(sfm_reference(ctx, SFM_PAIRS) == NULL);
    CHECK(sfm_camera_left(ctx, SFM_PAIRS) == NULL);
    CHECK(sfm_camera_right(ctx, SFM_PAIRS) == NULL);
    CHECK(sfm_gt_R(ctx, SFM_PAIRS) == NULL);
    CHECK(sfm_gt_t(ctx, SFM_PAIRS) == NULL);
    CHECK_U64(sfm_visible(ctx, SFM_PAIRS), 0);
    CHECK(sfm_planted_of(ctx, 0, SFM_MAX_VISIBLE) == NULL);
}
