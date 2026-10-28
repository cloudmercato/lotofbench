/* LotofBench tests — workload #2, Navigation (workload_navigation.[ch]).
 *
 * Determinism (identical graph and routes for a seed — the published
 * comparable-scores promise), graph invariants (CSR consistency,
 * bidirectionality, route band), the published default-seed edge count,
 * thread-count independence of that count — and the centerpiece: a
 * differential test of the workload's early-exit Dijkstra against a
 * full, independent reference implementation on every route. */

#include "tests.h"

#include "lotofbench.h"
#include "workload_navigation.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static nav_ctx *build(unsigned seed)
{
    nav_ctx *g = nav_prepare(seed);

    if (!g)
        test_fail(__FILE__, __LINE__, "nav_prepare(%u) failed", seed);
    return g;
}

static uint64_t grid_d2(uint32_t a, uint32_t b)
{
    int64_t dx = (int64_t)(a % NAV_GRID_SIDE) -
                 (int64_t)(b % NAV_GRID_SIDE);
    int64_t dy = (int64_t)(a / NAV_GRID_SIDE) - (int64_t)(b / NAV_GRID_SIDE);

    return (uint64_t)(dx * dx + dy * dy);
}

/* --- independent reference search -------------------------------------------*/

typedef struct {
    uint64_t d;
    uint32_t v;
} ref_entry;

typedef struct {
    ref_entry *a;
    size_t n, cap;
} ref_heap;

static void ref_heap_push(ref_heap *h, uint64_t d, uint32_t v)
{
    size_t i;

    if (h->n == h->cap) {
        h->cap = h->cap ? h->cap * 2 : 1024;
        h->a = realloc(h->a, h->cap * sizeof(*h->a));
        if (!h->a)
            abort();
    }
    h->a[i = h->n++] = (ref_entry){ d, v };
    while (i > 0) {
        size_t p = (i - 1) / 2;

        if (h->a[p].d <= h->a[i].d)
            break;
        ref_entry t = h->a[p];
        h->a[p] = h->a[i];
        h->a[i] = t;
        i = p;
    }
}

static ref_entry ref_heap_pop(ref_heap *h)
{
    ref_entry root = h->a[0];

    h->a[0] = h->a[--h->n];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, m = i;

        if (l < h->n && h->a[l].d < h->a[m].d)
            m = l;
        if (r < h->n && h->a[r].d < h->a[m].d)
            m = r;
        if (m == i)
            break;
        ref_entry t = h->a[m];
        h->a[m] = h->a[i];
        h->a[i] = t;
        i = m;
    }
    return root;
}

/* Full Dijkstra without early exit, lazy deletion, its own heap code:
 * independent of the workload's indexed heap, early exit and
 * verification chain. Returns the final distance to target. */
static uint64_t ref_dijkstra(const nav_ctx *g, uint32_t src, uint32_t target)
{
    uint32_t n = nav_nodes(g);
    const uint32_t *off = nav_offsets(g);
    const uint32_t *dst = nav_dests(g);
    const uint32_t *w = nav_weights(g);
    uint64_t *dist = malloc((size_t)n * sizeof(*dist));
    ref_heap h = { NULL, 0, 0 };

    memset(dist, 0xFF, (size_t)n * sizeof(*dist)); /* UINT64_MAX */
    dist[src] = 0;
    ref_heap_push(&h, 0, src);
    while (h.n) {
        ref_entry e = ref_heap_pop(&h);

        if (e.d > dist[e.v])
            continue; /* stale lazy entry */
        for (uint32_t j = off[e.v]; j < off[e.v + 1]; j++) {
            uint64_t nd = e.d + w[j];

            if (nd < dist[dst[j]]) {
                dist[dst[j]] = nd;
                ref_heap_push(&h, nd, dst[j]);
            }
        }
    }
    uint64_t out = dist[target];

    free(dist);
    free(h.a);
    return out;
}

/* --- tests --------------------------------------------------------------------*/

void test_nav_graph_deterministic(void)
{
    nav_ctx *a = build(7), *b = build(7), *c = build(8);

    if (!a || !b || !c) {
        nav_free(a);
        nav_free(b);
        nav_free(c);
        return;
    }
    CHECK_U64(nav_nodes(a), NAV_GRID_SIDE * NAV_GRID_SIDE);
    CHECK_U64(nav_edge_count(a), nav_edge_count(b));
    size_t edges = nav_edge_count(a);

    CHECK(memcmp(nav_offsets(a), nav_offsets(b),
                 (size_t)(nav_nodes(a) + 1) * sizeof(uint32_t)) == 0);
    CHECK(memcmp(nav_dests(a), nav_dests(b),
                 edges * sizeof(uint32_t)) == 0);
    CHECK(memcmp(nav_weights(a), nav_weights(b),
                 edges * sizeof(uint32_t)) == 0);
    /* A different seed produces a different network. */
    CHECK(memcmp(nav_weights(a), nav_weights(c),
                 edges * sizeof(uint32_t)) != 0);
    for (unsigned i = 0; i < NAV_ROUTES; i++) {
        uint32_t s1, d1, s2, d2;

        if (nav_route_pair(a, i, &s1, &d1) != 0 ||
            nav_route_pair(b, i, &s2, &d2) != 0) {
            test_fail(__FILE__, __LINE__, "nav_route_pair(%u) failed", i);
            break;
        }
        if (s1 != s2 || d1 != d2) {
            test_fail(__FILE__, __LINE__, "route %u differs between runs",
                      i);
            break;
        }
    }
    nav_free(a);
    nav_free(b);
    nav_free(c);
}

void test_nav_csr_invariants(void)
{
    nav_ctx *g = build(7);

    if (!g)
        return;
    uint32_t n = nav_nodes(g), edges = nav_edge_count(g);
    const uint32_t *off = nav_offsets(g);
    const uint32_t *dst = nav_dests(g);
    const uint32_t *w = nav_weights(g);
    int bad = 0;

    CHECK_U64(off[0], 0);
    CHECK_U64(off[n], edges);

    for (uint32_t i = 0; i < n && !bad; i++)
        if (off[i + 1] < off[i])
            bad = 1;
    CHECK(!bad); /* offsets are non-decreasing */

    for (uint32_t e = 0; e < edges && !bad; e++)
        if (dst[e] >= n || w[e] == 0)
            bad = 1;
    CHECK(!bad); /* targets in range, positive travel times */

    /* Bidirectionality: every road exists both ways, same time. */
    for (uint32_t u = 0; u < n && !bad; u++)
        for (uint32_t e = off[u]; e < off[u + 1] && !bad; e++) {
            uint32_t v = dst[e];
            int found = 0;

            for (uint32_t j = off[v]; j < off[v + 1]; j++)
                if (dst[j] == u && w[j] == w[e]) {
                    found = 1;
                    break;
                }
            if (!found)
                bad = 1;
        }
    CHECK(!bad);
    nav_free(g);
}

void test_nav_route_band(void)
{
    /* Documented input property: every route's straight-line distance
     * sits in the 300..700 band (and no route is degenerate). */
    nav_ctx *g = build(7);

    if (!g)
        return;
    for (unsigned i = 0; i < NAV_ROUTES; i++) {
        uint32_t s, d;

        if (nav_route_pair(g, i, &s, &d) != 0) {
            test_fail(__FILE__, __LINE__, "nav_route_pair(%u) failed", i);
            break;
        }
        uint64_t d2 = grid_d2(s, d);

        if (s == d || d2 < (uint64_t)300 * 300 ||
            d2 > (uint64_t)700 * 700) {
            test_fail(__FILE__, __LINE__,
                      "route %u outside the distance band", i);
            break;
        }
    }
    nav_free(g);
}

void test_nav_differential_dijkstra(void)
{
    /* The strongest available correctness check: on every route, the
     * workload's own search must return exactly the distance a full,
     * independent Dijkstra computes. Pins the early exit, the decrease-
     * key heap and the parent-chain verification in one shot. */
    nav_ctx *g = build(7);

    if (!g)
        return;
    for (unsigned i = 0; i < NAV_ROUTES; i++) {
        uint32_t s, d;
        uint64_t got = 0, want;

        if (nav_route_pair(g, i, &s, &d) != 0) {
            test_fail(__FILE__, __LINE__, "nav_route_pair(%u) failed", i);
            break;
        }
        want = ref_dijkstra(g, s, d);
        if (nav_route_time(g, i, &got) != 0) {
            test_fail(__FILE__, __LINE__, "route %u failed verification",
                      i);
            break;
        }
        if (got != want) {
            test_fail(__FILE__, __LINE__,
                      "route %u: workload %llu, reference %llu", i,
                      (unsigned long long)got, (unsigned long long)want);
            break;
        }
    }
    nav_free(g);
}

void test_nav_edges_thread_independent(void)
{
    /* The edge-relaxation count is a property of the input, not of the
     * threading: same seed, same count at any thread count. The value
     * is the published default-seed figure (docs/benchmarks.md). */
    nav_ctx *g = build(1);
    unsigned t1 = 1, t4 = 4;
    double a, b;
    uint64_t ops1 = 0, ops4 = 0;

    if (!g)
        return;
    CHECK(nav_run(g, 1, &t1, &a, &ops1) == 0);
    CHECK(nav_run(g, 1, &t4, &b, &ops4) == 0);
    CHECK_U64(ops1, 52954982ull);
    CHECK_U64(ops1, ops4);
    nav_free(g);
}

void test_nav_thread_clamp(void)
{
    /* More threads than routes is pointless: the request is clamped to
     * the route count. */
    nav_ctx *g = build(7);
    unsigned t = 1000;
    double tp;
    uint64_t ops = 0;

    if (!g)
        return;
    CHECK(nav_run(g, 1, &t, &tp, &ops) == 0);
    CHECK_U64(t, NAV_ROUTES);
    nav_free(g);
}

void test_nav_route_time_guards(void)
{
    nav_ctx *g = build(7);
    uint64_t t = 0;

    CHECK(nav_route_time(NULL, 0, &t) == -1);
    if (!g)
        return;
    CHECK(nav_route_time(g, NAV_ROUTES, &t) == -1);
    CHECK(nav_route_time(g, NAV_ROUTES + 100, &t) == -1);
    CHECK(nav_route_time(g, 0, &t) == 0);
    CHECK(t > 0);
    nav_free(g);
}

void test_nav_run_sane(void)
{
    nav_ctx *g = build(7);
    unsigned t = 2;
    double tp = 0.0;
    uint64_t ops = 0;

    if (!g)
        return;
    CHECK(nav_run(g, 1, &t, &tp, &ops) == 0);
    CHECK(tp > 0.5 && tp < 100000.0);
    CHECK(ops > 0);
    nav_free(g);
}
