#ifndef QEMU_GZVM_H
#define QEMU_GZVM_H

#include "qemu/accel.h"
#include "qemu/typedefs.h"
#include "qom/object.h"



#define TYPE_GZVM_ACCEL ACCEL_CLASS_NAME("gzvm")
typedef struct GZVMState GZVMState;
DECLARE_INSTANCE_CHECKER(GZVMState, GZVM_STATE,
                         TYPE_GZVM_ACCEL)

extern bool gzvm_allowed;

#define gzvm_enabled() (gzvm_allowed)

/*
 * Whether VIRTIO_RING_F_EVENT_IDX may be offered to guests.  False by default
 * under gzvm; see the comment on the definition in accel/gzvm/gzvm-accel-ops.c.
 */
bool gzvm_event_idx_allowed(void);

/*
 * Whether a virtio-pci INTx line should be driven through a GZVM_IRQFD instead
 * of pci_set_irq().  On by default under gzvm; set GZVM_INTX_IRQFD=off to fall
 * back.  See the comment on the definition in accel/gzvm/gzvm-irq.c.
 */
bool gzvm_intx_irqfd_allowed(void);

/*
 * Whether to give the guest a GICv2m frame so that MSI works without an ITS.
 * On by default under gzvm; set GZVM_MSI=off to fall back to shared INTx lines.
 * See the comment on the definition in accel/gzvm/gzvm-irq.c.
 */
bool gzvm_msi_allowed(void);

/*
 * Bind or unbind an eventfd to a GSI.  gsi is the 0-based SPI number -- the same
 * numbering gzvm_arm_gicv3_set_irq() passes as knum, which is what the driver
 * hands gzvm_irqchip_inject_irq() from both the GZVM_IRQ_LINE and the irqfd
 * path.  rn is a resample eventfd and must be NULL: the flag exists in the UAPI
 * but the driver never acts on it.
 */
int gzvm_add_irqfd(EventNotifier *n, EventNotifier *rn, int gsi);
int gzvm_remove_irqfd(EventNotifier *n, int gsi);

/*
 * Virtqueue re-poll interval in ms, 0 (the default) to disable.  A diagnostic
 * for splitting the EVENT_IDX failure by direction; see the comment on the
 * definition in accel/gzvm/gzvm-accel-ops.c.
 */
int gzvm_vq_repoll_ms(void);

/*
 * Whether to ignore the guest's used_event and interrupt on every completion.
 * Off by default; set GZVM_NOTIFY_FORCE=on.  The host -> guest half of the
 * EVENT_IDX direction split, which came out negative -- it is kept as the
 * negative control, never as a fix.  See the comment on the definition in
 * accel/gzvm/gzvm-accel-ops.c.
 */
bool gzvm_notify_force(void);

int gzvm_arm_set_dtb(uint64_t dtb_start, uint64_t dtb_size);
void gzvm_set_gic_bases(uint64_t dist_base, uint64_t redist_base,
                        uint64_t redist_size);
void gzvm_set_ram_base(uint64_t base);

#define gzvm_msi_via_irqfd_enabled() gzvm_enabled()

#endif
