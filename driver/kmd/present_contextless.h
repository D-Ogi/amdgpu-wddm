/* BD-403: DXGK_DRIVERCAPS.MiscCaps.SupportContextlessPresent and the DxgkDdiPresent calls that come without a
 * context. Plain C without WDK types, so the decision is tested on the development PC
 * (driver/kmd/test/present_contextless_test.c, the 'present-contextless' gate of tools/quality/quick.ps1).
 *
 * The contract, from the sources in the order they win:
 *   - WDK 10.0.26100 shared/d3dkmddi.h:2462-2467: the bit is the first member of DXGK_DRIVERCAPS.MiscCaps, inside
 *     the DXGKDDI_INTERFACE_VERSION_WDDM2_4 guard. The header has no comment on it.
 *   - ns-d3dkmddi-_dxgk_drivercaps.md:261-263: "Supports null context in DDI calls. When this value is set, the OS
 *     will pass NULL context in present related DDIs. Supported starting in WDDM 2.4." The page does not name the
 *     DDIs.
 *   - dxgkrnl 10.0.22621.6197, static reading (evidence/windows/2026-11-25-bd403-contextless-present/, fact M971):
 *     DXGADAPTER reads MiscCaps only when the reported version is 2.4 or higher, and keeps bit 0 as one byte.
 *     PrepareIndependentFlipToken is the one caller of ADAPTER_RENDER::DdiPresent that reads that byte. With the
 *     byte set it passes hContext NULL, also when the token has a context. The DXGKARG_PRESENT it builds has
 *     Flags.RedirectedFlip (and the stereo and multiplane-overlay bits of the token), no DMA buffer, no
 *     DMA private data, and NumSrcAllocations entries of DXGK_PRESENTALLOCATIONINFO from index 0. The
 *     DMA-buffer presents (DXGCONTEXT::SubmitPresent, SubmitPresentWithDmaBuffer) do not read the byte: a Blt
 *     keeps its context.
 *   - HLK "WDDM2.4 Mandatory Caps" (featurecheck, FeatureCheck::VerifyWddm24Caps): "SupportContextlessPresent
 *     required on WDDM2.4+".
 *
 * So a present without a context asks for no GPU work: nothing is built, and success is the answer. The DMA
 * producers of Bc250WddmPresent need a context (its root page table and allocation list), so a call without one
 * that still asks for a copy, a fill or a DMA buffer cannot be served. It is refused with STATUS_ILLEGAL_INSTRUCTION,
 * the row of the DxgkDdiPresent table for "instructions that graphics hardware cannot support", as WddmPresentDmaFlip
 * refuses a DMA-buffer flip (BD-170). Nothing is dropped in silence. */
#pragma once

#include "wddm_level.h"

enum bc250_present_route {
    BC250_PRESENT_WITH_CONTEXT = 0,         /* hContext names a context: the producers of Bc250WddmPresent decide */
    BC250_PRESENT_CONTEXTLESS_TOKEN = 1,    /* no context and no work asked: nothing to build, STATUS_SUCCESS */
    BC250_PRESENT_CONTEXTLESS_REFUSED = 2,  /* no context, but a copy, a fill or a DMA buffer: refused */
};

/* The cap is reported from WDDM 2.4: dxgkrnl reads MiscCaps from that version only, and the HLK requires the bit
 * there. Below 2.4 (a WddmVersionCap setting of 2.0 to 2.2) the bit stays clear, so every present keeps its
 * context as before. */
static __inline int Bc250ContextlessPresentCap(unsigned long Level)
{
    return Level >= BC250_WDDM_LEVEL_2_4;
}

/* HasContext: hContext names a context of this driver. WantsCopy: Flags.Blt or Flags.ColorFill. HasDmaBuffer:
 * pDmaBuffer is not NULL. */
static __inline enum bc250_present_route Bc250PresentRoute(int HasContext, int WantsCopy, int HasDmaBuffer)
{
    if (HasContext) return BC250_PRESENT_WITH_CONTEXT;
    if (WantsCopy || HasDmaBuffer) return BC250_PRESENT_CONTEXTLESS_REFUSED;
    return BC250_PRESENT_CONTEXTLESS_TOKEN;
}
