/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The IP versions of the AMD GPUs that this driver knows, by PCI id. Pure data and a lookup, no hardware access, so
 * the host tests (queue_reset_test.c, power_loss_test.c) read the same table as the driver.
 *
 * amdgpu reads the IP versions from the discovery table. This driver binds one device today, and its versions are
 * amdgpu's hard-coded ones (amdgpu_discovery.c:2783-2794, CHIP_CYAN_SKILLFISH with AMD_APU_IS_CYAN_SKILLFISH2).
 * Every per-IP decision (queue_reset_caps.h, power_loss.h) starts from a row of this table. A device that is not in
 * the table gets no per-IP capability at all. */
#ifndef BC250_ASIC_IP_H
#define BC250_ASIC_IP_H

/* amdgpu's IP_VERSION(maj, min, rev) packing (amdgpu.h). */
#define BC250_IP_VERSION(maj, min, rev) (((unsigned)(maj) << 16) | ((unsigned)(min) << 8) | (unsigned)(rev))
#define BC250_IP_MAJOR(v) (((v) >> 16) & 0xFFu)
#define BC250_IP_MINOR(v) (((v) >> 8) & 0xFFu)

/* The capability bits that a measurement on one device adds to amdgpu's list (queue_reset_caps.h). */
#define BC250_QRESET_CAP_COMPUTE  0x1u /* gfx_v10_0_reset_kcq: KIQ UNMAP_QUEUES RESET_QUEUES, MQD, MAP_QUEUES */
#define BC250_QRESET_CAP_GFX      0x2u /* gfx_v10_0_reset_kgq: CP_VMID_RESET through the KIQ, MQD, MAP_QUEUES */
#define BC250_QRESET_CAP_SDMA     0x4u /* sdma_v5_0_reset_queue: stop, GRBM soft reset of the instance, restart */

typedef struct _BC250_ASIC_IP {
    unsigned short PciDevice;
    unsigned Gc;
    unsigned Sdma;
    unsigned MeasuredCaps;  /* BC250_QRESET_CAP_* that a measurement on this device adds (queue_reset_caps.h) */
} BC250_ASIC_IP;

static const BC250_ASIC_IP g_Bc250AsicIp[] = {
    { 0x13FEu, BC250_IP_VERSION(10, 1, 3), BC250_IP_VERSION(5, 0, 1), BC250_QRESET_CAP_SDMA /* M943 */ },
};

/* The PCI id as configuration dword 0 holds it: vendor in the low half, device in the high half. */
#define BC250_PCI_VENDOR(id) ((unsigned)(id) & 0xFFFFu)
#define BC250_PCI_DEVICE(id) (((unsigned)(id) >> 16) & 0xFFFFu)

/* 1 and the versions when the device is in the table, 0 otherwise. */
static __inline int Bc250AsicIpLookup(unsigned vendor, unsigned device, BC250_ASIC_IP* out)
{
    unsigned i;

    if (vendor != 0x1002u) return 0;
    for (i = 0; i < sizeof(g_Bc250AsicIp) / sizeof(g_Bc250AsicIp[0]); i++)
        if (g_Bc250AsicIp[i].PciDevice == device) {
            *out = g_Bc250AsicIp[i];
            return 1;
        }
    return 0;
}

#endif
