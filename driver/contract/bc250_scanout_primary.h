// SPDX-License-Identifier: MIT
#pragma once
// M15.14 increment 3: the one rule that decides whether an application shell asks the kernel driver for a
// scan-out primary in place of the composed primary. Both application shells call it: the D3D12 shell
// (driver/umd/d3d12/scanout-mode.h) and the D3D11 shell (driver/umd/dxvk/scanout-primary.h). Each shell
// asks its own switches first; this rule holds everything the two have in common, so the two cannot
// disagree about a chain.
//
// The rule is a stand-down, never a failure. Every answer except ADMITTED keeps the composed primary that
// each shell made before M15.14: the same buffer, in the shared aperture, with the CPU mapping that the
// compositor's readers need. A scan-out request that the kernel driver would refuse at flip time is worse
// than no request: the buffer moves to the local segment with no CPU access, and the refusal comes after
// the OS took SharedPrimaryTransition, which does not fall back to composition seamlessly (a black output).
//
// The clauses, in the order they are asked:
//   FORCE_CPU        the desktop route's kill switch DwmForceCpu is on, so the compositor is the CPU UMD. It
//                    reads every primary on the CPU, it has no CheckDirectFlipSupport and no flip exists,
//                    and the request alone would cost that reader its write-combined aperture mapping
//                    (experiments 104 and 107).
//   DESKTOP_ROUTE    the compositor did not publish the GPU route (bc250_desktop_route.h): the router in
//                    dwm.exe sent it to the CPU UMD (the kill switch, the kernel driver's desktop switches,
//                    or a hosted open that failed), or no record from the compositor's account exists. The
//                    stand-down has the reason of FORCE_CPU. This clause also covers the fallback, which no
//                    registry value shows.
//   CAPS_CLOSED      the kernel driver published no scan-out trailer, or one without
//                    BC250_SCANOUT_CAPS_DIRECT_FLIP: an older driver, or an operator switch that is off.
//   SOURCE_GEOMETRY  the chain is neither the geometry of the source mode that the trailer carries, which is
//                    the one geometry Bc250ScanoutAdmit admits a flip at, nor a source mode that the
//                    kernel driver offers for the video present source (offered_mode). The caller reads the
//                    trailer again for every primary it creates, so the clause follows the source mode that
//                    the kernel driver has committed at that moment (display modes) and never a size of the
//                    caller's own. The router's front applies the committed half on the compositor's side.
//                    The offered half exists because a game can make its chain before the mode commit: in
//                    session 480 The Witcher 3 made its exclusive 1920x1080 chain while the trailer still
//                    said 1920x1200, and its 1920x1200 chain on the way back while the trailer said
//                    1920x1080, so both stayed composed. Admitting such a chain moves no flip forward:
//                    the scan-out property is fixed at creation, and every flip of the chain is decided
//                    later by two checks that read the committed mode at that moment, the front's
//                    CheckDirectFlipSupport answer and Bc250ScanoutAdmit. Until the commit, the compositor
//                    composes the chain with the GPU, as it composes a scan-out chain under a window. A
//                    geometry the kernel driver does not offer can never be committed, so it stays here.
//   FORMAT           the chain's format is not a SCANOUT_PRIMARY row of the shared table, or is a row with
//                    no DXGI name (the compositor's opener could not take its record), or is a row other
//                    than the firmware's own format while the trailer lacks BC250_SCANOUT_CAPS_PLANE_FORMATS
//                    (bc250_scanout_format_admitted: a kernel driver before 0.7.216.20 does not program the
//                    plane's pixel format, and its refusal would come after SharedPrimaryTransition). RGBA8
//                    and RGB10A2 are SCANOUT_PRIMARY rows, so this clause is what keeps them composed on an
//                    older kernel driver.
//   PITCH            the row pitch is not bc250_scanout_primary_pitch. Nothing downstream can check a pitch
//                    that only the shell knows.
//
// The caller's own switches (the shell's off switch, an opposite intent for the same buffer) come before
// these clauses, so that a trial reads "the operator said no" and not "the kernel driver said no".
#include "bc250_scanout_caps.h"
#include "amdgpu_wddm_surface_format.h"

#define BC250_SCANOUT_PRIMARY_ADMITTED        0u
#define BC250_SCANOUT_PRIMARY_FORCE_CPU       1u
#define BC250_SCANOUT_PRIMARY_CAPS_CLOSED     2u
#define BC250_SCANOUT_PRIMARY_SOURCE_GEOMETRY 3u
#define BC250_SCANOUT_PRIMARY_FORMAT          4u
#define BC250_SCANOUT_PRIMARY_PITCH           5u
#define BC250_SCANOUT_PRIMARY_DESKTOP_ROUTE   6u   /* asked second; numbered last so the older numbers stay */

// The byte pitch of a scan-out row, and the only one every component derives on its own: the compositor's
// hosted driver rounds a row (width * bytes_per_pixel) up to 256 bytes (the router's HostedSurfacePitch),
// and for a 4-byte row that is also the kernel driver's primary layout, DcnPrimaryPitch
// (driver/kmd/dcn_translate.c), which rounds the width up to 64 pixels of 4 bytes. The D3D11 shell's
// runtime surfaces use the same rounding (runtime_surface_pitch). 0 for a width or a row size that this
// pitch cannot express.
static __inline unsigned int bc250_scanout_primary_pitch(unsigned int width, unsigned int bytes_per_pixel)
{
    const unsigned long long row = (unsigned long long)width * bytes_per_pixel;
    if (!width || !bytes_per_pixel || row > 0xffffff00ull) return 0u;
    return (unsigned int)((row + 255ull) & ~255ull);
}

// caps is the trailer as read for this primary (all zero when there is none), force_cpu the desktop
// router's kill switch as that router reads it (any non-zero value is on), desktop_gpu whether the
// compositor's record says GPU (bc250_desktop_route_gpu, read for this primary), dxgi the chain's DXGI
// format, width, height and pitch the chain's buffer as the shell describes it to the kernel driver, and
// offered_mode whether width x height is a source mode that the kernel driver offers for this video present
// source now (the D3D12 shell asks D3DKMTGetDisplayModeList, and only for a chain that is not the committed
// mode; the D3D11 shell compares the primary descriptor's ModeDesc, the mode the runtime sets with this
// primary; 0 keeps the committed mode as the only geometry).
static __inline unsigned int bc250_scanout_primary_rule(const struct bc250_scanout_caps* caps,
    unsigned long force_cpu, int desktop_gpu, unsigned int dxgi, unsigned int width, unsigned int height,
    unsigned int pitch, int offered_mode)
{
    const AMDGPU_WDDM_SURFACE_FORMAT* row;
    if (force_cpu) return BC250_SCANOUT_PRIMARY_FORCE_CPU;
    if (!desktop_gpu) return BC250_SCANOUT_PRIMARY_DESKTOP_ROUTE;
    if (!caps || caps->magic != BC250_SCANOUT_CAPS_MAGIC || caps->version != BC250_SCANOUT_CAPS_VERSION ||
        !(caps->flags & BC250_SCANOUT_CAPS_DIRECT_FLIP))
        return BC250_SCANOUT_PRIMARY_CAPS_CLOSED;
    if (!width || !height ||
        ((width != caps->post_width || height != caps->post_height) && !offered_mode))
        return BC250_SCANOUT_PRIMARY_SOURCE_GEOMETRY;
    row = amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_dxgi(dxgi), AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY);
    if (!row || !row->dxgi || !bc250_scanout_format_admitted(row, caps->flags))
        return BC250_SCANOUT_PRIMARY_FORMAT;
    if (!pitch || pitch != bc250_scanout_primary_pitch(width, row->bytes_per_pixel))
        return BC250_SCANOUT_PRIMARY_PITCH;
    return BC250_SCANOUT_PRIMARY_ADMITTED;
}

static __inline const char* bc250_scanout_primary_text(unsigned int reason)
{
    switch (reason) {
    case BC250_SCANOUT_PRIMARY_ADMITTED: return "admitted";
    case BC250_SCANOUT_PRIMARY_FORCE_CPU: return "force-cpu";
    case BC250_SCANOUT_PRIMARY_CAPS_CLOSED: return "caps-closed";
    case BC250_SCANOUT_PRIMARY_SOURCE_GEOMETRY: return "source-geometry";
    case BC250_SCANOUT_PRIMARY_FORMAT: return "format";
    case BC250_SCANOUT_PRIMARY_PITCH: return "pitch";
    case BC250_SCANOUT_PRIMARY_DESKTOP_ROUTE: return "desktop-route";
    default: return "unknown";
    }
}
