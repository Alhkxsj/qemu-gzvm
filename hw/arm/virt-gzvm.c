#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/arm/virt.h"
#include "hw/arm/virt-gzvm.h"
#include "hw/loader.h"
#include "hw/nvram/fw_cfg.h"
#include "qemu/error-report.h"
#include "system/gzvm.h"

void virt_gzvm_init(VirtMachineState *vms)
{
    if (!gzvm_enabled()) {
        return;
    }

    gzvm_set_ram_base(vms->memmap[VIRT_MEM].base);

    vms->highmem_ecam = false;
    vms->memmap[VIRT_PCIE_ECAM].base = 0x0F000000ULL;

    vms->memmap[VIRT_PCIE_MMIO].base = 0x0B000000ULL;
    vms->memmap[VIRT_PCIE_MMIO].size = 16 * MiB;

    /*
     * The 64-bit PCI MMIO window has to go too, not just the high ECAM.
     *
     * virt_set_memmap() has already placed VIRT_HIGH_PCIE_MMIO above RAM by the
     * time we run, and machvirt_init() advertises it in the /pcie node's
     * "ranges" as FDT_PCI_RANGE_MMIO_64BIT whenever highmem_mmio is set (which
     * virt_instance_init() does by default).  EDK2's FdtPciHostBridgeLib turns
     * that range into a Mem64 aperture, and PciBusDxe then places 64-bit BARs
     * there -- virtio-pci's modern BAR4 is a 64-bit BAR, so a driven virtio
     * device ends up being accessed at a GPA far outside the low device window
     * that the rest of this function deliberately arranges.  With the range
     * gone, FdtPciHostBridgeLib reports no Mem64 aperture and PciBusDxe
     * degrades 64-bit BARs into the 32-bit window at VIRT_PCIE_MMIO.
     *
     * Only a device whose BAR is actually accessed trips over this, which is
     * why enumeration always looked fine: EDK2 has no virtio-input driver, so
     * virtio-tablet-pci / virtio-keyboard-pci get BARs assigned and never
     * touched, while virtio-blk-pci is driven by VirtioBlkDxe.
     */
    vms->highmem_mmio = false;

    /*
     * Same class of bug, different region: the second GICv3 redistributor
     * window also lives above RAM.  virt_gicv3_redist_region_count() only asks
     * for it once smp_cpus exceeds the first window's capacity
     * (VIRT_GIC_REDIST is 0x00F60000 bytes == 123 CPUs), so it is unreachable
     * in practice -- but if it were reached, machvirt_init() would put a
     * high-GPA region into the GIC node's "reg" and the guest would die the
     * same way, with no diagnostic.  Clearing it instead makes
     * machvirt_init_gic()'s max_cpus check fail loudly with the usual "Number
     * of SMP CPUs requested exceeds max CPUs supported by machine" error.
     *
     * GZ also only ever hears about one redistributor window: gzvm_create_vm()
     * issues a single GZVM_CREATE_DEVICE for GZVM_DEV_TYPE_ARM_VGIC_V3_REDIST.
     */
    vms->highmem_redists = false;
}

void virt_gzvm_post_gic(VirtMachineState *vms)
{
    if (!gzvm_enabled()) {
        return;
    }

    gzvm_set_gic_bases(vms->memmap[VIRT_GIC_DIST].base,
                       vms->memmap[VIRT_GIC_REDIST].base,
                       vms->memmap[VIRT_GIC_REDIST].size);
}

void virt_gzvm_post_dtb(VirtMachineState *vms, hwaddr dtb_start, int dtb_size,
                        AddressSpace *as)
{
    void *dtb_data;
    void *dtb_copy;

    if (!gzvm_enabled()) {
        return;
    }

    gzvm_arm_set_dtb(dtb_start, dtb_size);
    dtb_data = rom_ptr_for_as(as, dtb_start, dtb_size);
    if (dtb_data) {
        dtb_copy = g_memdup2(dtb_data, dtb_size);
        if (!dtb_copy) {
            error_report("GZVM: failed to allocate memory for DTB copy");
            return;
        }
        fw_cfg_add_file(vms->fw_cfg, "etc/fdt", dtb_copy, dtb_size);
    } else {
        warn_report("GZVM: cannot find DTB in ROM -- fw_cfg 'etc/fdt' not added");
    }
}

void virt_gzvm_set_bootinfo(VirtMachineState *vms, bool firmware_loaded)
{
    if (!gzvm_enabled() || !firmware_loaded) {
        return;
    }

    vms->bootinfo.entry = vms->memmap[VIRT_FLASH].base;
    vms->bootinfo.dtb_start = vms->memmap[VIRT_MEM].base;
}
