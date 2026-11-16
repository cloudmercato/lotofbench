/* LotofBench tests — system profile + environment detection
 * (upload/sysinfo.[ch]). The classifiers are fed fixture buffers, not
 * the live filesystem (plan §WS-C/C4: "parsers test buffers"). */

#include "tests.h"

#include "upload/sysinfo.h"

#include <string.h>

static const char CPUINFO_HYPERVISOR[] =
    "processor\t: 0\n"
    "vendor_id\t: GenuineIntel\n"
    "flags\t\t: fpu vme de pse tsc msr pae mce cx8 hypervisor "
    "apic sep mtrr\n"
    "bugs\t\t: cpu_meltdown\n";
static const char CPUINFO_PLAIN[] =
    "processor\t: 0\n"
    "vendor_id\t: GenuineIntel\n"
    "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr\n";

void test_upsys_cpuinfo_hypervisor(void)
{
    CHECK(lb_cpuinfo_has_hypervisor(CPUINFO_HYPERVISOR,
                                    sizeof CPUINFO_HYPERVISOR - 1));
    CHECK(!lb_cpuinfo_has_hypervisor(CPUINFO_PLAIN, sizeof CPUINFO_PLAIN - 1));
    CHECK(!lb_cpuinfo_has_hypervisor("", 0));
    CHECK(!lb_cpuinfo_has_hypervisor(NULL, 10));
}

void test_upsys_environment(void)
{
    char virt[LB_SYSTEM_VIRT_MAX];

    /* Nothing detected → bare-metal, empty product. */
    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, NULL, NULL, 0, virt,
                                    sizeof virt),
              LB_ENV_BARE_METAL);
    CHECK_STR(virt, "");

    /* /.dockerenv is the strongest container signal. */
    CHECK_U64(lb_detect_environment(NULL, 0, 1, NULL, NULL, NULL, 0, virt,
                                    sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "docker");

    /* /proc/1/cgroup tokens → container + product. */
    CHECK_U64(lb_detect_environment("12:devices:/docker/a1b2c3", 22, 0, NULL,
                                    NULL, NULL, 0, virt, sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "docker");

    CHECK_U64(lb_detect_environment("10:cpuset:/lxc/ci-runner", 24, 0, NULL,
                                    NULL, NULL, 0, virt, sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "lxc");

    CHECK_U64(lb_detect_environment("9:cpuset:/kubepods/pod123", 23, 0, NULL,
                                    NULL, NULL, 0, virt, sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "kubernetes");

    CHECK_U64(lb_detect_environment("0::/containerd.service", 21, 0, NULL,
                                    NULL, NULL, 0, virt, sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "containerd");

    CHECK_U64(lb_detect_environment("1:name=systemd:/system.slice/"
                                    "systemd-nspawn@one.service",
                                    56, 0, NULL, NULL, NULL, 0, virt,
                                    sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "systemd-nspawn");

    /* cgroup v2 inside a container shows "0::/" only — the $container
     * variable is the fallback signal. */
    CHECK_U64(lb_detect_environment("0::/", 4, 0, "podman", NULL, NULL, 0,
                                    virt, sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "podman");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, "docker", NULL, NULL, 0, virt,
                                    sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "docker");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, "libpod", NULL, NULL, 0, virt,
                                    sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "libpod");

    /* DMI ids → VM + canonical product. */
    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "QEMU", "Standard PC "
                                    "(Q35 + ICH9, 2009)",
                                    0, virt, sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "QEMU");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "Red Hat",
                                    "KVM Virtual Machine", 0, virt,
                                    sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "KVM");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "VMware, Inc.",
                                    "VMware Virtual Platform", 0, virt,
                                    sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "VMware");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "Xen", "HVM domU", 0,
                                    virt, sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "Xen");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL,
                                    "Microsoft Corporation",
                                    "Virtual Machine", 0, virt,
                                    sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "Hyper-V");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "Amazon EC2",
                                    "t3.micro", 0, virt, sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "EC2");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "innotek GmbH",
                                    "VirtualBox", 0, virt, sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "VirtualBox");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL,
                                    "Parallels Software International Inc.",
                                    "Parallels Virtual Platform", 0, virt,
                                    sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "Parallels");

    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "Bochs", "Bochs", 0,
                                    virt, sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "Bochs");

    /* Case-insensitivity of the DMI match. */
    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, "qemu systems",
                                    "standard pc", 0, virt, sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "QEMU");

    /* The cpuinfo flag alone: VM, but no product is knowable. */
    CHECK_U64(lb_detect_environment(NULL, 0, 0, NULL, NULL, NULL, 1, virt,
                                    sizeof virt),
              LB_ENV_VM);
    CHECK_STR(virt, "");

    /* Container evidence beats VM evidence (priority order). */
    CHECK_U64(lb_detect_environment("11:cpuset:/docker/xyz", 18, 1, NULL,
                                    "QEMU", "Standard PC", 0, virt,
                                    sizeof virt),
              LB_ENV_CONTAINER);
    CHECK_STR(virt, "docker");

    /* A narrow product buffer is not a crash. */
    CHECK_U64(lb_detect_environment("12:devices:/docker/abc", 20, 0, NULL,
                                    NULL, NULL, 0, virt, 5),
              LB_ENV_CONTAINER);
    CHECK_U64(strlen(virt), 4); /* "dock" truncated, still NUL-terminated */
}

void test_upsys_names(void)
{
    /* The JSON values must match the portal's accepted set exactly:
     * ('bare-metal', 'vm', 'container'). */
    CHECK_STR(lb_environment_name(LB_ENV_BARE_METAL), "bare-metal");
    CHECK_STR(lb_environment_name(LB_ENV_VM), "vm");
    CHECK_STR(lb_environment_name(LB_ENV_CONTAINER), "container");
}

void test_upsys_profile_capture(void)
{
    lb_sysinfo sys;
    lb_system_profile profile;

    lb_sysinfo_capture(&sys);
    lb_system_profile_capture(&profile, &sys);

    /* Derived fields carry over from the hardware report. */
    CHECK_STR(profile.os, sys.os);
    CHECK_STR(profile.arch, sys.arch);
    CHECK_STR(profile.cpu_model, sys.brand);
    CHECK(profile.cores > 0);
    CHECK_U64(profile.ram_mb, sys.ram_total / 1048576);

    /* The classification is always one of the three portal values and
     * the product name is either empty or a single short token. */
    const char *name = lb_environment_name(profile.environment);

    CHECK(!strcmp(name, "bare-metal") || !strcmp(name, "vm") ||
          !strcmp(name, "container"));
    CHECK(strlen(profile.virtualization) < sizeof profile.virtualization);

    /* Capture is repeatable and side-effect free. */
    lb_system_profile profile2;
    lb_system_profile_capture(&profile2, &sys);
    CHECK_U64(profile2.environment, profile.environment);
    CHECK_STR(profile2.virtualization, profile.virtualization);
}
