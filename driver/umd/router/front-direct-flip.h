// SPDX-License-Identifier: MIT
//
// M15.14: the answer of `pfnCheckDirectFlipSupport`, as a pure function of the adapter's published
// scan-out capabilities and the two surfaces alone, so that a host gate drives the production rule
// instead of a copy of it (tests/test-router.cpp).
//
// The runtime asks whether the application's back buffer may take the place of the compositor's front
// buffer with no copy in between. Answering TRUE is not a promise of hardware scan-out: the kernel
// driver's `SetVidPnSourceAddress` still re-derives the POST geometry, the pitch, the 4 KiB address and
// the segment. Answering TRUE wrongly is worse than a lost optimisation. The flip that then fails
// carries `SharedPrimaryTransition`, and the contract says of that case: "the operating system will not
// seamlessly fail back to composition mode, and presentation will be incorrect"
// (ref/ddi-display/d3dkmddi.md:12793). The screen goes black and nothing composes it again. So the rule
// is the narrowest one that can be true of both surfaces, and every clause has a name a trace prints.
//
// THE TWO SIDES ARE NOT SYMMETRIC. Both surfaces must be ones the display core can read, and they earn
// that differently (driver/kmd/gdi_private.h states the pair and the reason):
//
//   the application's   hResource1: a surface opened into the compositor's device
//                       (ref/ddi-display/d3d10umddi.md:8809-8817). Its E26R record travelled with the
//                       handle and says what its creator asked for. A shared surface is aperture
//                       resident unless its own SCANOUT bit moved it to the local segment, so here the
//                       bit is the whole question: `WddmGdiRecordScannable`.
//   the compositor's    hResource2: a surface this device created itself. It is not shared, so its
//                       type-0 placement is already VRAM with AccessedPhysically and the display core
//                       has been reading it at the refresh rate all along. It never asks for scan-out,
//                       and demanding the SCANOUT bit of it would make this rule refuse every pair the
//                       operating system can pass: `WddmGdiCreatedScannable`.
//
// NEVER `WddmGdiScannable(WddmGdiRecordPolicy(record))`. That composition is fail-OPEN by its own
// contract comment: it says yes about a surface whose record could not be read, because the kernel
// driver's CreateAllocation must still place a standard allocation that carries no record at all. Read
// as "can the display core read this", the same answer is a TRUE about an aperture-resident buffer whose
// address `DcnTranslateCardAddress` refuses - after the runtime had stopped copying. Both functions this
// rule uses refuse every record they could not read.
//
// Both derivations end in the kernel driver's own placement arithmetic, called here from user mode, so
// an answer given here and the placement given at `DxgkDdiCreateAllocation` cannot disagree.
//
// IMMEDIATE (`D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE`) says the runtime would present without waiting
// for the vertical blank. Nothing in the rule changes with it: the front neither queues the flip nor
// owns the retire, the flip is an address and a pitch with no swizzle that must change at VSync, and
// refusing the flag would only send an immediate present back to a copy.
#ifndef BC250_FRONT_DIRECT_FLIP_H
#define BC250_FRONT_DIRECT_FLIP_H

#include "front-resource.h"
#include "../../contract/bc250_scanout_caps.h"
#include "../../contract/amdgpu_wddm_surface_format.h"

namespace bc250front {

enum class FlipRefusal {
    none,
    handle,          // a null handle, or one surface given twice
    gated,           // the kernel driver published no DirectFlip admission for this adapter start
    record,          // one of the two has no front record: not created or opened through this front
    sides,           // hResource1 is not an opened surface, or hResource2 is not one this device created
    primary,         // one of them is not a primary of a swap chain
    vidpn_source,    // they do not name the one video present source this adapter has
    client_scannable,      // the application's record never asked for scan-out, or its placement is unreadable
    compositor_scannable,  // the display core cannot read the compositor's own buffer where it is placed
    format,          // not one storage row the shared table enables for SCANOUT_PRIMARY
    geometry,        // the two differ in width or height
    post_geometry,   // or they differ from the only mode this adapter offers
    pitch_unknown,   // one side's pitch is not known here yet; see the clause below
    pitch,           // the two differ in pitch
    forced_false,    // increment 1: the rule was computed for the log and the answer is FALSE anyway
};

inline const char *FlipRefusalText(FlipRefusal reason)
{
    switch (reason) {
    case FlipRefusal::none: return "supported";
    case FlipRefusal::handle: return "handle";
    case FlipRefusal::gated: return "gated";
    case FlipRefusal::record: return "record";
    case FlipRefusal::sides: return "sides";
    case FlipRefusal::primary: return "primary";
    case FlipRefusal::vidpn_source: return "vidpn-source";
    case FlipRefusal::client_scannable: return "client-scannable";
    case FlipRefusal::compositor_scannable: return "compositor-scannable";
    case FlipRefusal::format: return "format";
    case FlipRefusal::geometry: return "geometry";
    case FlipRefusal::post_geometry: return "post-geometry";
    case FlipRefusal::pitch_unknown: return "pitch-unknown";
    case FlipRefusal::pitch: return "pitch";
    case FlipRefusal::forced_false: return "forced-false";
    }
    return "unknown";
}

// `client` is hResource1 (the application's buffer, opened into this device) and `compositor` is
// hResource2 (this device's own front buffer). The rule is deliberately not symmetric in that order: a
// pair given the other way round is refused with "sides", which is a fail-safe FALSE and never a flip of
// the wrong buffer.
inline FlipRefusal FlipReason(const bc250_scanout_caps &caps, const Resource *client,
                              const Resource *compositor)
{
    if (!client || !compositor || client == compositor) return FlipRefusal::handle;
    // Clause 1. An older kernel driver, a closed EnableDirectFlipHandshake, a closed EnableScanoutAdmit,
    // a closed MMIO or VidPn flip path, or a POST mode the driver could not publish all land here, and
    // the answer is then exactly the one this stack gave before M15.14.
    if (caps.magic != BC250_SCANOUT_CAPS_MAGIC || caps.version != BC250_SCANOUT_CAPS_VERSION ||
        !(caps.flags & BC250_SCANOUT_CAPS_DIRECT_FLIP))
        return FlipRefusal::gated;
    if (!client->recorded || !compositor->recorded) return FlipRefusal::record;
    if (!client->opened || compositor->opened) return FlipRefusal::sides;
    if (!client->primary || !compositor->primary) return FlipRefusal::primary;
    if (client->vidpn_source != BC250_SCANOUT_VIDPN_SOURCE ||
        compositor->vidpn_source != BC250_SCANOUT_VIDPN_SOURCE)
        return FlipRefusal::vidpn_source;
    // The two derivations. Each reads its own E26R record and refuses one it could not read.
    if (!WddmGdiRecordScannable(&client->record, sizeof(client->record)))
        return FlipRefusal::client_scannable;
    if (!WddmGdiCreatedScannable(&compositor->record, sizeof(compositor->record)))
        return FlipRefusal::compositor_scannable;
    const AMDGPU_WDDM_SURFACE_FORMAT *row =
        amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_dxgi(client->format),
                                  AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY);
    if (!row || row != amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_dxgi(compositor->format),
                                                 AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY))
        return FlipRefusal::format;
    if (client->width != compositor->width || client->height != compositor->height)
        return FlipRefusal::geometry;
    // The geometry clause Bc250ScanoutAdmit will apply, said here rather than inferred from the
    // compositor's chain being the desktop's size: display.c offers the POST mode alone.
    if (client->width != caps.post_width || client->height != caps.post_height)
        return FlipRefusal::post_geometry;
    // The last clause, and the one increment 1 cannot satisfy. An opened surface carries its pitch in the
    // LB7A blob the kernel driver wrote, so the application's side is known. A surface this device created
    // does not: the kernel driver chooses the pitch at CreateAllocation and user mode never learns it.
    // Pitch 0 therefore means "not known here", which is a refusal of its own name rather than a silent
    // pass, and carrying the compositor's pitch into user mode is an increment-2 item
    // (docs/design/direct-flip-handshake.md). A trial that reaches pitch-unknown has passed every other
    // clause, which is exactly what the log must be able to say.
    if (!client->pitch || !compositor->pitch) return FlipRefusal::pitch_unknown;
    if (client->pitch != compositor->pitch) return FlipRefusal::pitch;
    return FlipRefusal::none;
}

inline bool FlipSupported(const bc250_scanout_caps &caps, const Resource *client,
                          const Resource *compositor)
{
    return FlipReason(caps, client, compositor) == FlipRefusal::none;
}

}  // namespace bc250front

#endif
