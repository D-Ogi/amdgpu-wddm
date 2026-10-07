// SPDX-License-Identifier: MIT
#pragma once
// M15.14 increment 2. Optional trailer that follows the adapter identity trailer, which itself follows
// the unchanged v3 UMDRIVERPRIVATE caps blob. It carries the two start-latched facts the compositor's
// user-mode driver must know before it may answer CheckDirectFlipSupport with TRUE:
//
//   flags  whether this adapter start will admit a client scan-out flip at all. It is the operator's
//          EnableDirectFlipHandshake value ANDed with EnableScanoutAdmit, so a closed kernel gate can
//          never leave the shell agreeing to a flip the kernel driver would refuse.
//   post_* the POST mode's geometry, which is the only video present source mode display.c offers and
//          therefore the only geometry Bc250ScanoutAdmit admits. The shell applies it directly instead
//          of inferring it from the compositor's own chain being the desktop's size.
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
// pixel format. flags is the trailer's flags word; a reader without a trailer passes 0. The D3D12 shell
// (scanout-mode.h) and the router's front (front-direct-flip.h) both call this, so they cannot disagree.
static __inline int bc250_scanout_format_admitted(const AMDGPU_WDDM_SURFACE_FORMAT* row, unsigned int flags)
{
    if (!amdgpu_wddm_surface_admit(row, AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY)) return 0;
    if (row->policy & AMDGPU_WDDM_SURFACE_FIRMWARE_PLANE) return 1;
    return (flags & BC250_SCANOUT_CAPS_PLANE_FORMATS) != 0;
}
