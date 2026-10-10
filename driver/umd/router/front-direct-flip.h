// SPDX-License-Identifier: MIT
//
// M15.14: the answer of `pfnCheckDirectFlipSupport`, as a pure function of the adapter's published
// scan-out capabilities and the two surfaces alone, so that a host gate drives the production rule
// instead of a copy of it (tests/test-router.cpp). From increment 2 the entry writes this answer: TRUE
// exactly when every clause below holds (front-device.cpp, CheckDirectFlipSupport).
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
// for the vertical blank. For a pair of one and the same swizzle nothing in the rule changes with it:
// the front neither queues the flip nor owns the retire, and refusing the flag would only send an
// immediate present back to a copy.
//
// THE TWO FORMATS MAY DIFFER (0.7.216.20), BUT NOT UNDER IMMEDIATE (0.7.216.26, audit finding K4). The
// contract asks for compatible "swizzle formats" and then says: "If the swizzle can only be changed at
// every VSync interval, ensure that the CheckDirectFlipFlags parameter does not have a value of
// D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE" (ref/ddi-display/d3d10umddi.md:8833-8838). This hardware can
// change the swizzle at VSync and at no other moment: the kernel driver writes the plane's pixel format,
// its red/blue crossbar and the address of every flip in one OTG master-update-lock window, and it
// clears SURFACE_FLIP_TYPE, so both latch at the next VUPDATE (driver/kmd/dcn.c, DcnFlipWriteSequence).
// Latching them together keeps the colours right - no frame shows one buffer's bytes read in the other
// buffer's order - but it does not make the swizzle changeable off the vertical blank, which is the only
// thing the clause asks about. The earlier comment read that atomicity as an exemption; it is not one,
// and an answer of TRUE here is given before the runtime commits to DirectFlip, so later ignoring the
// immediate request cannot repair it.
//
// So: an RGBA8 back buffer may take the place of the compositor's BGRA8 front buffer when both rows pass
// bc250_scanout_format_admitted with the trailer's flags and have the same bytes per pixel, and the
// question is not an IMMEDIATE one (`FlipRefusal::immediate_swizzle` when it is). The kernel driver
// programs the client's format at the flip and the firmware's BGRA8 at the flip back. A pair of the same
// row changes no swizzle at all and keeps the IMMEDIATE answer it had. The day a plane can take a new
// format off the vertical blank, that mechanism - and not this comment - lifts the clause.
#ifndef BC250_FRONT_DIRECT_FLIP_H
#define BC250_FRONT_DIRECT_FLIP_H

#include "front-resource.h"
#include "front-flip-log.h"
#include <cstdio>
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
    format,          // a side's row is not admitted for scan-out on this start
                     // (bc250_scanout_format_admitted), or the two rows differ in bytes per pixel
    geometry,        // the two differ in width or height
    source_geometry, // or they differ from the source mode the kernel driver admits a flip at now
    pitch_unknown,   // one side's pitch is not known here; see the clause below
    pitch,           // the two differ in pitch
    immediate_swizzle,  // the question is IMMEDIATE and the two rows need a VSync-only swizzle change
};

// D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE (d3d10umddi.h). Mirrored so that this rule stays a pure
// function of its arguments; front-device.cpp static_asserts the mirror against the WDK enumeration.
static const unsigned kFlipCheckImmediate = 0x00000001u;

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
    case FlipRefusal::source_geometry: return "source-geometry";
    case FlipRefusal::pitch_unknown: return "pitch-unknown";
    case FlipRefusal::pitch: return "pitch";
    case FlipRefusal::immediate_swizzle: return "immediate-swizzle";
    }
    return "unknown";
}

// `client` is hResource1 (the application's buffer, opened into this device) and `compositor` is
// hResource2 (this device's own front buffer). The rule is deliberately not symmetric in that order: a
// pair given the other way round is refused with "sides", which is a fail-safe FALSE and never a flip of
// the wrong buffer.
inline FlipRefusal FlipReason(const bc250_scanout_caps &caps, const Resource *client,
                              const Resource *compositor, unsigned checkFlags)
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
    // Each side's row on its own: the shared table's SCANOUT_PRIMARY bit, and for a row other than the
    // firmware's own format the trailer's PLANE_FORMATS flag (an older kernel driver does not program the
    // plane's pixel format). The rows may differ; see THE TWO FORMATS MAY DIFFER above.
    const AMDGPU_WDDM_SURFACE_FORMAT *row = amdgpu_wddm_surface_format_by_dxgi(client->format);
    const AMDGPU_WDDM_SURFACE_FORMAT *own = amdgpu_wddm_surface_format_by_dxgi(compositor->format);
    if (!bc250_scanout_format_admitted(row, caps.flags) || !bc250_scanout_format_admitted(own, caps.flags) ||
        row->bytes_per_pixel != own->bytes_per_pixel)
        return FlipRefusal::format;
    // The IMMEDIATE clause. Two different rows mean the plane's pixel format and its red/blue crossbar
    // must change, and this hardware changes them at VUPDATE alone, so the contract forbids the TRUE for
    // an IMMEDIATE question (see THE TWO FORMATS MAY DIFFER above). One and the same row changes no
    // swizzle, so it keeps the answer it had before this clause existed.
    if ((checkFlags & kFlipCheckImmediate) != 0u && row->d3dddi != own->d3dddi)
        return FlipRefusal::immediate_swizzle;
    if (client->width != compositor->width || client->height != compositor->height)
        return FlipRefusal::geometry;
    // The geometry clause Bc250ScanoutAdmit will apply, said here rather than inferred from the
    // compositor's chain being the desktop's size. The trailer carries the geometry of the source mode at
    // which the kernel driver admits a flip when the query runs, and the caller reads the trailer again
    // for every question, so this rule assumes no mode: today display.c offers the POST mode alone, and a
    // driver that offers more modes writes the committed one into the same two fields.
    if (client->width != caps.post_width || client->height != caps.post_height)
        return FlipRefusal::source_geometry;
    // The last clause. An opened surface carries its pitch in the LB7A blob its creator wrote, so the
    // application's side is known. The compositor's side is the pitch the hosted user-mode driver gives a
    // surface it allocates itself (HostedSurfacePitch in front-resource.h). Pitch 0 means "not known
    // here", which is a refusal of its own name rather than a silent pass: a format with no row in the
    // shared table leaves it 0. A trial that reaches pitch-unknown has passed every other clause, which is
    // exactly what the log must be able to say.
    if (!client->pitch || !compositor->pitch) return FlipRefusal::pitch_unknown;
    if (client->pitch != compositor->pitch) return FlipRefusal::pitch;
    return FlipRefusal::none;
}

inline bool FlipSupported(const bc250_scanout_caps &caps, const Resource *client,
                          const Resource *compositor, unsigned checkFlags)
{
    return FlipReason(caps, client, compositor, checkFlags) == FlipRefusal::none;
}

// The log counts every answer per rule (front-flip-log.h), one counter per value of the enum above.
static_assert(static_cast<unsigned>(FlipRefusal::immediate_swizzle) + 1 == kFlipRules,
              "FlipRefusal changed: kFlipRules in front-flip-log.h must count every rule");
static_assert(static_cast<unsigned>(FlipRefusal::none) == 0, "rules[0] of the flip log is the TRUE count");

// The rules part of a summary line: "supported:12,gated:3", in the enum's order, rules with a zero count left
// out, "none" when there was no call. Always terminated; a short buffer cuts the list, never the line.
inline void FlipLogRules(const FlipLog *log, char *out, size_t size)
{
    if (!out || !size) return;
    out[0] = 0;
    size_t used = 0;
    for (unsigned i = 0; i < kFlipRules; ++i) {
        const long count = log->rules[i];
        if (!count) continue;
        const int n = _snprintf_s(out + used, size - used, _TRUNCATE, "%s%s:%ld", used ? "," : "",
                                  FlipRefusalText(static_cast<FlipRefusal>(i)), count);
        if (n < 0) return;
        used += (size_t)n;
    }
    if (!used) _snprintf_s(out, size, _TRUNCATE, "none");
}

}  // namespace bc250front

#endif
