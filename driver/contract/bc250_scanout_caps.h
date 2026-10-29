// SPDX-License-Identifier: MIT
#pragma once
// M15.14 increment 2. Optional trailer that follows the adapter identity trailer, which itself follows
// the unchanged v3 UMDRIVERPRIVATE caps blob. It carries the two start-latched facts the compositor's
// user-mode driver must know before it may answer CheckDirectFlipSupport with TRUE:
//
//   flags  whether this adapter start will admit a client scan-out flip at all. It is the operator's
//          EnableDirectFlipHandshake value ANDed with EnableScanoutAdmit, so a closed kernel gate can
//          never leave the shell agreeing to a flip the kernel driver would refuse.
//   post_* the geometry Bc250ScanoutAdmit admits at the time of the query: the committed video present
//          source mode (display modes, KMD 0.7.216.19), or the POST mode's geometry when no smaller mode
//          is committed. The name keeps "post" for the ABI. A reader that keeps the value from adapter
//          open has the geometry of that moment; the kernel driver's admission is the final check. The
//          shell applies it directly instead of inferring it from the compositor's own chain being the
//          desktop's size.
//
// Read it the way bc250_adapter_identity is read: query a zero-initialized extended buffer and require
// every header field before using the payload. An older kernel driver succeeds and writes only the
// prefix it knows, so the reader sees zeros and must behave exactly as it did before this trailer.
//
// A driver that has this trailer and a closed switch writes nothing here either, rather than a header
// with flags 0: a start with the handshake off is then byte for byte the revision before it, for every
// buffer size, and a reader that keys on the magic and forgets to test the flag cannot act on a closed
// switch nor read a live POST geometry out of a start that offers no flip. "Absent" and "off" are one
// state on the wire, and the only correct reading of both is the behaviour from before M15.14.
//
// The reader must size its query buffer BC250_SCANOUT_CAPS_TOTAL, not BC250_ADAPTER_CAPS_BYTES: the
// driver writes the trailer only when the whole of it fits, so a 1496-byte query can never see it.
#include "bc250_adapter_identity.h"
#include "amdgpu_wddm_surface_format.h"

#define BC250_SCANOUT_CAPS_OFFSET (BC250_ADAPTER_IDENTITY_OFFSET+BC250_ADAPTER_IDENTITY_BYTES) /* 1496 */
#define BC250_SCANOUT_CAPS_MAGIC 0x46533242u   /* B2SF */
#define BC250_SCANOUT_CAPS_VERSION 1u
#define BC250_SCANOUT_CAPS_BYTES 24u
#define BC250_SCANOUT_CAPS_TOTAL (BC250_SCANOUT_CAPS_OFFSET+BC250_SCANOUT_CAPS_BYTES) /* 1520 */
// This start admits a client scan-out flip. Absent, or zero, means the answer stays what 0.7.207.1 gave.
#define BC250_SCANOUT_CAPS_DIRECT_FLIP 0x1u
// This start also programs the plane's pixel format at each flip, for every SCANOUT_PRIMARY row of the
// surface format table (driver/kmd/plane_format.h). The kernel driver sets it from 0.7.216.20, only with
// DIRECT_FLIP, and only when it read the firmware's format registers at start and found the BGRA8 encoding
// it restores to. Without it, a reader admits for scan-out only the rows that carry FIRMWARE_PLANE: a kernel
// driver that does not program the format refuses any other row after the OS has taken
// SharedPrimaryTransition, which blanks the output (ref/ddi-display/d3dkmddi.md:12793).
#define BC250_SCANOUT_CAPS_PLANE_FORMATS 0x2u
// The committed video present source mode is a 10-bit mode (D3DDDIFMT_A2B10G10R10), so a flip of an
// RGB10A2 primary would keep the mode's pixel depth. No kernel driver sets it yet. BD-224, measured on
// Windows 11 23H2 (22631) with this kernel driver: under the 8-bit desktop mode, dxgkrnl sent no flip of a
// 10-bit primary to the kernel driver during the whole session. The compositor enters
// the independent flip transition, leaves it 150 us later with no call to the driver, blanks the source
// (SetVidPnSourceVisibility FALSE), rebuilds its own primaries and shows them again, about twice a second
// while the game presents: the screen is black most of the time (Clair Obscur: Expedition 33, UE5, a
// R10G10B10A2 chain of 1920x1200, lab 2026-10-23). The 8-bit rows RGBA8 and BGRA8 flip under that mode
// (The Witcher 3, Quake II RTX). Whether another OS build or a committed 10-bit mode flips such a primary is
// not measured. So a 10-bit row is admitted only while the kernel driver says the committed mode has the
// same depth, and until a driver measures such a flip end to end the row stays a composed primary.
#define BC250_SCANOUT_CAPS_SOURCE_MODE_10BIT 0x4u
// BD-223 (docs/design/mpo.md, the user-mode contract): this start reports DXGK_DRIVERCAPS
// SupportMultiPlaneOverlay and answers DxgkDdiCheckMultiPlaneOverlaySupport2 and
// DxgkDdiSetVidPnSourceAddressWithMultiPlaneOverlay2. The kernel driver sets it only with DIRECT_FLIP. The
// compositor's front publishes the WDDM 1.3 interface and the DXGI 1.3 table only while this bit is set, and
// the application shells admit a scan-out primary smaller than the source mode only while it is set
// (bc250_scanout_mpo_geometry). The kernel driver's check is the authority for every plane configuration:
// the user-mode caps below are advisory, and a configuration the kernel driver refuses is composed by DWM.
#define BC250_SCANOUT_CAPS_MPO 0x8u
// The kernel driver also scans out an overlay plane on a second pipe (MaxOverlayPlanes 2). No kernel driver
// sets it yet; until one does, the user-mode MaxPlanes is 1.
#define BC250_SCANOUT_CAPS_MPO_PLANE1 0x10u

// The plane limits that the user-mode multiplane-overlay caps report and that the shells apply to a scan-out
// primary smaller than the source mode. They are the kernel driver's stage limits (driver/kmd/dcn_plane.h):
// the DCN 2.0.1 plane cap of 16 times up (Linux dcn201_resource.c plane_cap max_upscale_factor 16000), no
// downscale yet (max_downscale_factor 250 needs the DPP clock and DCHUB step), and the 64-pixel minimum of
// the same plane cap. A kernel driver that lifts a limit changes it here in the same commit.
#define BC250_MPO_MAX_STRETCH_MILLI 16000u
#define BC250_MPO_MAX_SHRINK_MILLI 1000u
#define BC250_MPO_MIN_DIMENSION 64u

struct bc250_scanout_caps {
    unsigned int magic;
    unsigned int version;
    unsigned int size;
    unsigned int flags;
    unsigned int post_width;
    unsigned int post_height;
};
typedef char bc250_scanout_caps_size_check[
    sizeof(struct bc250_scanout_caps)==BC250_SCANOUT_CAPS_BYTES ? 1 : -1];

// The one user-mode rule for "may a surface of this row be scanned out on this start". The row must be a
// SCANOUT_PRIMARY row of the shared table. A row that is not the firmware's own format also needs
// BC250_SCANOUT_CAPS_PLANE_FORMATS, because only a kernel driver that publishes it programs the plane's
// pixel format. A 10-bit row (RGB10A2) also needs BC250_SCANOUT_CAPS_SOURCE_MODE_10BIT, because on the
// measured configuration dxgkrnl flipped no 10-bit primary under the 8-bit mode (BD-224, above). flags is
// the trailer's flags
// word; a reader without a trailer passes 0. The D3D12 shell (scanout-mode.h), the D3D11 shell
// (runtime-surface-format.h) and the router's front (front-direct-flip.h) all call this, so they cannot
// disagree.
static __inline int bc250_scanout_row_is_10bit(const AMDGPU_WDDM_SURFACE_FORMAT* row)
{
    return row->d3dddi == AMDGPU_WDDM_D3DDDI_A2B10G10R10;
}

static __inline int bc250_scanout_format_admitted(const AMDGPU_WDDM_SURFACE_FORMAT* row, unsigned int flags)
{
    if (!amdgpu_wddm_surface_admit(row, AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY)) return 0;
    if (row->policy & AMDGPU_WDDM_SURFACE_FIRMWARE_PLANE) return 1;
    if (!(flags & BC250_SCANOUT_CAPS_PLANE_FORMATS)) return 0;
    if (bc250_scanout_row_is_10bit(row) && !(flags & BC250_SCANOUT_CAPS_SOURCE_MODE_10BIT)) return 0;
    return 1;
}

// BD-223: may a scan-out primary of width x height exist although it is not the source mode? Only while the
// kernel driver answers the multiplane-overlay check (BC250_SCANOUT_CAPS_MPO), and only for a surface that
// plane 0 can scale to the whole source mode: at least BC250_MPO_MIN_DIMENSION in each direction, not larger
// than the mode, and at most BC250_MPO_MAX_STRETCH_MILLI / 1000 times up. Such a primary is never flipped by
// SetVidPnSourceAddress (Bc250ScanoutAdmit refuses a size other than the mode, and the front's
// CheckDirectFlipSupport answers only for the committed geometry): it reaches the plane only through
// SetVidPnSourceAddressWithMultiPlaneOverlay2, after the kernel driver's check approved its shape.
static __inline int bc250_scanout_mpo_geometry(const struct bc250_scanout_caps* caps, unsigned int width,
    unsigned int height)
{
    if (!caps || !(caps->flags & BC250_SCANOUT_CAPS_MPO) || !(caps->flags & BC250_SCANOUT_CAPS_DIRECT_FLIP))
        return 0;
    if (width < BC250_MPO_MIN_DIMENSION || height < BC250_MPO_MIN_DIMENSION) return 0;
    if (width > caps->post_width || height > caps->post_height) return 0;
    return (unsigned long long)caps->post_width * 1000ull <= (unsigned long long)width * BC250_MPO_MAX_STRETCH_MILLI &&
           (unsigned long long)caps->post_height * 1000ull <= (unsigned long long)height * BC250_MPO_MAX_STRETCH_MILLI;
}
