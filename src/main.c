/* LotofBench — suite driver.
 *
 * Runs every implemented workload single-core and multi-core, in the
 * order of the Geekbench 6 CPU workload list (docs/benchmarks.md), and
 * prints one calibrated score per workload. The per-workload machinery
 * lives in workload_*.c; the shared-task threading and timing in
 * harness.c; this file owns the CLI and the output. */

#define _POSIX_C_SOURCE 200809L

#include "harness.h"
#include "lotofbench.h"
#include "workload_clang.h"
#include "workload_compression.h"
#include "workload_html5browser.h"
#include "workload_navigation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    unsigned corpus_mb, runs, seed, threads; /* requested thread count */
    int json;
} options;

static void usage(FILE *out)
{
    fprintf(out,
            "usage: lotofbench [--size-mb N] [--runs N] [--threads N] "
            "[--seed N] [--json]\n");
}

static options parse_args(int argc, char **argv)
{
    options o = { DEFAULT_CORPUS_MB, DEFAULT_RUNS, DEFAULT_SEED, 0, 0 };
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    if (nproc < 1)
        nproc = 1;
    o.threads = (unsigned)nproc;
    if (o.threads > DEFAULT_THREADS_CAP)
        o.threads = DEFAULT_THREADS_CAP;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) {
            o.corpus_mb = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--runs") && i + 1 < argc) {
            o.runs = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            o.threads = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            o.seed = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if (!strcmp(argv[i], "--json")) {
            o.json = 1;
        } else if (!strcmp(argv[i], "--help")) {
            usage(stdout);
            exit(0);
        } else {
            fprintf(stderr, "lotofbench: bad or incomplete option '%s'\n",
                    argv[i]);
            usage(stderr);
            exit(2);
        }
    }
    if (o.corpus_mb < 1)
        o.corpus_mb = 1;
    if (o.runs < 1 || o.runs > MAX_RUNS) {
        fprintf(stderr, "lotofbench: --runs must be between 1 and %u\n",
                MAX_RUNS);
        exit(2);
    }
    if (o.threads < 1)
        o.threads = 1;
    return o;
}

/* --- running the suite ------------------------------------------------------*/

static int run_fc(fc_ctx *fc, unsigned runs, unsigned threads_req,
                  lb_result *out)
{
    unsigned t = threads_req;

    if (fc_run(fc, runs, &t, &out->throughput) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_FILECOMPRESSION_MBPS);
    return 0;
}

static int run_nav(nav_ctx *nav, unsigned runs, unsigned threads_req,
                   lb_result *out, uint64_t *edges_relaxed)
{
    unsigned t = threads_req;

    if (nav_run(nav, runs, &t, &out->throughput, edges_relaxed) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_NAVIGATION_MEDGES);
    return 0;
}

static int run_h5(html5_ctx *h5, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *pages_rendered)
{
    unsigned t = threads_req;

    if (html5_run(h5, runs, &t, &out->throughput, pages_rendered) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_HTML5BROWSER_PPS);
    return 0;
}

static int run_cl(cl_ctx *cl, unsigned runs, unsigned threads_req,
                  lb_result *out, uint64_t *tokens)
{
    unsigned t = threads_req;

    if (cl_run(cl, runs, &t, &out->throughput, tokens) != 0)
        return -1;
    out->threads = t;
    out->score = lb_score(out->throughput, BASELINE_CLANG_KTOKENS);
    return 0;
}

/* --- output ------------------------------------------------------------------*/

static void print_block(const char *unit, double baseline,
                        const lb_result *single, const lb_result *multi)
{
    printf("Single-Core\n");
    printf("  throughput  %9.1f %s\n", single->throughput, unit);
    printf("  score       %9ld   (baseline %.1f %s = 1000)\n\n",
           single->score, baseline, unit);
    printf("Multi-Core (%u threads, shared task)\n", multi->threads);
    printf("  throughput  %9.1f %s\n", multi->throughput, unit);
    printf("  score       %9ld\n", multi->score);
}

static void print_json(const options *opt, const lb_result *fc_single,
                       const lb_result *fc_multi,
                       const lb_result *nav_single,
                       const lb_result *nav_multi, uint64_t nav_ops,
                       const lb_result *h5_single,
                       const lb_result *h5_multi, uint64_t h5_pages,
                       uint64_t h5_elements, const lb_result *cl_single,
                       const lb_result *cl_multi, uint64_t cl_tokens)
{
    printf("{\n");
    printf("  \"benchmark\": \"lotofbench\",\n");
    printf("  \"version\": \"%s\",\n", LOTOFBENCH_VERSION);
    printf("  \"seed\": %u,\n", opt->seed);
    printf("  \"runs\": %u,\n", opt->runs);
    printf("  \"workloads\": [\n");
    printf("    {\"name\": \"%s\", \"corpus_mb\": %u, "
           "\"baseline_mbps\": %.1f,\n",
           FC_WORKLOAD_SLUG, opt->corpus_mb, BASELINE_FILECOMPRESSION_MBPS);
    printf("     \"single\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld},\n",
           fc_single->threads, fc_single->throughput, fc_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_mbps\": %.1f, "
           "\"score\": %ld}},\n",
           fc_multi->threads, fc_multi->throughput, fc_multi->score);
    printf("    {\"name\": \"%s\", \"grid_side\": %u, \"routes\": %u, "
           "\"edges_relaxed\": %llu, \"baseline_medges\": %.1f,\n",
           NAV_WORKLOAD_SLUG, NAV_GRID_SIDE, NAV_ROUTES,
           (unsigned long long)nav_ops, BASELINE_NAVIGATION_MEDGES);
    printf("     \"single\": {\"threads\": %u, \"throughput_medges\": %.1f, "
           "\"score\": %ld},\n",
           nav_single->threads, nav_single->throughput, nav_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_medges\": %.1f, "
           "\"score\": %ld}},\n",
           nav_multi->threads, nav_multi->throughput, nav_multi->score);
    printf("    {\"name\": \"%s\", \"pages\": %u, \"view\": \"%ux%u\", "
           "\"elements\": %llu, \"pages_rendered\": %llu, "
           "\"baseline_pps\": %.1f,\n",
           HTML5_WORKLOAD_SLUG, HTML5_PAGES, HTML5_VIEW_W, HTML5_VIEW_H,
           (unsigned long long)h5_elements,
           (unsigned long long)h5_pages, BASELINE_HTML5BROWSER_PPS);
    printf("     \"single\": {\"threads\": %u, \"throughput_pps\": %.1f, "
           "\"score\": %ld},\n",
           h5_single->threads, h5_single->throughput, h5_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_pps\": %.1f, "
           "\"score\": %ld}},\n",
           h5_multi->threads, h5_multi->throughput, h5_multi->score);
    printf("    {\"name\": \"%s\", \"units\": %u, \"tokens\": %llu, "
           "\"baseline_ktokens\": %.1f,\n",
           CL_WORKLOAD_SLUG, CLANG_UNITS, (unsigned long long)cl_tokens,
           BASELINE_CLANG_KTOKENS);
    printf("     \"single\": {\"threads\": %u, \"throughput_ktokens\": %.1f, "
           "\"score\": %ld},\n",
           cl_single->threads, cl_single->throughput, cl_single->score);
    printf("     \"multi\": {\"threads\": %u, \"throughput_ktokens\": %.1f, "
           "\"score\": %ld}}\n",
           cl_multi->threads, cl_multi->throughput, cl_multi->score);
    printf("  ]\n");
    printf("}\n");
}

int main(int argc, char **argv)
{
    options opt = parse_args(argc, argv);
    fc_ctx *fc = fc_prepare(opt.corpus_mb, opt.seed);
    nav_ctx *nav = nav_prepare(opt.seed);
    html5_ctx *h5 = html5_prepare(opt.seed);
    cl_ctx *cl = cl_prepare(opt.seed);

    if (!fc || !nav || !h5 || !cl) {
        fprintf(stderr,
                "lotofbench: out of memory building the workload inputs\n");
        fc_free(fc);
        nav_free(nav);
        html5_free(h5);
        cl_free(cl);
        return 1;
    }

    lb_result fc_single = { 0, 0.0, 0 }, fc_multi = { 0, 0.0, 0 };
    lb_result nav_single = { 0, 0.0, 0 }, nav_multi = { 0, 0.0, 0 };
    lb_result h5_single = { 0, 0.0, 0 }, h5_multi = { 0, 0.0, 0 };
    lb_result cl_single = { 0, 0.0, 0 }, cl_multi = { 0, 0.0, 0 };
    uint64_t nav_ops = 0, h5_pages = 0, cl_tokens = 0;

    if (run_fc(fc, opt.runs, 1, &fc_single) != 0 ||
        run_fc(fc, opt.runs, opt.threads, &fc_multi) != 0 ||
        run_nav(nav, opt.runs, 1, &nav_single, &nav_ops) != 0 ||
        run_nav(nav, opt.runs, opt.threads, &nav_multi, &nav_ops) != 0 ||
        run_h5(h5, opt.runs, 1, &h5_single, &h5_pages) != 0 ||
        run_h5(h5, opt.runs, opt.threads, &h5_multi, &h5_pages) != 0 ||
        run_cl(cl, opt.runs, 1, &cl_single, &cl_tokens) != 0 ||
        run_cl(cl, opt.runs, opt.threads, &cl_multi, &cl_tokens) != 0) {
        fprintf(stderr,
                "lotofbench: workload failed — a result did not verify\n");
        fc_free(fc);
        nav_free(nav);
        html5_free(h5);
        cl_free(cl);
        return 1;
    }

    if (opt.json) {
        print_json(&opt, &fc_single, &fc_multi, &nav_single, &nav_multi,
                   nav_ops, &h5_single, &h5_multi, h5_pages,
                   html5_elements(h5), &cl_single, &cl_multi, cl_tokens);
    } else {
        char fc_cfg[64], nav_cfg[64], h5_cfg[64], cl_cfg[64];

        snprintf(fc_cfg, sizeof(fc_cfg), "corpus %u MB synthetic",
                 opt.corpus_mb);
        snprintf(nav_cfg, sizeof(nav_cfg), "grid %ux%u, %u routes",
                 NAV_GRID_SIDE, NAV_GRID_SIDE, NAV_ROUTES);
        snprintf(h5_cfg, sizeof(h5_cfg), "%u pages, %llux%llu viewport",
                 HTML5_PAGES, (unsigned long long)HTML5_VIEW_W,
                 (unsigned long long)HTML5_VIEW_H);
        snprintf(cl_cfg, sizeof(cl_cfg), "%u units, %llu source tokens",
                 CLANG_UNITS, (unsigned long long)cl_tokens);

        printf("LotofBench %s\n", LOTOFBENCH_VERSION);
        printf("seed %u, %u runs, median\n\n", opt.seed, opt.runs);
        printf("%s (%s)\n", FC_WORKLOAD_NAME, fc_cfg);
        print_block("MB/s", BASELINE_FILECOMPRESSION_MBPS, &fc_single,
                    &fc_multi);
        printf("\n%s (%s)\n", NAV_WORKLOAD_NAME, nav_cfg);
        print_block("Medge/s", BASELINE_NAVIGATION_MEDGES, &nav_single,
                    &nav_multi);
        printf("\n%s (%s, %llu DOM elements)\n", HTML5_WORKLOAD_NAME,
               h5_cfg, (unsigned long long)html5_elements(h5));
        print_block("pages/s", BASELINE_HTML5BROWSER_PPS, &h5_single,
                    &h5_multi);
        printf("\n%s (%s)\n", CL_WORKLOAD_NAME, cl_cfg);
        print_block("Ktokens/s", BASELINE_CLANG_KTOKENS, &cl_single,
                    &cl_multi);
    }

    fc_free(fc);
    nav_free(nav);
    html5_free(h5);
    cl_free(cl);
    return 0;
}
