// SPDX-License-Identifier: MIT
/* Original bounded diagnostic provider; no imported implementation.
 * Linux DMA API contracts: see ASTRA-DMA-PROVIDER-DESIGN-20261117.md.
 * No mailbox, MMIO, PCI configuration write, bind, enable or power operation.
 * After first address publication both allocations survive client death.
 * Recovery after exposure is poweroff + cold AC reset, never forced unload.
 * DRAFT: build/source/lifetime gates required before loading on the lab.
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/dmi.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>
#include <linux/cc_platform.h>
#include <linux/iommu.h>
#include <linux/swiotlb.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/capability.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/io.h>
#include "dma-provider-C096.h"

struct buffer_slot {
    void *cpu;
    dma_addr_t dma;
    bool filled;
    bool exposed;
};
static struct pci_dev *gpu;
static struct buffer_slot slots[2];
static const u32 canaries[2] = { 0xA55A3CC3, 0x6DB6E719 };
static DEFINE_MUTEX(provider_lock);
static bool opened;
static bool retained;

/* Must be evaluated under device_lock to exclude an in-progress bind. This
 * check does not prevent a later privileged sysfs bind/remove. The dedicated
 * network-only boot and sole-operator contract prohibit those operations.
 */
static bool dma_route_ok(void)
{
    struct device *dev = &gpu->dev;
    u16 command;
    if (dev->driver || pci_dev_is_disconnected(gpu) ||
        gpu->current_state != PCI_D0 || !pm_runtime_active(dev) || get_dma_ops(dev) ||
        iommu_get_domain_for_dev(dev) || cc_platform_has(CC_ATTR_MEM_ENCRYPT) ||
        !dev_is_dma_coherent(dev) || !dev->dma_mask ||
        *dev->dma_mask != DMA_BIT_MASK(32) ||
        dev->coherent_dma_mask != DMA_BIT_MASK(32))
        return false;
    if (pci_read_config_word(gpu, PCI_COMMAND, &command) || command == 0xFFFF ||
        (command & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
        (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
        return false;
#ifdef CONFIG_SWIOTLB
    if (dev->dma_io_tlb_mem && (dev->dma_io_tlb_mem->for_alloc ||
                               dev->dma_io_tlb_mem->force_bounce))
        return false;
#endif
    return true;
}

static bool valid_request(const struct bc250_dma_request *r)
{
    return r->abi == BC250_DMA_ABI && r->slot < 2 &&
           !r->reserved[0] && !r->reserved[1];
}

static int provider_open(struct inode *inode, struct file *file)
{
    int ret = 0;
    if (!capable(CAP_SYS_RAWIO))
        return -EPERM;
    mutex_lock(&provider_lock);
    if (opened)
        ret = -EBUSY;
    else
        opened = true;
    mutex_unlock(&provider_lock);
    return ret;
}

static int provider_release(struct inode *inode, struct file *file)
{
    mutex_lock(&provider_lock);
    opened = false; /* No memory release, state reset or reference put. */
    mutex_unlock(&provider_lock);
    return 0;
}

static long provider_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct bc250_dma_request r;
    struct bc250_dma_descriptor descriptor = {0};
    struct bc250_dma_snapshot *snapshot = NULL;
    struct buffer_slot *slot;
    void __user *user = (void __user *)arg;
    int ret = 0, i;
    if (!capable(CAP_SYS_RAWIO))
        return -EPERM;
    if (cmd != BC250_DMA_FILL && cmd != BC250_DMA_EXPOSE &&
        cmd != BC250_DMA_SNAPSHOT)
        return -ENOTTY;
    if (copy_from_user(&r, user, sizeof(r)))
        return -EFAULT;
    if (!valid_request(&r))
        return -EINVAL;
    if (cmd == BC250_DMA_SNAPSHOT) {
        snapshot = kzalloc(sizeof(*snapshot), GFP_KERNEL);
        if (!snapshot)
            return -ENOMEM;
    }
    mutex_lock(&provider_lock);
    device_lock(&gpu->dev);
    if (!dma_route_ok()) {
        ret = -ENODEV;
        goto out;
    }
    slot = &slots[r.slot];
    if (cmd == BC250_DMA_FILL) {
        if (retained || slot->filled) {
            ret = -EPERM;
            goto out;
        }
        for (i = 0; i < BC250_DMA_BYTES / sizeof(u32); i++)
            WRITE_ONCE(((u32 *)slot->cpu)[i], canaries[r.slot]);
        dma_wmb();
        for (i = 0; i < BC250_DMA_BYTES / sizeof(u32); i++) {
            if (READ_ONCE(((u32 *)slot->cpu)[i]) != canaries[r.slot]) {
                ret = -EIO;
                goto out;
            }
        }
        slot->filled = true;
    } else if (cmd == BC250_DMA_EXPOSE) {
        if (!slots[0].filled || !slots[1].filled || slot->exposed) {
            ret = -EPERM;
            goto out;
        }
        /* Never roll these back, including copy_to_user failure. The open
         * file already holds a module reference, so this cannot race unload.
         */
        if (!retained) {
            __module_get(THIS_MODULE);
            retained = true;
        }
        slot->exposed = true;
        dma_wmb();
        descriptor.request = r;
        descriptor.dma_address = slot->dma;
        descriptor.bytes = BC250_DMA_BYTES;
        descriptor.canary = canaries[r.slot];
    } else {
        if (!slot->filled) {
            ret = -EPERM;
            goto out;
        }
        /* A snapshot during DMA can be torn. This is not completion proof. */
        snapshot->request = r;
        dma_rmb();
        memcpy(snapshot->bytes, slot->cpu, BC250_DMA_BYTES);
    }
out:
    device_unlock(&gpu->dev);
    mutex_unlock(&provider_lock);
    /* User faults must not hold device/allocator locks needed by shutdown.
     * Exposure and its permanent reference remain set if copyout fails.
     */
    if (!ret && cmd == BC250_DMA_EXPOSE &&
        copy_to_user(user, &descriptor, sizeof(descriptor)))
        ret = -EFAULT;
    if (!ret && cmd == BC250_DMA_SNAPSHOT &&
        copy_to_user(user, snapshot, sizeof(*snapshot)))
        ret = -EFAULT;
    kfree(snapshot);
    return ret;
}

static const struct file_operations provider_fops = {
    .owner = THIS_MODULE,
    .open = provider_open,
    .release = provider_release,
    .unlocked_ioctl = provider_ioctl,
    .llseek = NULL,
};
static struct miscdevice provider_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "bc250_dma_c096",
    .fops = &provider_fops,
    .mode = 0600,
};

static void free_unexposed(void)
{
    int i;
    if (WARN_ON(retained))
        return;
    for (i = 0; i < 2; i++) {
        if (slots[i].cpu) {
            dma_free_coherent(&gpu->dev, BC250_DMA_BYTES, slots[i].cpu, slots[i].dma);
            slots[i].cpu = NULL;
        }
    }
    if (gpu) {
        pci_dev_put(gpu);
        gpu = NULL;
    }
}

static int __init provider_init(void)
{
    struct pci_dev *scan = NULL;
    int count = 0, i, ret = -ENODEV;
    phys_addr_t phys;
    if (PAGE_SIZE != BC250_DMA_BYTES ||
        !dmi_match(DMI_BOARD_NAME, "AMD BC-250") ||
        !dmi_match(DMI_BOARD_VENDOR, "ASRock") ||
        !dmi_match(DMI_BIOS_VERSION, "P3.00") ||
        !dmi_match(DMI_BIOS_VENDOR, "American Megatrends Inc.") ||
        !dmi_match(DMI_BIOS_DATE, "12/09/2021"))
        return -ENODEV;
    while ((scan = pci_get_device(0x1002, 0x13fe, scan))) {
        count++;
        if (!gpu)
            gpu = pci_dev_get(scan);
    }
    if (count != 1)
        goto fail;
    device_lock(&gpu->dev);
    if (!dma_route_ok())
        goto unlock_fail;
    for (i = 0; i < 2; i++) {
        /* Existing 32-bit coherent mask, not GFP_DMA32, constrains address. */
        slots[i].cpu = dma_alloc_coherent(&gpu->dev, BC250_DMA_BYTES,
                                         &slots[i].dma, GFP_KERNEL);
        if (!slots[i].cpu) {
            ret = -ENOMEM;
            goto unlock_fail;
        }
        if (!slots[i].dma || (slots[i].dma & (BC250_DMA_BYTES - 1)) ||
            slots[i].dma > DMA_BIT_MASK(32) - (BC250_DMA_BYTES - 1) ||
            (gpu->dev.bus_dma_limit && slots[i].dma + BC250_DMA_BYTES - 1 >
             gpu->dev.bus_dma_limit) || is_vmalloc_addr(slots[i].cpu) ||
            !virt_addr_valid(slots[i].cpu) ||
            !virt_addr_valid((char *)slots[i].cpu + BC250_DMA_BYTES - 1))
            goto unlock_fail;
        phys = virt_to_phys(slots[i].cpu);
        if (phys != slots[i].dma || swiotlb_find_pool(&gpu->dev, phys))
            goto unlock_fail;
    }
    if (slots[0].dma == slots[1].dma || slots[0].cpu == slots[1].cpu)
        goto unlock_fail;
    device_unlock(&gpu->dev);
    ret = misc_register(&provider_device);
    if (ret)
        goto fail;
    pr_info("bc250_dma_c096: two unexposed coherent pages, direct 32-bit route\n");
    return 0;
unlock_fail:
    device_unlock(&gpu->dev);
fail:
    free_unexposed();
    return ret;
}

static void __exit provider_exit(void)
{
    /* Normal unload cannot reach here after permanent exposure reference. */
    misc_deregister(&provider_device);
    free_unexposed();
}
module_init(provider_init);
module_exit(provider_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Bounded unit-A diagnostic DMA buffers; retain after exposure");
