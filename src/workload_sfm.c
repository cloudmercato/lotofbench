/* Workload #16 — Structure from Motion. See workload_sfm.h for the
 * contract; docs/benchmarks.md ("Workload 16") for the human version. */

#include "workload_sfm.h"

#include "harness.h"
#include "itrig.h"
#include "rng.h"
#include "sha1.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Hard caps shared by the generator and the pipeline workspaces. */
#define SFM_MAX_CORNERS 2048u /* NMS cap; high-contrast rims flood scores */

/* Patch geometry: every visible point is stamped as a bright disk of
 * radius 4 with a single dark centre pixel. The dark centre is always
 * the strongest corner of its patch; the disk boundary also produces
 * genuine FAST corners (a bright disk on dark background has dark
 * 9-arcs at its rim) — they are real detections, counted as such, and
 * simply never win the nearest-corner matching (the centre corner sits
 * at distance zero). See docs/benchmarks.md. */
#define SFM_PATCH_R2 16 /* bright disk: dx*dx + dy*dy <= 16 */

/* Detection/matching and verification bounds. */
#define SFM_MATCH_PX2    9.0f     /* pairing radius, squared (3 px) */
#define SFM_REPROJ_PX    2.0f     /* per-view reprojection bound */
#define SFM_POSE_TOL     0.02f    /* max |R_est - R_gt| entry */
#define SFM_TDIR_COS     0.99939f /* cos(2 deg) translation direction */
#define SFM_SHAPE_TOL    0.10f    /* relative cloud-shape error bound */
#define SFM_CAND_MIN_NUM 9        /* winner needs >= 90% positive-depth */
#define SFM_VIS_MIN      64u      /* generation floor on visible points */

/* Fixed grid steps of the synthetic point cloud (hard-coded decimal
 * literals — bit-identical constants, no runtime trigonometry). */
#define SFM_STEP_X  0.8f  /* ground/ceiling x grid, 16 columns */
#define SFM_STEP_ZG 1.3f  /* ground/ceiling z rows, 8 rows from z=3 */
#define SFM_STEP_Y  0.55f /* facade/back-wall y rows, 8 from y=0.5 */

/* Serialised reconstruction: 4-byte inlier count + pose (12 floats as
 * q6 fixed point) + cloud (3 floats per inlier as q16 fixed point). */
#define SFM_Q6          64.0f
#define SFM_Q16         65536.0f
#define SFM_SERIAL_SIZE (4u + 12u * 4u + SFM_MAX_VISIBLE * 12u)

/* Deterministic stream mixes: every generator owns one stream, so the
 * inputs are independent of each other and of every other workload. */
#define SFM_STREAM_SCENE  11u
#define SFM_STREAM_CAM    101u
#define SFM_STREAM_FRAME  1009u
#define SFM_STREAM_OUT    5003u
#define SFM_STREAM_RANSAC 50021u

static uint64_t sfm_stream(unsigned seed, unsigned salt, unsigned pair)
{
    return (uint64_t)seed * 1000003u + salt + (uint64_t)pair * 131u;
}

/* --- tiny float helpers (IEEE-exact ops only) ------------------------------*/

static float fabs1f(float x)
{
    return x < 0.0f ? -x : x;
}

static float v3_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void v3_cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float v3_norm(const float a[3])
{
    return sqrtf(v3_dot(a, a));
}

static void v3_normalize(float a[3])
{
    float n = v3_norm(a);

    if (n > 0.0f) {
        a[0] /= n;
        a[1] /= n;
        a[2] /= n;
    }
}

/* out = a + k * b */
static void v3_mad(const float a[3], const float b[3], float k, float out[3])
{
    out[0] = a[0] + b[0] * k;
    out[1] = a[1] + b[1] * k;
    out[2] = a[2] + b[2] * k;
}

static void v3_scale(const float a[3], float k, float out[3])
{
    out[0] = a[0] * k;
    out[1] = a[1] * k;
    out[2] = a[2] * k;
}

/* out = R * x (R row-major 3x3). */
static void m3_mul_vec(const float R[9], const float x[3], float out[3])
{
    out[0] = R[0] * x[0] + R[1] * x[1] + R[2] * x[2];
    out[1] = R[3] * x[0] + R[4] * x[1] + R[5] * x[2];
    out[2] = R[6] * x[0] + R[7] * x[1] + R[8] * x[2];
}

/* out = Rt * x (transpose of row-major R). */
static void m3_mul_vec_t(const float R[9], const float x[3], float out[3])
{
    out[0] = R[0] * x[0] + R[3] * x[1] + R[6] * x[2];
    out[1] = R[1] * x[0] + R[4] * x[1] + R[7] * x[2];
    out[2] = R[2] * x[0] + R[5] * x[1] + R[8] * x[2];
}

/* out = A * B (row-major 3x3). */
static void m3_mul(const float A[9], const float B[9], float out[9])
{
    for (unsigned r = 0; r < 3; r++)
        for (unsigned c = 0; c < 3; c++)
            out[r * 3 + c] = A[r * 3] * B[c] + A[r * 3 + 1] * B[3 + c] +
                             A[r * 3 + 2] * B[6 + c];
}

static void m3_transpose(const float A[9], float out[9])
{
    for (unsigned r = 0; r < 3; r++)
        for (unsigned c = 0; c < 3; c++)
            out[c * 3 + r] = A[r * 3 + c];
}

static float m3_det(const float A[9])
{
    return A[0] * (A[4] * A[8] - A[5] * A[7]) -
           A[1] * (A[3] * A[8] - A[5] * A[6]) +
           A[2] * (A[3] * A[7] - A[4] * A[6]);
}

/* Deterministic float quantization for digests: round half away from
 * zero at q steps per unit. */
static int32_t fx(float x, float q)
{
    float s = x * q;

    return (int32_t)(s >= 0.0f ? s + 0.5f : s - 0.5f);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
    p[2] = (uint8_t)((v >> 16) & 0xffu);
    p[3] = (uint8_t)((v >> 24) & 0xffu);
}

/* --- FAST-9 ----------------------------------------------------------------*/

/* The 16-pixel Bresenham ring of radius 3, clockwise from north
 * (antipodal pairs sit 8 slots apart; every offset satisfies
 * dx*dx + dy*dy in {8, 9, 10}). */
static const int8_t FAST_RING[16][2] = {
    { 0, -3 }, { 1, -3 }, { 2, -2 }, { 3, -1 },
    { 3, 0 },  { 3, 1 },  { 2, 2 },  { 1, 3 },
    { 0, 3 },  { -1, 3 }, { -2, 2 }, { -3, 1 },
    { -3, 0 }, { -3, -1 }, { -2, -2 }, { -1, -3 }
};

/* Corner test at one pixel: +1 when at least 9 contiguous ring pixels
 * are brighter than the centre by t, -1 when at least 9 are darker by
 * t, 0 otherwise. *score is the ring SAD (the NMS key). */
static int fast9_test(const uint8_t *g, unsigned w, int x, int y, int t,
                      uint32_t *score)
{
    const uint8_t *centre = g + (size_t)y * w + x;
    int c = *centre;
    int flags[16];
    int run = 0, best = 0, cur_sign = 0;
    uint32_t sad = 0;

    for (int i = 0; i < 16; i++) {
        int v = (int)centre[(ptrdiff_t)FAST_RING[i][1] * (ptrdiff_t)w +
                            FAST_RING[i][0]] - c;

        flags[i] = v >= t ? 1 : (v <= -t ? -1 : 0);
        sad += (uint32_t)(v < 0 ? -v : v);
    }
    /* Longest circular run of one sign: scan the doubled sequence. */
    for (int k = 0; k < 32; k++) {
        int f = flags[k & 15];

        if (f != 0 && f == cur_sign) {
            run++;
        } else {
            cur_sign = f;
            run = f != 0 ? 1 : 0;
        }
        if (run > best)
            best = run;
    }
    if (score)
        *score = sad;
    return best >= 9 ? cur_sign : 0;
}

/* NMS order: score descending, then y, then x — a total order on
 * distinct pixels, so the sort is deterministic. */
static int corner_cmp(const void *a, const void *b)
{
    const sfm_corner *ca = a, *cb = b;

    if (ca->score != cb->score)
        return ca->score > cb->score ? -1 : 1;
    if (ca->y != cb->y)
        return ca->y < cb->y ? -1 : 1;
    if (ca->x != cb->x)
        return ca->x < cb->x ? -1 : 1;
    return 0;
}

int sfm_fast9_detect(const uint8_t *g, unsigned w, unsigned h, int threshold,
                     unsigned max_out, sfm_corner *out)
{
    size_t cap = 1024, count = 0;
    sfm_corner *all;
    unsigned kept = 0;

    if (!g || w < 8u || h < 8u || threshold < 1 || !out || max_out == 0u)
        return -1;
    all = malloc(cap * sizeof(*all));
    if (!all)
        return -1;
    for (unsigned y = 3u; y + 3u < h; y++) {
        for (unsigned x = 3u; x + 3u < w; x++) {
            uint32_t score;
            int fire = fast9_test(g, w, (int)x, (int)y, threshold, &score);

            if (fire == 0)
                continue;
            if (count == cap) {
                sfm_corner *grown;

                if (cap > ((size_t)-1) / (2u * sizeof(*all)))
                    goto oom;
                grown = realloc(all, cap * 2u * sizeof(*all));
                if (!grown)
                    goto oom;
                all = grown;
                cap *= 2u;
            }
            all[count].x = (uint16_t)x;
            all[count].y = (uint16_t)y;
            all[count].score = score;
            count++;
        }
    }
    qsort(all, count, sizeof(*all), corner_cmp);
    /* Greedy non-max suppression: keep a corner only if no kept corner
     * lies within the 3x3-neighbourhood radius of 3 pixels. */
    for (size_t i = 0; i < count && kept < max_out; i++) {
        int keep = 1;

        for (unsigned k = 0; k < kept; k++) {
            int dx = (int)all[i].x - (int)out[k].x;
            int dy = (int)all[i].y - (int)out[k].y;

            if (dx < 0)
                dx = -dx;
            if (dy < 0)
                dy = -dy;
            if (dx <= 3 && dy <= 3) {
                keep = 0;
                break;
            }
        }
        if (keep)
            out[kept++] = all[i];
    }
    free(all);
    return (int)kept;
oom:
    free(all);
    return -1;
}

/* --- Jacobi eigensolvers -----------------------------------------------------*/

/* One Jacobi rotation on the n x n symmetric matrix m (row-major),
 * accumulating the eigenvector basis in v. Zeroes m[p][q]. IEEE-exact
 * arithmetic only (Golub-Van Loan / Numerical Recipes form). */
static void jacobi_rot(float *m, float *v, unsigned n, unsigned p, unsigned q)
{
    float apq = m[p * n + q];

    if (apq == 0.0f)
        return;
    {
        float app = m[p * n + p];
        float aqq = m[q * n + q];
        float tau = (aqq - app) / (2.0f * apq);
        float t = (tau >= 0.0f ? 1.0f : -1.0f) /
                  (fabs1f(tau) + sqrtf(1.0f + tau * tau));
        float c = 1.0f / sqrtf(1.0f + t * t);
        float s = t * c;

        m[p * n + p] = app - t * apq;
        m[q * n + q] = aqq + t * apq;
        m[p * n + q] = 0.0f;
        m[q * n + p] = 0.0f;
        for (unsigned k = 0; k < n; k++) {
            if (k == p || k == q)
                continue;
            {
                float akp = m[k * n + p];
                float akq = m[k * n + q];

                m[k * n + p] = c * akp - s * akq;
                m[p * n + k] = m[k * n + p];
                m[k * n + q] = s * akp + c * akq;
                m[q * n + k] = m[k * n + q];
            }
        }
        for (unsigned k = 0; k < n; k++) {
            float vkp = v[k * n + p];
            float vkq = v[k * n + q];

            v[k * n + p] = c * vkp - s * vkq;
            v[k * n + q] = s * vkp + c * vkq;
        }
    }
}

/* Canonical eigenvector sign: largest-|component| made positive (the
 * sign of an eigenvector is arbitrary; pinning it keeps every
 * downstream result deterministic). */
static void canon_sign_col(float *v, unsigned n, unsigned col)
{
    unsigned bi = 0;
    float bv = 0.0f;

    for (unsigned k = 0; k < n; k++) {
        float a = fabs1f(v[k * n + col]);

        if (a > bv) {
            bv = a;
            bi = k;
        }
    }
    if (v[bi * n + col] < 0.0f)
        for (unsigned k = 0; k < n; k++)
            v[k * n + col] = -v[k * n + col];
}

int sfm_jacobi3(float m[9], float vals[3], float vecs[9])
{
    float v[9] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f };
    float d[3];
    unsigned order[3] = { 0, 1, 2 };

    if (!m || !vals || !vecs)
        return -1;
    for (unsigned sweep = 0; sweep < 12u; sweep++)
        for (unsigned p = 0; p < 2u; p++)
            for (unsigned q = p + 1u; q < 3u; q++)
                jacobi_rot(m, v, 3u, p, q);
    for (unsigned i = 0; i < 3u; i++)
        d[i] = m[i * 3 + i];
    /* Selection sort, eigenvalues descending, ties keep the lower
     * index — deterministic. */
    for (unsigned i = 0; i < 2u; i++) {
        unsigned bi = i;

        for (unsigned k = i + 1u; k < 3u; k++)
            if (d[order[k]] > d[order[bi]])
                bi = k;
        if (bi != i) {
            unsigned tmp = order[i];

            order[i] = order[bi];
            order[bi] = tmp;
        }
    }
    for (unsigned c = 0; c < 3u; c++) {
        canon_sign_col(v, 3u, order[c]);
        for (unsigned r = 0; r < 3u; r++)
            vecs[c * 3 + r] = v[r * 3 + order[c]];
    }
    /* Right-handed basis: flip the smallest-eigenvalue column if the
     * determinant came out negative (its sign is arbitrary anyway). */
    {
        float V[9];

        for (unsigned c = 0; c < 3u; c++)
            for (unsigned r = 0; r < 3u; r++)
                V[r * 3 + c] = vecs[c * 3 + r];
        if (m3_det(V) < 0.0f)
            for (unsigned r = 0; r < 3u; r++)
                vecs[2 * 3 + r] = -vecs[2 * 3 + r];
    }
    vals[0] = d[order[0]];
    vals[1] = d[order[1]];
    vals[2] = d[order[2]];
    return 0;
}

int sfm_jacobi9_min(const float m[81], float vec[9])
{
    float a[81];
    float v[81];
    unsigned bi = 0;
    float bd = 0.0f, norm;

    if (!m || !vec)
        return -1;
    memcpy(a, m, sizeof(a));
    for (unsigned i = 0; i < 81u; i++)
        v[i] = 0.0f;
    for (unsigned i = 0; i < 9u; i++)
        v[i * 9 + i] = 1.0f;
    for (unsigned sweep = 0; sweep < 12u; sweep++)
        for (unsigned p = 0; p < 8u; p++)
            for (unsigned q = p + 1u; q < 9u; q++)
                jacobi_rot(a, v, 9u, p, q);
    for (unsigned i = 0; i < 9u; i++) {
        float di = a[i * 9 + i];

        if (i == 0u || di < bd) {
            bd = di;
            bi = i;
        }
    }
    norm = 0.0f;
    for (unsigned k = 0; k < 9u; k++)
        norm += v[k * 9 + bi] * v[k * 9 + bi];
    norm = sqrtf(norm);
    if (norm < 1e-12f)
        return -1;
    for (unsigned k = 0; k < 9u; k++)
        vec[k] = v[k * 9 + bi] / norm;
    /* canonical sign: largest-|component| made positive */
    {
        unsigned bmax = 0;
        float bval = 0.0f;

        for (unsigned k = 0; k < 9u; k++) {
            float av = fabs1f(vec[k]);

            if (av > bval) {
                bval = av;
                bmax = k;
            }
        }
        if (vec[bmax] < 0.0f)
            for (unsigned k = 0; k < 9u; k++)
                vec[k] = -vec[k];
    }
    return 0;
}

/* --- essential-matrix machinery ---------------------------------------------*/

float sfm_sampson2(const float E[9], const sfm_corr *c)
{
    float a = E[0] * c->u1 + E[1] * c->v1 + E[2];
    float b = E[3] * c->u1 + E[4] * c->v1 + E[5];
    float d = E[0] * c->u2 + E[3] * c->v2 + E[6];
    float e = E[1] * c->u2 + E[4] * c->v2 + E[7];
    float num = c->u2 * a + c->v2 * b + E[6] * c->u1 + E[7] * c->v1 + E[8];
    float den = a * a + b * b + d * d + e * e;

    if (den < 1e-12f)
        return 1e30f;
    return num * num / den;
}

/* Hartley normalization of one side of a correspondence subset:
 * translate to the centroid, scale so the mean distance to the origin
 * is sqrt(2). Fills the transform parameters. */
static int hartley_norm(const sfm_corr *c, const unsigned *idx,
                        unsigned count, int right, float *cx, float *cy,
                        float *sc)
{
    float mu = 0.0f, mv = 0.0f, md = 0.0f;

    if (count == 0u)
        return -1;
    for (unsigned i = 0; i < count; i++) {
        const sfm_corr *ci = &c[idx[i]];

        mu += right ? ci->u2 : ci->u1;
        mv += right ? ci->v2 : ci->v1;
    }
    mu /= (float)count;
    mv /= (float)count;
    for (unsigned i = 0; i < count; i++) {
        const sfm_corr *ci = &c[idx[i]];
        float du = (right ? ci->u2 : ci->u1) - mu;
        float dv = (right ? ci->v2 : ci->v1) - mv;

        md += sqrtf(du * du + dv * dv);
    }
    md /= (float)count;
    if (md < 1e-9f)
        return -1;
    *cx = mu;
    *cy = mv;
    *sc = sqrtf(2.0f) / md;
    return 0;
}

/* Denormalize: E = T2inv * En * T1 with T = [[s,0,-s cx],[0,s,-s cy],
 * [0,0,1]] (pixel space -> normalized space) on the left side and its
 * inverse on the right side. */
static void essential_denorm(const float En[9], float c1x, float c1y,
                             float s1, float c2x, float c2y, float s2,
                             float E[9])
{
    /* x2^T E x1 = (T2 x2)^T En (T1 x1)  =>  E = T2^T En T1 */
    const float T1[9] = { s1, 0.0f, -s1 * c1x, 0.0f, s1, -s1 * c1y,
                          0.0f, 0.0f, 1.0f };
    const float T2t[9] = { s2, 0.0f, 0.0f, 0.0f, s2, 0.0f,
                           -s2 * c2x, -s2 * c2y, 1.0f };
    float tmp[9];

    m3_mul(T2t, En, tmp);
    m3_mul(tmp, T1, E);
}

/* Per-workspace RANSAC state (fixed caps; no allocation on the timed
 * path). */
typedef struct {
    uint32_t perm[SFM_MAX_VISIBLE];
    uint8_t  mcur[SFM_MAX_VISIBLE];
    uint8_t  best_mask[SFM_MAX_VISIBLE];
    float    srow[8][9];
    float    AtA[81];
    float    v9[9];
    float    Ecur[9];
    float    Ebest[9];
    unsigned sample[8];
} sfm_ransac_ws;

static int ransac_essential(sfm_ransac_ws *ws, const sfm_corr *c,
                            unsigned n, unsigned iters, uint64_t salt,
                            float E_out[9], uint8_t *mask_out,
                            unsigned *count_out)
{
    rng_t rng;
    unsigned best = 0, inlier_idx[SFM_MAX_VISIBLE], n_in;
    float best_sum = 1e30f;

    if (!ws || !c || n < 8u || n > SFM_MAX_VISIBLE || iters == 0u ||
        !E_out || !mask_out || !count_out)
        return -1;
    rng_init(&rng, salt);
    for (unsigned i = 0; i < n; i++)
        ws->perm[i] = i;
    for (unsigned it = 0; it < iters; it++) {
        float c1x, c1y, s1, c2x, c2y, s2;
        unsigned cnt = 0;
        float sum = 0.0f;

        /* Partial Fisher-Yates: 8 distinct samples, deterministic. */
        for (unsigned k = 0; k < 8u; k++) {
            unsigned j = k + rng_below(&rng, n - k);
            unsigned tmp = ws->perm[k];

            ws->perm[k] = ws->perm[j];
            ws->perm[j] = tmp;
            ws->sample[k] = ws->perm[k];
        }
        if (hartley_norm(c, ws->sample, 8u, 0, &c1x, &c1y, &s1) != 0 ||
            hartley_norm(c, ws->sample, 8u, 1, &c2x, &c2y, &s2) != 0)
            continue; /* degenerate sample — deterministic skip */
        for (unsigned k = 0; k < 8u; k++) {
            const sfm_corr *ci = &c[ws->sample[k]];
            float a = (ci->u1 - c1x) * s1;
            float b = (ci->v1 - c1y) * s1;
            float cc = (ci->u2 - c2x) * s2;
            float d = (ci->v2 - c2y) * s2;
            float *row = ws->srow[k];

            row[0] = cc * a;
            row[1] = cc * b;
            row[2] = cc;
            row[3] = d * a;
            row[4] = d * b;
            row[5] = d;
            row[6] = a;
            row[7] = b;
            row[8] = 1.0f;
        }
        for (unsigned i = 0; i < 9u; i++)
            for (unsigned j = 0; j < 9u; j++) {
                float s = 0.0f;

                for (unsigned k = 0; k < 8u; k++)
                    s += ws->srow[k][i] * ws->srow[k][j];
                ws->AtA[i * 9 + j] = s;
            }
        if (sfm_jacobi9_min(ws->AtA, ws->v9) != 0)
            continue;
        {
            const float En[9] = { ws->v9[0], ws->v9[1], ws->v9[2],
                                  ws->v9[3], ws->v9[4], ws->v9[5],
                                  ws->v9[6], ws->v9[7], ws->v9[8] };

            essential_denorm(En, c1x, c1y, s1, c2x, c2y, s2, ws->Ecur);
        }
        for (unsigned i = 0; i < n; i++) {
            float e2 = sfm_sampson2(ws->Ecur, &c[i]);

            ws->mcur[i] = e2 <= SFM_RANSAC_PX * SFM_RANSAC_PX;
            if (ws->mcur[i]) {
                cnt++;
                sum += e2;
            }
        }
        if (cnt > best || (cnt == best && sum < best_sum)) {
            if (cnt > best && it < 200u)
            best = cnt;
            best_sum = sum;
            memcpy(ws->Ebest, ws->Ecur, sizeof(ws->Ebest));
            memcpy(ws->best_mask, ws->mcur, n);
        }
    }
    if (best < 8u) {
        return -1;
    }
    /* Least-squares refinement over all inliers of the best sample. */
    n_in = 0;
    for (unsigned i = 0; i < n; i++)
        if (ws->best_mask[i])
            inlier_idx[n_in++] = i;
    {
        float c1x, c1y, s1, c2x, c2y, s2;

        if (hartley_norm(c, inlier_idx, n_in, 0, &c1x, &c1y, &s1) != 0 ||
            hartley_norm(c, inlier_idx, n_in, 1, &c2x, &c2y, &s2) != 0) {
            return -1;
        }
        for (unsigned i = 0; i < 81u; i++)
            ws->AtA[i] = 0.0f;
        for (unsigned k = 0; k < n_in; k++) {
            const sfm_corr *ci = &c[inlier_idx[k]];
            float a = (ci->u1 - c1x) * s1;
            float b = (ci->v1 - c1y) * s1;
            float cc = (ci->u2 - c2x) * s2;
            float d = (ci->v2 - c2y) * s2;
            float row[9] = { cc * a, cc * b, cc, d * a, d * b, d,
                             a,      b,      1.0f };

            for (unsigned i = 0; i < 9u; i++)
                for (unsigned j = 0; j < 9u; j++)
                    ws->AtA[i * 9 + j] += row[i] * row[j];
        }
        if (sfm_jacobi9_min(ws->AtA, ws->v9) != 0) {
            return -1;
        }
        {
            const float En[9] = { ws->v9[0], ws->v9[1], ws->v9[2],
                                  ws->v9[3], ws->v9[4], ws->v9[5],
                                  ws->v9[6], ws->v9[7], ws->v9[8] };

            essential_denorm(En, c1x, c1y, s1, c2x, c2y, s2, ws->Ecur);
        }
    }
    {
        unsigned cnt = 0;

        for (unsigned i = 0; i < n; i++) {
            float e2 = sfm_sampson2(ws->Ecur, &c[i]);

            ws->mcur[i] = e2 <= SFM_RANSAC_PX * SFM_RANSAC_PX;
            if (ws->mcur[i])
                cnt++;
        }
        if (cnt < 8u) {
            return -1;
        }
        memcpy(E_out, ws->Ecur, sizeof(ws->Ebest));
        memcpy(mask_out, ws->mcur, n);
        *count_out = cnt;
    }
    return 0;
}

int sfm_essential_ransac(const sfm_corr *c, unsigned n, unsigned iters,
                         uint64_t salt, float E[9], uint8_t *inlier,
                         unsigned *n_inliers)
{
    sfm_ransac_ws ws;

    return ransac_essential(&ws, c, n, iters, salt, E, inlier, n_inliers);
}

int sfm_essential_from_pose(const float R[9], const float t[3], float E[9])
{
    const float tx[9] = { 0.0f,  -t[2], t[1],
                          t[2],  0.0f,  -t[0],
                          -t[1], t[0],  0.0f };

    if (!R || !t || !E)
        return -1;
    m3_mul(tx, R, E);
    return 0;
}

int sfm_decompose_essential(const float E[9], float R1[9], float R2[9],
                            float t[3])
{
    float EtE[9], vals[3], V[9], U[9], UW[9], UWt[9], Vt[9];
    float s1, s2, u1[3], u2[3], u3[3];

    if (!E || !R1 || !R2 || !t)
        return -1;
    for (unsigned i = 0; i < 3u; i++)
        for (unsigned j = 0; j < 3u; j++) {
            float sum = 0.0f;

            for (unsigned k = 0; k < 3u; k++)
                sum += E[k * 3 + i] * E[k * 3 + j];
            EtE[i * 3 + j] = sum;
        }
    if (sfm_jacobi3(EtE, vals, V) != 0)
        return -1;
    s1 = sqrtf(vals[0] > 0.0f ? vals[0] : 0.0f);
    s2 = sqrtf(vals[1] > 0.0f ? vals[1] : 0.0f);
    if (s1 < 1e-9f)
        return -1; /* rank-deficient: not an essential matrix */
    for (unsigned r = 0; r < 3u; r++) {
        u1[r] = V[r];       /* vecs[3*c + r]: column 0 */
        u2[r] = V[3 + r];   /* column 1 */
    }
    {
        float Ev1[3], Ev2[3];

        m3_mul_vec(E, u1, Ev1);
        m3_mul_vec(E, u2, Ev2);
        for (unsigned r = 0; r < 3u; r++) {
            u1[r] = Ev1[r] / s1;
            u2[r] = Ev2[r] / s2;
        }
    }
    v3_normalize(u1);
    v3_normalize(u2);
    v3_cross(u1, u2, u3);
    v3_normalize(u3);
    for (unsigned c = 0; c < 3u; c++)
        for (unsigned r = 0; r < 3u; r++) {
            U[r * 3 + c] = c == 0 ? u1[r] : (c == 1 ? u2[r] : u3[r]);
            /* Vt = V^T: eigenvector c is V[r][c] = vecs[3c + r] */
            Vt[r * 3 + c] = V[3 * r + c];
        }
    {
        const float W[9] = { 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                             0.0f, 0.0f,  1.0f };
        const float Wt[9] = { 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f,
                              0.0f, 0.0f, 1.0f };

        m3_mul(U, W, UW);
        m3_mul(UW, Vt, R1);
        m3_mul(U, Wt, UWt);
        m3_mul(UWt, Vt, R2);
    }
    t[0] = u3[0];
    t[1] = u3[1];
    t[2] = u3[2];
    return 0;
}

int sfm_triangulate_midpoint(const float R[9], const float t[3],
                             const sfm_corr *c, float X[3])
{
    float d1[3], d2r[3], d2[3], o2[3];
    float a, b, cc, den, k1, k2, A[3], B[3];

    if (!R || !t || !c || !X)
        return -1;
    d1[0] = (c->u1 - SFM_CX) / SFM_FOCAL;
    d1[1] = (c->v1 - SFM_CY) / SFM_FOCAL;
    d1[2] = 1.0f;
    v3_normalize(d1);
    d2r[0] = (c->u2 - SFM_CX) / SFM_FOCAL;
    d2r[1] = (c->v2 - SFM_CY) / SFM_FOCAL;
    d2r[2] = 1.0f;
    v3_normalize(d2r);
    m3_mul_vec_t(R, d2r, d2);
    /* Right camera centre in the left frame: X_l with R X_l + t = 0. */
    m3_mul_vec_t(R, t, o2);
    o2[0] = -o2[0];
    o2[1] = -o2[1];
    o2[2] = -o2[2];
    a = v3_dot(d1, d2);
    b = v3_dot(d1, o2);
    cc = v3_dot(d2, o2);
    den = 1.0f - a * a;
    if (fabs1f(den) < 1e-9f)
        return -1; /* near-parallel rays */
    k1 = (b - a * cc) / den;
    k2 = (a * b - cc) / den;
    v3_scale(d1, k1, A);
    v3_mad(o2, d2, k2, B);
    X[0] = (A[0] + B[0]) * 0.5f;
    X[1] = (A[1] + B[1]) * 0.5f;
    X[2] = (A[2] + B[2]) * 0.5f;
    return 0;
}

/* --- pair data and generation ------------------------------------------------*/

typedef struct {
    uint8_t frame_l[SFM_VIEW_W * SFM_VIEW_H];
    uint8_t frame_r[SFM_VIEW_W * SFM_VIEW_H];
    sfm_camera cam_l, cam_r;
    float R_rel[9];      /* planted relative rotation (left -> right) */
    float t_rel[3];      /* planted relative translation (metric) */
    uint64_t ransac_salt;
    unsigned visible;
    sfm_planted planted[SFM_MAX_VISIBLE];
    float left3[SFM_MAX_VISIBLE][3]; /* true left-frame coordinates */
    sfm_result ref;
} sfm_pair_data;

struct sfm_ctx {
    sfm_pair_data *pd[SFM_PAIRS];
};

/* Orthonormal look-at basis: rows of R are (right, up, forward), the
 * world-to-camera rotation of a camera at C looking at T. Cross
 * products only — no trigonometry, no libm. */
static void look_at(const float C[3], const float T[3], float R[9])
{
    static const float world_up[3] = { 0.0f, 1.0f, 0.0f };
    float f[3], s[3], u[3];

    f[0] = T[0] - C[0];
    f[1] = T[1] - C[1];
    f[2] = T[2] - C[2];
    v3_normalize(f);
    v3_cross(world_up, f, s);
    v3_normalize(s);
    v3_cross(f, s, u);
    R[0] = s[0];
    R[1] = s[1];
    R[2] = s[2];
    R[3] = u[0];
    R[4] = u[1];
    R[5] = u[2];
    R[6] = f[0];
    R[7] = f[1];
    R[8] = f[2];
}

static void stamp_patch(uint8_t *frame, unsigned x0, unsigned y0,
                        uint8_t bright, uint8_t dark)
{
    for (int dy = -4; dy <= 4; dy++)
        for (int dx = -4; dx <= 4; dx++)
            if (dx * dx + dy * dy <= SFM_PATCH_R2)
                frame[(size_t)(y0 + dy) * SFM_VIEW_W + (x0 + dx)] = bright;
    frame[(size_t)y0 * SFM_VIEW_W + x0] = dark;
}

/* Build one pair: cameras, visibility, frames, planted outliers, the
 * ground-truth relative pose and the RANSAC stream salt. */
static int build_pair(sfm_pair_data *pd, unsigned seed, unsigned pair,
                      unsigned attempt, const float pts[SFM_SCENE_POINTS][3])
{
    static const float target[3] = { 0.0f, 1.5f, 8.0f };
    rng_t rng;
    int32_t c1024, s1024;
    float sn, cs, radius, height, baseline;
    unsigned theta_bin, axis_deg;
    const uint64_t bump = (uint64_t)attempt * 0x9e3779b9ull;

    rng_init(&rng, sfm_stream(seed, SFM_STREAM_CAM, pair) + bump);
    axis_deg = (int)rng_below(&rng, 111) - 55;
    theta_bin = (unsigned)((4 * (int)axis_deg + 720) % 720);
    itrig_trig(theta_bin, &c1024, &s1024);
    sn = (float)s1024 * (1.0f / 1024.0f);
    cs = (float)c1024 * (1.0f / 1024.0f);
    radius = 10.0f + (float)rng_below(&rng, 41) * 0.1f;
    height = 1.5f + ((int)rng_below(&rng, 21) - 10) * 0.1f;
    baseline = 1.2f + (float)rng_below(&rng, 13) * 0.1f;
    pd->cam_l.c[0] = target[0] + radius * sn;
    pd->cam_l.c[1] = height;
    pd->cam_l.c[2] = target[2] - radius * cs;
    look_at(pd->cam_l.c, target, pd->cam_l.R);
    /* right camera = baseline step along the left camera's right axis */
    {
        float s_l[3] = { pd->cam_l.R[0], pd->cam_l.R[1], pd->cam_l.R[2] };

        pd->cam_r.c[0] = pd->cam_l.c[0] + baseline * s_l[0];
        pd->cam_r.c[1] = pd->cam_l.c[1] + baseline * s_l[1];
        pd->cam_r.c[2] = pd->cam_l.c[2] + baseline * s_l[2];
    }
    look_at(pd->cam_r.c, target, pd->cam_r.R);
    /* planted relative pose: X_r = R_rel X_l + t_rel */
    {
        float Rlt[9], d[3];

        m3_transpose(pd->cam_l.R, Rlt);
        m3_mul(pd->cam_r.R, Rlt, pd->R_rel);
        d[0] = pd->cam_l.c[0] - pd->cam_r.c[0];
        d[1] = pd->cam_l.c[1] - pd->cam_r.c[1];
        d[2] = pd->cam_l.c[2] - pd->cam_r.c[2];
        m3_mul_vec(pd->cam_r.R, d, pd->t_rel);
    }
    pd->ransac_salt = sfm_stream(seed, SFM_STREAM_RANSAC, pair);

    /* visibility with 9-px spacing in both views, in scene order */
    pd->visible = 0;
    for (unsigned p = 0; p < SFM_SCENE_POINTS && pd->visible < SFM_MAX_VISIBLE;
         p++) {
        float Xl[3], Xr[3], Xw[3];
        float ul, vl, ur, vr;

        /* left-frame and right-frame coordinates of the point */
        Xw[0] = pts[p][0] - pd->cam_l.c[0];
        Xw[1] = pts[p][1] - pd->cam_l.c[1];
        Xw[2] = pts[p][2] - pd->cam_l.c[2];
        m3_mul_vec(pd->cam_l.R, Xw, Xl);
        Xw[0] = pts[p][0] - pd->cam_r.c[0];
        Xw[1] = pts[p][1] - pd->cam_r.c[1];
        Xw[2] = pts[p][2] - pd->cam_r.c[2];
        m3_mul_vec(pd->cam_r.R, Xw, Xr);
        if (Xl[2] < 1.0f || Xr[2] < 1.0f)
            continue;
        ul = SFM_FOCAL * Xl[0] / Xl[2] + SFM_CX;
        vl = SFM_FOCAL * Xl[1] / Xl[2] + SFM_CY;
        ur = SFM_FOCAL * Xr[0] / Xr[2] + SFM_CX;
        vr = SFM_FOCAL * Xr[1] / Xr[2] + SFM_CY;
        if (ul < 8.0f || ul > (float)(SFM_VIEW_W - 9u) ||
            vl < 8.0f || vl > (float)(SFM_VIEW_H - 9u) ||
            ur < 8.0f || ur > (float)(SFM_VIEW_W - 9u) ||
            vr < 8.0f || vr > (float)(SFM_VIEW_H - 9u))
            continue;
        {
            int ok = 1;

            for (unsigned q = 0; q < pd->visible && ok; q++) {
                /* spacing on the ROUNDED stamp centres: patches must
                 * keep 9 px of clearance in BOTH views — a pair can be
                 * close in one frame and far in the other (disparity) */
                float dxl = fabs1f((float)(int)(ul + 0.5f) -
                                   pd->planted[q].u1);
                float dyl = fabs1f((float)(int)(vl + 0.5f) -
                                   pd->planted[q].v1);
                float dxr = fabs1f((float)(int)(ur + 0.5f) -
                                   pd->planted[q].u2);
                float dyr = fabs1f((float)(int)(vr + 0.5f) -
                                   pd->planted[q].v2);

                if ((dxl > dyl ? dxl : dyl) < 9.0f ||
                    (dxr > dyr ? dxr : dyr) < 9.0f) {
                    ok = 0;
                    break;
                }
            }
            if (!ok)
                continue;
        }
        {
            sfm_planted *pl = &pd->planted[pd->visible];

            pl->u1 = (float)(int)(ul + 0.5f);
            pl->v1 = (float)(int)(vl + 0.5f);
            pl->u2 = (float)(int)(ur + 0.5f);
            pl->v2 = (float)(int)(vr + 0.5f);
            pl->scene_id = p;
            pl->is_outlier = 0;
            pd->left3[pd->visible][0] = Xl[0];
            pd->left3[pd->visible][1] = Xl[1];
            pd->left3[pd->visible][2] = Xl[2];
        }
        pd->visible++;
    }
    if (pd->visible < SFM_VIS_MIN) {
        return -1;
    }

    /* planted mismatched outliers. A candidate correspondence is only
     * accepted when it sits well off its epipolar line (3x the RANSAC
     * margin), otherwise a moved point would be a legitimate inlier of
     * the planted geometry and the verification could not hold. */
    {
        const float f = SFM_FOCAL;
        const float Kinv[9] = { 1.0f / f, 0.0f, -SFM_CX / f,
                                0.0f, 1.0f / f, -SFM_CY / f,
                                0.0f, 0.0f, 1.0f };
        float Tx[9] = {
            0.0f, -pd->t_rel[2], pd->t_rel[1],
            pd->t_rel[2], 0.0f, -pd->t_rel[0],
            -pd->t_rel[1], pd->t_rel[0], 0.0f
        };
        float Ecam[9], EK[9], Kt[9], F_true[9];

        m3_mul(Tx, pd->R_rel, Ecam);
        m3_transpose(Kinv, Kt);
        m3_mul(Kt, Ecam, EK);
        m3_mul(EK, Kinv, F_true);

        rng_init(&rng, sfm_stream(seed, SFM_STREAM_OUT, pair) + bump);
        for (unsigned i = 0; i < pd->visible; i++) {
            if (rng_below(&rng, 7) != 0)
                continue;
            for (unsigned attempt = 0; attempt < 8u; attempt++) {
                unsigned j = rng_below(&rng, pd->visible);
                sfm_corr probe;
                float e2;

                if (j == i)
                    continue;
                probe.u1 = pd->planted[i].u1;
                probe.v1 = pd->planted[i].v1;
                probe.u2 = pd->planted[j].u2;
                probe.v2 = pd->planted[j].v2;
                e2 = sfm_sampson2(F_true, &probe);
                if (e2 > 100.0f * SFM_RANSAC_PX * SFM_RANSAC_PX) {
                    pd->planted[i].u2 = probe.u2;
                    pd->planted[i].v2 = probe.v2;
                    pd->planted[i].is_outlier = 1;
                    break;
                }
            }
        }
    }

    /* frames: flat background with bounded noise, one patch per point */
    rng_init(&rng, sfm_stream(seed, SFM_STREAM_FRAME, pair));
    {
        uint8_t base = (uint8_t)(40u + rng_below(&rng, 51));
        size_t n = (size_t)SFM_VIEW_W * SFM_VIEW_H;

        for (size_t i = 0; i < n; i++) {
            int v = (int)base + (int)rng_below(&rng, 13) - 6;

            pd->frame_l[i] = (uint8_t)v;
            pd->frame_r[i] = (uint8_t)v;
        }
    }
    for (unsigned i = 0; i < pd->visible; i++) {
        uint8_t bright = (uint8_t)(170u + rng_below(&rng, 51));
        uint8_t dark = (uint8_t)(25u + rng_below(&rng, 36));

        stamp_patch(pd->frame_l, (unsigned)pd->planted[i].u1,
                    (unsigned)pd->planted[i].v1, bright, dark);
        stamp_patch(pd->frame_r, (unsigned)pd->planted[i].u2,
                    (unsigned)pd->planted[i].v2, bright, dark);
    }
    return 0;
}

/* --- per-pair reconstruction (the timed pipeline) ---------------------------*/

typedef struct {
    sfm_corner corners_l[SFM_MAX_CORNERS];
    sfm_corner corners_r[SFM_MAX_CORNERS];
    sfm_corr corr[SFM_MAX_VISIBLE];
    uint8_t mask[SFM_MAX_VISIBLE];
    float X[SFM_MAX_VISIBLE][3];
    float Xc[SFM_MAX_VISIBLE][3];
    uint8_t serial[SFM_SERIAL_SIZE];
    sfm_ransac_ws rws;
} sfm_ws;

/* Project a reconstructed point into one camera (left: identity pose,
 * right: R, t). Returns 0 when the depth is usable. */
static int reproject(const float R[9], const float t[3], const float X[3],
                     float *u, float *v)
{
    float xr[3], z;

    if (R) {
        m3_mul_vec(R, X, xr);
        xr[0] += t[0];
        xr[1] += t[1];
        xr[2] += t[2];
    } else {
        xr[0] = X[0];
        xr[1] = X[1];
        xr[2] = X[2];
    }
    z = xr[2];
    if (z < 1e-4f)
        return -1;
    *u = SFM_FOCAL * xr[0] / z + SFM_CX;
    *v = SFM_FOCAL * xr[1] / z + SFM_CY;
    return 0;
}

static int reconstruct_pair(const sfm_pair_data *pd, sfm_ws *ws,
                            sfm_result *res)
{
    const unsigned n = pd->visible;
    unsigned cl = 0, cr = 0, n_inliers = 0;
    float E[9], E_ess[9], fro = 0.0f, inv;
    float R1[9], R2[9], t3[3];

    memset(ws, 0, sizeof(*ws));
    cl = (unsigned)sfm_fast9_detect(pd->frame_l, SFM_VIEW_W, SFM_VIEW_H,
                                    SFM_FAST_THRESHOLD, SFM_MAX_CORNERS,
                                    ws->corners_l);
    cr = (unsigned)sfm_fast9_detect(pd->frame_r, SFM_VIEW_W, SFM_VIEW_H,
                                    SFM_FAST_THRESHOLD, SFM_MAX_CORNERS,
                                    ws->corners_r);
    /* The disk rims fire too (a bright disk on dark background has
     * dark 9-arcs at its boundary), so the detection count is higher
     * than the patch count; every planted centre must simply be found
     * exactly — checked below per point. */
    if (cl < n || cr < n)
        return -1;

    /* correspondence pairing: the nearest detected corner of each
     * planted point must be the patch centre itself */
    for (unsigned i = 0; i < n; i++) {
        const sfm_planted *pl = &pd->planted[i];
        unsigned bl = 0, br = 0;
        float bdl = 1e30f, bdr = 1e30f;

        for (unsigned k = 0; k < cl; k++) {
            float dx = ws->corners_l[k].x - pl->u1;
            float dy = ws->corners_l[k].y - pl->v1;
            float d2 = dx * dx + dy * dy;

            if (d2 < bdl) {
                bdl = d2;
                bl = k;
            }
        }
        for (unsigned k = 0; k < cr; k++) {
            float dx = ws->corners_r[k].x - pl->u2;
            float dy = ws->corners_r[k].y - pl->v2;
            float d2 = dx * dx + dy * dy;

            if (d2 < bdr) {
                bdr = d2;
                br = k;
            }
        }
        if (bdl > SFM_MATCH_PX2 || bdr > SFM_MATCH_PX2)
            return -1; /* no corner at the planted patch centre */
        if (ws->corners_l[bl].x != (uint16_t)(pl->u1 + 0.5f) ||
            ws->corners_l[bl].y != (uint16_t)(pl->v1 + 0.5f) ||
            ws->corners_r[br].x != (uint16_t)(pl->u2 + 0.5f) ||
            ws->corners_r[br].y != (uint16_t)(pl->v2 + 0.5f))
            return -1; /* detection drifted off the stamp centre */
        ws->corr[i].u1 = pl->u1;
        ws->corr[i].v1 = pl->v1;
        ws->corr[i].u2 = pl->u2;
        ws->corr[i].v2 = pl->v2;
        ws->corr[i].id = pl->scene_id;
    }

    if (ransac_essential(&ws->rws, ws->corr, n, SFM_RANSAC_ITERS,
                         pd->ransac_salt, E, ws->mask, &n_inliers) != 0) {
        return -1;
    }
    /* the accepted inlier set must be exactly the planted one */
    {
        unsigned expect = 0, fp = 0, fn = 0;

        for (unsigned i = 0; i < n; i++)
            if (!pd->planted[i].is_outlier)
                expect++;
        for (unsigned i = 0; i < n; i++) {
            /* mask[i] = 1 means inlier, planted.is_outlier = 1 means a
             * moved correspondence: agreement is the mismatch */
            if (!!ws->mask[i] != !!pd->planted[i].is_outlier)
                continue;
            if (pd->planted[i].is_outlier)
                fp++;
            else
                fn++;
        }
        if (fp != 0u || fn != 0u) {
            return -1;
        }
    }

    /* pixel space -> essential space through the shared intrinsics */
    {
        const float K[9] = { SFM_FOCAL, 0.0f, SFM_CX, 0.0f, SFM_FOCAL,
                             SFM_CY, 0.0f, 0.0f, 1.0f };
        float EK[9];

        m3_mul(E, K, EK);
        {
            const float KT[9] = { SFM_FOCAL, 0.0f, 0.0f, 0.0f, SFM_FOCAL,
                                  0.0f, SFM_CX, SFM_CY, 1.0f };

            m3_mul(KT, EK, E_ess);
        }
    }
    for (unsigned i = 0; i < 9u; i++)
        fro += E_ess[i] * E_ess[i];
    fro = sqrtf(fro);
    if (fro < 1e-9f) {
        return -1;
    }
    inv = 1.0f / fro;
    for (unsigned i = 0; i < 9u; i++)
        E_ess[i] *= inv;
    if (sfm_decompose_essential(E_ess, R1, R2, t3) != 0) {
        return -1;
    }

    /* pose selection by cheirality over the inliers */
    {
        const float *candR[4] = { R1, R1, R2, R2 };
        const float *candT[4] = { t3, t3, t3, t3 };
        const float candSign[4] = { 1.0f, -1.0f, 1.0f, -1.0f };
        unsigned best_c = 4u, best_cnt = 0;
        float best_err = 1e30f;
        float tsel[3], Rsel[9];

        for (unsigned cd = 0; cd < 4u; cd++) {
            float t[3] = { candT[cd][0] * candSign[cd],
                           candT[cd][1] * candSign[cd],
                           candT[cd][2] * candSign[cd] };
            unsigned cnt = 0;
            float err = 0.0f;
            int bad = 0;

            for (unsigned i = 0; i < n && !bad; i++) {
                float ul, vl, ur, vr;

                if (!ws->mask[i])
                    continue;
                if (sfm_triangulate_midpoint(candR[cd], t, &ws->corr[i],
                                             ws->Xc[i]) != 0) {
                    bad = 1;
                    break;
                }
                if (ws->Xc[i][2] <= 1e-4f) {
                    continue; /* behind the left camera */
                }
                if (reproject(NULL, NULL, ws->Xc[i], &ul, &vl) != 0 ||
                    reproject(candR[cd], t, ws->Xc[i], &ur, &vr) != 0) {
                    continue;
                }
                {
                    float e1 = (ul - ws->corr[i].u1) * (ul - ws->corr[i].u1) +
                               (vl - ws->corr[i].v1) * (vl - ws->corr[i].v1);
                    float e2 = (ur - ws->corr[i].u2) * (ur - ws->corr[i].u2) +
                               (vr - ws->corr[i].v2) * (vr - ws->corr[i].v2);

                    if (e1 > SFM_REPROJ_PX * SFM_REPROJ_PX ||
                        e2 > SFM_REPROJ_PX * SFM_REPROJ_PX)
                        continue; /* counts as non-positive evidence */
                    cnt++;
                    err += e1 + e2;
                }
            }
            if (bad)
                continue;
            if (cnt > best_cnt || (cnt == best_cnt && err < best_err)) {
                best_cnt = cnt;
                best_err = err;
                best_c = cd;
            }
        }
        if (best_c == 4u || best_cnt * 10u < n_inliers * SFM_CAND_MIN_NUM) {
            return -1;
        }
        memcpy(Rsel, candR[best_c], sizeof(Rsel));
        tsel[0] = candT[best_c][0] * candSign[best_c];
        tsel[1] = candT[best_c][1] * candSign[best_c];
        tsel[2] = candT[best_c][2] * candSign[best_c];

        /* final triangulation + per-point verification */
        for (unsigned i = 0; i < n; i++) {
            float ul, vl, ur, vr, k, rel;

            if (!ws->mask[i])
                continue;
            if (sfm_triangulate_midpoint(Rsel, tsel, &ws->corr[i], ws->X[i]) !=
                0) {
                return -1;
            }
            if (ws->X[i][2] <= 1e-4f)
                return -1;
            if (reproject(NULL, NULL, ws->X[i], &ul, &vl) != 0 ||
                reproject(Rsel, tsel, ws->X[i], &ur, &vr) != 0)
                return -1;
            {
                float e1 = (ul - ws->corr[i].u1) * (ul - ws->corr[i].u1) +
                           (vl - ws->corr[i].v1) * (vl - ws->corr[i].v1);
                float e2 = (ur - ws->corr[i].u2) * (ur - ws->corr[i].u2) +
                           (vr - ws->corr[i].v2) * (vr - ws->corr[i].v2);

                if (e1 > SFM_REPROJ_PX * SFM_REPROJ_PX ||
                    e2 > SFM_REPROJ_PX * SFM_REPROJ_PX) {
                    return -1;
                }
            }
            /* cloud-shape check against the planted metric cloud, up to
             * the essential-scale factor k = ||t_rel|| */
            k = v3_norm(pd->t_rel);
            {
                float e[3], en = 0.0f, tn = 0.0f;

                for (unsigned r = 0; r < 3u; r++) {
                    e[r] = k * ws->X[i][r] - pd->left3[i][r];
                    en += e[r] * e[r];
                    tn += pd->left3[i][r] * pd->left3[i][r];
                }
                rel = sqrtf(en) / sqrtf(tn);
                if (rel > SFM_SHAPE_TOL) {
                    return -1;
                }
            }
        }

        /* estimated pose must match the planted relative pose */
        for (unsigned i = 0; i < 9u; i++)
            if (fabs1f(Rsel[i] - pd->R_rel[i]) > SFM_POSE_TOL) {
                return -1;
            }
        {
            float tn = v3_norm(pd->t_rel);
            float tgu[3] = { pd->t_rel[0] / tn, pd->t_rel[1] / tn,
                             pd->t_rel[2] / tn };

            if (v3_dot(tsel, tgu) < SFM_TDIR_COS) {
                return -1;
            }
        }

        /* canonical serialisation: count + pose (q6) + cloud (q16) */
        {
            uint8_t *buf = ws->serial;
            size_t off = 0;
            uint32_t little;

            put_u32(buf + off, n_inliers);
            off += 4;
            for (unsigned i = 0; i < 9u; i++) {
                little = (uint32_t)fx(Rsel[i], SFM_Q6);
                put_u32(buf + off, little);
                off += 4;
            }
            for (unsigned i = 0; i < 3u; i++) {
                little = (uint32_t)fx(tsel[i], SFM_Q6);
                put_u32(buf + off, little);
                off += 4;
            }
            for (unsigned i = 0; i < n; i++) {
                if (!ws->mask[i])
                    continue;
                for (unsigned r = 0; r < 3u; r++) {
                    little = (uint32_t)fx(ws->X[i][r], SFM_Q16);
                    put_u32(buf + off, little);
                    off += 4;
                }
            }
            sha1(buf, off, res->digest);
        }
        res->corners_left = cl;
        res->corners_right = cr;
        res->inliers = n_inliers;
        res->triangulated = n_inliers;
    }
    return 0;
}

/* --- workload API ----------------------------------------------------------*/

sfm_ctx *sfm_prepare(unsigned seed)
{
    sfm_ctx *ctx = calloc(1, sizeof(*ctx));
    float pts[SFM_SCENE_POINTS][3];
    rng_t rng;
    unsigned p = 0;

    if (!ctx)
        return NULL;
    rng_init(&rng, sfm_stream(seed, SFM_STREAM_SCENE, 0));
    /* ground: 16 x 8 at y = 0 */
    for (unsigned iz = 0; iz < 8u; iz++)
        for (unsigned ix = 0; ix < 16u; ix++) {
            pts[p][0] = -6.0f + (float)ix * SFM_STEP_X +
                        ((int)rng_below(&rng, 51) - 25) * 0.01f;
            pts[p][1] = ((int)rng_below(&rng, 51) - 25) * 0.01f;
            pts[p][2] = 3.0f + (float)iz * SFM_STEP_ZG +
                        ((int)rng_below(&rng, 51) - 25) * 0.01f;
            p++;
        }
    /* two facades at x = -6.5 / +6.5, 12 x 8 */
    for (unsigned side = 0; side < 2u; side++)
        for (unsigned iy = 0; iy < 8u; iy++)
            for (unsigned iz = 0; iz < 12u; iz++) {
                pts[p][0] = (side ? 6.5f : -6.5f) +
                            ((int)rng_below(&rng, 51) - 25) * 0.01f;
                pts[p][1] = 0.5f + (float)iy * SFM_STEP_Y +
                            ((int)rng_below(&rng, 51) - 25) * 0.01f;
                pts[p][2] = 3.0f + (float)iz * SFM_STEP_ZG +
                            ((int)rng_below(&rng, 51) - 25) * 0.01f;
                p++;
            }
    /* back wall at z = 13, 12 x 8 */
    for (unsigned iy = 0; iy < 8u; iy++)
        for (unsigned ix = 0; ix < 12u; ix++) {
            pts[p][0] = -5.5f + (float)ix * 1.0f +
                        ((int)rng_below(&rng, 51) - 25) * 0.01f;
            pts[p][1] = 0.5f + (float)iy * SFM_STEP_Y +
                        ((int)rng_below(&rng, 51) - 25) * 0.01f;
            pts[p][2] = 13.0f + ((int)rng_below(&rng, 51) - 25) * 0.01f;
            p++;
        }
    /* ceiling: 16 x 8 at y = 4.6 */
    for (unsigned iz = 0; iz < 8u; iz++)
        for (unsigned ix = 0; ix < 16u; ix++) {
            pts[p][0] = -6.0f + (float)ix * SFM_STEP_X +
                        ((int)rng_below(&rng, 51) - 25) * 0.01f;
            pts[p][1] = 4.6f + ((int)rng_below(&rng, 51) - 25) * 0.01f;
            pts[p][2] = 3.0f + (float)iz * SFM_STEP_ZG +
                        ((int)rng_below(&rng, 51) - 25) * 0.01f;
            p++;
        }
    for (unsigned pair = 0; pair < SFM_PAIRS; pair++) {
        sfm_pair_data *pd = calloc(1, sizeof(*pd));
        sfm_ws ws;

        if (!pd) {
            sfm_free(ctx);
            return NULL;
        }
        ctx->pd[pair] = pd;
        /* A pair occasionally lands on a geometry whose reconstruction
         * does not verify (a planted mismatch absorbed by a slightly
         * distorted estimate, or a pose candidate under the 90%
         * cheirality floor). The input contract is that every seeded
         * pair verifies, so the attempt re-rolls the pair's camera
         * pose, mismatch placement and RANSAC salt deterministically
         * until the reconstruction passes — attempt 0 is the plain
         * seeded pair, and the attempt count is part of the data. */
        for (unsigned attempt = 0;; attempt++) {
            if (build_pair(pd, seed, pair, attempt,
                           (const float (*)[3])pts) == 0 &&
                reconstruct_pair(pd, &ws, &pd->ref) == 0)
                break;
            if (attempt == 15u) {
                sfm_free(ctx);
                return NULL;
            }
        }
    }
    return ctx;
}

void sfm_free(sfm_ctx *ctx)
{
    if (!ctx)
        return;
    for (unsigned i = 0; i < SFM_PAIRS; i++)
        free(ctx->pd[i]);
    free(ctx);
}

/* One thread's slice: contiguous pair instances of the pass, own
 * workspace — threads never share writable state. */
typedef struct {
    sfm_ctx *ctx;
    sfm_ws  *ws;
    size_t   begin, end; /* instance range [begin, end) of the pass */
} sfm_slice;

static int sfm_cycle(void *arg)
{
    sfm_slice *sl = arg;

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned p = (unsigned)(i % SFM_PAIRS);
        sfm_result r;
        const sfm_result *ref = &sl->ctx->pd[p]->ref;

        if (reconstruct_pair(sl->ctx->pd[p], sl->ws, &r) != 0)
            return -1;
        if (memcmp(r.digest, ref->digest, 20) != 0 ||
            r.corners_left != ref->corners_left ||
            r.corners_right != ref->corners_right ||
            r.inliers != ref->inliers ||
            r.triangulated != ref->triangulated)
            return -1;
    }
    return 0;
}

uint64_t sfm_task_pairs(unsigned multi)
{
    return multi ? SFM_PAIRS_MULTI : SFM_PAIRS;
}

int sfm_run(sfm_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_pairs, uint64_t *pairs)
{
    unsigned t_count = *threads;
    unsigned pair_set = t_count > 1u ? SFM_PAIRS_MULTI : SFM_PAIRS;
    sfm_slice *slices = NULL;
    void **ctxs = NULL;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    uint64_t total_pairs;
    int rc = -1;

    if (!ctx || runs == 0u || runs > MAX_RUNS || !threads ||
        !throughput_pairs || !pairs)
        return -1;
    if (t_count > pair_set) /* one pair instance per thread at least */
        t_count = pair_set;
    if (t_count < 1u)
        t_count = 1u;
    total_pairs = pair_set; /* every instance reconstructs one pair */

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;
    {
        size_t per = pair_set / t_count;
        unsigned extra = pair_set % t_count;
        size_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            size_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            slices[t].ws = malloc(sizeof(sfm_ws));
            if (!slices[t].ws)
                goto out;
            ctxs[t] = &slices[t];
        }
    }
    rc = harness_run(t_count, runs, sfm_cycle, ctxs, elapsed);
    if (rc == 0) {
        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0 ? (double)total_pairs / elapsed[r]
                                        : -1.0;
        *throughput_pairs = lb_median(rates, runs);
        *threads = t_count;
        *pairs = total_pairs;
    }
out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            free(slices[t].ws);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors ------------------------------------------------------------------*/

const uint8_t *sfm_view(const sfm_ctx *ctx, unsigned pair, int right)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair])
        return NULL;
    return right ? ctx->pd[pair]->frame_r : ctx->pd[pair]->frame_l;
}

const sfm_result *sfm_reference(const sfm_ctx *ctx, unsigned pair)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair])
        return NULL;
    return &ctx->pd[pair]->ref;
}

const sfm_camera *sfm_camera_left(const sfm_ctx *ctx, unsigned pair)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair])
        return NULL;
    return &ctx->pd[pair]->cam_l;
}

const sfm_camera *sfm_camera_right(const sfm_ctx *ctx, unsigned pair)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair])
        return NULL;
    return &ctx->pd[pair]->cam_r;
}

const float *sfm_gt_R(const sfm_ctx *ctx, unsigned pair)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair])
        return NULL;
    return ctx->pd[pair]->R_rel;
}

const float *sfm_gt_t(const sfm_ctx *ctx, unsigned pair)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair])
        return NULL;
    return ctx->pd[pair]->t_rel;
}

unsigned sfm_visible(const sfm_ctx *ctx, unsigned pair)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair])
        return 0;
    return ctx->pd[pair]->visible;
}

const sfm_planted *sfm_planted_of(const sfm_ctx *ctx, unsigned pair,
                                  unsigned i)
{
    if (!ctx || pair >= SFM_PAIRS || !ctx->pd[pair] ||
        i >= ctx->pd[pair]->visible)
        return NULL;
    return &ctx->pd[pair]->planted[i];
}
