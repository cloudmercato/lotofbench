/* LotofBench — Navigation workload (workload #2).
 *
 * Mirrors the second Geekbench 6 CPU workload (Navigation): Dijkstra
 * route planning over a road network. Geekbench 6 routes over
 * OpenStreetMap data; lotofbench reproduces the shape of the workload
 * dependency-free, with the same honesty rules as File Compression:
 *
 *   - deterministic synthetic input — a seeded road grid (streets in a
 *     lattice, some diagonal shortcuts, long-range fast highways) and a
 *     seeded set of source/target pairs. Fixed --seed means an identical
 *     graph and identical routes on every machine, so scores are
 *     comparable across runs;
 *   - correctness inside the timed cycle — every found route is verified
 *     (parent chain re-summed edge by edge, straight-line lower bound)
 *     before the run counts; a broken search aborts instead of scoring;
 *   - shared-task multi-core through the common harness — the route set
 *     is split into equal contiguous slices, one per thread.
 *
 * The throughput metric is edge relaxations: every directed edge scanned
 * by the search is counted, and the score measures how many millions of
 * edges per second the machine relaxes over the whole route set. The
 * count is exact and machine-independent, like the corpus size of
 * workload #1. */

#include "workload_navigation.h"

#include "harness.h"
#include "lotofbench.h"
#include "rng.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Graph shape. The lattice guarantees connectivity on its own; diagonal
 * shortcuts and highways only add alternative routes. (NAV_GRID_SIDE and
 * NAV_ROUTES — the input-size knobs surfaced by the driver — live in the
 * header.) */
#define NAV_DIAGONAL_PCT         40u  /* % of cells with a NE shortcut */
#define NAV_HIGHWAYS             2048u /* long-range fast roads */
#define NAV_ROAD_COST_PER_UNIT   10u   /* travel time per grid unit */
#define NAV_HIGHWAY_COST_PER_UNIT 4u   /* highways are 2.5x faster */
#define NAV_JITTER_PCT           20u   /* per-road travel-time jitter */

/* Route set: source/target pairs at least NAV_ROUTE_MIN_DIST grid units
 * apart, at most NAV_ROUTE_MAX_DIST. Far enough to make every route a
 * real search, close enough to stay inside the grid. */
#define NAV_ROUTE_MIN_DIST       300u
#define NAV_ROUTE_MAX_DIST       700u

#define NAV_NONE UINT32_MAX

struct nav_ctx {
    uint32_t side;
    uint32_t nodes;
    uint32_t edge_count; /* directed edges in the CSR arrays */
    uint32_t *off;       /* nodes + 1 offsets */
    uint32_t *dst;       /* edge targets */
    uint32_t *w;         /* edge travel times */
    uint32_t route_src[NAV_ROUTES];
    uint32_t route_dst[NAV_ROUTES];
};

/* Straight-line squared distance between two grid nodes. */
static uint64_t grid_d2(uint32_t side, uint32_t a, uint32_t b)
{
    int64_t dx = (int64_t)(a % side) - (int64_t)(b % side);
    int64_t dy = (int64_t)(a / side) - (int64_t)(b / side);

    return (uint64_t)(dx * dx + dy * dy);
}

/* Travel time of one road: euclidean length times the road's cost per
 * unit, +/- jitter, rounded to an integer >= 1. Integer travel times
 * keep every Dijkstra result machine-independent. */
static uint32_t nav_weight(rng_t *rng, uint64_t d2, unsigned per_unit)
{
    uint32_t base = (uint32_t)(sqrt((double)d2) * per_unit + 0.5);
    int32_t jit = (int32_t)rng_below(rng, 2 * NAV_JITTER_PCT + 1) -
                  (int32_t)NAV_JITTER_PCT;
    uint64_t w = ((uint64_t)base * (100u + (unsigned)jit) + 50u) / 100u;

    return w ? (uint32_t)w : 1;
}

/* --- graph construction ---------------------------------------------------*/

typedef struct {
    uint32_t u, v, w; /* one undirected road */
} raw_edge;

static int raw_push(raw_edge **arr, size_t *n, size_t cap,
                    uint32_t u, uint32_t v, uint32_t w)
{
    if (*n >= cap)
        return -1;
    (*arr)[(*n)++] = (raw_edge){ u, v, w };
    return 0;
}

nav_ctx *nav_prepare(unsigned seed)
{
    nav_ctx *ctx = calloc(1, sizeof(*ctx));

    if (!ctx)
        return NULL;
    ctx->side = NAV_GRID_SIDE;
    ctx->nodes = NAV_GRID_SIDE * NAV_GRID_SIDE;

    /* Independent PRNG stream: same seed => same graph and routes, but
     * uncorrelated with workload #1's corpus stream. */
    rng_t rng;
    rng_init(&rng, (uint64_t)seed * 0xD1B54A32D192ED03ull +
                       0x2545F4914F6CDD1Dull);

    /* Upper bound: 2 lattice + 1 diagonal per cell, plus highways. */
    size_t cap = (size_t)ctx->nodes * 3 + NAV_HIGHWAYS + 64;
    size_t n_raw = 0;
    uint32_t *cur = NULL;
    raw_edge *raw = malloc(cap * sizeof(*raw));

    if (!raw) {
        free(ctx);
        return NULL;
    }

    /* Streets: lattice right + down, seeded NE diagonal shortcuts. */
    for (uint32_t y = 0; y < ctx->side; y++) {
        for (uint32_t x = 0; x < ctx->side; x++) {
            uint32_t id = y * ctx->side + x;

            if (x + 1 < ctx->side &&
                raw_push(&raw, &n_raw, cap, id, id + 1,
                         nav_weight(&rng, 1, NAV_ROAD_COST_PER_UNIT)))
                goto fail;
            if (y + 1 < ctx->side &&
                raw_push(&raw, &n_raw, cap, id, id + ctx->side,
                         nav_weight(&rng, 1, NAV_ROAD_COST_PER_UNIT)))
                goto fail;
            if (x + 1 < ctx->side && y + 1 < ctx->side &&
                rng_below(&rng, 100) < NAV_DIAGONAL_PCT &&
                raw_push(&raw, &n_raw, cap, id, id + ctx->side + 1,
                         nav_weight(&rng, 2, NAV_ROAD_COST_PER_UNIT)))
                goto fail;
        }
    }

    /* Highways: long, straight and fast — 2.5x the base speed, so the
     * search has real routing decisions instead of a flat grid. */
    for (unsigned i = 0; i < NAV_HIGHWAYS; i++) {
        uint32_t u = (uint32_t)rng_below(&rng, ctx->nodes);
        uint32_t v = u;
        unsigned tries = 0;

        while ((v == u || grid_d2(ctx->side, u, v) <
                              (uint64_t)ctx->side * ctx->side / 4) &&
               ++tries < 64)
            v = (uint32_t)rng_below(&rng, ctx->nodes);
        if (v == u || grid_d2(ctx->side, u, v) <
                          (uint64_t)ctx->side * ctx->side / 4)
            continue; /* keep the RNG stream deterministic, skip */
        if (raw_push(&raw, &n_raw, cap, u, v,
                     nav_weight(&rng, grid_d2(ctx->side, u, v),
                                NAV_HIGHWAY_COST_PER_UNIT)))
            goto fail;
    }

    /* Flatten the undirected roads into a directed CSR adjacency. */
    ctx->edge_count = (uint32_t)(2 * n_raw);
    ctx->off = calloc((size_t)ctx->nodes + 1, sizeof(*ctx->off));
    ctx->dst = malloc((size_t)ctx->edge_count * sizeof(*ctx->dst));
    ctx->w = malloc((size_t)ctx->edge_count * sizeof(*ctx->w));
    cur = malloc((size_t)ctx->nodes * sizeof(*cur));

    if (!ctx->off || !ctx->dst || !ctx->w || !cur)
        goto fail;
    for (size_t i = 0; i < n_raw; i++) {
        ctx->off[raw[i].u + 1]++;
        ctx->off[raw[i].v + 1]++;
    }
    for (uint32_t i = 0; i < ctx->nodes; i++)
        ctx->off[i + 1] += ctx->off[i];
    memcpy(cur, ctx->off, (size_t)ctx->nodes * sizeof(*cur));
    for (size_t i = 0; i < n_raw; i++) {
        ctx->dst[cur[raw[i].u]] = raw[i].v;
        ctx->w[cur[raw[i].u]] = raw[i].w;
        cur[raw[i].u]++;
        ctx->dst[cur[raw[i].v]] = raw[i].u;
        ctx->w[cur[raw[i].v]] = raw[i].w;
        cur[raw[i].v]++;
    }
    free(cur);

    /* Routes: seeded source/target pairs inside the distance band. */
    for (unsigned i = 0; i < NAV_ROUTES; i++) {
        uint32_t s = (uint32_t)rng_below(&rng, ctx->nodes);
        uint32_t t = s;

        for (unsigned tries = 0; tries < 1000; tries++) {
            t = (uint32_t)rng_below(&rng, ctx->nodes);
            uint64_t d2 = grid_d2(ctx->side, s, t);

            if (d2 >= (uint64_t)NAV_ROUTE_MIN_DIST * NAV_ROUTE_MIN_DIST &&
                d2 <= (uint64_t)NAV_ROUTE_MAX_DIST * NAV_ROUTE_MAX_DIST)
                break;
        }
        if (t == s) /* cannot happen in practice; a degenerate route
                     * would still verify, but keep the set honest */
            t = (s + 1) % ctx->nodes;
        ctx->route_src[i] = s;
        ctx->route_dst[i] = t;
    }

    free(raw);
    return ctx;

fail:
    free(cur);
    free(ctx->w);
    free(ctx->dst);
    free(ctx->off);
    free(raw);
    free(ctx);
    return NULL;
}

void nav_free(nav_ctx *ctx)
{
    if (!ctx)
        return;
    free(ctx->off);
    free(ctx->dst);
    free(ctx->w);
    free(ctx);
}

/* --- the search ------------------------------------------------------------*/

/* One thread's scratch state and route slice. */
typedef struct {
    const nav_ctx *g;
    uint64_t *dist;    /* tentative travel times */
    uint32_t *parent;  /* predecessor on the best known path */
    int32_t *pos;      /* heap slot of each node, -1 when absent */
    uint32_t *heap_id; /* binary min-heap of node ids, keyed by dist */
    uint32_t heap_n;
    uint64_t ops;      /* directed edges scanned */
    size_t route_begin, route_end;
} nav_slice;

/* Indexed binary min-heap: decrease-key instead of lazy deletion keeps
 * the heap bounded by the node count and the sift counts honest. */

static void nav_sift_up(nav_slice *s, uint32_t i)
{
    uint32_t v = s->heap_id[i];

    while (i > 0) {
        uint32_t p = (i - 1) / 2;

        if (s->dist[s->heap_id[p]] <= s->dist[v])
            break;
        s->heap_id[i] = s->heap_id[p];
        s->pos[s->heap_id[i]] = (int32_t)i;
        i = p;
    }
    s->heap_id[i] = v;
    s->pos[v] = (int32_t)i;
}

static void nav_sift_down(nav_slice *s, uint32_t i)
{
    uint32_t n = s->heap_n;
    uint32_t v = s->heap_id[i];

    for (;;) {
        uint32_t c = 2 * i + 1;

        if (c >= n)
            break;
        if (c + 1 < n && s->dist[s->heap_id[c + 1]] < s->dist[s->heap_id[c]])
            c++;
        if (s->dist[s->heap_id[c]] >= s->dist[v])
            break;
        s->heap_id[i] = s->heap_id[c];
        s->pos[s->heap_id[i]] = (int32_t)i;
        i = c;
    }
    s->heap_id[i] = v;
    s->pos[v] = (int32_t)i;
}

static void nav_heap_push(nav_slice *s, uint32_t v)
{
    s->heap_id[s->heap_n] = v;
    s->pos[v] = (int32_t)s->heap_n;
    s->heap_n++;
    nav_sift_up(s, s->heap_n - 1);
}

static uint32_t nav_heap_pop(nav_slice *s)
{
    uint32_t root = s->heap_id[0];

    s->pos[root] = -1;
    s->heap_n--;
    if (s->heap_n > 0) {
        s->heap_id[0] = s->heap_id[s->heap_n];
        s->pos[s->heap_id[0]] = 0;
        nav_sift_down(s, 0);
    }
    return root;
}

/* One route: early-exit Dijkstra from src, stopping when target is
 * settled, then verification. Returns 0 or -1. */
static int nav_route(nav_slice *s, uint32_t src, uint32_t target)
{
    const nav_ctx *g = s->g;
    const uint32_t n = g->nodes;

    memset(s->dist, 0xFF, (size_t)n * sizeof(*s->dist));
    memset(s->parent, 0xFF, (size_t)n * sizeof(*s->parent));
    for (uint32_t i = 0; i < n; i++)
        s->pos[i] = -1;
    s->heap_n = 0;

    s->dist[src] = 0;
    nav_heap_push(s, src);

    while (s->heap_n > 0) {
        uint32_t u = nav_heap_pop(s);

        if (u == target)
            break; /* settled: first pop of a node is its final time */
        uint64_t du = s->dist[u];

        for (uint32_t e = g->off[u]; e < g->off[u + 1]; e++) {
            uint32_t v = g->dst[e];
            uint64_t nd = du + g->w[e];

            s->ops++;
            if (nd < s->dist[v]) {
                s->dist[v] = nd;
                s->parent[v] = u;
                if (s->pos[v] < 0)
                    nav_heap_push(s, v);
                else
                    nav_sift_up(s, (uint32_t)s->pos[v]);
            }
        }
    }

    if (s->dist[target] == UINT64_MAX || s->dist[src] != 0)
        return -1;

    /* Verify: walk the parent chain back to the source, re-summing the
     * travel times edge by edge. A correct search reproduces exactly the
     * settled distance; a corrupt parent or a wrong key does not. */
    uint64_t sum = 0;
    uint32_t cur = target, hops = 0;

    while (cur != src) {
        uint32_t p = s->parent[cur];
        int found = 0;

        if (p >= n)
            return -1;
        for (uint32_t e = g->off[p]; e < g->off[p + 1]; e++) {
            if (g->dst[e] == cur) {
                sum += g->w[e];
                found = 1;
                break;
            }
        }
        if (!found || ++hops > n)
            return -1;
        cur = p;
    }
    if (sum != s->dist[target])
        return -1;

    /* Straight-line lower bound: no road is faster than 3 time units per
     * grid unit (highways run 4 x 0.8 = 3.2, rounded down for safety), so
     * a "shorter" reported route means a broken search. */
    uint64_t d2 = grid_d2(g->side, src, target);
    uint64_t lb = (uint64_t)(3.0 * sqrt((double)d2));

    if (s->dist[target] < lb)
        return -1;
    return 0;
}

/* One thread's pass over its route slice. ops reset per run, so after
 * the last run the sum over threads is exactly one pass's edge count. */
static int nav_cycle(void *arg)
{
    nav_slice *s = arg;

    s->ops = 0;
    for (size_t r = s->route_begin; r < s->route_end; r++)
        if (nav_route(s, s->g->route_src[r], s->g->route_dst[r]) != 0)
            return -1;
    return 0;
}

int nav_run(nav_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_medges, uint64_t *edges_relaxed)
{
    unsigned t_count = *threads;

    if (t_count > NAV_ROUTES) /* one route per thread at most */
        t_count = NAV_ROUTES;

    nav_slice *slices = calloc(t_count, sizeof(*slices));
    void **ctxs = malloc(t_count * sizeof(*ctxs));
    double elapsed[MAX_RUNS], times[MAX_RUNS];
    int rc = -1;

    if (!slices || !ctxs)
        goto out;

    size_t per_route = NAV_ROUTES / t_count;
    unsigned extra = NAV_ROUTES % t_count;
    size_t begin = 0;

    for (unsigned t = 0; t < t_count; t++) {
        size_t count = per_route + (t < extra ? 1u : 0u);
        nav_slice *s = &slices[t];

        s->g = ctx;
        s->dist = malloc((size_t)ctx->nodes * sizeof(*s->dist));
        s->parent = malloc((size_t)ctx->nodes * sizeof(*s->parent));
        s->pos = malloc((size_t)ctx->nodes * sizeof(*s->pos));
        s->heap_id = malloc((size_t)ctx->nodes * sizeof(*s->heap_id));
        if (!s->dist || !s->parent || !s->pos || !s->heap_id)
            goto out;
        s->route_begin = begin;
        s->route_end = begin + count;
        begin += count;
        ctxs[t] = s;
    }

    rc = harness_run(t_count, runs, nav_cycle, ctxs, elapsed);
    if (rc == 0) {
        uint64_t ops = 0;

        for (unsigned t = 0; t < t_count; t++)
            ops += slices[t].ops;
        for (unsigned r = 0; r < runs; r++)
            times[r] = elapsed[r] > 0.0 ? (double)ops / elapsed[r] : -1.0;
        *throughput_medges = lb_median(times, runs) / 1e6;
        *threads = t_count;
        *edges_relaxed = ops;
    }

out:
    if (slices)
        for (unsigned t = 0; t < t_count; t++) {
            free(slices[t].dist);
            free(slices[t].parent);
            free(slices[t].pos);
            free(slices[t].heap_id);
        }
    free(slices);
    free(ctxs);
    return rc;
}
