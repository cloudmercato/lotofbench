/* LotofBench — suite driver command line.
 *
 * Owns the argv parsing of main.c. Every numeric flag goes through one
 * strict parser (lb_parse_uint), so malformed values ("12abc", "-4",
 * overflow past the type) are rejected with a diagnostic instead of
 * being silently truncated or wrapped like strtoul would do. */

#define _POSIX_C_SOURCE 200809L
/* macOS gates _SC_NPROCESSORS_ONLN (and friends) behind the Darwin
 * extensions: a strict _POSIX_C_SOURCE alone hides it from <unistd.h>.
 * Re-expose them so the one-command build works on macOS. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 200809L
#endif

#include "cli.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lotofbench.h"
#include "upload/upload.h"

int lb_parse_uint(const char *s, unsigned *out)
{
    unsigned long acc = 0;

    if (!s || !*s)
        return -1;
    for (const char *p = s; *p; p++) {
        unsigned long d;

        if (*p < '0' || *p > '9') /* signs, spaces, trailing junk */
            return -1;
        d = (unsigned long)(*p - '0');
        if (acc > (ULONG_MAX - d) / 10)
            return -1; /* multiplying by 10 would overflow */
        acc = acc * 10 + d;
    }
    if (acc > (unsigned long)UINT_MAX)
        return -1;
    *out = (unsigned)acc;
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out,
            "usage: lotofbench [--size-mb N] [--runs N] [--threads N] "
            "[--seed N] [--json] [--json-out PATH] "
            "[--upload | --no-upload]\n");
}

/* One numeric flag value: parse strictly or exit(2) with a diagnostic. */
static unsigned parse_uint_arg(const char *flag, const char *value)
{
    unsigned v;

    if (lb_parse_uint(value, &v) != 0) {
        fprintf(stderr,
                "lotofbench: %s: invalid value '%s' (unsigned integer "
                "expected)\n",
                flag, value);
        usage(stderr);
        exit(2);
    }
    return v;
}

options parse_args(int argc, char **argv)
{
    options o = { DEFAULT_CORPUS_MB, DEFAULT_RUNS, DEFAULT_SEED, 0, 0,
              NULL, UPLOAD_DEFAULT_ENABLED };
    long nproc = sysconf(_SC_NPROCESSORS_ONLN);

    if (nproc < 1)
        nproc = 1;
    o.threads = (unsigned)nproc;
    if (o.threads > DEFAULT_THREADS_CAP)
        o.threads = DEFAULT_THREADS_CAP;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size-mb") && i + 1 < argc) {
            const char *flag = argv[i];

            o.corpus_mb = parse_uint_arg(flag, argv[++i]);
        } else if (!strcmp(argv[i], "--runs") && i + 1 < argc) {
            const char *flag = argv[i];

            o.runs = parse_uint_arg(flag, argv[++i]);
        } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            const char *flag = argv[i];

            o.threads = parse_uint_arg(flag, argv[++i]);
        } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
            const char *flag = argv[i];

            o.seed = parse_uint_arg(flag, argv[++i]);
        } else if (!strcmp(argv[i], "--json")) {
            o.json = 1;
        } else if (!strcmp(argv[i], "--json-out") && i + 1 < argc) {
            o.json_out = argv[++i];
        } else if (!strcmp(argv[i], "--upload")) {
#ifdef HAVE_CURL
            o.upload = 1;
#else
            fprintf(stderr,
                    "lotofbench: --upload: this binary was built without "
                    "libcurl support\n");
            usage(stderr);
            exit(2);
#endif
        } else if (!strcmp(argv[i], "--no-upload")) {
            /* Short-circuit everything network: no session handshake,
             * no proof of work, no submit. The offline contract
             * (zero sockets) and the CloudBeast play
             * (cs_web/player/playbooks/run_lotofbench.yml) both rely
             * on it. */
            o.upload = 0;
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
    if (o.runs < 1 || o.runs > MAX_RUNS) {
        fprintf(stderr, "lotofbench: --runs must be between 1 and %u\n",
                MAX_RUNS);
        exit(2);
    }
    if (o.corpus_mb < 1)
        o.corpus_mb = 1;
    if (o.threads < 1)
        o.threads = 1;
    return o;
}
