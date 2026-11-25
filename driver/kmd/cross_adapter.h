/* Cross-adapter resources, tier 1 (copy): DXGK_DRIVERCAPS.MemoryManagementCaps.CrossAdapterResource and the
 * DxgkDdiDescribeAllocation answer for a cross-adapter buffer. Plain C without WDK types, so the decision is tested on
 * the development PC (driver/kmd/test/cross_adapter_test.c, the 'cross-adapter' gates of tools/quality/quick.ps1).
 *
 * The contract, from the sources in the order they win:
 *   - WDK 10.0.26100 shared/d3dkmddi.h:2267: CrossAdapterResource is bit 4 of DXGK_VIDMMCAPS. Bits 15 and 16
 *     (CrossAdapterResourceTexture, CrossAdapterResourceScanout) are tiers 2 and 3. This driver reports tier 1 only.
 *   - supporting-caso.md, "Indicating tier support": tier 1 is "Copy to and from cross-adapter resources", exposed to
 *     user mode as D3DKMT_WDDM_1_3_CAPS.SupportCrossAdapterResource. "Drivers must declare support for this feature
 *     based on their own adapter capability, regardless of device configuration".
 *   - using-cross-adapter-resources-in-a-hybrid-system.md:32-41: a cross-adapter resource is shared, has one linear
 *     allocation, a pitch aligned to 128 bytes, a height aligned to 4 rows, a page-aligned start, and is paged in to
 *     the aperture segment only.
 *   - dxgkrnl 10.0.26100.9974, static reading (fact M972, evidence/windows/2026-11-25-cross-adapter-tier1/):
 *       DXGADAPTER::Initialize checks only the consistency of the bits (the 1.3 bits need WDDMVersion 1.3, tier 2
 *       needs tier 1, tier 3 needs tier 2). It infers IsHybridIntegrated from bit 4 only below interface 0x5011;
 *       this driver declares DXGKDDI_INTERFACE_VERSION_WDDM2_9, so the inference does not run.
 *       DXGDEVICE::CreateAllocation refuses D3DKMT_CREATEALLOCATION.Flags.CrossAdapter without the bit, requires
 *       CreateShared with it, and then calls ADAPTER_RENDER::DdiDescribeAllocation for every cross-adapter
 *       allocation. It refuses an answer of Format 0 (D3DDDIFMT_UNKNOWN) with a Height other than 1.
 *       No other reader of bit 4 exists among the accesses of the stored caps.
 *     DXGKARG_CREATEALLOCATION carries no cross-adapter flag (d3dkmddi.h, DXGK_CREATEALLOCATIONFLAGS has Resource
 *     only), so DescribeAllocation is the one call in which this driver learns that an allocation is cross-adapter.
 *   - D3D12 (debug layer of the D3D12 runtime on a tier 1 driver, 2026-11-25): a committed SHARED_CROSS_ADAPTER
 *     resource must be a TEXTURE2D, which needs ROW_MAJOR (tier 2). Tier 1 is thus a buffer placed in a
 *     SHARED|SHARED_CROSS_ADAPTER heap, and the heap is a UMD (BC2A) allocation of this driver.
 *
 * So the answer for a UMD allocation that did not ask for scan-out is a buffer: Width is the byte count, Height 1,
 * Format D3DDDIFMT_UNKNOWN, no refresh rate and no rotation. It does not depend on the display signal: a buffer is
 * nobody's primary, and the answer must not fail on an adapter without a connected display. An allocation of more
 * than 4 GiB - 1 bytes has no 32-bit Width and is refused. LB7A surfaces and a BC2A scan-out request keep the
 * surface answer of the primary, as before. */
#pragma once

#include "wddm_level.h"

enum bc250_describe_route {
    BC250_DESCRIBE_SURFACE = 0,         /* LB7A or a BC2A scan-out request: the primary's answer, as before */
    BC250_DESCRIBE_BUFFER = 1,          /* BC2A, no scan-out: Width = bytes, Height 1, Format UNKNOWN */
    BC250_DESCRIBE_REFUSED = 2,         /* BC2A, no scan-out, but no 32-bit Width: STATUS_INVALID_PARAMETER */
};

#define BC250_CROSS_ADAPTER_MAX_WIDTH 0xFFFFFFFFull

/* The cap is reported from WDDM 2.4, the level from which the HLK job "WDDM2.4 Mandatory Caps"
 * (FeatureCheck::VerifyWddm24Caps) requires it. Below 2.4 (a WddmVersionCap setting of 2.0 to 2.2, a lab bisect)
 * the bit stays clear and dxgkrnl refuses every cross-adapter allocation itself, as before. */
static __inline int Bc250CrossAdapterCap(unsigned long Level)
{
    return Level >= BC250_WDDM_LEVEL_2_4;
}

/* UmdAlloc: the object is a BC2A allocation. ScanoutRequested: it asked for scan-out (BC2A v3). Bytes: the byte
 * count the blob asked for. */
static __inline enum bc250_describe_route Bc250DescribeRoute(int UmdAlloc, int ScanoutRequested, unsigned long long Bytes)
{
    if (!UmdAlloc || ScanoutRequested) return BC250_DESCRIBE_SURFACE;
    if (Bytes == 0 || Bytes > BC250_CROSS_ADAPTER_MAX_WIDTH) return BC250_DESCRIBE_REFUSED;
    return BC250_DESCRIBE_BUFFER;
}
