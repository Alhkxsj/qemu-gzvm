#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "gzvm-internal.h"

int gzvm_start_vm(void)
{
    AccelState *accel = current_accel();
    GZVMState *s;
    int ret;

    if (!accel) {
        return -1;
    }
    s = GZVM_STATE(accel);

    if (s->msi_vectors) {
        EventNotifier *notifiers = g_new0(EventNotifier, s->msi_vectors);
        int registered = 0;
        uint32_t i;

        for (i = 0; i < s->msi_vectors; i++) {
            if (event_notifier_init(&notifiers[i], 0) < 0) {
                continue;
            }
            if (gzvm_add_irqfd(&notifiers[i], NULL,
                               GZVM_MSI_SPI_BASE + i) < 0) {
                event_notifier_cleanup(&notifiers[i]);
                continue;
            }
            registered++;
        }
        gzvm_gic_register_irq_notifiers(notifiers, s->msi_vectors,
                                        GZVM_MSI_SPI_BASE);
        g_free(notifiers);
        gz_report("%d/%u virtio IRQFDs created OK", registered,
                  s->msi_vectors);
    }

    if (s->dtb_start) {
        struct gzvm_dtb_config dtb;
        dtb.dtb_addr = s->dtb_start;
        dtb.dtb_size = s->dtb_size;
        ret = gzvm_vm_ioctl(GZVM_SET_DTB_CONFIG, &dtb);
        if (ret != 0) {
            gz_report("gzvm: GZVM_SET_DTB_CONFIG failed: %s (errno=%d)",
                         strerror(errno), errno);
            return -1;
        }
    }

    return 0;
}
