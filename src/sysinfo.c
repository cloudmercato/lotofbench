/* LotofBench — best-effort hardware/system report (sysinfo.[ch]).
 *
 * Sources, by platform:
 *   both     uname, sysconf(_SC_NPROCESSORS_ONLN), getloadavg
 *   both x86 cpuid.h — vendor, family/model/stepping, brand string,
 *            L2/L3 sizes (deterministic cache leaf)
 *   Linux    /proc/cpuinfo (brand, sockets, cores), /proc/meminfo
 *            (RAM/swap), /sys/devices/system/node (NUMA), cpufreq
 *   macOS    sysctl (hw.* topology, caches, RAM), Mach host
 *            statistics (free RAM), vm.swapusage, perf/efficiency
 *            core split
 *
 * Linux big.LITTLE exposes no per-core topology through /proc/cpuinfo,
 * so cores fall back to the logical count there; macOS reports the
 * P/E split through hw.perflevelN instead. */

#define _POSIX_C_SOURCE 200809L
/* getloadavg() is a BSD/misc extension glibc hides under a strict
 * _POSIX_C_SOURCE. */
#define _DEFAULT_SOURCE 200809L
/* macOS: the sysctl family and the Mach statistics. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 200809L
#endif

#include "sysinfo.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/utsname.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

/* --- small helpers ----------------------------------------------------------*/

static void set_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%.*s", (int)(cap - 1), src);
}

static void trim(char *s)
{
    size_t n;

    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                 s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

/* Append to a bounded, NUL-terminated buffer; silently stops at the cap. */
static void appendf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    size_t len = strlen(buf);

    if (len + 1 >= cap)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + len, cap - len, fmt, ap);
    va_end(ap);
}

/* --- shared /proc parsers (Linux) -------------------------------------------*/

/* First brand-like key wins: x86 "model name", ARM "Model Name" /
 * "Model" / "Hardware". Also counts unique sockets ("physical id")
 * and unique (socket, core) pairs. */
static void parse_cpuinfo(lb_sysinfo *si)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    char line[512];
    unsigned phys[64], pairs_phys[256], pairs_core[256];
    unsigned nphys = 0, npairs = 0, cur_phys = 0;
    int have_brand = si->brand[0] != 0;

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':'), *key, *val;
        unsigned id, core;

        if (!colon)
            continue;
        *colon = 0;
        key = line;
        val = colon + 1;
        trim(key);
        trim(val);
        if (!strcasecmp(key, "processor")) {
            cur_phys = 0; /* new cpu block: reset the pair tracking */
        } else if (!strcasecmp(key, "physical id") &&
                   sscanf(val, "%u", &id) == 1) {
            cur_phys = id;
            if (nphys < sizeof phys / sizeof phys[0]) {
                int seen = 0;
                for (unsigned i = 0; i < nphys; i++)
                    if (phys[i] == id)
                        seen = 1;
                if (!seen)
                    phys[nphys++] = id;
            }
        } else if (!strcasecmp(key, "core id") &&
                   sscanf(val, "%u", &core) == 1 &&
                   npairs < sizeof pairs_phys / sizeof pairs_phys[0]) {
            int seen = 0;
            for (unsigned i = 0; i < npairs; i++)
                if (pairs_phys[i] == cur_phys && pairs_core[i] == core)
                    seen = 1;
            if (!seen) {
                pairs_phys[npairs] = cur_phys;
                pairs_core[npairs] = core;
                npairs++;
            }
        } else if (!have_brand &&
                   (!strcasecmp(key, "model name") ||
                    !strcasecmp(key, "model") ||
                    !strcasecmp(key, "hardware")) &&
                   val[0]) {
            set_str(si->brand, sizeof si->brand, val);
            have_brand = 1;
        }
    }
    fclose(f);
    if (nphys)
        si->sockets = nphys;
    else if (npairs)
        si->sockets = 1;
    if (npairs)
        si->cores = npairs;
}

/* MemAvailable (or MemFree), MemTotal, SwapTotal, SwapFree — kB units. */
static void parse_meminfo(lb_sysinfo *si)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];

    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        unsigned long kb;

        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1 && !si->ram_total)
            si->ram_total = (unsigned long long)kb * 1024;
        else if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1)
            si->ram_free = (unsigned long long)kb * 1024;
        else if (sscanf(line, "MemFree: %lu kB", &kb) == 1 &&
                 !si->ram_free)
            si->ram_free = (unsigned long long)kb * 1024;
        else if (sscanf(line, "SwapTotal: %lu kB", &kb) == 1)
            si->swap_total = (unsigned long long)kb * 1024;
        else if (sscanf(line, "SwapFree: %lu kB", &kb) == 1)
            si->swap_free = (unsigned long long)kb * 1024;
    }
    fclose(f);
}

/* Best-effort max frequency: cpufreq first (kHz), then the per-cpu
 * "cpu MHz" of /proc/cpuinfo. VMs often expose neither. */
static void read_freq_linux(lb_sysinfo *si)
{
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
                    "r");
    unsigned long khz = 0;

    if (f) {
        if (fscanf(f, "%lu", &khz) == 1 && khz)
            si->freq_mhz = (double)khz / 1000.0;
        fclose(f);
    }
    if (si->freq_mhz == 0) {
        f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof line, f)) {
                unsigned long mhz;
                if (sscanf(line, "cpu MHz : %lu", &mhz) == 1 && mhz) {
                    si->freq_mhz = (double)mhz;
                    break;
                }
            }
            fclose(f);
        }
    }
}

/* numactl-style summary: node count plus "nodeN: <cpulist> (<MB>)" for
 * the first few nodes. Systems without sysfs nodes report one node. */
static void read_numa_linux(lb_sysinfo *si)
{
    DIR *d = opendir("/sys/devices/system/node");
    struct dirent *e;
    unsigned ids[64], count = 0;
    char base[96], path[160], list[128], line[256];

    if (!d) {
        si->numa_nodes = 1; /* single-node UMA assumption */
        return;
    }
    while ((e = readdir(d)) && count < sizeof ids / sizeof ids[0]) {
        unsigned n;
        if (sscanf(e->d_name, "node%u", &n) == 1)
            ids[count++] = n;
    }
    closedir(d);
    si->numa_nodes = count ? count : 1;

    /* sort the printed ids (readdir order is arbitrary) */
    for (unsigned i = 1; i < count; i++)
        for (unsigned j = i; j > 0 && ids[j - 1] > ids[j]; j--) {
            unsigned t = ids[j - 1];
            ids[j - 1] = ids[j];
            ids[j] = t;
        }

    for (unsigned i = 0; i < count && i < 8; i++) {
        FILE *f;
        unsigned long kb = 0;

        snprintf(base, sizeof base, "/sys/devices/system/node/node%u",
                 ids[i]);
        list[0] = 0;
        snprintf(path, sizeof path, "%s/cpulist", base);
        f = fopen(path, "r");
        if (f) {
            if (fgets(list, sizeof list, f))
                trim(list);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/meminfo", base);
        f = fopen(path, "r");
        if (f) {
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "Node %*u MemTotal: %lu kB", &kb) == 1)
                    break;
            fclose(f);
        }
        appendf(si->numa_map, sizeof si->numa_map, "%snode%u: %s",
                i ? "; " : "", ids[i], list);
        if (kb)
            appendf(si->numa_map, sizeof si->numa_map,
                    " (%lu MB)", kb / 1024);
    }
    if (count > 8)
        appendf(si->numa_map, sizeof si->numa_map, "; ...");
}

/* --- macOS -------------------------------------------------------------------*/

#if defined(__APPLE__)

static void sysctl_str_apple(const char *name, char *dst, size_t cap)
{
    size_t len = cap;

    memset(dst, 0, cap);
    if (sysctlbyname(name, dst, &len, NULL, 0) != 0)
        dst[0] = 0;
}

static long long sysctl_ll_apple(const char *name)
{
    long long v = 0;
    size_t len = sizeof v;

    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return 0;
    return v;
}

/* "total = 4096.00M used = 256.00M free = 3840.00M" */
static int parse_swapusage_field(const char *s, const char *label,
                                 unsigned long long *bytes)
{
    const char *p = strstr(s, label);
    double v;
    char unit;

    if (!p)
        return -1;
    if (sscanf(p + strlen(label), "%lf %c", &v, &unit) != 2)
        return -1;
    switch (unit) {
    case 'T': v *= 1024; /* fall through */
    case 'G': v *= 1024; /* fall through */
    case 'M': v *= 1024; /* fall through */
    case 'K': v *= 1024; break;
    default: break;
    }
    *bytes = (unsigned long long)(v + 0.5);
    return 0;
}

static void capture_apple(lb_sysinfo *si)
{
    char swap[128];
    long long memsize, fmax;
    mach_port_t host;
    vm_statistics64_data_t vmst;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    int pcores = (int)sysctl_ll_apple("hw.perflevel0.physicalcpu");
    int ecores = (int)sysctl_ll_apple("hw.perflevel1.physicalcpu");

    memsize = sysctl_ll_apple("hw.memsize");
    if (memsize > 0)
        si->ram_total = (unsigned long long)memsize;

    si->sockets = (unsigned)sysctl_ll_apple("hw.packages");
    si->cores = (unsigned)sysctl_ll_apple("hw.physicalcpu");
    si->threads = (unsigned)sysctl_ll_apple("hw.logicalcpu");

    if (pcores > 0 && ecores > 0)
        snprintf(si->core_layout, sizeof si->core_layout, "%dP+%dE",
                 pcores, ecores);

    fmax = sysctl_ll_apple("hw.cpufrequency_max");
    if (fmax > 0)
        si->freq_mhz = (double)fmax / 1e6;

    si->l2_kb = (unsigned long)sysctl_ll_apple("hw.l2cachesize") / 1024;
    si->l3_kb = (unsigned long)sysctl_ll_apple("hw.l3cachesize") / 1024;

    /* free + inactive ≈ the memory available to a fresh allocation */
    host = mach_host_self();
    if (host_statistics64(host, HOST_VM_INFO64,
                          (host_info64_t)&vmst, &count) == KERN_SUCCESS) {
        unsigned long long page = (unsigned long long)sysconf(_SC_PAGESIZE);
        si->ram_free = page * ((unsigned long long)vmst.free_count +
                               (unsigned long long)vmst.inactive_count);
    }

    sysctl_str_apple("vm.swapusage", swap, sizeof swap);
    if (swap[0]) {
        parse_swapusage_field(swap, "total =", &si->swap_total);
        parse_swapusage_field(swap, "free =", &si->swap_free);
    }

    sysctl_str_apple("machdep.cpu.brand_string", si->brand,
                     sizeof si->brand);
    if (!si->brand[0])
        sysctl_str_apple("hw.model", si->brand, sizeof si->brand);

    si->numa_nodes = 1; /* UMA */
}

#endif /* __APPLE__ */

/* --- x86 CPUID (both OSes) ----------------------------------------------------*/

#if defined(__x86_64__) || defined(__i386__)

static void cpuid_fill(lb_sysinfo *si)
{
    unsigned a, b, c, d;
    char vendor[16];

    if (__get_cpuid(0, &a, &b, &c, &d)) {
        snprintf(vendor, sizeof vendor, "%.4s%.4s%.4s", (char *)&b,
                 (char *)&d, (char *)&c);
        set_str(si->vendor, sizeof si->vendor, vendor);
        if (__get_cpuid(1, &a, &b, &c, &d)) {
            unsigned fam = (a >> 8) & 0xf, mdl = (a >> 4) & 0xf;

            if (fam == 0xf)
                fam += (a >> 20) & 0xff;
            if (fam == 6 || fam == 0xf)
                mdl += ((a >> 16) & 0xf) << 4;
            si->family = fam;
            si->model = mdl;
            si->stepping = a & 0xf;
        }
    }

    /* extended brand string: three 16-byte leaves */
    if (__get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000004) {
        char brand[64];

        __get_cpuid(0x80000002, &a, &b, &c, &d);
        memcpy(brand + 0, &a, 4); memcpy(brand + 4, &b, 4);
        memcpy(brand + 8, &c, 4); memcpy(brand + 12, &d, 4);
        __get_cpuid(0x80000003, &a, &b, &c, &d);
        memcpy(brand + 16, &a, 4); memcpy(brand + 20, &b, 4);
        memcpy(brand + 24, &c, 4); memcpy(brand + 28, &d, 4);
        __get_cpuid(0x80000004, &a, &b, &c, &d);
        memcpy(brand + 32, &a, 4); memcpy(brand + 36, &b, 4);
        memcpy(brand + 40, &c, 4); memcpy(brand + 44, &d, 4);
        brand[48] = 0;
        trim(brand);
        if (brand[0])
            set_str(si->brand, sizeof si->brand, brand);
    }

    /* deterministic cache parameters: leaf 4 subleaves */
    for (unsigned sub = 0; sub < 16; sub++) {
        unsigned level, type, ways, parts, line, sets;
        unsigned long kb;

        if (!__get_cpuid_count(4, sub, &a, &b, &c, &d))
            break;
        type = a & 0x1f;
        if (!type) /* no more cache entries */
            break;
        level = (a >> 5) & 7;
        ways = ((b >> 22) & 0x3ff) + 1;
        parts = ((b >> 12) & 0x3ff) + 1;
        line = (b & 0xfff) + 1;
        sets = c + 1;
        kb = (unsigned long)ways * parts * line * sets / 1024;
        if (level == 2)
            si->l2_kb = kb;
        else if (level == 3)
            si->l3_kb = kb;
    }

    /* Some AMD parts (Zen 4 included) answer leaf 4 with no entries
     * but do implement the extended leaf: L2 in ECX[31:16] (KB), L3 in
     * EDX[31:18] (512 KB units). Same layout on Intel. */
    if ((!si->l2_kb || !si->l3_kb) &&
        __get_cpuid(0x80000000, &a, &b, &c, &d) && a >= 0x80000006 &&
        __get_cpuid(0x80000006, &a, &b, &c, &d)) {
        if (!si->l2_kb)
            si->l2_kb = (c >> 16) & 0xffff;
        if (!si->l3_kb)
            si->l3_kb = ((d >> 18) & 0x3fff) * 512UL;
    }
}

#endif /* x86 */

/* --- capture ------------------------------------------------------------------*/

void lb_sysinfo_capture(lb_sysinfo *out)
{
    struct utsname un;
    double load[3];
    long nproc;

    memset(out, 0, sizeof *out);
    out->load_1m = -1.0;

    if (uname(&un) == 0) {
        set_str(out->os, sizeof out->os, un.sysname);
        set_str(out->kernel, sizeof out->kernel, un.release);
        set_str(out->machine, sizeof out->machine, un.machine);
    }

#if defined(__x86_64__)
    set_str(out->arch, sizeof out->arch, "x86_64");
#elif defined(__aarch64__)
    set_str(out->arch, sizeof out->arch, "aarch64");
#elif defined(__arm__)
    set_str(out->arch, sizeof out->arch, "arm");
#elif defined(__i386__)
    set_str(out->arch, sizeof out->arch, "i386");
#elif defined(__powerpc64__) || defined(__ppc64__)
    set_str(out->arch, sizeof out->arch, "ppc64");
#else
    set_str(out->arch, sizeof out->arch, "unknown");
#endif

#if defined(__clang__)
    snprintf(out->compiler, sizeof out->compiler, "clang %d.%d.%d",
             __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    snprintf(out->compiler, sizeof out->compiler, "gcc %d.%d.%d",
             __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    set_str(out->compiler, sizeof out->compiler, "unknown");
#endif

    nproc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nproc >= 1)
        out->threads = (unsigned)nproc;

#if defined(__APPLE__)
    capture_apple(out);
#else
    parse_cpuinfo(out);
    parse_meminfo(out);
    read_freq_linux(out);
    read_numa_linux(out);
#endif

#if defined(__x86_64__) || defined(__i386__)
    cpuid_fill(out); /* authoritative brand/vendor/caches on x86 */
#endif

    /* sane defaults when the platform exposes nothing */
    if (!out->sockets)
        out->sockets = 1;
    if (!out->cores)
        out->cores = out->threads;
    if (!out->threads)
        out->threads = out->cores;

    if (getloadavg(load, 3) >= 1)
        out->load_1m = load[0];
}
