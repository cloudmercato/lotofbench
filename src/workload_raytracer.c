/* LotofBench — Ray Tracer workload (workload #15).
 *
 * Mirrors the fifteenth Geekbench 6 CPU workload: render a 3D scene with
 * a Whitted-style ray tracer over a BVH (Geekbench 6 renders the Blender
 * BMW scene with an Embree-style tracer). LotofBench reproduces the
 * shape dependency-free, with the same honesty rules as the other
 * workloads:
 *
 *   - deterministic synthetic scene — a seeded showroom: a 16x16-cell
 *     checker floor and six slots filled with seeded primitives (UV
 *     geodesic spheres, yaw-rotated boxes, 16-segment cylinders) in
 *     matte, painted and chrome materials, lit by three seeded point
 *     lights, seen from eight seeded cameras. Every angle enters the
 *     scene through the shared integer trig table (src/itrig.h) and the
 *     timed path uses only IEEE-exact float operations (+ - * / sqrtf,
 *     fminf/fmaxf/fabsf) compiled with -ffp-contract=off — no libm
 *     call, no transcendental, no FMA contraction — so frames are
 *     bit-identical across machines.
 *   - Whitted shading: ambient + Lambert diffuse + Phong specular per
 *     light (integer power loop) with one shadow ray per light, and
 *     mirror reflections up to RT_MAX_DEPTH bounces. Every primary,
 *     shadow and reflection ray is counted; the ray count is exact and
 *     deterministic, like the corpus size for File Compression.
 *   - BVH acceleration: median split on the longest centroid axis
 *     (merge sort tie-broken by triangle index), leaves of at most
 *     RT_LEAF_MAX triangles, near-child-first traversal. Built off the
 *     clock; the timed cycles only traverse.
 *   - verification — the whole view set is re-rendered inside every
 *     timed cycle and each frame must reproduce its prepare-time SHA-1
 *     digest AND its exact ray count; any drift aborts the run instead
 *     of scoring. rt_render_bruteforce renders without the BVH and is
 *     bit-identical by construction — the differential test that
 *     validates the acceleration structure.
 *
 * Metric: millions of traced rays per second (Mray/s). Documented in
 * docs/benchmarks.md, section "Workload 15". */

#include "workload_raytracer.h"

#include "harness.h"
#include "itrig.h"
#include "lotofbench.h"
#include "rng.h"
#include "sha1.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Numerical constants of the timed path. RT_EPS lifts the ray origin
 * off the surface (scene units; objects span 0.5..6 units); the
 * intersection determinant floor skips near-parallel rays. */
#define RT_EPS        1e-3f
#define RT_DET_EPS    1e-12f

/* --- small float vector helpers -------------------------------------------*/

static void v_sub(float r[3], const float a[3], const float b[3])
{
    r[0] = a[0] - b[0];
    r[1] = a[1] - b[1];
    r[2] = a[2] - b[2];
}

static void v_add(float r[3], const float a[3], const float b[3])
{
    r[0] = a[0] + b[0];
    r[1] = a[1] + b[1];
    r[2] = a[2] + b[2];
}

static void v_scale(float r[3], const float a[3], float k)
{
    r[0] = a[0] * k;
    r[1] = a[1] * k;
    r[2] = a[2] * k;
}

static float v_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void v_cross(float r[3], const float a[3], const float b[3])
{
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
}

static void v_normalize(float r[3])
{
    float len = sqrtf(v_dot(r, r));

    if (len > 0.0f) {
        r[0] /= len;
        r[1] /= len;
        r[2] /= len;
    }
}

/* Integer-exponent power for the Phong specular term: square-and-multiply
 * with a fixed operation order — deterministic, no libm powf. */
static float powi(float x, unsigned n)
{
    float result = 1.0f;

    while (n) {
        if (n & 1u)
            result *= x;
        x *= x;
        n >>= 1;
    }
    return result;
}

static uint8_t tonemap(float c)
{
    if (c <= 0.0f)
        return 0;
    if (c >= 1.0f)
        return 255;
    return (uint8_t)(c * 255.0f + 0.5f);
}

/* --- scene API --------------------------------------------------------------*/

int rt_scene_init(rt_scene *s)
{
    if (!s)
        return -1;
    memset(s, 0, sizeof(*s));
    return 0;
}

int rt_scene_add_tri(rt_scene *s, const rt_tri *t)
{
    if (!s || !t || t->mat >= RT_MATERIALS)
        return -1;
    if (s->tri_count == s->tri_cap) {
        uint32_t cap = s->tri_cap ? s->tri_cap * 2u : 256u;
        rt_tri *grown = realloc(s->tris, cap * sizeof(*grown));

        if (!grown)
            return -1;
        s->tris = grown;
        s->tri_cap = cap;
    }
    s->tris[s->tri_count++] = *t;
    return 0;
}

void rt_scene_free(rt_scene *s)
{
    if (!s)
        return;
    free(s->tris);
    memset(s, 0, sizeof(*s));
}

/* --- BVH ---------------------------------------------------------------------*/

struct rt_bvh_node {
    float    bmin[3], bmax[3];
    uint32_t first;  /* leaf: first triangle position; inner: left child */
    uint32_t count;  /* leaf: n | RT_LEAF_FLAG; inner: right child */
};

/* Leaf mark in the count field (triangle counts stay < 2^31). */
#define RT_LEAF_FLAG 0x80000000u

struct rt_bvh {
    struct rt_bvh_node *nodes;
    uint32_t node_count;
};

/* Centroid of one triangle along one axis. */
static float tri_centroid(const rt_scene *s, uint32_t i, int axis)
{
    const rt_tri *t = &s->tris[i];

    return (t->v0[axis] + t->v1[axis] + t->v2[axis]) / 3.0f;
}

/* Merge sort of an index range by centroid along one axis, ties broken
 * by index — a total order, so the sort is deterministic on every
 * platform regardless of the libc qsort implementation. */
static void idx_sort(uint32_t *a, uint32_t n, uint32_t *tmp,
                     const rt_scene *s, int axis)
{
    if (n < 2)
        return;
    {
        uint32_t half = n / 2u;

        idx_sort(a, half, tmp, s, axis);
        idx_sort(a + half, n - half, tmp, s, axis);
        {
            uint32_t i = 0, j = half, k = 0;

            while (i < half && j < n) {
                float ci = tri_centroid(s, a[i], axis);
                float cj = tri_centroid(s, a[j], axis);

                if (ci < cj || (ci == cj && a[i] < a[j]))
                    tmp[k++] = a[i++];
                else
                    tmp[k++] = a[j++];
            }
            while (i < half)
                tmp[k++] = a[i++];
            while (j < n)
                tmp[k++] = a[j++];
            memcpy(a, tmp, n * sizeof(*a));
        }
    }
}

static void tri_bounds(const rt_scene *s, uint32_t i, float bmin[3],
                       float bmax[3])
{
    const rt_tri *t = &s->tris[i];

    for (int a = 0; a < 3; a++) {
        float lo = t->v0[a], hi = lo;

        if (t->v1[a] < lo)
            lo = t->v1[a];
        if (t->v1[a] > hi)
            hi = t->v1[a];
        if (t->v2[a] < lo)
            lo = t->v2[a];
        if (t->v2[a] > hi)
            hi = t->v2[a];
        bmin[a] = lo;
        bmax[a] = hi;
    }
}

static void bounds_grow(float bmin[3], float bmax[3], const float lo[3],
                        const float hi[3])
{
    for (int a = 0; a < 3; a++) {
        if (lo[a] < bmin[a])
            bmin[a] = lo[a];
        if (hi[a] > bmax[a])
            bmax[a] = hi[a];
    }
}

/* Recursive median split. idx[first..first+n) is sorted in place per
 * node; nodes are appended to the array as they are allocated (children
 * after the parent, so left = parent + 1). */
static uint32_t bvh_build_rec(struct rt_bvh_node *nodes, uint32_t *next,
                              const rt_scene *s, uint32_t *idx,
                              uint32_t first, uint32_t n, unsigned depth)
{
    uint32_t self = (*next)++;
    float bmin[3] = { 0, 0, 0 }, bmax[3] = { 0, 0, 0 };
    float cmin[3], cmax[3];
    int axis = 0;

    for (int a = 0; a < 3; a++) {
        bmin[a] = 3.0e38f;
        bmax[a] = -3.0e38f;
        cmin[a] = 3.0e38f;
        cmax[a] = -3.0e38f;
    }
    for (uint32_t i = 0; i < n; i++) {
        float lo[3], hi[3];
        float c[3];

        tri_bounds(s, idx[first + i], lo, hi);
        bounds_grow(bmin, bmax, lo, hi);
        for (int a = 0; a < 3; a++) {
            c[a] = tri_centroid(s, idx[first + i], a);
            if (c[a] < cmin[a])
                cmin[a] = c[a];
            if (c[a] > cmax[a])
                cmax[a] = c[a];
        }
    }

    if (n <= RT_LEAF_MAX || depth >= 28u) {
        nodes[self].first = first;
        nodes[self].count = n | RT_LEAF_FLAG;
        memcpy(nodes[self].bmin, bmin, sizeof(bmin));
        memcpy(nodes[self].bmax, bmax, sizeof(bmax));
        return self;
    }

    {
        float ext0 = cmax[0] - cmin[0], ext1 = cmax[1] - cmin[1];
        float ext2 = cmax[2] - cmin[2];

        axis = ext0 >= ext1 && ext0 >= ext2 ? 0 : (ext1 >= ext2 ? 1 : 2);
    }
    idx_sort(idx + first, n, idx + (size_t)s->tri_count + first, s, axis);

    {
        uint32_t half = n / 2u;
        uint32_t left = bvh_build_rec(nodes, next, s, idx, first, half,
                                      depth + 1u);
        uint32_t right = bvh_build_rec(nodes, next, s, idx,
                                       first + half, n - half, depth + 1u);

        /* inner node: first = left child, count = right child (child
         * indices are always > 0, so count == 0 stays the leaf mark) */
        nodes[self].first = left;
        nodes[self].count = right;
        memcpy(nodes[self].bmin, bmin, sizeof(bmin));
        memcpy(nodes[self].bmax, bmax, sizeof(bmax));
    }
    return self;
}

rt_bvh *rt_bvh_build(rt_scene *s)
{
    rt_bvh *b;
    uint32_t *idx, next = 0;

    if (!s || s->tri_count == 0)
        return NULL;
    b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    /* 2n-1 nodes upper bound for a binary tree; idx needs n entries plus
     * n scratch entries for the merge sort. */
    b->nodes = calloc((size_t)2 * s->tri_count, sizeof(*b->nodes));
    idx = malloc((size_t)2 * s->tri_count * sizeof(*idx));
    if (!b->nodes || !idx) {
        rt_bvh_free(b);
        free(idx);
        return NULL;
    }
    for (uint32_t i = 0; i < s->tri_count; i++)
        idx[i] = i;
    bvh_build_rec(b->nodes, &next, s, idx, 0, s->tri_count, 0);
    b->node_count = next;
    /* Physically reorder the triangles into leaf order: leaves address
     * s->tris by position (the brute-force renderer then scans the very
     * same order, which makes the tie-breaks identical). */
    {
        rt_tri *sorted = malloc((size_t)s->tri_count * sizeof(*sorted));

        if (!sorted) {
            rt_bvh_free(b);
            free(idx);
            return NULL;
        }
        for (uint32_t i = 0; i < s->tri_count; i++)
            sorted[i] = s->tris[idx[i]];
        memcpy(s->tris, sorted, (size_t)s->tri_count * sizeof(*sorted));
        free(sorted);
    }
    free(idx);
    return b;
}

void rt_bvh_free(rt_bvh *b)
{
    if (!b)
        return;
    free(b->nodes);
    free(b);
}

/* Entry t of the ray into an AABB (slab test, IEEE inf semantics handle
 * axis-parallel rays). Returns -1 when the box is missed or lies fully
 * behind the origin. */
static float aabb_entry(const struct rt_bvh_node *nd, const float o[3],
                        const float inv[3])
{
    float tmin = -3.0e38f, tmax = 3.0e38f;

    for (int a = 0; a < 3; a++) {
        float t0 = (nd->bmin[a] - o[a]) * inv[a];
        float t1 = (nd->bmax[a] - o[a]) * inv[a];
        float lo = t0 < t1 ? t0 : t1;
        float hi = t0 < t1 ? t1 : t0;

        if (lo > tmin)
            tmin = lo;
        if (hi < tmax)
            tmax = hi;
    }
    if (tmax < tmin || tmax < 0.0f)
        return -1.0f;
    return tmin > 0.0f ? tmin : 0.0f;
}

/* --- intersection ------------------------------------------------------------*/

/* Two-sided Moller-Trumbore. Returns 1 and fills *t, *u, *v when the
 * ray hits the triangle at t > eps. */
static int tri_hit(const rt_tri *tr, const float o[3], const float d[3],
                   float *t, float *u, float *v)
{
    float e1[3], e2[3], p[3], tv[3], q[3];
    float det, inv, uu, vv, tt;

    v_sub(e1, tr->v1, tr->v0);
    v_sub(e2, tr->v2, tr->v0);
    v_cross(p, d, e2);
    det = v_dot(e1, p);
    if (fabsf(det) < RT_DET_EPS)
        return 0;
    inv = 1.0f / det;
    v_sub(tv, o, tr->v0);
    uu = v_dot(tv, p) * inv;
    if (uu < 0.0f || uu > 1.0f)
        return 0;
    v_cross(q, tv, e1);
    vv = v_dot(d, q) * inv;
    if (vv < 0.0f || uu + vv > 1.0f)
        return 0;
    tt = v_dot(e2, q) * inv;
    if (tt <= RT_EPS)
        return 0;
    *t = tt;
    *u = uu;
    *v = vv;
    return 1;
}

/* Closest hit over a leaf's triangles. Ties are broken by triangle
 * position in BOTH acceleration paths (the BVH references physically
 * reordered triangles — see rt_bvh_build), so the BVH and the
 * brute-force renderer always pick the same triangle. */
static void leaf_closest(const rt_scene *s, uint32_t first, uint32_t count,
                         const float o[3], const float d[3], float *best_t,
                         uint32_t *best_i, float *best_u, float *best_v)
{
    for (uint32_t k = 0; k < count; k++) {
        float t, u, v;

        if (tri_hit(&s->tris[first + k], o, d, &t, &u, &v) &&
            (t < *best_t || (t == *best_t && first + k < *best_i))) {
            *best_t = t;
            *best_i = first + k;
            *best_u = u;
            *best_v = v;
        }
    }
}

/* Closest hit over the whole scene (brute-force path): same scan order
 * (physical position), same tie-break as the BVH leaves. */
static void scene_closest(const rt_scene *s, const float o[3],
                          const float d[3], float *best_t,
                          uint32_t *best_i, float *best_u, float *best_v)
{
    for (uint32_t i = 0; i < s->tri_count; i++) {
        float t, u, v;

        if (tri_hit(&s->tris[i], o, d, &t, &u, &v) &&
            (t < *best_t || (t == *best_t && i < *best_i))) {
            *best_t = t;
            *best_i = i;
            *best_u = u;
            *best_v = v;
        }
    }
}

/* Closest hit through the BVH: near-child-first descent, children
 * pruned once their entry distance exceeds the best hit so far. */
static void bvh_closest(const rt_bvh *b, const rt_scene *s,
                        const float o[3], const float d[3],
                        const float inv[3],
                        float *best_t, uint32_t *best_i, float *best_u,
                        float *best_v)
{
    uint32_t stack[64];
    int sp = 0;

    stack[sp++] = 0;
    while (sp > 0) {
        const struct rt_bvh_node *nd = &b->nodes[stack[--sp]];

        if (nd->count & RT_LEAF_FLAG) {
            leaf_closest(s, nd->first, nd->count & ~RT_LEAF_FLAG, o, d,
                         best_t, best_i, best_u, best_v);
            continue;
        }
        {
            const struct rt_bvh_node *l = &b->nodes[nd->first];
            const struct rt_bvh_node *r = &b->nodes[nd->count];
            float tl = aabb_entry(l, o, inv);
            float tr = aabb_entry(r, o, inv);

            /* descend the nearer child first; visit a child whose entry
             * can still TIE the best hit (t == best_t must still win on
             * the position tie-break, so the test is <=, not <) */
            if (tl >= 0.0f && tl <= *best_t && tr >= 0.0f &&
                tr <= *best_t) {
                if (tl <= tr) {
                    stack[sp++] = nd->count;
                    stack[sp++] = nd->first;
                } else {
                    stack[sp++] = nd->first;
                    stack[sp++] = nd->count;
                }
            } else if (tl >= 0.0f && tl <= *best_t) {
                stack[sp++] = nd->first;
            } else if (tr >= 0.0f && tr <= *best_t) {
                stack[sp++] = nd->count;
            }
        }
    }
}

/* Any hit strictly before max_t (shadow rays). */
static int occluded(const rt_scene *s, const rt_bvh *b, const float o[3],
                    const float d[3], float max_t)
{
    if (!b) {
        for (uint32_t i = 0; i < s->tri_count; i++) {
            float t, u, v;

            if (tri_hit(&s->tris[i], o, d, &t, &u, &v) && t < max_t)
                return 1;
        }
        return 0;
    }
    {
        float inv[3] = { 1.0f / d[0], 1.0f / d[1], 1.0f / d[2] };
        uint32_t stack[64];
        int sp = 0;

        stack[sp++] = 0;
        while (sp > 0) {
            const struct rt_bvh_node *nd = &b->nodes[stack[--sp]];

            if (nd->count & RT_LEAF_FLAG) {
                uint32_t n = nd->count & ~RT_LEAF_FLAG;

                for (uint32_t k = 0; k < n; k++) {
                    float t, u, v;

                    if (tri_hit(&s->tris[nd->first + k], o, d, &t, &u,
                                &v) && t < max_t)
                        return 1;
                }
                continue;
            }
            {
                const struct rt_bvh_node *l = &b->nodes[nd->first];
                const struct rt_bvh_node *r = &b->nodes[nd->count];

                /* no early pruning by distance: an any-hit may hide
                 * behind the entry point of the nearer child */
                if (aabb_entry(l, o, inv) >= 0.0f)
                    stack[sp++] = nd->first;
                if (aabb_entry(r, o, inv) >= 0.0f)
                    stack[sp++] = nd->count;
            }
        }
    }
    return 0;
}

/* Whitted integrator: one ray (primary, reflection), depth-bounded.
 * col accumulates ambient + per-light diffuse/specular (each light with
 * its own shadow ray) + mirror reflections. Every cast ray increments
 * *rays. */
static void trace(const rt_scene *s, const rt_bvh *b, const float o[3],
                  const float d[3], unsigned depth, uint64_t *rays,
                  float col[3])
{
    float best_t = 3.0e38f;
    uint32_t best_i = 0xffffffffu;
    float best_u = 0.0f, best_v = 0.0f;
    const rt_tri *tr;
    const rt_material *mat;
    float hit[3], n[3], v[3];
    float w0, w1, w2;

    (*rays)++;
    if (b) {
        float inv[3] = { 1.0f / d[0], 1.0f / d[1], 1.0f / d[2] };

        bvh_closest(b, s, o, d, inv, &best_t, &best_i, &best_u, &best_v);
    } else {
        scene_closest(s, o, d, &best_t, &best_i, &best_u, &best_v);
    }
    if (best_i == 0xffffffffu) {
        /* sky / ground haze gradient by ray elevation */
        if (d[1] > 0.0f) {
            col[0] = 0.55f + 0.30f * d[1];
            col[1] = 0.65f + 0.25f * d[1];
            col[2] = 0.85f + 0.13f * d[1];
        } else {
            float t = -d[1];

            col[0] = 0.50f - 0.20f * t;
            col[1] = 0.52f - 0.21f * t;
            col[2] = 0.55f - 0.22f * t;
        }
        return;
    }

    tr = &s->tris[best_i];
    mat = &s->mats[tr->mat];

    /* hit point and interpolated, unit normal (two-sided) */
    w2 = 1.0f - best_u - best_v;
    w0 = best_u;
    w1 = best_v;
    for (int a = 0; a < 3; a++)
        hit[a] = w2 * tr->v0[a] + w0 * tr->v1[a] + w1 * tr->v2[a];
    for (int a = 0; a < 3; a++)
        n[a] = w2 * tr->n0[a] + w0 * tr->n1[a] + w1 * tr->n2[a];
    v_normalize(n);
    if (v_dot(n, d) > 0.0f)
        v_scale(n, n, -1.0f);

    v_scale(v, d, -1.0f);
    col[0] = 0.10f * mat->albedo[0];
    col[1] = 0.10f * mat->albedo[1];
    col[2] = 0.10f * mat->albedo[2];

    for (unsigned li = 0; li < RT_LIGHTS; li++) {
        const rt_light *light = &s->lights[li];
        float ldir[3], ndotl, dist;
        float ro[3];

        v_sub(ldir, light->pos, hit);
        dist = sqrtf(v_dot(ldir, ldir));
        v_scale(ldir, ldir, 1.0f / dist);
        ndotl = v_dot(n, ldir);
        if (ndotl <= 0.0f)
            continue;
        v_add(ro, hit, (float[3]) { n[0] * RT_EPS, n[1] * RT_EPS,
                                    n[2] * RT_EPS });
        (*rays)++;
        if (occluded(s, b, ro, ldir, dist))
            continue;
        col[0] += mat->albedo[0] * light->rgb[0] * ndotl;
        col[1] += mat->albedo[1] * light->rgb[1] * ndotl;
        col[2] += mat->albedo[2] * light->rgb[2] * ndotl;
        {
            float r[3], rv;

            /* Phong: R = 2*(N.L)*N - L, V = -D */
            for (int a = 0; a < 3; a++)
                r[a] = 2.0f * ndotl * n[a] - ldir[a];
            rv = v_dot(r, v);
            if (rv > 0.0f && mat->ks > 0.0f) {
                float spec = powi(rv, mat->phong_n) * mat->ks;

                col[0] += spec * light->rgb[0];
                col[1] += spec * light->rgb[1];
                col[2] += spec * light->rgb[2];
            }
        }
    }

    if (mat->reflect > 0.0f && depth < RT_MAX_DEPTH) {
        float rd[3], ro2[3], sub[3];
        float dn = v_dot(d, n);

        for (int a = 0; a < 3; a++)
            rd[a] = d[a] - 2.0f * dn * n[a];
        v_add(ro2, hit, (float[3]) { n[0] * RT_EPS, n[1] * RT_EPS,
                                     n[2] * RT_EPS });
        sub[0] = sub[1] = sub[2] = 0.0f;
        trace(s, b, ro2, rd, depth + 1u, rays, sub);
        col[0] += mat->reflect * sub[0];
        col[1] += mat->reflect * sub[1];
        col[2] += mat->reflect * sub[2];
    }
}

/* --- renderers ----------------------------------------------------------------*/

static int render_check(const rt_scene *s, const rt_camera *cam,
                        unsigned w, unsigned h, uint8_t *rgb)
{
    if (!s || !cam || !rgb || w == 0 || h == 0)
        return -1;
    if (s->tri_count == 0 || s->mat_count == 0)
        return -1;
    return 0;
}

int rt_render(const rt_scene *s, const rt_bvh *b, const rt_camera *cam,
              unsigned w, unsigned h, uint8_t *rgb, uint64_t *rays)
{
    float half_w, half_h;
    uint64_t count = 0;

    if (render_check(s, cam, w, h, rgb) != 0 || !b || !rays)
        return -1;
    half_w = 0.5f * (float)w;
    half_h = 0.5f * (float)h;
    for (unsigned py = 0; py < h; py++) {
        for (unsigned px = 0; px < w; px++) {
            float sx = ((float)px + 0.5f - half_w) / cam->focal;
            float sy = (half_h - ((float)py + 0.5f)) / cam->focal;
            float o[3], d[3], col = 0.0f;
            uint8_t *px8 = rgb + ((size_t)py * w + px) * 3;
            float fcol[3];

            for (int a = 0; a < 3; a++)
                d[a] = cam->fwd[a] + sx * cam->right[a] +
                       sy * cam->up[a];
            v_normalize(d);
            memcpy(o, cam->pos, sizeof(o));
            fcol[0] = fcol[1] = fcol[2] = 0.0f;
            trace(s, b, o, d, 0, &count, fcol);
            px8[0] = tonemap(fcol[0]);
            px8[1] = tonemap(fcol[1]);
            px8[2] = tonemap(fcol[2]);
            (void)col;
        }
    }
    *rays = count;
    return 0;
}

int rt_render_bruteforce(const rt_scene *s, const rt_camera *cam,
                         unsigned w, unsigned h, uint8_t *rgb,
                         uint64_t *rays)
{
    float half_w, half_h;
    uint64_t count = 0;

    if (render_check(s, cam, w, h, rgb) != 0 || !rays)
        return -1;
    half_w = 0.5f * (float)w;
    half_h = 0.5f * (float)h;
    for (unsigned py = 0; py < h; py++) {
        for (unsigned px = 0; px < w; px++) {
            float sx = ((float)px + 0.5f - half_w) / cam->focal;
            float sy = (half_h - ((float)py + 0.5f)) / cam->focal;
            float o[3], d[3];
            uint8_t *px8 = rgb + ((size_t)py * w + px) * 3;
            float fcol[3];

            for (int a = 0; a < 3; a++)
                d[a] = cam->fwd[a] + sx * cam->right[a] +
                       sy * cam->up[a];
            v_normalize(d);
            memcpy(o, cam->pos, sizeof(o));
            fcol[0] = fcol[1] = fcol[2] = 0.0f;
            trace(s, NULL, o, d, 0, &count, fcol);
            px8[0] = tonemap(fcol[0]);
            px8[1] = tonemap(fcol[1]);
            px8[2] = tonemap(fcol[2]);
        }
    }
    *rays = count;
    return 0;
}

/* --- scene generation (off the clock) -------------------------------------------*/

/* Materials: fixed palette, indexed by the generator. */
enum {
    M_CHROME = 0,      /* polished metal sphere */
    M_PAINT_RED = 1,   /* glossy car paint */
    M_PAINT_BLUE = 2,
    M_MATTE_WHITE = 3,
    M_MATTE_GREEN = 4,
    M_FLOOR_DARK = 5,
    M_FLOOR_LIGHT = 6
};

static void add_material(rt_scene *s, unsigned idx, const float albedo[3],
                         float ks, unsigned phong_n, float reflect)
{
    rt_material *m = &s->mats[idx];

    memcpy(m->albedo, albedo, sizeof(m->albedo));
    m->ks = ks;
    m->phong_n = phong_n;
    m->reflect = reflect;
    if (idx + 1u > s->mat_count)
        s->mat_count = idx + 1u;
}

static void add_tri3(rt_scene *s, const float a[3], const float b[3],
                     const float c[3], const float na[3],
                     const float nb[3], const float nc[3], unsigned mat)
{
    rt_tri t;

    memcpy(t.v0, a, sizeof(t.v0));
    memcpy(t.v1, b, sizeof(t.v1));
    memcpy(t.v2, c, sizeof(t.v2));
    memcpy(t.n0, na, sizeof(t.n0));
    memcpy(t.n1, nb, sizeof(t.n1));
    memcpy(t.n2, nc, sizeof(t.n2));
    t.mat = mat;
    rt_scene_add_tri(s, &t);
}

static void add_quad(rt_scene *s, const float a[3], const float b[3],
                     const float c[3], const float d[3], const float n[3],
                     unsigned mat)
{
    add_tri3(s, a, b, c, n, n, n, mat);
    add_tri3(s, a, c, d, n, n, n, mat);
}

/* Checker floor: 16x16 cells of 0.5 units over [-4, 4]^2. */
static void add_floor(rt_scene *s)
{
    const unsigned cells = 16;
    const float lo = -4.0f, cell = 8.0f / (float)cells;
    const float up[3] = { 0.0f, 1.0f, 0.0f };

    for (unsigned i = 0; i < cells; i++) {
        for (unsigned j = 0; j < cells; j++) {
            float x0 = lo + (float)i * cell;
            float z0 = lo + (float)j * cell;
            float a[3] = { x0, 0.0f, z0 };
            float b[3] = { x0 + cell, 0.0f, z0 };
            float c[3] = { x0 + cell, 0.0f, z0 + cell };
            float d[3] = { x0, 0.0f, z0 + cell };

            add_quad(s, a, b, c, d, up,
                     (i + j) % 2 == 0 ? M_FLOOR_DARK : M_FLOOR_LIGHT);
        }
    }
}

/* Geodesic sphere: an octahedron subdivided twice (8 -> 128 triangles),
 * vertices pushed to the unit sphere by normalization (sqrtf only),
 * scaled and translated. Vertex normals are the radial directions —
 * smooth shading without a single transcendental. */
static void add_sphere(rt_scene *s, const float center[3], float radius,
                       unsigned mat)
{
    static const float oct[6][3] = {
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
        { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }
    };
    static const unsigned faces[8][3] = {
        { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 },
        { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 }
    };
    const unsigned max_tris = 8u * 16u; /* 8 * 4^subdiv */
    float tri_v[max_tris][3][3];
    unsigned n_tris = 8;

    for (unsigned f = 0; f < 8; f++)
        for (int k = 0; k < 3; k++) {
            for (int a = 0; a < 3; a++)
                tri_v[f][k][a] = oct[faces[f][k]][a];
            v_normalize(tri_v[f][k]);
        }
    for (unsigned pass = 0; pass < 2u; pass++) {
        float next[max_tris][3][3];
        unsigned out = 0;

        for (unsigned t = 0; t < n_tris; t++) {
            float(*tv)[3] = tri_v[t];
            const float *c[3] = { tv[0], tv[1], tv[2] };
            float m[3][3];
            static const unsigned pat[4][3] = {
                { 0, 3, 5 }, { 3, 1, 4 }, { 4, 2, 5 }, { 3, 4, 5 }
            };

            for (int e = 0; e < 3; e++) {
                for (int a = 0; a < 3; a++)
                    m[e][a] = 0.5f * (tv[e][a] + tv[(e + 1) % 3][a]);
                v_normalize(m[e]);
            }
            for (unsigned q = 0; q < 4; q++) {
                for (int k = 0; k < 3; k++) {
                    const float *src =
                        pat[q][k] < 3 ? c[pat[q][k]] : m[pat[q][k] - 3];

                    memcpy(next[out][k], src, sizeof(next[out][k]));
                }
                out++;
            }
        }
        memcpy(tri_v, next, (size_t)out * sizeof(tri_v[0]));
        n_tris = out;
    }
    for (unsigned t = 0; t < n_tris; t++) {
        float a[3], b[3], c[3], na[3], nb[3], nc[3];

        for (int k = 0; k < 3; k++) {
            const float *p = tri_v[t][k];
            float *dst = k == 0 ? a : (k == 1 ? b : c);
            float *ndst = k == 0 ? na : (k == 1 ? nb : nc);

            for (int ax = 0; ax < 3; ax++)
                dst[ax] = center[ax] + radius * p[ax];
            memcpy(ndst, p, sizeof(nc)); /* unit by construction */
        }
        add_tri3(s, a, b, c, na, nb, nc, mat);
    }
}

/* Yaw-rotated axis-aligned box (12 triangles, face normals). */
static void add_box(rt_scene *s, const float center[3], float hx, float hy,
                    float hz, unsigned yaw_bin, unsigned mat)
{
    int32_t c1024, s1024;
    float cs, sn;
    float corner[8][3];
    static const unsigned quads[6][4] = {
        { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, /* -y, +y */
        { 0, 2, 6, 4 }, { 1, 5, 7, 3 }, /* -z, +z */
        { 0, 4, 5, 1 }, { 2, 3, 7, 6 }  /* -x, +x */
    };
    static const float qn[6][3] = {
        { 0, -1, 0 }, { 0, 1, 0 },
        { 0, 0, -1 }, { 0, 0, 1 },
        { -1, 0, 0 }, { 1, 0, 0 }
    };
    static const float csign[8][3] = {
        { -1, -1, -1 }, { 1, -1, -1 }, { -1, -1, 1 }, { 1, -1, 1 },
        { -1, 1, -1 },  { 1, 1, -1 },  { -1, 1, 1 },  { 1, 1, 1 }
    };

    itrig_trig(yaw_bin, &c1024, &s1024);
    cs = (float)c1024 * (1.0f / 1024.0f);
    sn = (float)s1024 * (1.0f / 1024.0f);
    for (unsigned k = 0; k < 8; k++) {
        float lx = csign[k][0] * hx, lz = csign[k][2] * hz;

        corner[k][0] = center[0] + lx * cs + lz * sn;
        corner[k][1] = center[1] + csign[k][1] * hy;
        corner[k][2] = center[2] - lx * sn + lz * cs;
    }
    for (unsigned f = 0; f < 6; f++) {
        float n[3];

        n[0] = qn[f][0] * cs + qn[f][2] * sn;
        n[1] = qn[f][1];
        n[2] = -qn[f][0] * sn + qn[f][2] * cs;
        add_quad(s, corner[quads[f][0]], corner[quads[f][1]],
                 corner[quads[f][2]], corner[quads[f][3]], n, mat);
    }
}

/* 16-segment cylinder with flat caps; the ring vertices come from the
 * shared integer trig table (segments of exactly 22.5 degrees plus the
 * table's constant 0.125-degree phase — a fixed rotation, invisible and
 * deterministic). */
static void add_cylinder(rt_scene *s, const float center[3], float radius,
                         float height, unsigned mat)
{
    const unsigned segs = 16;
    const float y0 = center[1] - 0.5f * height;
    const float y1 = center[1] + 0.5f * height;
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float dn[3] = { 0.0f, -1.0f, 0.0f };
    float ring0[16][3], ring1[16][3], rnorm[16][3];

    for (unsigned k = 0; k < segs; k++) {
        int32_t c1024, s1024;
        float cs, sn;

        itrig_trig((k * (720u / segs)) % 720u, &c1024, &s1024);
        cs = (float)c1024 * (1.0f / 1024.0f);
        sn = (float)s1024 * (1.0f / 1024.0f);
        ring0[k][0] = center[0] + radius * cs;
        ring0[k][1] = y0;
        ring0[k][2] = center[2] + radius * sn;
        ring1[k][0] = ring0[k][0];
        ring1[k][1] = y1;
        ring1[k][2] = ring0[k][2];
        rnorm[k][0] = cs;
        rnorm[k][1] = 0.0f;
        rnorm[k][2] = sn;
    }
    for (unsigned k = 0; k < segs; k++) {
        unsigned k1 = (k + 1) % segs;
        float top[3] = { center[0], y1, center[2] };
        float bot[3] = { center[0], y0, center[2] };

        add_quad(s, ring0[k], ring1[k], ring1[k1], ring0[k1], rnorm[k],
                 mat);
        add_tri3(s, ring1[k], ring1[k1], top, up, up, up, mat);
        add_tri3(s, ring0[k1], ring0[k], bot, dn, dn, dn, mat);
    }
}

/* --- workload: prepare / run --------------------------------------------------*/

struct rt_ctx {
    rt_scene         scene;
    rt_bvh          *bvh;
    rt_camera        cam[RT_VIEWS];
    uint8_t         *frame[RT_VIEWS]; /* reference renders, w * h * 3 */
    rt_result        ref[RT_VIEWS];
};

/* Seed-derived camera: orbit angle from the integer trig table,
 * radius/height from exact float steps. The target is fixed so every
 * view looks at the middle of the showroom. */
static void build_camera(rt_camera *cam, unsigned yaw_bin, float radius,
                         float height)
{
    static const float target[3] = { 0.0f, 0.75f, 0.0f };
    const float world_up[3] = { 0.0f, 1.0f, 0.0f };
    int32_t c1024, s1024;
    float sn, cs, fwd[3], right[3], up[3];

    itrig_trig(yaw_bin, &c1024, &s1024);
    sn = (float)s1024 * (1.0f / 1024.0f);
    cs = (float)c1024 * (1.0f / 1024.0f);
    cam->pos[0] = target[0] + radius * sn;
    cam->pos[1] = target[1] + height;
    cam->pos[2] = target[2] + radius * cs;
    v_sub(fwd, target, cam->pos);
    v_normalize(fwd);
    v_cross(right, fwd, world_up);
    v_normalize(right);
    v_cross(up, right, fwd);
    memcpy(cam->fwd, fwd, sizeof(cam->fwd));
    memcpy(cam->right, right, sizeof(cam->right));
    memcpy(cam->up, up, sizeof(cam->up));
    cam->focal = (RT_VIEW_W * 0.5f) / (float)RT_FOV_TAN_HALF;
}

rt_ctx *rt_prepare(unsigned seed)
{
    rt_ctx *ctx = calloc(1, sizeof(*ctx));
    rng_t rng;
    /* six object slots in a 3x2 arrangement on the floor */
    static const float slot_x[6] = { -2.5f, 0.0f, 2.5f, -2.5f, 0.0f, 2.5f };
    static const float slot_z[6] = { -1.5f, -1.5f, -1.5f, 1.5f, 1.5f, 1.5f };
    static const float radius_opts[3] = { 0.55f, 0.70f, 0.85f };
    static const float half_opts[3] = { 0.45f, 0.60f, 0.75f };
    static const float cyl_r_opts[2] = { 0.35f, 0.50f };
    static const float cyl_h_opts[2] = { 1.0f, 1.4f };
    static const float cam_r_opts[4] = { 4.5f, 5.0f, 5.5f, 6.0f };
    static const float cam_h_opts[4] = { 1.2f, 1.6f, 2.0f, 2.4f };
    static const float light_r_opts[2] = { 4.0f, 5.0f };
    static const float light_h_opts[3] = { 3.0f, 4.0f, 5.0f };
    static const float palette[3][3] = {
        { 0.90f, 0.90f, 0.90f }, /* white */
        { 0.85f, 0.78f, 0.65f }, /* warm */
        { 0.70f, 0.75f, 0.85f }  /* cool */
    };

    if (!ctx)
        return NULL;
    rng_init(&rng, seed);

    add_material(&ctx->scene, M_CHROME,
                 (float[3]) { 0.90f, 0.90f, 0.95f }, 0.0f, 1u, 0.85f);
    add_material(&ctx->scene, M_PAINT_RED,
                 (float[3]) { 0.75f, 0.12f, 0.10f }, 0.55f, 32u, 0.25f);
    add_material(&ctx->scene, M_PAINT_BLUE,
                 (float[3]) { 0.12f, 0.20f, 0.70f }, 0.55f, 32u, 0.25f);
    add_material(&ctx->scene, M_MATTE_WHITE,
                 (float[3]) { 0.82f, 0.80f, 0.78f }, 0.10f, 8u, 0.0f);
    add_material(&ctx->scene, M_MATTE_GREEN,
                 (float[3]) { 0.20f, 0.55f, 0.25f }, 0.12f, 8u, 0.0f);
    add_material(&ctx->scene, M_FLOOR_DARK,
                 (float[3]) { 0.30f, 0.30f, 0.32f }, 0.30f, 16u, 0.15f);
    add_material(&ctx->scene, M_FLOOR_LIGHT,
                 (float[3]) { 0.72f, 0.72f, 0.74f }, 0.18f, 16u, 0.08f);

    add_floor(&ctx->scene);
    for (unsigned slot = 0; slot < 6; slot++) {
        float center[3] = { slot_x[slot], 0.0f, slot_z[slot] };
        unsigned arch = slot % 5u;

        switch (arch) {
        case 0: { /* chrome sphere */
            float r = radius_opts[rng_below(&rng, 3)];

            center[1] = r;
            add_sphere(&ctx->scene, center, r, M_CHROME);
            break;
        }
        case 1: { /* painted sphere */
            float r = radius_opts[rng_below(&rng, 3)];

            center[1] = r;
            add_sphere(&ctx->scene, center, r,
                       rng_below(&rng, 2) ? M_PAINT_BLUE : M_PAINT_RED);
            break;
        }
        case 2: { /* matte box */
            float h = half_opts[rng_below(&rng, 3)];

            center[1] = h;
            add_box(&ctx->scene, center, h, h, h,
                    rng_below(&rng, (uint32_t)ITRIG_BINS),
                    rng_below(&rng, 2) ? M_MATTE_GREEN : M_MATTE_WHITE);
            break;
        }
        case 3: { /* painted box */
            float h = half_opts[rng_below(&rng, 3)];

            center[1] = h;
            add_box(&ctx->scene, center, h, h, h,
                    rng_below(&rng, (uint32_t)ITRIG_BINS),
                    rng_below(&rng, 2) ? M_PAINT_BLUE : M_PAINT_RED);
            break;
        }
        default: { /* matte cylinder */
            float r = cyl_r_opts[rng_below(&rng, 2)];
            float h = cyl_h_opts[rng_below(&rng, 2)];

            center[1] = 0.5f * h;
            add_cylinder(&ctx->scene, center, r, h,
                         rng_below(&rng, 2) ? M_MATTE_GREEN
                                            : M_MATTE_WHITE);
            break;
        }
        }
    }

    for (unsigned li = 0; li < RT_LIGHTS; li++) {
        rt_light *light = &ctx->scene.lights[li];
        int32_t c1024, s1024;
        unsigned yaw_bin = rng_below(&rng, (uint32_t)ITRIG_BINS);
        float r = light_r_opts[rng_below(&rng, 2)];
        float h = light_h_opts[rng_below(&rng, 3)];
        float sn, cs;

        itrig_trig(yaw_bin, &c1024, &s1024);
        sn = (float)s1024 * (1.0f / 1024.0f);
        cs = (float)c1024 * (1.0f / 1024.0f);
        light->pos[0] = r * sn;
        light->pos[1] = h;
        light->pos[2] = r * cs;
        memcpy(light->rgb, palette[li], sizeof(light->rgb));
    }

    /* The BVH comes first (it physically reorders the triangle array),
     * then the reference render of every view. */
    ctx->bvh = rt_bvh_build(&ctx->scene);
    if (!ctx->bvh) {
        rt_free(ctx);
        return NULL;
    }
    for (unsigned v = 0; v < RT_VIEWS; v++) {
        build_camera(&ctx->cam[v],
                     rng_below(&rng, (uint32_t)ITRIG_BINS),
                     cam_r_opts[rng_below(&rng, 4)],
                     cam_h_opts[rng_below(&rng, 4)]);
        ctx->frame[v] = malloc((size_t)RT_VIEW_W * RT_VIEW_H * 3);
        if (!ctx->frame[v] ||
            rt_render(&ctx->scene, ctx->bvh, &ctx->cam[v], RT_VIEW_W,
                      RT_VIEW_H, ctx->frame[v], &ctx->ref[v].rays) != 0) {
            rt_free(ctx);
            return NULL;
        }
        sha1(ctx->frame[v], (size_t)RT_VIEW_W * RT_VIEW_H * 3,
             ctx->ref[v].digest);
    }
    return ctx;
}

void rt_free(rt_ctx *ctx)
{
    if (!ctx)
        return;
    rt_bvh_free(ctx->bvh);
    rt_scene_free(&ctx->scene);
    for (unsigned v = 0; v < RT_VIEWS; v++)
        free(ctx->frame[v]);
    free(ctx);
}

/* One thread's slice: contiguous view instances of the pass, own frame
 * buffer — threads never share writable state. */
typedef struct {
    rt_ctx  *ctx;
    uint8_t *buf;        /* RT_VIEW_W * RT_VIEW_H * 3 */
    size_t   begin, end; /* instance range [begin, end) of the pass */
} rt_slice;

static int rt_cycle(void *arg)
{
    rt_slice *sl = arg;
    rt_ctx *ctx = sl->ctx;

    for (size_t i = sl->begin; i < sl->end; i++) {
        unsigned v = (unsigned)(i % RT_VIEWS);
        rt_result r;
        const rt_result *ref = &ctx->ref[v];

        if (rt_render(&ctx->scene, ctx->bvh, &ctx->cam[v], RT_VIEW_W,
                      RT_VIEW_H, sl->buf, &r.rays) != 0)
            return -1;
        sha1(sl->buf, (size_t)RT_VIEW_W * RT_VIEW_H * 3, r.digest);
        if (memcmp(r.digest, ref->digest, 20) != 0 || r.rays != ref->rays)
            return -1;
    }
    return 0;
}

uint64_t rt_task_pixels(unsigned multi)
{
    unsigned set = multi ? RT_VIEWS_MULTI : RT_VIEWS;

    return (uint64_t)set * RT_VIEW_W * RT_VIEW_H;
}

int rt_run(rt_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mrays, uint64_t *rays)
{
    unsigned t_count = *threads;
    unsigned view_set = t_count > 1 ? RT_VIEWS_MULTI : RT_VIEWS;
    rt_slice *slices = NULL;
    void **ctxs = NULL;
    double elapsed[MAX_RUNS], rates[MAX_RUNS];
    uint64_t total_rays = 0;
    int rc = -1;

    if (!ctx || runs == 0 || runs > MAX_RUNS || !threads || !rays ||
        !throughput_mrays)
        return -1;
    if (t_count > view_set) /* one view instance per thread at least */
        t_count = view_set;
    if (t_count < 1)
        t_count = 1;

    for (unsigned v = 0; v < RT_VIEWS; v++) {
        unsigned instances = view_set / RT_VIEWS; /* 1 or 4 */

        total_rays += (uint64_t)instances * ctx->ref[v].rays;
    }

    slices = calloc(t_count, sizeof(*slices));
    ctxs = malloc(t_count * sizeof(*ctxs));
    if (!slices || !ctxs)
        goto out;

    {
        size_t per = view_set / t_count;
        unsigned extra = view_set % t_count;
        size_t begin = 0;

        for (unsigned t = 0; t < t_count; t++) {
            size_t count = per + (t < extra ? 1u : 0u);

            slices[t].ctx = ctx;
            slices[t].begin = begin;
            slices[t].end = begin + count;
            begin += count;
            slices[t].buf = malloc((size_t)RT_VIEW_W * RT_VIEW_H * 3);
            if (!slices[t].buf)
                goto out;
            ctxs[t] = &slices[t];
        }
    }

    rc = harness_run(t_count, runs, rt_cycle, ctxs, elapsed);
    if (rc == 0) {
        for (unsigned r = 0; r < runs; r++)
            rates[r] = elapsed[r] > 0.0
                           ? (double)total_rays / elapsed[r] / 1e6
                           : -1.0;
        *throughput_mrays = lb_median(rates, runs);
        *threads = t_count;
        *rays = total_rays;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++)
            free(slices[t].buf);
    free(slices);
    free(ctxs);
    return rc;
}

/* --- accessors ------------------------------------------------------------------*/

const uint8_t *rt_view_frame(const rt_ctx *ctx, unsigned view)
{
    if (!ctx || view >= RT_VIEWS)
        return NULL;
    return ctx->frame[view];
}

size_t rt_view_len(const rt_ctx *ctx, unsigned view)
{
    if (!ctx || view >= RT_VIEWS)
        return 0;
    return (size_t)RT_VIEW_W * RT_VIEW_H * 3;
}

const rt_result *rt_reference(const rt_ctx *ctx, unsigned view)
{
    if (!ctx || view >= RT_VIEWS)
        return NULL;
    return &ctx->ref[view];
}

uint32_t rt_tri_count(const rt_ctx *ctx)
{
    if (!ctx)
        return 0;
    return ctx->scene.tri_count;
}

const rt_camera *rt_camera_of(const rt_ctx *ctx, unsigned view)
{
    if (!ctx || view >= RT_VIEWS)
        return NULL;
    return &ctx->cam[view];
}

const rt_scene *rt_scene_of(const rt_ctx *ctx)
{
    if (!ctx)
        return NULL;
    return &ctx->scene;
}

const rt_bvh *rt_bvh_of(const rt_ctx *ctx)
{
    if (!ctx)
        return NULL;
    return ctx->bvh;
}

