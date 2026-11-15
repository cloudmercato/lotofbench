/* LotofBench tests — the hardware/system report (sysinfo.[ch]).
 *
 * The capture is best-effort by design, so the tests pin the invariants
 * that must hold on every supported platform (Linux/macOS, x86/ARM):
 * the identity fields are non-empty, the topology counts are sane and
 * RAM is known. They run on the machine building them — no goldens. */

#include "tests.h"

#include "sysinfo.h"

void test_sysinfo_sane(void)
{
    lb_sysinfo s;

    lb_sysinfo_capture(&s);

    /* identity fields are always present */
    CHECK(s.os[0] != 0);
    CHECK(s.kernel[0] != 0);
    CHECK(s.machine[0] != 0);
    CHECK(s.arch[0] != 0);
    CHECK(s.compiler[0] != 0);
    CHECK(s.brand[0] != 0);

    /* topology: at least one of everything, cores never above threads */
    CHECK_U64(s.sockets >= 1, 1);
    CHECK_U64(s.cores >= 1, 1);
    CHECK_U64(s.threads >= 1, 1);
    CHECK(s.cores <= s.threads);

    /* RAM is knowable on every supported platform */
    CHECK(s.ram_total > 0);

    /* numactl-style report: at least one node */
    CHECK_U64(s.numa_nodes >= 1, 1);

    /* load average is captured or explicitly unknown */
    CHECK(s.load_1m >= 0.0);
}

void test_sysinfo_repeatable(void)
{
    lb_sysinfo a, b;

    lb_sysinfo_capture(&a);
    lb_sysinfo_capture(&b);

    /* the static fields do not drift between two captures */
    CHECK_STR(a.arch, b.arch);
    CHECK_STR(a.os, b.os);
    CHECK_STR(a.kernel, b.kernel);
    CHECK_STR(a.brand, b.brand);
    CHECK_U64(a.threads, b.threads);
    CHECK_U64(a.sockets, b.sockets);
    CHECK_U64(a.cores, b.cores);
    CHECK_U64(a.ram_total, b.ram_total);
}
