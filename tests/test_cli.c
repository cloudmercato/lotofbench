/* LotofBench tests — the CLI parser (cli.[ch]) upload flags. */

#include "tests.h"

#include "cli.h"
#include "upload/upload.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

void test_parse_uint_valid(void)
{
    unsigned v;

    CHECK(lb_parse_uint("0", &v) == 0);
    CHECK_U64(v, 0);
    CHECK(lb_parse_uint("12", &v) == 0);
    CHECK_U64(v, 12);
    CHECK(lb_parse_uint("00", &v) == 0);
    CHECK_U64(v, 0);
    CHECK(lb_parse_uint("00012", &v) == 0);
    CHECK_U64(v, 12);
    CHECK(lb_parse_uint("4294967295", &v) == 0); /* UINT_MAX */
    CHECK_U64(v, 4294967295u);
}

void test_parse_uint_invalid(void)
{
    unsigned v = 42;

    /* Every shape strtoul would silently mis-parse must be rejected
     * with the value left untouched. */
    const char *bad[] = { "", "abc", "12abc", "abc12", "-4", "+4", " 4",
                          "4 ", "1.5", "--", "99999999999999999999" };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(lb_parse_uint(bad[i], &v) == -1);
    CHECK_U64(v, 42); /* untouched on every rejection */
}

/* The CloudBeast play (run_lotofbench.yml) always invokes the binary
 * with --no-upload: the flag must be accepted and force the offline
 * contract (zero sockets) whatever the build. */
void test_parse_args_no_upload_noop(void)
{
    char *plain[] = { "lotofbench", NULL };
    char *flagged[] = { "lotofbench", "--no-upload", "--runs", "2",
                        "--json", NULL };
    options base, with_flag;

    base = parse_args(1, plain);
    with_flag = parse_args(5, flagged);

    /* Every other field matches a plain parse of the same defaults. */
    CHECK_U64(with_flag.corpus_mb, base.corpus_mb);
    CHECK_U64(with_flag.seed, base.seed);
    CHECK_U64(with_flag.threads, base.threads);
    /* The other flags on the same command line still apply. */
    CHECK_U64(with_flag.runs, 2);
    CHECK_U64(with_flag.json, 1);
    CHECK(with_flag.json_out == NULL);
    /* --no-upload always disables the upload. */
    CHECK_U64(with_flag.upload, 0);
}

void test_parse_args_upload_flags(void)
{
    char *plain[] = { "lotofbench", NULL };
    options base;

    /* The default is compile-time: upload on curl builds, offline on
     * the zero-dependency build. */
    base = parse_args(1, plain);
    CHECK_U64(base.upload, UPLOAD_DEFAULT_ENABLED);

#ifdef HAVE_CURL
    char *up[] = { "lotofbench", "--upload", NULL };

    CHECK_U64(parse_args(2, up).upload, 1);
#else
    /* --upload on a build without libcurl refuses loudly (exit 2);
     * the child's expected diagnostic is silenced. */
    pid_t pid = fork();

    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);

        if (devnull >= 0) {
            dup2(devnull, 2);
            close(devnull);
        }
        char *up[] = { (char *)"lotofbench", (char *)"--upload" };

        parse_args(2, up);
        _exit(0); /* not reached: parse_args exits 2 */
    }
    int st = 0;

    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st));
    CHECK_U64(WEXITSTATUS(st), 2);
#endif
}
