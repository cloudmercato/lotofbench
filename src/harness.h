#ifndef HARNESS_H
#define HARNESS_H

#include <stddef.h>

#include "lotofbench.h"

/* Shared run harness: Geekbench 6's multi-core "shared task" model.
 *
 * Threads cooperate on one task, each owning an equal slice, and the
 * reported time covers the task as a whole. Runs are synchronised through
 * a start gate — every thread must be ready before the clock starts, and
 * the clock stops when the last thread finishes — so elapsed time
 * measures the task, not each thread's own wake-up jitter. Workloads
 * provide a cycle() callback that executes one slice; this module owns
 * the gating, timing and medians. */

/* Execute one slice of one run. Returns 0 on success, -1 if any result
 * failed verification (the whole run then fails). */
typedef int (*lb_cycle_fn)(void *ctx);

double now_sec(void);

/* Median of a small array (the values are reordered in place). */
double lb_median(double *values, size_t n);

/* The Geekbench scoring formula: score = throughput / baseline * 1000. */
long lb_score(double throughput, double baseline);

/* Run `runs` synchronised runs of `cycle` over `threads` pre-built
 * contexts. On success fills elapsed[0..runs) with the per-run wall-clock
 * seconds (gate-open to last-thread-out) and returns 0. Returns -1 if
 * thread creation failed or any cycle reported a verification failure. */
int harness_run(unsigned threads, unsigned runs, lb_cycle_fn cycle,
                void **ctxs, double *elapsed);

#endif /* HARNESS_H */
