#include "qemu/osdep.h"
#include <stdio.h>
#include "qemu/error-report.h"
#include "cpu.h"
#include "internals.h"
#include "gzvm_arm.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"

#ifdef CONFIG_LINUX
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#ifndef HWCAP_CPUID
#define HWCAP_CPUID (1 << 11)
#endif

#define GZVM_CORE_REG(offset)  (GZVM_REG_ARM64 | GZVM_REG_SIZE_U64 | \
                                GZVM_REG_ARM_CORE | ((offset) / 4))

#define GZVM_REGS_X(i)      ((i) * 8)
#define GZVM_REGS_PC        (32 * 8)
#define GZVM_REGS_PSTATE    (33 * 8)

#define GZVM_ID_REG_LIST(X) \
    X(ID_PFR0_EL1,      3, 0, 0, 1, 0, isar.id_pfr0) \
    X(ID_PFR1_EL1,      3, 0, 0, 1, 1, isar.id_pfr1) \
    X(ID_DFR0_EL1,      3, 0, 0, 1, 2, isar.id_dfr0) \
    X(ID_MMFR0_EL1,     3, 0, 0, 1, 4, isar.id_mmfr0) \
    X(ID_MMFR1_EL1,     3, 0, 0, 1, 5, isar.id_mmfr1) \
    X(ID_MMFR2_EL1,     3, 0, 0, 1, 6, isar.id_mmfr2) \
    X(ID_MMFR3_EL1,     3, 0, 0, 1, 7, isar.id_mmfr3) \
    X(ID_ISAR0_EL1,     3, 0, 0, 2, 0, isar.id_isar0) \
    X(ID_ISAR1_EL1,     3, 0, 0, 2, 1, isar.id_isar1) \
    X(ID_ISAR2_EL1,     3, 0, 0, 2, 2, isar.id_isar2) \
    X(ID_ISAR3_EL1,     3, 0, 0, 2, 3, isar.id_isar3) \
    X(ID_ISAR4_EL1,     3, 0, 0, 2, 4, isar.id_isar4) \
    X(ID_ISAR5_EL1,     3, 0, 0, 2, 5, isar.id_isar5) \
    X(ID_MMFR4_EL1,     3, 0, 0, 2, 6, isar.id_mmfr4) \
    X(ID_ISAR6_EL1,     3, 0, 0, 2, 7, isar.id_isar6) \
    X(MVFR0_EL1,        3, 0, 0, 3, 0, isar.mvfr0) \
    X(MVFR1_EL1,        3, 0, 0, 3, 1, isar.mvfr1) \
    X(MVFR2_EL1,        3, 0, 0, 3, 2, isar.mvfr2) \
    X(ID_PFR2_EL1,      3, 0, 0, 3, 4, isar.id_pfr2) \
    X(ID_DFR1_EL1,      3, 0, 0, 3, 5, isar.id_dfr1) \
    X(ID_MMFR5_EL1,     3, 0, 0, 3, 6, isar.id_mmfr5) \
    X(ID_AA64PFR0_EL1,  3, 0, 0, 4, 0, isar.id_aa64pfr0) \
    X(ID_AA64PFR1_EL1,  3, 0, 0, 4, 1, isar.id_aa64pfr1) \
    X(ID_AA64ZFR0_EL1,  3, 0, 0, 4, 4, isar.id_aa64zfr0) \
    X(ID_AA64SMFR0_EL1, 3, 0, 0, 4, 5, isar.id_aa64smfr0) \
    X(ID_AA64DFR0_EL1,  3, 0, 0, 5, 0, isar.id_aa64dfr0) \
    X(ID_AA64DFR1_EL1,  3, 0, 0, 5, 1, isar.id_aa64dfr1) \
    X(ID_AA64AFR0_EL1,  3, 0, 0, 5, 4, id_aa64afr0) \
    X(ID_AA64AFR1_EL1,  3, 0, 0, 5, 5, id_aa64afr1) \
    X(ID_AA64ISAR0_EL1, 3, 0, 0, 6, 0, isar.id_aa64isar0) \
    X(ID_AA64ISAR1_EL1, 3, 0, 0, 6, 1, isar.id_aa64isar1) \
    X(ID_AA64ISAR2_EL1, 3, 0, 0, 6, 2, isar.id_aa64isar2) \
    X(ID_AA64MMFR0_EL1, 3, 0, 0, 7, 0, isar.id_aa64mmfr0) \
    X(ID_AA64MMFR1_EL1, 3, 0, 0, 7, 1, isar.id_aa64mmfr1) \
    X(ID_AA64MMFR2_EL1, 3, 0, 0, 7, 2, isar.id_aa64mmfr2) \
    X(ID_AA64MMFR3_EL1, 3, 0, 0, 7, 3, isar.id_aa64mmfr3) \
    X(CTR_EL0,          3, 3, 0, 0, 1, ctr)

static int gzvm_set_one_reg(CPUState *cs, uint64_t id, void *source)
{
    struct gzvm_one_reg reg = {
        .id = id,
        .addr = (uint64_t)(uintptr_t)source,
    };
    return gzvm_vcpu_ioctl(cs, GZVM_SET_ONE_REG, &reg);
}

static void gzvm_arch_set_id_regs(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    static bool logged;
    bool warned = false;
    int pushed = 0;

#define X(NAME, OP0, OP1, CRN, CRM, OP2, MEMBER)                            \
    do {                                                                   \
        uint64_t reg = cpu->MEMBER;                                        \
        if (reg) {                                                         \
            uint64_t enc = ((uint64_t)(OP0) << 14) |                       \
                           ((uint64_t)(OP1) << 11) |                       \
                           ((uint64_t)(CRN) << 7) |                        \
                           ((uint64_t)(CRM) << 3) |                        \
                           (uint64_t)(OP2);                                \
            uint64_t sysid = GZVM_REG_ARM64 | GZVM_REG_SIZE_U64 |          \
                             GZVM_REG_ARM64_SYSREG | (enc & 0x3fff);       \
            if (gzvm_set_one_reg(cs, sysid, &reg)) {                       \
                if (!warned) {                                             \
                    warn_report("gzvm: failed to set CPU ID register %s: %s", \
                                #NAME, strerror(errno));                   \
                    warned = true;                                         \
                }                                                          \
            } else {                                                       \
                pushed++;                                                  \
                if (!logged) {                                             \
                    info_report("gzvm:   id %s = 0x%016" PRIx64,           \
                                #NAME, reg);                               \
                }                                                          \
            }                                                              \
        }                                                                  \
    } while (0);
    GZVM_ID_REG_LIST(X)
#undef X

    if (!logged) {
        info_report("gzvm: programmed %d host ID registers into vcpu", pushed);
        logged = true;
    }
}

int gzvm_arm_set_dtb(uint64_t dtb_start, uint64_t dtb_size)
{
    assert(gzvm_enabled());
    GZVMState *state = GZVM_STATE(current_accel());
    state->dtb_start = dtb_start;
    state->dtb_size = dtb_size;
    return 0;
}

void gzvm_set_gic_bases(uint64_t dist_base, uint64_t redist_base,
                        uint64_t redist_size)
{
    assert(gzvm_enabled());
    GZVMState *state = GZVM_STATE(current_accel());
    state->gic_dist_base = dist_base;
    state->gic_redist_base = redist_base;
    state->gic_redist_size = redist_size;

    if (dist_base != 0x08000000ULL || redist_base != 0x080A0000ULL) {
        warn_report("gzvm: GIC base address mismatch:");
        warn_report("  QEMU virt: DIST=0x%08" PRIx64
                    " REDIST=0x%08" PRIx64 " (size=0x%" PRIx64 ")",
                    dist_base, redist_base, redist_size);
        warn_report("  Kernel:    DIST=0x%08x REDIST=0x%08x",
                    0x08000000, 0x080A0000);
        warn_report("  GIC will likely not work.  The kernel driver "
                    "ignores dev_addr and uses fixed addresses.");
    }
}

void gzvm_set_ram_base(uint64_t base)
{
    assert(gzvm_enabled());
    GZVMState *state = GZVM_STATE(current_accel());
    state->ram_base = base;
}

static int gzvm_get_one_reg_sw(CPUState *cs, uint64_t id, void *target)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    switch (id) {
    case GZVM_CORE_REG(GZVM_REGS_PSTATE):
        *(uint64_t *)target = env->pstate;
        return 0;
    case GZVM_CORE_REG(GZVM_REGS_PC):
        *(uint64_t *)target = env->pc;
        return 0;
    case GZVM_CORE_REG(GZVM_REGS_X(0)):
        *(uint64_t *)target = env->xregs[0];
        return 0;
    default:
        return -EOPNOTSUPP;
    }
}

int gzvm_arch_get_registers(CPUState *cs, int level)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    uint64_t val;

    if (gzvm_get_one_reg_sw(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE), &val) == 0) {
        env->pstate = val;
    }
    if (gzvm_get_one_reg_sw(cs, GZVM_CORE_REG(GZVM_REGS_PC), &val) == 0) {
        env->pc = val;
    }
    if (gzvm_get_one_reg_sw(cs, GZVM_CORE_REG(GZVM_REGS_X(0)), &val) == 0) {
        env->xregs[0] = val;
    }
    return 0;
}

static int gzvm_set_one_reg_err(CPUState *cs, uint64_t reg_id, uint64_t *val,
                                const char *name)
{
    int ret = gzvm_set_one_reg(cs, reg_id, val);
    if (ret) {
        error_report("gzvm: put_registers: %s failed: %s",
                     name, strerror(errno));
    }
    return ret;
}

int gzvm_arch_put_registers(CPUState *cs, int level)
{
    uint64_t val;
    int ret;
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    gzvm_arch_set_id_regs(cs);

    info_report("gzvm: vcpu%d entry PC=0x%" PRIx64 " X0=0x%" PRIx64
                " reset_pstate=0x%" PRIx64,
                cs->cpu_index, (uint64_t)env->pc, (uint64_t)env->xregs[0],
                (uint64_t)(PSTATE_DAIF | PSTATE_MODE_EL1h));

    val = PSTATE_DAIF | PSTATE_MODE_EL1h;
    ret = gzvm_set_one_reg_err(cs, GZVM_CORE_REG(GZVM_REGS_PSTATE),
                               &val, "pstate");
    if (ret) {
        return ret;
    }

    if (cs->cpu_index != 0) {
        return 0;
    }

    val = env->pc;
    ret = gzvm_set_one_reg_err(cs, GZVM_CORE_REG(GZVM_REGS_PC),
                               &val, "pc");
    if (ret) {
        return ret;
    }

    val = env->xregs[0];
    ret = gzvm_set_one_reg_err(cs, GZVM_CORE_REG(GZVM_REGS_X(0)),
                               &val, "x0");
    if (ret) {
        return ret;
    }

    return 0;
}

static uint32_t gzvm_arm_read_midr(void)
{
    static uint32_t cached_midr;
    static bool cached;
    g_autofree char *contents = NULL;
    g_auto(GStrv) lines = NULL;
    uint32_t midr = 0x410fd810;

    if (cached) {
        return cached_midr;
    }

    if (!g_file_get_contents("/proc/cpuinfo", &contents, NULL, NULL)) {
        cached_midr = midr;
        cached = true;
        return midr;
    }

    lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        unsigned long val;
        if (sscanf(lines[i], "CPU implementer : 0x%lx", &val) == 1) {
            midr = (midr & ~0xff000000) | ((val & 0xff) << 24);
        } else if (sscanf(lines[i], "CPU variant : 0x%lx", &val) == 1) {
            midr = (midr & ~0x00f00000) | ((val & 0xf) << 20);
        } else if (sscanf(lines[i], "CPU part : 0x%lx", &val) == 1) {
            midr = (midr & ~0x000fff00) | ((val & 0xfff) << 8);
        } else if (sscanf(lines[i], "CPU revision : %lu", &val) == 1) {
            midr = (midr & ~0x0000000f) | (val & 0xf);
        }
    }

    cached_midr = midr;
    cached = true;
    return midr;
}

static bool gzvm_read_host_sysreg(const char *name, uint64_t *value)
{
    char *lower = g_ascii_strdown(name, -1);
    char *path = g_strdup_printf(
        "/sys/devices/system/cpu/cpu0/regs/identification/%s", lower);
    char *contents = NULL;
    char *end;
    bool ok = false;

    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        *value = g_ascii_strtoull(contents, &end, 0);
        ok = end != contents;
    }

    g_free(contents);
    g_free(path);
    g_free(lower);
    return ok;
}

static sigjmp_buf gzvm_sysreg_jmp;

static void gzvm_sysreg_sigill(int sig)
{
    siglongjmp(gzvm_sysreg_jmp, 1);
}

static bool gzvm_cpuid_present(void)
{
    static bool checked;
    static bool present;

    if (!checked) {
        present = (qemu_getauxval(AT_HWCAP) & HWCAP_CPUID) != 0;
        checked = true;
    }
    return present;
}

#define GZVM_MRS_INST(OP0, OP1, CRN, CRM, OP2) \
    (0xd5200000u | (((((OP0) << 14) | ((OP1) << 11) | ((CRN) << 7) | \
                      ((CRM) << 3) | (OP2)) << 5)))

#define GZVM_READ_SYSREG(OP0, OP1, CRN, CRM, OP2, valp, okp)               \
    do {                                                                   \
        *(okp) = false;                                                    \
        if (((OP1) == 3) ||                                                \
            ((OP1) == 0 && (CRM) >= 4 && gzvm_cpuid_present())) {          \
            if (sigsetjmp(gzvm_sysreg_jmp, 1) == 0) {                      \
                register uint64_t v_ asm("x0");                            \
                asm volatile(".inst "                                      \
                    stringify(GZVM_MRS_INST(OP0, OP1, CRN, CRM, OP2))      \
                    : "=r"(v_));                                           \
                *(valp) = v_;                                              \
                *(okp) = true;                                             \
            }                                                              \
        }                                                                  \
    } while (0)

static uint64_t gzvm_arm_host_features_from_idregs(ARMISARegisters *isar)
{
    uint64_t features = BIT(ARM_FEATURE_V8) |
                        BIT(ARM_FEATURE_AARCH64) |
                        BIT(ARM_FEATURE_V7) |
                        BIT(ARM_FEATURE_V7VE) |
                        BIT(ARM_FEATURE_GENERIC_TIMER);
    uint64_t pfr0 = isar->id_aa64pfr0;
    uint64_t dfr0 = isar->id_aa64dfr0;

    if (FIELD_EX64(pfr0, ID_AA64PFR0, ADVSIMD) != 0xf) {
        features |= BIT(ARM_FEATURE_NEON);
    }
    if (FIELD_EX64(pfr0, ID_AA64PFR0, EL2) != 0) {
        features |= BIT(ARM_FEATURE_EL2);
    }
    if (FIELD_EX64(pfr0, ID_AA64PFR0, EL3) != 0) {
        features |= BIT(ARM_FEATURE_EL3);
    }
    if (FIELD_EX64(dfr0, ID_AA64DFR0, PMUVER) != 0 &&
        FIELD_EX64(dfr0, ID_AA64DFR0, PMUVER) != 0xf) {
        features |= BIT(ARM_FEATURE_PMU);
    }

    return features;
}

static bool gzvm_arm_read_host_cpu_features(ARMCPU *cpu)
{
    ARMISARegisters *isar = &cpu->isar;
    struct sigaction sa, old;
    uint64_t value;
    int read = 0;
    bool ok;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = gzvm_sysreg_sigill;
    sa.sa_flags = SA_NODEFER;
    if (sigaction(SIGILL, &sa, &old) < 0) {
        return false;
    }

#define X(NAME, OP0, OP1, CRN, CRM, OP2, MEMBER)                        \
    do {                                                                \
        ok = gzvm_read_host_sysreg(#NAME, &value);                      \
        if (!ok) {                                                      \
            GZVM_READ_SYSREG(OP0, OP1, CRN, CRM, OP2, &value, &ok);     \
        }                                                               \
        if (ok) {                                                       \
            cpu->MEMBER = value;                                        \
            read++;                                                     \
        }                                                               \
    } while (0);
    GZVM_ID_REG_LIST(X)
#undef X

    sigaction(SIGILL, &old, NULL);

    if (!read || !isar->id_aa64pfr0) {
        return false;
    }

    if (gzvm_read_host_sysreg("MIDR_EL1", &value)) {
        cpu->midr = value;
    } else {
        cpu->midr = gzvm_arm_read_midr();
    }
    if (gzvm_read_host_sysreg("REVIDR_EL1", &value)) {
        cpu->revidr = value;
    }

    return true;
}

static void gzvm_override_ipa_size(ARMISARegisters *isar)
{
    uint64_t cap = GZVM_CAP_ARM_VM_IPA_SIZE;
    int r = gzvm_vm_ioctl(GZVM_CHECK_EXTENSION, &cap);
    unsigned int gzvm_parange;
    unsigned int cur_parange;

    if (r == 0 && cap > 0) {
        gzvm_parange = round_down_to_parange_index(cap);
    } else {
        gzvm_parange = 2;
    }

    cur_parange = FIELD_EX64(isar->id_aa64mmfr0, ID_AA64MMFR0, PARANGE);
    if (gzvm_parange > cur_parange) {
        isar->id_aa64mmfr0 = FIELD_DP64(isar->id_aa64mmfr0, ID_AA64MMFR0,
                                        PARANGE, gzvm_parange);
    }
}

static void gzvm_mask_sve_sme(ARMISARegisters *isar)
{
    isar->id_aa64pfr0 = FIELD_DP64(isar->id_aa64pfr0, ID_AA64PFR0, SVE, 0);
    isar->id_aa64pfr1 = FIELD_DP64(isar->id_aa64pfr1, ID_AA64PFR1, SME, 0);
    isar->id_aa64pfr1 = FIELD_DP64(isar->id_aa64pfr1, ID_AA64PFR1, NMI, 0);
    isar->id_aa64smfr0 = 0;
    isar->id_aa64zfr0 = 0;
}

static void gzvm_mask_pmu(ARMISARegisters *isar, CPUARMState *env,
                          ARMCPU *cpu)
{
    isar->id_aa64dfr0 = FIELD_DP64(isar->id_aa64dfr0, ID_AA64DFR0, PMUVER, 0);
    env->features &= ~BIT(ARM_FEATURE_PMU);
    cpu->has_pmu = false;
}

void gzvm_arm_set_cpu_features_from_host(ARMCPU *cpu)
{
    ARMISARegisters *isar = &cpu->isar;
    CPUARMState *env = &cpu->env;

    if (!gzvm_arm_read_host_cpu_features(cpu)) {
        cpu->host_cpu_probe_failed = true;
        return;
    }

    env->features = gzvm_arm_host_features_from_idregs(isar);
    cpu->reset_sctlr = 0x00c50078;
    cpu->dtb_compatible = "arm,armv8";

    gzvm_override_ipa_size(isar);
    gzvm_mask_sve_sme(isar);
    gzvm_mask_pmu(isar, env, cpu);
}

void arm_cpu_gzvm_set_irq(void *arm_cpu, int irq, int level)
{
    ARMCPU *cpu = arm_cpu;
    CPUARMState *env = &cpu->env;
    struct gzvm_irq_level irq_level;
    uint32_t linestate_bit;
    int irq_id = 0;

    if (!arm_feature(env, ARM_FEATURE_EL2) &&
        (irq == ARM_CPU_VIRQ || irq == ARM_CPU_VFIQ)) {
        return;
    }

    switch (irq) {
    case ARM_CPU_IRQ:
        irq_id = GZVM_IRQ_CPU_IRQ;
        linestate_bit = CPU_INTERRUPT_HARD;
        break;
    case ARM_CPU_FIQ:
        irq_id = GZVM_IRQ_CPU_FIQ;
        linestate_bit = CPU_INTERRUPT_FIQ;
        break;
    case ARM_CPU_VIRQ:
        if (level) {
            env->irq_line_state |= CPU_INTERRUPT_VIRQ;
        } else {
            env->irq_line_state &= ~CPU_INTERRUPT_VIRQ;
        }
        arm_cpu_update_virq(cpu);
        return;
    case ARM_CPU_VFIQ:
        if (level) {
            env->irq_line_state |= CPU_INTERRUPT_VFIQ;
        } else {
            env->irq_line_state &= ~CPU_INTERRUPT_VFIQ;
        }
        arm_cpu_update_vfiq(cpu);
        return;
    case ARM_CPU_NMI:
        if (level) {
            env->irq_line_state |= CPU_INTERRUPT_NMI;
        } else {
            env->irq_line_state &= ~CPU_INTERRUPT_NMI;
        }
        return;
    case ARM_CPU_VINMI:
        if (level) {
            env->irq_line_state |= CPU_INTERRUPT_VINMI;
        } else {
            env->irq_line_state &= ~CPU_INTERRUPT_VINMI;
        }
        return;
    default:
        g_assert_not_reached();
    }

    if (level) {
        env->irq_line_state |= linestate_bit;
    } else {
        env->irq_line_state &= ~linestate_bit;
    }

    irq_level.irq = (GZVM_IRQ_TYPE_CPU << GZVM_IRQ_TYPE_SHIFT) | irq_id;
    irq_level.level = !!level;

    if (gzvm_vm_ioctl(GZVM_IRQ_LINE, &irq_level)) {
        warn_report("gzvm: GZVM_IRQ_LINE failed for CPU irq=%d level=%d: %s",
                    irq, level, strerror(errno));
    }
}
