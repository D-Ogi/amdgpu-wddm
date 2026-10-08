// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-surface.h"
#include "runtime-surface-format.h"
namespace bc250::umd {
// M15.14: the answer of pfnCheckDirectFlipSupport, as a function of the two surfaces alone, so that a
// host gate drives the production rule rather than a copy of it (ddi-wddm2-test.cpp).
//
// The runtime asks whether the application's back buffer may take the place of the window's current
// front buffer with no copy in between. Answering TRUE is not a promise of hardware scan-out: the
// kernel driver's SetVidPnSourceAddress still re-derives the POST geometry, the pitch, the 4 KiB
// address and the segment, and the OS composes the window again whenever something overlaps it.
// Answering TRUE wrongly, though, costs a copy the runtime no longer makes, so the rule is the
// narrowest one that can be true of both surfaces:
//
//   - both are runtime surfaces of this shell, in the ready phase, with storage of their own;
//   - both are primaries of the same video present source - a surface the runtime never gave a
//     primary descriptor is a window buffer, not a front buffer;
//   - both asked the kernel driver for scan-out, through their own resource record's SCANOUT bit.
//     This is the clause that keeps the answer honest: a primary without the bit is
//     aperture-resident, the display core cannot read the aperture, and SetVidPnSourceAddress would
//     be given an address DcnTranslateCardAddress refuses - after the runtime had already stopped
//     copying. The shell sets the bit only under the opt-in switch of scanout-primary.h (M15.14
//     increment 3), so by default it answers FALSE, as it did before M15.14. Changing the answer
//     without changing the placement is the one thing this function must not do;
//   - both carry a format the shared surface format table enables for SCANOUT_PRIMARY. On this part
//     that is the 8-bit rows only, which is also what the kernel driver admits, so a 10-bit or FP16
//     swap chain keeps the composed-primary path of M14.1;
//   - the two layouts agree exactly: width, height, pitch, and the same storage row of that table. A
//     flip substitutes one allocation for the other under one display mode; a difference in any of
//     them would be a different mode. The row and not the DXGI format, so that a BGRA8_UNORM front
//     buffer and a BGRA8_UNORM_SRGB back buffer are one pair - they share the storage row, as they
//     already do for composition, and sRGB is a view of the same bytes.
//
// IMMEDIATE (D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE) says the runtime would present without waiting
// for the vertical blank. Nothing in the rule above changes with it: the shell neither queues the flip
// nor owns the retire, and refusing the flag would only send an immediate present back to a copy.
inline bool direct_flip_supported(const RuntimeSurface *front,const RuntimeSurface *back) {
    if (!front || !back || front==back) return false;
    if (front->phase!=SurfacePhase::ready || back->phase!=SurfacePhase::ready) return false;
    if (!front->allocation.allocation || !back->allocation.allocation) return false;
    if (!front->primary || !back->primary || front->vidpn_source!=back->vidpn_source) return false;
    if (!front->scanout || !back->scanout) return false;
    const auto *row=runtime_scanout_format(front->desc.Format);
    if (!row || row!=runtime_scanout_format(back->desc.Format)) return false;
    return front->desc.Width==back->desc.Width && front->desc.Height==back->desc.Height &&
        front->pitch==back->pitch && front->pitch!=0;
}
}
