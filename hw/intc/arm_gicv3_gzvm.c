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
#include "qemu/thread.h"
#include "qemu/lockable.h"

struct GZVMARMGICv3Class {
    ARMGICv3CommonClass parent_class;
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#define TYPE_GZVM_ARM_GICV3 "gzvm-arm-gicv3"
typedef struct GZVMARMGICv3Class GZVMARMGICv3Class;

DECLARE_OBJ_CHECKERS(GICv3State, GZVMARMGICv3Class,
                     GZVM_ARM_GICV3, TYPE_GZVM_ARM_GICV3)

/*
 * Serialises the shadow level state in gzvm_arm_gicv3_set_irq() against the
 * GZVM_IRQ_LINE ioctl that acts on it.  See the comment in that function.
 */
static QemuMutex gzvm_irq_level_lock;

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
    level = !!level;

    /*
     * The shadow level state and the ioctl that acts on it have to move as one
     * unit, hence the lock.
     *
     * This function is not always called under the BQL.  virtio-blk completions
     * run in their iothread and reach us as virtio_notify() ->
     * virtio_pci_notify() -> pci_set_irq() with no BQL held, because
     * with_irqfd in virtio_pci_vector_unmask()/virtio_pci_notify() is
     * msix_enabled() and there is no MSI-X here: this tree has no ITS, so
     * msi_nonbroken stays false, msix_init() fails with -ENOTSUP and every
     * virtio-pci device falls back to a level-triggered INTx line.  Meanwhile a
     * vCPU thread drives that same line low from its ISR read, under the BQL.
     *
     * Two threads doing an unlocked test-then-set on one line lose transitions.
     * On a shared level-triggered INTx line a lost 0->1 is not a hiccup, it is
     * terminal: the line stays deasserted with work still pending in the used
     * ring and the guest never gets another edge, so the disk simply stops
     * answering.  That is the "dracut-initqueue / dev-disk-by-uuid" stall and
     * the "kworker blocked for more than 122 seconds" traces, and it gets more
     * likely the more vCPUs there are to race with.  Before this function
     * suppressed anything the redundant injections happened to paper over it.
     *
     * Only suppress redundant *deassertions*.  A redundant assertion has to go
     * through, and that asymmetry is a correctness requirement, not a tuning
     * choice.
     *
     * Note that letting them through here is necessary but not sufficient for
     * PCI devices, which do not reach us via qemu_set_irq() directly.
     * pci_irq_handler() de-duplicates in both directions one layer above us and
     * returns before pci_change_irq_level(), so a re-assertion from a device
     * that already holds INTx high never becomes a call into this function at
     * all.  hw/virtio/virtio-pci.c uses pci_irq_reassert() to get past that;
     * see the comment in virtio_pci_intx_update().  That fix and the EVENT_IDX
     * withdrawal in gzvm_event_idx_allowed() are both required: an -smp 8 UEFI
     * boot hangs with either one missing, which is why each of them looked
     * useless when it was first tested on its own.
     *
     * GZ gives userspace no way to read back or resample a line.  The only
     * primitive is GZVM_IRQ_LINE -> gzvm_irqchip_inject_irq() -> an HVC into the
     * hypervisor, and GZVM_GET_ONE_REG answers -EOPNOTSUPP.  The behaviour says
     * GZ treats an injection as "make this INTID pending" rather than "hold this
     * line at this level": once the guest acks and EOIs, the pending bit is
     * consumed and nothing re-presents it, because GZ is not tracking a level to
     * re-present from.  A real GICv3 keeps re-asserting for as long as the
     * device holds the line high.
     *
     * So QEMU's shadow is not authoritative and cannot be used to skip work.
     * See level=1 with shadow=1, stay quiet, and if GZ has meanwhile consumed
     * that pending bit the guest never gets another interrupt on that line --
     * permanently, because a level-triggered INTx device just holds the line
     * high and waits.  Every device sharing the line dies with it.  At -smp 8
     * that showed up as virtio-net going silent ~2s in (its watchdog reported
     * the last good TX 6031 ms before the first timeout at t=8.16) with
     * virtio-blk following, then jbd2, ext4lazyinit and systemd itself blocked
     * forever -- on separate SPIs, which is what rules out a single racing line.
     * More vCPUs make it likelier because there are more chances for an EOI to
     * land between our shadow update and the next notify.  Before this function
     * suppressed anything, the redundant injections re-armed the pending bit
     * constantly and hid the problem entirely, which is why multicore worked
     * before the de-duplication existed.
     *
     * Suppressing redundant level=0 stays safe: nothing has to be re-armed to
     * remain quiescent, and that is where nearly all the waste was.
     * qemu_set_irq() de-duplicates nothing, and several device models re-assert
     * their current level on every register access -- pl011_update() runs from
     * every UART read and write, which produced ~157 redundant level=0
     * injections per real assertion in a "-d int" trace (44526 against 284 on
     * SPI 1, 99.2% of all interrupt traffic).  Each one is an ioctl plus an HVC,
     * and gzvm_irqchip_inject_irq() also calls gzvm_vcpu_wakeup_all(), which
     * wakes *every* vCPU in the VM, so the cost scales with -smp.
     *
     * The shadow state is still tracked in both directions to keep the 0->0
     * test meaningful.
     */
    QEMU_LOCK_GUARD(&gzvm_irq_level_lock);

    if (irqtype == GZVM_IRQ_TYPE_SPI) {
        if (!level && !gicv3_gicd_level_test(s, irq)) {
            return;
        }
        gicv3_gicd_level_replace(s, irq, level);
    } else {
        if (!level && !(cs->level & (1U << irq))) {
            return;
        }
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

    qemu_mutex_init(&gzvm_irq_level_lock);

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
