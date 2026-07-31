#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qapi/error.h"
#include "cpu.h"
#include "hw/intc/arm_gicv3_common.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "system/runstate.h"
#include "gicv3_internal.h"
#include "qom/object.h"
#include "target/arm/cpregs.h"
#include "qemu/event_notifier.h"

struct GZVMARMGICv3Class {
    ARMGICv3CommonClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#define TYPE_GZVM_ARM_GICV3 "gzvm-arm-gicv3"
typedef struct GZVMARMGICv3Class GZVMARMGICv3Class;

DECLARE_OBJ_CHECKERS(GICv3State, GZVMARMGICv3Class,
                     GZVM_ARM_GICV3, TYPE_GZVM_ARM_GICV3)

static void gzvm_arm_gicv3_set_irq(void *opaque, int irq, int level)
{
    GICv3State *s = ARM_GICV3_COMMON(opaque);
    struct gzvm_irq_level irq_level;
    GICv3CPUState *cs;
    int irqtype;
    int cpu;
    int knum;

    if (irq < (int)(s->num_irq - GIC_INTERNAL)) {
        irqtype = GZVM_IRQ_TYPE_SPI;
        cpu = 0;
        knum = irq;
        irq += GIC_INTERNAL;
    } else {
        irqtype = GZVM_IRQ_TYPE_PPI;
        irq -= s->num_irq - GIC_INTERNAL;
        cpu = irq / GIC_INTERNAL;
        irq %= GIC_INTERNAL;
        knum = irq;
    }

    cs = &s->cpu[cpu];

    if (irqtype == GZVM_IRQ_TYPE_SPI) {
        gicv3_gicd_level_replace(s, irq, level);
    } else {
        if (level) {
            cs->level |= (1U << irq);
        } else {
            cs->level &= ~(1U << irq);
        }
    }

    irq_level.irq = (irqtype << GZVM_IRQ_TYPE_SHIFT) |
                    ((cpu & GZVM_IRQ_VCPU_MASK) << GZVM_IRQ_VCPU_SHIFT) |
                    (((cpu >> 8) & GZVM_IRQ_VCPU2_MASK) << GZVM_IRQ_VCPU2_SHIFT) |
                    (knum << GZVM_IRQ_NUM_SHIFT);
    irq_level.level = level;

    qemu_log_mask(CPU_LOG_INT,
                  "gzvm inject: type=%s cpu=%d spi/ppi=%d (intid=%d) level=%d\n",
                  irqtype == GZVM_IRQ_TYPE_SPI ? "SPI" : "PPI",
                  cpu, knum, irqtype == GZVM_IRQ_TYPE_SPI ? knum + GIC_INTERNAL : irq,
                  level);

    if (gzvm_vm_ioctl(GZVM_IRQ_LINE, &irq_level)) {
        warn_report("gzvm: GZVM_IRQ_LINE failed for irq=%d level=%d: %s",
                    knum, level, strerror(errno));
    }
}

static MemTxResult gzvm_gicv3_dist_read(void *opaque, hwaddr offset,
                                         uint64_t *data, unsigned size,
                                         MemTxAttrs attrs)
{
    *data = 0;
    return MEMTX_OK;
}

static MemTxResult gzvm_gicv3_dist_write(void *opaque, hwaddr offset,
                                          uint64_t data, unsigned size,
                                          MemTxAttrs attrs)
{
    return MEMTX_OK;
}

static const MemoryRegionOps gzvm_gicv3_dist_ops = {
    .read_with_attrs = gzvm_gicv3_dist_read,
    .write_with_attrs = gzvm_gicv3_dist_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static const MemoryRegionOps gzvm_gicv3_redist_ops = {
    .read_with_attrs = gicv3_redist_read,
    .write_with_attrs = gicv3_redist_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static const MemoryRegionOps gzvm_gicv3_ops[2] = {
    [0] = gzvm_gicv3_dist_ops,
    [1] = gzvm_gicv3_redist_ops,
};

static void gzvm_arm_gicv3_realize(DeviceState *dev, Error **errp)
{
    GICv3State *s = GZVM_ARM_GICV3(dev);
    GZVMARMGICv3Class *ggc = GZVM_ARM_GICV3_GET_CLASS(s);
    Error *local_err = NULL;

    if (s->revision != 3) {
        error_setg(errp, "unsupported GIC revision %d",
                   s->revision);
        return;
    }

    ggc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    gicv3_init_irqs_and_mmio(s, gzvm_arm_gicv3_set_irq, gzvm_gicv3_ops);
}

static void gzvm_arm_gicv3_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    GZVMARMGICv3Class *ggc = GZVM_ARM_GICV3_CLASS(klass);

    device_class_set_parent_realize(dc, gzvm_arm_gicv3_realize,
                                    &ggc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, NULL, NULL,
                                       &ggc->parent_phases);
}

static const TypeInfo gzvm_arm_gicv3_info = {
    .name = TYPE_GZVM_ARM_GICV3,
    .parent = TYPE_ARM_GICV3_COMMON,
    .instance_size = sizeof(GICv3State),
    .class_init = gzvm_arm_gicv3_class_init,
    .class_size = sizeof(GZVMARMGICv3Class),
};

static void gzvm_arm_gicv3_register_types(void)
{
    type_register_static(&gzvm_arm_gicv3_info);
}

type_init(gzvm_arm_gicv3_register_types)
