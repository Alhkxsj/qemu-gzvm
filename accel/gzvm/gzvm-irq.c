#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "exec/memory.h"
#include "qemu/event_notifier.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "linux-headers/linux/gzvm.h"
#include "gzvm-internal.h"

static int
gzvm_set_ioeventfd_mmio(int fd, hwaddr addr, uint32_t size, uint64_t data,
                         bool datamatch, bool assign)
{
    int ret;
    struct gzvm_ioeventfd io;

    memset(&io, 0, sizeof(io));
    io.fd = fd;
    io.datamatch = datamatch ? data : 0;
    io.len = size;
    io.addr = addr;
    io.flags = datamatch ? GZVM_IOEVENTFD_FLAG_DATAMATCH : 0;
    if (!assign) {
        io.flags |= GZVM_IOEVENTFD_FLAG_DEASSIGN;
    }

    ret = gzvm_vm_ioctl(GZVM_IOEVENTFD, &io);
    return ret;
}

static void
gzvm_mem_ioeventfd_add(MemoryListener *listener, MemoryRegionSection *section,
                        bool match_data, uint64_t data, EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = gzvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                                 int128_get64(section->size), data,
                                 match_data, true);
    if (r < 0 && errno == EEXIST) {
        return;
    }
    if (r < 0) {
        error_report("gzvm: ioeventfd_add failed addr=0x%" PRIx64 ": %s",
                     (uint64_t)section->offset_within_address_space,
                     strerror(errno));
    }
}

static void
gzvm_mem_ioeventfd_del(MemoryListener *listener, MemoryRegionSection *section,
                        bool match_data, uint64_t data, EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = gzvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                                 int128_get64(section->size), data,
                                 match_data, false);
    if (r < 0 && errno != ENOENT) {
        error_report("gzvm: ioeventfd_del failed addr=0x%" PRIx64 ": %s",
                     (uint64_t)section->offset_within_address_space,
                     strerror(errno));
    }
}

/*
 * Whether virtio-pci should drive INTx through an irqfd.
 *
 * GZ has no notion of a level: an injection means "make this INTID pending",
 * and once the guest EOIs it there is nothing left for the hypervisor to
 * re-present.  A level-triggered device that keeps its line high therefore goes
 * silent, which is what hw/intc/arm_gicv3_gzvm.c's asymmetric de-duplication
 * exists to paper over for the devices that still use the ioctl path.
 *
 * The driver's irqfd path is a much better match, because it is edge-triggered
 * by construction -- irqfd_set_irq() in drivers/virt/geniezone/gzvm_irqfd.c is
 * literally "if (level) inject", so level 0 is dropped and one eventfd signal is
 * exactly one "make pending".  It also injects inline from the eventfd wake-up
 * ("gzvm's irq injection is not blocked, don't need workq"), so a device thread
 * can signal it without taking the BQL.
 *
 * On by default.  Set GZVM_INTX_IRQFD=off to go back to the ioctl path, which is
 * still what gets used for any device whose irqfd fails to bind.
 */
bool gzvm_intx_irqfd_allowed(void)
{
    static int allowed = -1;

    if (allowed < 0) {
        const char *val = getenv("GZVM_INTX_IRQFD");

        allowed = !(val && (!strcmp(val, "off") || !strcmp(val, "0")));
    }

    return allowed;
}

/*
 * Whether to expose an MSI controller to the guest.
 *
 * GZ has no ITS: enum gzvm_device_type only has VGIC_V3_DIST and VGIC_V3_REDIST,
 * so there is no LPI injection primitive and an emulated ITS is not an option.
 * What is available is an SPI, and the GICv2m frame is exactly the standard way
 * to spell "an MSI write becomes an SPI".  The guest reaches it without any
 * driver changes: irq-gic-v3.c calls gicv2m_init() unconditionally on the
 * !gic_dist_supports_lpis() path, and arm64 selects ARM_GIC_V2M if PCI.
 *
 * On by default under gzvm.  Set GZVM_MSI=off to hide the frame again, which
 * leaves msi_nonbroken false, makes msix_init() fail with -ENOTSUP as before and
 * puts every virtio-pci device back on its shared INTx line.
 */
bool gzvm_msi_allowed(void)
{
    static int allowed = -1;

    if (allowed < 0) {
        const char *val = getenv("GZVM_MSI");

        allowed = !(val && (!strcmp(val, "off") || !strcmp(val, "0")));
    }

    return allowed;
}

/*
 * rn must be NULL.  GZVM_IRQFD_FLAG_RESAMPLE is accepted by the driver's
 * validity mask and then ignored -- nothing in drivers/virt/geniezone/ ever
 * reads it -- so there is no EOI notification to be had and a resample eventfd
 * would simply never fire.  The flag is set here to keep the UAPI intent visible
 * if the driver ever grows an implementation.
 */
int gzvm_add_irqfd(EventNotifier *n, EventNotifier *rn, int gsi)
{
    struct gzvm_irqfd irqfd = {
        .fd = event_notifier_get_fd(n),
        .gsi = gsi,
        .flags = 0,
    };

    if (rn) {
        irqfd.flags |= GZVM_IRQFD_FLAG_RESAMPLE;
        irqfd.resamplefd = event_notifier_get_fd(rn);
    }

    return gzvm_vm_ioctl(GZVM_IRQFD, &irqfd);
}

int gzvm_remove_irqfd(EventNotifier *n, int gsi)
{
    struct gzvm_irqfd irqfd = {
        .fd = event_notifier_get_fd(n),
        .gsi = gsi,
        .flags = GZVM_IRQFD_FLAG_DEASSIGN,
    };

    return gzvm_vm_ioctl(GZVM_IRQFD, &irqfd);
}

MemoryListener gzvm_ioeventfd_listener = {
    .name = "gzvm-ioeventfd",
    .eventfd_add = gzvm_mem_ioeventfd_add,
    .eventfd_del = gzvm_mem_ioeventfd_del,
    .priority = MEMORY_LISTENER_PRIORITY_ACCEL,
};

static int
gzvm_set_ioeventfd_pio(int fd, uint16_t addr, uint32_t size, uint64_t data,
                       bool datamatch, bool assign)
{
    struct gzvm_ioeventfd io;

    memset(&io, 0, sizeof(io));
    io.fd = fd;
    io.datamatch = datamatch ? data : 0;
    io.len = size;
    io.addr = addr;
    io.flags = GZVM_IOEVENTFD_FLAG_PIO;
    if (datamatch) io.flags |= GZVM_IOEVENTFD_FLAG_DATAMATCH;
    if (!assign) io.flags |= GZVM_IOEVENTFD_FLAG_DEASSIGN;

    return gzvm_vm_ioctl(GZVM_IOEVENTFD, &io);
}

static void
gzvm_io_ioeventfd_add(MemoryListener *listener, MemoryRegionSection *section,
                      bool match_data, uint64_t data, EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = gzvm_set_ioeventfd_pio(fd, section->offset_within_address_space,
                               int128_get64(section->size), data,
                               match_data, true);
    if (r < 0 && errno == EEXIST) return;
    if (r < 0) {
        error_report("gzvm: pio ioeventfd_add failed addr=0x%" PRIx64 ": %s",
                     (uint64_t)section->offset_within_address_space,
                     strerror(errno));
    }
}

static void
gzvm_io_ioeventfd_del(MemoryListener *listener, MemoryRegionSection *section,
                      bool match_data, uint64_t data, EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = gzvm_set_ioeventfd_pio(fd, section->offset_within_address_space,
                               int128_get64(section->size), data,
                               match_data, false);
    if (r < 0 && errno != ENOENT) {
        error_report("gzvm: pio ioeventfd_del failed addr=0x%" PRIx64 ": %s",
                     (uint64_t)section->offset_within_address_space,
                     strerror(errno));
    }
}

MemoryListener gzvm_io_listener = {
    .name = "gzvm-io",
    .eventfd_add = gzvm_io_ioeventfd_add,
    .eventfd_del = gzvm_io_ioeventfd_del,
    .priority = MEMORY_LISTENER_PRIORITY_DEV_BACKEND,
};
