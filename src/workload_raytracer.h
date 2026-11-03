#ifndef WORKLOAD_RAYTRACER_H
#define WORKLOAD_RAYTRACER_H

#include <stddef.h>
#include <stdint.h>

/* Workload #15 — Ray Tracer.
 *
 * Mirrors the fifteenth Geekbench 6 CPU workload: render a 3D scene
 * with a Whitted-style ray tracer over a BVH (Geekbench 6 renders the
 * Blender BMW scene with an Embree-style tracer). LotofBench reproduces
 * the shape dependency-free: a seeded synthetic showroom (tessellated
 * floor, spheres, boxes and cylinders; matte, painted and chrome
 * materials; three point lights) is rendered from eight seeded cameras
 * at a fixed viewport. Every timed cycle re-renders the whole view set
 * and each frame must reproduce its prepare-time SHA-1 digest AND its
 * exact ray count; any drift aborts the run instead of scoring.
 *
 * The timed path is float arithmetic restricted to +, -, *, / and
 * sqrtf — IEEE-exact operations, compiled with -ffp-contract=off — and
 * the seeded geometry uses the shared integer trig table (src/itrig.h),
 * so frames are bit-identical across machines. No transcendental
 * function, no libm call, no FMA contraction. Documented in
 * docs/benchmarks.md, section "Workload 15". */

typedef struct rt_scene rt_scene;
typedef struct rt_bvh   rt_bvh;
typedef struct rt_ctx   rt_ctx;

/* Fixed input sizes, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the views are the views. */
#define RT_VIEWS        8u  /* the single-core pass renders these eight */
#define RT_VIEWS_MULTI 32u  /* multi-core pass = 4 x the view set */
#define RT_VIEW_W      480u /* viewport pixels (RGB888) */
#define RT_VIEW_H      360u

/* Whitted recursion: primary ray = depth 0, up to RT_MAX_DEPTH mirror
 * bounces. Three seeded point lights, so a primary ray spawns at most
 * 3 shadow rays per hit level plus one reflection ray per reflective
 * level: the worst case is 1 + 3*4 + 3 = 16 rays per pixel. */
#define RT_MAX_DEPTH   3u
#define RT_LIGHTS      3u
#define RT_MATERIALS   8u

/* BVH leaves hold at most this many triangles (Embree-style). */
#define RT_LEAF_MAX    4u

/* The camera FOV is a fixed 60 degrees; the focal length is
 * (W / 2) / tan(30 deg) with tan(30 deg) hard-coded at full double
 * precision — no libm tan call, bit-identical everywhere. */
#define RT_FOV_TAN_HALF 0.5773502691896257

/* Result of one full view render. */
typedef struct {
    uint8_t  digest[20];       /* SHA-1 of the RGB888 frame */
    uint64_t rays;             /* primary + shadow + reflection rays */
} rt_result;

/* --- low-level API (public for the gate and the unit tests) ---------------*/

typedef struct {
    float     v0[3], v1[3], v2[3];  /* vertices, counter-clockwise */
    float     n0[3], n1[3], n2[3];  /* per-vertex normals (unit) */
    unsigned  mat;                  /* material index < RT_MATERIALS */
} rt_tri;

typedef struct {
    float albedo[3];   /* diffuse reflectance, 0..1 */
    float ks;          /* Phong specular weight */
    unsigned phong_n;  /* Phong exponent (integer power loop) */
    float reflect;     /* mirror reflectivity, 0..1 (0 = none) */
} rt_material;

typedef struct {
    float pos[3];      /* point light position */
    float rgb[3];      /* per-channel intensity, 0..1 */
} rt_light;

/* Precomputed camera: position, orthonormal basis (fwd/right/up) and
 * the focal length in pixels. Built off the clock from quantized,
 * itrig-derived parameters — see docs/benchmarks.md. */
typedef struct {
    float pos[3];
    float fwd[3], right[3], up[3];
    float focal;
} rt_camera;

struct rt_scene {
    rt_tri      *tris;
    uint32_t     tri_count, tri_cap;
    rt_material  mats[RT_MATERIALS];
    unsigned     mat_count;
    rt_light     lights[RT_LIGHTS];
};

/* Empty scene (no triangles, no materials); returns 0, or -1 on NULL. */
int  rt_scene_init(rt_scene *s);
/* Append one triangle (copied); grows the array. Returns 0, or -1 on
 * NULL / allocation failure / full material index. */
int  rt_scene_add_tri(rt_scene *s, const rt_tri *t);
void rt_scene_free(rt_scene *s);

/* Build the BVH over the scene's triangles (median split on the longest
 * centroid axis, leaves of at most RT_LEAF_MAX triangles, merge sort
 * tie-broken by index — fully deterministic). The triangle array is
 * PHYSICALLY REORDERED into leaf order (the rendered image does not
 * depend on the triangle order: hit ties are broken by position in
 * both acceleration paths). Returns NULL on failure. */
rt_bvh *rt_bvh_build(rt_scene *s);
void    rt_bvh_free(rt_bvh *b);

/* Render one view: for every pixel, cast the primary ray through the
 * pixel centre, shade the closest hit (ambient + Lambert + Phong
 * specular per light with a shadow ray, mirror reflections up to
 * RT_MAX_DEPTH), tonemap to RGB888. Fills rgb (w * h * 3) and *rays
 * with the exact ray count. Returns 0, or -1 on NULL / bad sizes. */
int rt_render(const rt_scene *s, const rt_bvh *b, const rt_camera *cam,
              unsigned w, unsigned h, uint8_t *rgb, uint64_t *rays);

/* The same renderer without the BVH: every ray scans all triangles
 * linearly. Bit-identical to rt_render by construction — the
 * differential test that validates the BVH. */
int rt_render_bruteforce(const rt_scene *s, const rt_camera *cam,
                         unsigned w, unsigned h, uint8_t *rgb,
                         uint64_t *rays);

/* --- workload API ----------------------------------------------------------*/

/* Build the seeded showroom (off the clock; fixed seed => bit-identical
 * scene and cameras on every machine), build the BVH once and render
 * the reference frame of every view. */
rt_ctx *rt_prepare(unsigned seed);
void    rt_free(rt_ctx *ctx);

/* One workload pass with *threads threads (clamped to the view set of
 * the pass: 8 single-core, 32 multi-core). Every cycle re-renders its
 * slice and verifies each frame's digest AND ray count against the
 * prepare-time reference. Fills *throughput_mrays with the median over
 * the runs, in millions of traced rays per second, and *rays with the
 * exact ray count of the pass (deterministic, identical for every
 * run). Returns 0, or -1 if any view failed to verify. */
int rt_run(rt_ctx *ctx, unsigned runs, unsigned *threads,
           double *throughput_mrays, uint64_t *rays);

/* Read-only access for tests and tooling. */
const uint8_t  *rt_view_frame(const rt_ctx *ctx, unsigned view);
size_t          rt_view_len(const rt_ctx *ctx, unsigned view);
const rt_result *rt_reference(const rt_ctx *ctx, unsigned view);
uint32_t        rt_tri_count(const rt_ctx *ctx);
const rt_camera *rt_camera_of(const rt_ctx *ctx, unsigned view);
const rt_scene  *rt_scene_of(const rt_ctx *ctx);
const rt_bvh    *rt_bvh_of(const rt_ctx *ctx);

/* Exact rendered-pixel count of one pass (single- or multi-core). */
uint64_t rt_task_pixels(unsigned multi);

#endif /* WORKLOAD_RAYTRACER_H */
