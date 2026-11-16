/* System profile + environment detection for the upload path.
 *
 * The pure classifiers (lb_detect_environment, lb_cpuinfo_has_hypervisor)
 * work on buffers so the unit suite can pin them with fixtures; the
 * capture wrapper is the only place that reads the filesystem. On
 * macOS the Linux probes are simply absent: the classification falls
 * through to bare-metal with an empty product, which is honest about
 * what the platform exposes. */

#define _POSIX_C_SOURCE 200809L
/* macOS gates _SC_NPROCESSORS_ONLN behind the Darwin extensions, like
 * cli.c and sysinfo.c — re-expose them so the one-command build works. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 200809L
#endif

#include "sysinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *lb_environment_name(lb_environment env)
{
    switch (env) {
    case LB_ENV_VM:
        return "vm";
    case LB_ENV_CONTAINER:
        return "container";
    case LB_ENV_BARE_METAL:
        break;
    }
    return "bare-metal";
}

/* ASCII case-insensitive substring search ("Microsoft Corporation"
 * vs "microsoft corporation" must both match). */
static int icontains(const char *hay, size_t hay_len, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (!hay || needle_len == 0 || hay_len < needle_len)
        return 0;
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        size_t j;

        for (j = 0; j < needle_len; j++) {
            char h = hay[i + j], n = needle[j];

            if (h >= 'A' && h <= 'Z')
                h = (char)(h - 'A' + 'a');
            if (n >= 'A' && n <= 'Z')
                n = (char)(n - 'A' + 'a');
            if (h != n)
                break;
        }
        if (j == needle_len)
            return 1;
    }
    return 0;
}

/* One virtualizer keyword → its canonical product name. */
static const char *dmi_match(const char *vendor, size_t vendor_len,
                             const char *product, size_t product_len,
                             const char *keyword, const char *canonical)
{
    if (icontains(vendor, vendor_len, keyword) ||
        icontains(product, product_len, keyword))
        return canonical;
    return NULL;
}

lb_environment lb_detect_environment(const char *cgroup, size_t cgroup_len,
                                     int has_dockerenv,
                                     const char *container_env,
                                     const char *sys_vendor,
                                     const char *product_name,
                                     int cpuinfo_hypervisor,
                                     char *virtualization, size_t virt_len)
{
    const char *product = NULL;

#define SET_PRODUCT(p)                                                        \
    do {                                                                      \
        if (virtualization && virt_len > 0)                                   \
            snprintf(virtualization, virt_len, "%s", (p));                    \
    } while (0)
#define CLEAR_PRODUCT()                                                       \
    do {                                                                      \
        if (virtualization && virt_len > 0)                                   \
            virtualization[0] = '\0';                                         \
    } while (0)

    /* --- container evidence wins ------------------------------------*/
    if (has_dockerenv) {
        SET_PRODUCT("docker");
        return LB_ENV_CONTAINER;
    }
    if (cgroup && cgroup_len) {
        static const struct {
            const char *token;
            const char *product;
        } tokens[] = {
            { "docker", "docker" },
            { "/lxc", "lxc" },
            { "kubepods", "kubernetes" },
            { "containerd", "containerd" },
            { "systemd-nspawn", "systemd-nspawn" },
        };

        for (size_t i = 0; i < sizeof tokens / sizeof tokens[0]; i++) {
            if (icontains(cgroup, cgroup_len, tokens[i].token)) {
                SET_PRODUCT(tokens[i].product);
                return LB_ENV_CONTAINER;
            }
        }
    }
    if (container_env && container_env[0]) {
        SET_PRODUCT(container_env);
        return LB_ENV_CONTAINER;
    }

    /* --- VM evidence --------------------------------------------------*/
    {
        size_t vendor_len = sys_vendor ? strlen(sys_vendor) : 0;
        size_t product_len = product_name ? strlen(product_name) : 0;
        static const struct {
            const char *keyword;
            const char *canonical;
        } dmi[] = {
            { "QEMU", "QEMU" },
            { "KVM", "KVM" },
            { "VMware", "VMware" },
            { "Xen", "Xen" },
            { "Microsoft Corporation", "Hyper-V" },
            { "Amazon EC2", "EC2" },
            { "VirtualBox", "VirtualBox" },
            { "Parallels", "Parallels" },
            { "Bochs", "Bochs" },
            { "innotek", "VirtualBox" },
        };

        for (size_t i = 0; i < sizeof dmi / sizeof dmi[0]; i++) {
            product = dmi_match(sys_vendor, vendor_len, product_name,
                                product_len, dmi[i].keyword,
                                dmi[i].canonical);
            if (product) {
                SET_PRODUCT(product);
                return LB_ENV_VM;
            }
        }
    }
    if (cpuinfo_hypervisor) {
        /* The flag alone proves a hypervisor but names no product. */
        CLEAR_PRODUCT();
        return LB_ENV_VM;
    }

    CLEAR_PRODUCT();
    return LB_ENV_BARE_METAL;

#undef SET_PRODUCT
#undef CLEAR_PRODUCT
}

int lb_cpuinfo_has_hypervisor(const char *cpuinfo, size_t len)
{
    if (!cpuinfo)
        return 0;
    return icontains(cpuinfo, len, "hypervisor");
}

/* Read a small file into buf (NUL-terminated); returns the length or
 * -1 when unreadable. Never fails the caller. */
static size_t read_small_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    size_t got = 0;

    if (!f)
        return (size_t)-1;
    got = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[got] = '\0';
    /* strip the trailing newline DMI files end with */
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r'))
        buf[--got] = '\0';
    return got;
}

void lb_system_profile_capture(lb_system_profile *out, const lb_sysinfo *sys)
{
    char cgroup[4096] = "";
    char vendor[128] = "";
    char product[128] = "";
    int has_dockerenv;
    int hypervisor = 0;

    memset(out, 0, sizeof(*out));
    if (sys) {
        snprintf(out->os, sizeof out->os, "%s", sys->os);
        snprintf(out->arch, sizeof out->arch, "%s", sys->arch);
        snprintf(out->cpu_model, sizeof out->cpu_model, "%s", sys->brand);
        out->cores = sys->threads;
        out->ram_mb = sys->ram_total / 1048576;
    }
    if (out->cores == 0) {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);

        out->cores = (nproc > 0) ? (unsigned)nproc : 0;
    }

    /* Live environment probe — every source is best-effort. */
    has_dockerenv = access("/.dockerenv", F_OK) == 0;
    if (read_small_file("/proc/1/cgroup", cgroup, sizeof cgroup) ==
        (size_t)-1)
        cgroup[0] = '\0';
    if (read_small_file("/sys/class/dmi/id/sys_vendor", vendor,
                        sizeof vendor) == (size_t)-1)
        vendor[0] = '\0';
    if (read_small_file("/sys/class/dmi/id/product_name", product,
                        sizeof product) == (size_t)-1)
        product[0] = '\0';
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        char line[2048];

        /* One "flags"/"Features" line is enough: the flag repeats on
         * every processor block and big machines make the file huge. */
        while (f && fgets(line, sizeof line, f)) {
            if (!strncmp(line, "flags", 5) || !strncmp(line, "Features", 8)) {
                hypervisor = lb_cpuinfo_has_hypervisor(line, strlen(line));
                break;
            }
        }
        if (f)
            fclose(f);
    }

    out->environment = lb_detect_environment(
        cgroup, strlen(cgroup), has_dockerenv, getenv("container"),
        vendor, product, hypervisor, out->virtualization,
        sizeof out->virtualization);
}
