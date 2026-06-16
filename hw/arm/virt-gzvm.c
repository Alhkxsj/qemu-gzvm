#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/arm/virt.h"
#include "hw/arm/virt-gzvm.h"
#include "hw/core/loader.h"
#include "hw/nvram/fw_cfg.h"
#include "hw/pci/pci.h"
#include "qemu/error-report.h"
#include "system/gzvm.h"
#include "system/gzvm_int.h"
#include "system/system.h"

void virt_gzvm_init(VirtMachineState *vms)
{
    if (!gzvm_enabled()) {
        return;
    }

    gzvm_set_ram_base(vms->memmap[VIRT_MEM].base);

    /*
     * GZVM hypervisor only forwards stage-2 MMIO faults for
     * GPAs < 0x10000000.  Accesses above that threshold return
     * GZVM_EXIT_INTERNAL_ERROR and QEMU never gets to emulate
     * the device.  Relocate PCI windows below the boundary.
     */

    /* PCI ECAM: 0x3f000000 → 0x0f000000 (16 MiB) */
    vms->highmem_ecam = false;
    vms->memmap[VIRT_PCIE_ECAM].base = 0x0F000000ULL;

    /*
     * PCI MMIO32: 0x10000000 → 0x0b000000 (16 MiB).
     * Fits between virtio-mmio (ends ~0x0a004000) and
     * platform bus (0x0c000000).
     *
     * Also disable the 64-bit MMIO window (highmem_mmio);
     * it sits above 4 GiB, which is always ≥ 0x10000000.
     */
    vms->highmem_mmio = false;
    vms->memmap[VIRT_PCIE_MMIO].base = 0x0B000000ULL;
    vms->memmap[VIRT_PCIE_MMIO].size = 16 * MiB;
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

void virt_gzvm_create_virtio_gpu(VirtMachineState *vms)
{
    if (!gzvm_enabled() || !vms->bus || !MACHINE(vms)->enable_graphics ||
        vga_interface_created) {
        return;
    }

    pci_create_simple(vms->bus, -1, "virtio-gpu-pci");
}

void virt_gzvm_set_bootinfo(VirtMachineState *vms, bool firmware_loaded)
{
    if (!gzvm_enabled() || !firmware_loaded) {
        return;
    }

    vms->bootinfo.entry = vms->memmap[VIRT_MEM].base;

    AccelState *accel = current_accel();
    GZVMState *s = accel ? GZVM_STATE(accel) : NULL;
    if (s && s->firmware_size) {
        /* Place DTB immediately after the firmware blob, 8-byte aligned */
        vms->bootinfo.dtb_start = QEMU_ALIGN_UP(s->firmware_start + s->firmware_size, 8);
    } else {
        /* Fallback: assume 4 MiB is enough (firmware is capped at 4 MiB) */
        vms->bootinfo.dtb_start = vms->memmap[VIRT_MEM].base + 4 * MiB;
    }
}
