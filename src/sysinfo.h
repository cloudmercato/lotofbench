#ifndef SYSINFO_H
#define SYSINFO_H

#include <stddef.h>

/* LotofBench — best-effort hardware/system report.
 *
 * The machine context a benchmark score needs to be read correctly:
 * CPU identity and topology (sockets, cores, threads, frequency,
 * caches), memory and swap, NUMA layout, OS/kernel and the compiler
 * that built the binary. Works on Linux and macOS, x86 and ARM.
 *
 * Every field is best-effort: a value the platform does not expose
 * stays "" (strings), 0 (counts) or a negative load (load average).
 * Capture never fails and never touches the benchmark clock. */

#define LB_SYSINFO_BRAND_MAX 128
#define LB_SYSINFO_MAP_MAX   256

typedef struct {
    /* system */
    char os[32];         /* uname sysname: "Linux", "Darwin" */
    char kernel[64];     /* uname release */
    char machine[32];    /* uname machine */
    char arch[32];       /* compile-time target: x86_64, aarch64, arm */
    char compiler[64];   /* "gcc 12.2.0", "clang 15.0.0" */
    double load_1m;      /* 1-minute load average; -1 = unknown */
    /* cpu */
    char brand[LB_SYSINFO_BRAND_MAX]; /* CPUID brand string / model name */
    char vendor[32];     /* x86 vendor id ("GenuineIntel"...), "" elsewhere */
    unsigned family, model, stepping; /* x86 CPUID leaves, 0 elsewhere */
    unsigned sockets;    /* physical packages, 0 = unknown */
    unsigned cores;      /* physical cores, 0 = unknown */
    unsigned threads;    /* logical CPUs, 0 = unknown */
    char core_layout[64]; /* big.LITTLE annotation: "8P+2E" (Apple), "" */
    double freq_mhz;     /* best-effort max/base frequency, 0 = unknown */
    unsigned long l2_kb; /* 0 = unknown */
    unsigned long l3_kb; /* 0 = unknown */
    /* memory (bytes, 0 = unknown) */
    unsigned long long ram_total, ram_free;
    unsigned long long swap_total, swap_free;
    /* numa */
    unsigned numa_nodes; /* >= 1 when known, 0 = unknown */
    char numa_map[LB_SYSINFO_MAP_MAX]; /* "node0: 0-23 (32768 MB); ..." */
} lb_sysinfo;

/* Capture the machine report. Idempotent and side-effect free. */
void lb_sysinfo_capture(lb_sysinfo *out);

#endif /* SYSINFO_H */
