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
