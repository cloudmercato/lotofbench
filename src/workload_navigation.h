#ifndef WORKLOAD_NAVIGATION_H
#define WORKLOAD_NAVIGATION_H

#include <stddef.h>
#include <stdint.h>

/* Workload #2 — Navigation.
 *
 * Mirrors the second Geekbench 6 CPU workload: Dijkstra route planning
 * over a road network. The graph, the routes and the timings are
 * documented in docs/benchmarks.md, section "Workload 2". */

typedef struct nav_ctx nav_ctx;

/* Fixed input size, printed by the driver and documented in
 * docs/benchmarks.md — Geekbench-style: the map is the map. */
#define NAV_GRID_SIDE  512u
#define NAV_ROUTES     64u

/* Build the road network and the route set (off the clock; fixed seed
 * => identical graph and routes on every machine). */
nav_ctx *nav_prepare(unsigned seed);
void     nav_free(nav_ctx *ctx);

/* One workload pass with *threads threads (clamped to the route count).
 * Fills *throughput_medges with the median over the runs, in millions of
 * edges relaxed per second, and *edges_relaxed with the exact edge count
 * of one pass (deterministic, identical for every run). Returns 0, or -1
 * if a route failed verification. */
int nav_run(nav_ctx *ctx, unsigned runs, unsigned *threads,
            double *throughput_medges, uint64_t *edges_relaxed);

#endif /* WORKLOAD_NAVIGATION_H */
