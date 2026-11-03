/* Unit tests — workload #15: Ray Tracer. */

#include "tests.h"

#include "sha1.h"
#include "workload_raytracer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Golden pins produced by tools/gen_goldens (v0.9.0, seed 1). After a
 * DELIBERATE change to the scene generator or the renderer, re-run the
 * tool and port the printed values here in the same commit. */
static const uint8_t RT_GOLDEN_VIEW0_REF[20] = {
    0xc4, 0xdf, 0xca, 0x84, 0x72, 0xdf, 0x3a, 0x95, 0x27, 0x5f,
    0x8e, 0x43, 0xc5, 0x60, 0x84, 0x00, 0x55, 0x40, 0x24, 0x50
};
#define RT_GOLDEN_VIEW0_RAYS      750324ull
#define RT_GOLDEN_TRIANGLES       984u
#define RT_GOLDEN_TASK_PIXELS        1382400ull
#define RT_GOLDEN_TASK_PIXELS_MULTI  5529600ull

/* One shared prepare for the read-only tests (the scene is immutable;
 * the process exits right after the suite anyway). */
static rt_ctx *shared_rt(void)
{
    static rt_ctx *ctx;

    if (!ctx)
        ctx = rt_prepare(1);
    return ctx;
}

void test_rt_scene_deterministic(void)
{
    rt_ctx *a = rt_prepare(1), *b = rt_prepare(1);

    CHECK(a && b);
    if (!a || !b) {
        rt_free(a);
        rt_free(b);
        return;
    }
    CHECK_U64(rt_tri_count(a), rt_tri_count(b));
    CHECK(memcmp(rt_reference(a, 0), rt_reference(b, 0),
                 sizeof(rt_result)) == 0);
    for (unsigned v = 0; v < RT_VIEWS; v++) {
        CHECK_U64(rt_view_len(a, v), rt_view_len(b, v));
        CHECK(memcmp(rt_view_frame(a, v), rt_view_frame(b, v),
                     rt_view_len(a, v)) == 0);
        CHECK(memcmp(rt_camera_of(a, v), rt_camera_of(b, v),
                     sizeof(rt_camera)) == 0);
    }
    rt_free(a);
    rt_free(b);
}

void test_rt_input_invariants(void)
{
    rt_ctx *ctx = shared_rt();

    CHECK(ctx);
    if (!ctx)
        return;
    /* floor 512 + spheres/boxes/cylinder, well inside the cap */
    CHECK(rt_tri_count(ctx) >= 800u && rt_tri_count(ctx) <= 4000u);
    for (unsigned v = 0; v < RT_VIEWS; v++) {
        const rt_camera *cam = rt_camera_of(ctx, v);

        CHECK(cam != NULL);
        if (!cam)
            continue;
        /* the basis is orthonormal (right = fwd x world_up, up = right
         * x fwd) and the focal length is the fixed 60-degree constant */
        {
            const float *f = cam->fwd, *r = cam->right, *u = cam->up;

            CHECK(fabsf(f[0] * r[0] + f[1] * r[1] + f[2] * r[2]) < 1e-5f);
            CHECK(fabsf(f[0] * u[0] + f[1] * u[1] + f[2] * u[2]) < 1e-5f);
            CHECK(fabsf(r[0] * u[0] + r[1] * u[1] + r[2] * u[2]) < 1e-5f);
            CHECK(fabsf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2] - 1.0f) <
                  1e-5f);
        }
        CHECK(cam->focal > 400.0f && cam->focal < 430.0f);
        /* every camera looks at the showroom from outside the floor */
        CHECK(fabsf(cam->pos[0]) < 8.0f && fabsf(cam->pos[2]) < 8.0f);
        CHECK(cam->pos[1] > 1.0f && cam->pos[1] < 4.0f);
        /* the reference render verified at prepare time */
        CHECK(rt_reference(ctx, v) != NULL);
        CHECK(rt_reference(ctx, v)->rays >= (uint64_t)RT_VIEW_W *
              RT_VIEW_H);
    }
    /* three seeded point lights, quantized palette values */
    {
        const rt_scene *sc = rt_scene_of(ctx);

        CHECK(sc != NULL);
        if (!sc)
            return;
        for (unsigned li = 0; li < RT_LIGHTS; li++) {
            CHECK(sc->lights[li].pos[1] >= 3.0f);
            CHECK(sc->lights[li].rgb[0] > 0.0f &&
                  sc->lights[li].rgb[0] <= 1.0f);
        }
    }
}

void test_rt_render_differential(void)
{
    /* THE differential test: the BVH renderer and the brute-force
     * renderer are bit-identical on the real scene (small viewport) */
    const unsigned w = 96, h = 72;
    const size_t bytes = (size_t)w * h * 3;
    uint8_t *f1 = malloc(bytes), *f2 = malloc(bytes);
    rt_ctx *ctx = shared_rt();

    CHECK(f1 && f2 && ctx);
    if (!f1 || !f2 || !ctx) {
        free(f1);
        free(f2);
        return;
    }
    for (unsigned v = 0; v < RT_VIEWS; v++) {
        uint64_t r1 = 0, r2 = 0;
        uint8_t d1[20], d2[20];

        CHECK(rt_render(rt_scene_of(ctx), rt_bvh_of(ctx),
                        rt_camera_of(ctx, v), w, h, f1, &r1) == 0);
        CHECK(rt_render_bruteforce(rt_scene_of(ctx),
                                   rt_camera_of(ctx, v), w, h, f2,
                                   &r2) == 0);
        CHECK_U64(r1, r2);
        sha1(f1, bytes, d1);
        sha1(f2, bytes, d2);
        CHECK(memcmp(d1, d2, 20) == 0);
    }
    free(f1);
    free(f2);
}

void test_rt_unit_vectors(void)
{
    /* a lone matte quad under one white light: the centre pixel is
     * ambient + diffuse with ndotl ~ 1 (hand-computed 0.88 for channel
     * 0 within the +-1 tonemap step); with a wall between the light and
     * the quad it drops to ambient only (0.08, 0.05, 0.02) */
    const unsigned w = 64, h = 64;
    const size_t bytes = (size_t)w * h * 3;
    uint8_t *frame = malloc(bytes);
    rt_scene s;
    rt_camera cam;
    rt_bvh *bvh;
    uint64_t rays = 0;
    const uint8_t *px;

    CHECK(frame);
    if (!frame)
        return;

    rt_scene_init(&s);
    s.mats[0].albedo[0] = 0.8f;
    s.mats[0].albedo[1] = 0.5f;
    s.mats[0].albedo[2] = 0.2f;
    s.mats[0].ks = 0.0f;
    s.mats[0].phong_n = 1u;
    s.mats[0].reflect = 0.0f;
    s.mat_count = 1;
    {
        float a[3] = { -2, -2, 0 }, b[3] = { 2, -2, 0 };
        float c[3] = { 2, 2, 0 }, d[3] = { -2, 2, 0 };
        float n[3] = { 0, 0, 1 };
        rt_tri t;

        memset(&t, 0, sizeof(t));
        memcpy(t.v0, a, sizeof(t.v0));
        memcpy(t.v1, b, sizeof(t.v1));
        memcpy(t.v2, c, sizeof(t.v2));
        memcpy(t.n0, n, sizeof(t.n0));
        memcpy(t.n1, n, sizeof(t.n1));
        memcpy(t.n2, n, sizeof(t.n2));
        t.mat = 0;
        CHECK(rt_scene_add_tri(&s, &t) == 0);
        memcpy(t.v0, a, sizeof(t.v0));
        memcpy(t.v1, c, sizeof(t.v1));
        memcpy(t.v2, d, sizeof(t.v2));
        CHECK(rt_scene_add_tri(&s, &t) == 0);
    }
    s.lights[0].pos[0] = 0.0f;
    s.lights[0].pos[1] = 0.0f;
    s.lights[0].pos[2] = 5.0f;
    s.lights[0].rgb[0] = s.lights[0].rgb[1] = s.lights[0].rgb[2] = 1.0f;
    cam.pos[0] = cam.pos[1] = 0.0f;
    cam.pos[2] = 4.0f;
    cam.fwd[0] = cam.fwd[1] = 0.0f;
    cam.fwd[2] = -1.0f;
    cam.right[0] = 1.0f;
    cam.right[1] = cam.right[2] = 0.0f;
    cam.up[0] = cam.up[2] = 0.0f;
    cam.up[1] = 1.0f;
    cam.focal = (RT_VIEW_W * 0.5f) / (float)RT_FOV_TAN_HALF;
    bvh = rt_bvh_build(&s);
    CHECK(bvh != NULL);
    CHECK(rt_render(&s, bvh, &cam, w, h, frame, &rays) == 0);
    CHECK_U64(rays, (uint64_t)w * h * 2); /* primary + one shadow ray */
    px = frame + (32u * w + 32u) * 3;
    CHECK(abs((int)px[0] - 224) <= 1);
    CHECK(abs((int)px[1] - 140) <= 1);
    CHECK(abs((int)px[2] - 56) <= 1);

    /* now the wall: the centre pixel drops to ambient only */
    {
        float a[3] = { -3, -3, 2.5f }, b[3] = { 3, -3, 2.5f };
        float c[3] = { 3, 3, 2.5f }, d[3] = { -3, 3, 2.5f };
        float n[3] = { 0, 0, 1 };
        rt_tri t;

        s.mats[1].albedo[0] = s.mats[1].albedo[1] =
            s.mats[1].albedo[2] = 1.0f;
        s.mats[1].ks = 0.0f;
        s.mats[1].phong_n = 1u;
        s.mats[1].reflect = 0.0f;
        s.mat_count = 2;
        memset(&t, 0, sizeof(t));
        memcpy(t.v0, a, sizeof(t.v0));
        memcpy(t.v1, b, sizeof(t.v1));
        memcpy(t.v2, c, sizeof(t.v2));
        memcpy(t.n0, n, sizeof(t.n0));
        memcpy(t.n1, n, sizeof(t.n1));
        memcpy(t.n2, n, sizeof(t.n2));
        t.mat = 1;
        CHECK(rt_scene_add_tri(&s, &t) == 0);
        memcpy(t.v0, a, sizeof(t.v0));
        memcpy(t.v1, c, sizeof(t.v1));
        memcpy(t.v2, d, sizeof(t.v2));
        CHECK(rt_scene_add_tri(&s, &t) == 0);
        rt_bvh_free(bvh);
        bvh = rt_bvh_build(&s);
        CHECK(bvh != NULL);
        cam.pos[2] = 2.0f; /* below the wall: the primary still hits */
        CHECK(rt_render(&s, bvh, &cam, w, h, frame, &rays) == 0);
        px = frame + (32u * w + 32u) * 3;
        CHECK(abs((int)px[0] - 20) <= 1);
        CHECK(abs((int)px[1] - 13) <= 1);
        CHECK(abs((int)px[2] - 5) <= 1);
    }
    rt_bvh_free(bvh);
    rt_scene_free(&s);
    free(frame);
}

void test_rt_corpus_golden(void)
{
    rt_ctx *ctx = shared_rt();
    const rt_result *r;

    CHECK(ctx);
    if (!ctx)
        return;
    r = rt_reference(ctx, 0);
    CHECK(r != NULL);
    if (!r)
        return;
    CHECK(memcmp(r->digest, RT_GOLDEN_VIEW0_REF, 20) == 0);
    CHECK_U64(r->rays, RT_GOLDEN_VIEW0_RAYS);
    CHECK_U64(rt_tri_count(ctx), RT_GOLDEN_TRIANGLES);
    CHECK_U64(rt_task_pixels(0), RT_GOLDEN_TASK_PIXELS);
    CHECK_U64(rt_task_pixels(1), RT_GOLDEN_TASK_PIXELS_MULTI);
    /* every view's reference digest must match a fresh render */
    for (unsigned v = 0; v < RT_VIEWS; v++) {
        uint64_t rays = 0;
        uint8_t digest[20];
        uint8_t *buf = malloc(rt_view_len(ctx, v));

        CHECK(buf != NULL);
        if (!buf)
            continue;
        CHECK(rt_render(rt_scene_of(ctx), rt_bvh_of(ctx),
                        rt_camera_of(ctx, v), RT_VIEW_W, RT_VIEW_H,
                        buf, &rays) == 0);
        CHECK_U64(rays, rt_reference(ctx, v)->rays);
        sha1(buf, rt_view_len(ctx, v), digest);
        CHECK(memcmp(digest, rt_reference(ctx, v)->digest, 20) == 0);
        free(buf);
    }
}

void test_rt_seed_variation(void)
{
    rt_ctx *a = rt_prepare(1), *b = rt_prepare(2);

    CHECK(a && b);
    if (!a || !b) {
        rt_free(a);
        rt_free(b);
        return;
    }
    CHECK(memcmp(rt_view_frame(a, 0), rt_view_frame(b, 0),
                 rt_view_len(a, 0)) != 0); /* different seed => different
                                              scene and cameras */
    rt_free(a);
    rt_free(b);
}

/* Exact ray count of one pass: the sum over the view set (single-core
 * 8 views, multi-core 4x) of the per-view reference counts. */
static uint64_t pass_rays(const rt_ctx *ctx, unsigned multi)
{
    uint64_t total = 0;
    unsigned instances = multi ? RT_VIEWS_MULTI / RT_VIEWS : 1u;

    for (unsigned v = 0; v < RT_VIEWS; v++)
        total += (uint64_t)instances * rt_reference(ctx, v)->rays;
    return total;
}

void test_rt_run_sane(void)
{
    rt_ctx *ctx = shared_rt();
    unsigned threads = 1;
    double mrays = 0.0;
    uint64_t rays = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(rt_run(ctx, 1, &threads, &mrays, &rays) == 0);
    CHECK_U64(threads, 1);
    CHECK_U64(rays, pass_rays(ctx, 0)); /* the whole 8-view set */
    CHECK(mrays > 0.0);
}

void test_rt_thread_clamp(void)
{
    rt_ctx *ctx = shared_rt();
    unsigned threads = 999;
    double mrays = 0.0;
    uint64_t rays = 0;

    CHECK(ctx);
    if (!ctx)
        return;
    /* more threads than view instances: clamped to the 32-view set */
    CHECK(rt_run(ctx, 1, &threads, &mrays, &rays) == 0);
    CHECK_U64(threads, RT_VIEWS_MULTI);
    CHECK_U64(rays, pass_rays(ctx, 1));
}

void test_rt_accessors_null_safe(void)
{
    CHECK(rt_view_frame(NULL, 0) == NULL);
    CHECK_U64(rt_view_len(NULL, 0), 0);
    CHECK(rt_reference(NULL, 0) == NULL);
    CHECK_U64(rt_tri_count(NULL), 0);
    CHECK(rt_camera_of(NULL, 0) == NULL);
    CHECK(rt_scene_of(NULL) == NULL);
    CHECK(rt_bvh_of(NULL) == NULL);

    rt_ctx *ctx = shared_rt();

    CHECK(ctx);
    if (!ctx)
        return;
    CHECK(rt_view_frame(ctx, RT_VIEWS) == NULL);
    CHECK_U64(rt_view_len(ctx, RT_VIEWS), 0);
    CHECK(rt_reference(ctx, RT_VIEWS) == NULL);
    CHECK(rt_camera_of(ctx, RT_VIEWS) == NULL);
}
