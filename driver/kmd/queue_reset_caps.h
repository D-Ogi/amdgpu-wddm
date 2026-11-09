/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Which per-queue resets an AMD GPU has, by IP version (docs/design/queue-reset.md section 12). Pure decisions, no
 * hardware access, so driver/kmd/test/queue_reset_test.c checks them on the host.
 *
 * Two masks, intersected:
 *   - what amdgpu declares for the IP (AMDGPU_RESET_TYPE_PER_QUEUE in its sw_init), from ref/linux-src 7d0a66e4b:
 *       GC 10.x, gfx_v10_0.c:4955-4961: compute and gfx queues, on bare metal (amdgpu_sriov_vf() is false here).
 *       SDMA 5.0, sdma_v5_0.c:1425-1433: 5.0.0, 5.0.2 and 5.0.5 with SDMA firmware 35 or newer.
 *       SDMA 5.2, sdma_v5_2.c:1343-1357: 5.2.0, 5.2.2, 5.2.3, 5.2.4 with firmware 76 or newer, 5.2.5 with 34 or newer.
 *   - what this driver implements: the shim imports gfx_v10_0's kcq/kgq resets (bc250_gfx.c) and sdma_v5_0's
 *     register sequence (bc250_sdma.c). SDMA 5.2 has its own register layout (sdma_v5_2.c), which is not imported,
 *     so an SDMA 5.2 part gets no SDMA queue reset from this driver until it is.
 *
 * One deviation from amdgpu's list, with its evidence: SDMA 5.0.1 (Cyan Skillfish, the BC-250) is not in
 * sdma_v5_0.c's switch, but M943 measured sdma_v5_0_reset_queue on SDMA0 of unit A (amdgpu 6.18.52, the stock
 * cyan_skillfish2_sdma firmware): stop, soft reset and restore returned 0 in 1.2 ms and later copies passed. The
 * entry is therefore tied to a positively identified device (the PCI table of asic_ip.h), not to the IP version alone:
 * another 5.0.1 part would get it only after its own measurement.
 *
 * The compute fallback (queue-reset.md 5.3, CP_HQD_DEQUEUE_REQUEST after a KIQ unmap that left the HQD active) is
 * kfd's own handshake for every GC 10 part (amdgpu_amdkfd_gfx_v10.c kgd_hqd_destroy), so it is not board-specific;
 * M959 measured it on unit A. */
#ifndef BC250_QUEUE_RESET_CAPS_H
#define BC250_QUEUE_RESET_CAPS_H

#include "asic_ip.h"   /* the device table, the IP version packing and the BC250_QRESET_CAP_* bits */

/* The firmware version is unknown to the caller: no firmware gate can pass. */
#define BC250_SDMA_FW_UNKNOWN 0u

/* amdgpu's per-queue declaration for one GC and one SDMA version. */
static __inline unsigned Bc250QueueResetUpstreamCaps(unsigned gc, unsigned sdma, unsigned sdmaFw)
{
    unsigned caps = 0;

    if (BC250_IP_MAJOR(gc) == 10u) caps |= BC250_QRESET_CAP_COMPUTE | BC250_QRESET_CAP_GFX;
    switch (sdma) {
    case BC250_IP_VERSION(5, 0, 0):
    case BC250_IP_VERSION(5, 0, 2):
    case BC250_IP_VERSION(5, 0, 5):
        if (sdmaFw >= 35u) caps |= BC250_QRESET_CAP_SDMA;
        break;
    case BC250_IP_VERSION(5, 2, 0):
    case BC250_IP_VERSION(5, 2, 2):
    case BC250_IP_VERSION(5, 2, 3):
    case BC250_IP_VERSION(5, 2, 4):
        if (sdmaFw >= 76u) caps |= BC250_QRESET_CAP_SDMA;
        break;
    case BC250_IP_VERSION(5, 2, 5):
        if (sdmaFw >= 34u) caps |= BC250_QRESET_CAP_SDMA;
        break;
    default:
        break;
    }
    return caps;
}

/* What this driver's shim implements, for one GC and one SDMA version. */
static __inline unsigned Bc250QueueResetImplementedCaps(unsigned gc, unsigned sdma)
{
    unsigned caps = 0;

    if (BC250_IP_MAJOR(gc) == 10u && (BC250_IP_MINOR(gc) == 1u || BC250_IP_MINOR(gc) == 3u))
        caps |= BC250_QRESET_CAP_COMPUTE | BC250_QRESET_CAP_GFX;
    if (BC250_IP_MAJOR(sdma) == 5u && BC250_IP_MINOR(sdma) == 0u) caps |= BC250_QRESET_CAP_SDMA;
    return caps;
}

/* The queue resets this driver runs on one device: (amdgpu's list, plus what was measured on this device) and
 * what the shim implements. sdmaFw is the SDMA firmware version, or BC250_SDMA_FW_UNKNOWN. */
static __inline unsigned Bc250QueueResetCaps(unsigned vendor, unsigned device, unsigned sdmaFw)
{
    BC250_ASIC_IP ip;

    if (!Bc250AsicIpLookup(vendor, device, &ip)) return 0;
    return (Bc250QueueResetUpstreamCaps(ip.Gc, ip.Sdma, sdmaFw) | ip.MeasuredCaps) &
           Bc250QueueResetImplementedCaps(ip.Gc, ip.Sdma);
}

#endif
