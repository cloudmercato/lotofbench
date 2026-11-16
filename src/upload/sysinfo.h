#ifndef LOTOFBENCH_UPLOAD_SYSINFO_H
#define LOTOFBENCH_UPLOAD_SYSINFO_H

#include <stddef.h>
#include "../sysinfo.h"

/* System profile attached to an uploaded document (plan v2.1 §WS-C/C4).
 *
 * The portal only needs a flat, privacy-clean view of the machine that
 * ran the benchmark: os, arch, cpu_model, logical cores, RAM and —
 * Anthony's decision — the execution environment, classified
 * container → vm → bare-metal. No hostname, no IP address, nothing
 * that identifies a person: the raw document never carries them.
 *
 * The classifiers below are pure functions over buffers; the unit
 * tests feed them fixtures instead of touching the filesystem. Only
 * lb_system_profile_capture() performs I/O (best-effort, never fails). */

#define LB_SYSTEM_OS_MAX     32
#define LB_SYSTEM_ARCH_MAX   32
#define LB_SYSTEM_MODEL_MAX  LB_SYSINFO_BRAND_MAX
#define LB_SYSTEM_VIRT_MAX   64

typedef enum {
    LB_ENV_BARE_METAL = 0,
    LB_ENV_VM = 1,
    LB_ENV_CONTAINER = 2
} lb_environment;

typedef struct {
    char os[LB_SYSTEM_OS_MAX];   /* uname sysname: "Linux", "Darwin" */
    char arch[LB_SYSTEM_ARCH_MAX]; /* uname machine / compile target */
    char cpu_model[LB_SYSTEM_MODEL_MAX]; /* "" when unknown */
    unsigned cores;              /* logical CPUs, 0 = unknown */
    unsigned long long ram_mb;   /* total RAM in MB, 0 = unknown */
    lb_environment environment;
    char virtualization[LB_SYSTEM_VIRT_MAX]; /* product or "" */
} lb_system_profile;

/* Canonical JSON values, identical to the portal's accepted set:
 * "bare-metal" | "vm" | "container". */
const char *lb_environment_name(lb_environment env);

/* Classify an environment from pre-read evidence (pure, no I/O).
 * Priority (Anthony's decision): container evidence wins, then VM,
 * then bare-metal.
 *
 *  - container evidence: `has_dockerenv` (/.dockerenv seen), a
 *    /proc/1/cgroup buffer containing docker|lxc|kubepods|containerd|
 *    systemd-nspawn, or a non-empty `container_env` (the `container`
 *    environment variable);
 *  - VM evidence: `sys_vendor`/`product_name` (DMI ids) matching the
 *    known virtualizer list, or the `cpuinfo_hypervisor` flag;
 *  - `virtualization` receives the detected product ("docker", "lxc",
 *    "kubernetes", "QEMU", "VMware", "Hyper-V", "EC2", "VirtualBox",
 *    "Parallels", "Bochs", "Xen", "KVM", "containerd",
 *    "systemd-nspawn", …) or "" when nothing more specific is known.
 *
 * Any pointer may be NULL / any length 0: the evidence is simply
 * absent. Returns the environment class. */
lb_environment lb_detect_environment(const char *cgroup, size_t cgroup_len,
                                     int has_dockerenv,
                                     const char *container_env,
                                     const char *sys_vendor,
                                     const char *product_name,
                                     int cpuinfo_hypervisor,
                                     char *virtualization, size_t virt_len);

/* Scan a /proc/cpuinfo buffer for the "hypervisor" flag (pure). */
int lb_cpuinfo_has_hypervisor(const char *cpuinfo, size_t len);

/* Fill the profile from an already-captured hardware report (os, arch,
 * cpu model, cores, RAM come from `sys`) plus a live environment probe
 * (/proc/1/cgroup, /.dockerenv, $container, DMI ids, /proc/cpuinfo).
 * Best-effort: missing sources degrade to bare-metal/unknown values. */
void lb_system_profile_capture(lb_system_profile *out, const lb_sysinfo *sys);

#endif /* LOTOFBENCH_UPLOAD_SYSINFO_H */
