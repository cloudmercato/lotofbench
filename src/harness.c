/* LotofBench — shared run harness.
 *
 * Geekbench 6's multi-core model is a shared task: threads cooperate on
 * one task, each owning an equal slice, and the score measures the task
 * as a whole. Runs are synchronised through a start gate — every thread
 * must be ready before the clock starts, and the clock stops when the
 * last thread finishes — so elapsed time measures the whole task and not
 * each thread's own wake-up jitter. */

#define _POSIX_C_SOURCE 200809L

#include "harness.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

double now_sec(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int double_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;

    return (x > y) - (x < y);
}

double lb_median(double *values, size_t n)
{
    qsort(values, n, sizeof(double), double_cmp);
    if (n % 2)
        return values[n / 2];
    return (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

long lb_score(double throughput, double baseline)
{
    return (long)(throughput / baseline * 1000.0 + 0.5);
}

double lb_geomean(const long *scores, size_t n)
{
    double sum_log = 0.0;

    if (n == 0)
        return 0.0;
    for (size_t i = 0; i < n; i++) {
        if (scores[i] <= 0)
            return 0.0;
        sum_log += log((double)scores[i]);
    }
    return exp(sum_log / (double)n);
}

long lb_section(const long *scores, size_t n)
{
    return (long)(lb_geomean(scores, n) + 0.5);
}

long lb_composite(long integer_score, long floating_score)
{
    double composite = LB_INTEGER_WEIGHT * (double)integer_score +
                       LB_FLOATING_WEIGHT * (double)floating_score;

    return (long)(composite + 0.5);
}

typedef struct {
    lb_cycle_fn cycle;
    void *ctx;
    unsigned runs;
    int failed;
} hjob;

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cv = PTHREAD_COND_INITIALIZER;
static unsigned live_threads = 0;
static unsigned ready_count = 0, done_count = 0, run_index = 0;
static double run_t0[MAX_RUNS], run_t1[MAX_RUNS];

static void *worker(void *arg)
{
    hjob *j = arg;

    for (unsigned r = 0; r < j->runs; r++) {
        pthread_mutex_lock(&sync_lock);
        while (run_index < r) /* next run opens when the previous closes */
            pthread_cond_wait(&sync_cv, &sync_lock);
        if (++ready_count == live_threads) {
            run_t0[r] = now_sec(); /* start gate: all threads ready */
            pthread_cond_broadcast(&sync_cv);
        } else {
            while (ready_count < live_threads)
                pthread_cond_wait(&sync_cv, &sync_lock);
        }
        pthread_mutex_unlock(&sync_lock);

        int ok = j->cycle(j->ctx) == 0;

        pthread_mutex_lock(&sync_lock);
        if (!ok)
            j->failed = 1;
        if (++done_count == live_threads) {
            run_t1[r] = now_sec(); /* last thread out closes the run */
            done_count = 0;
            ready_count = 0;
            run_index++;
            pthread_cond_broadcast(&sync_cv);
        } else {
            while (run_index == r)
                pthread_cond_wait(&sync_cv, &sync_lock);
        }
        pthread_mutex_unlock(&sync_lock);
    }
    return NULL;
}

int harness_run(unsigned threads, unsigned runs, lb_cycle_fn cycle,
                void **ctxs, double *elapsed)
{
    hjob *jobs = calloc(threads, sizeof(*jobs));
    pthread_t *tids = malloc(threads * sizeof(*tids));
    unsigned created = 0;
    int rc = 0;

    live_threads = threads;
    ready_count = 0;
    done_count = 0;
    run_index = 0;

    if (!jobs || !tids) {
        free(tids);
        free(jobs);
        return -1;
    }

    for (unsigned t = 0; t < threads; t++) {
        jobs[t].cycle = cycle;
        jobs[t].ctx = ctxs[t];
        jobs[t].runs = runs;
        jobs[t].failed = 0;
        if (pthread_create(&tids[t], NULL, worker, &jobs[t]) != 0) {
            fprintf(stderr, "lotofbench: cannot start thread %u\n", t);
            rc = -1;
            break;
        }
        created++;
    }
    for (unsigned t = 0; t < created; t++)
        pthread_join(tids[t], NULL);

    for (unsigned t = 0; t < threads; t++)
        if (jobs[t].failed)
            rc = -1;

    if (rc == 0)
        for (unsigned r = 0; r < runs; r++)
            elapsed[r] = run_t1[r] - run_t0[r];

    free(tids);
    free(jobs);
    return rc;
}
