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

#define BC250_SCANOUT_CAPS_OFFSET (BC250_ADAPTER_IDENTITY_OFFSET+BC250_ADAPTER_IDENTITY_BYTES) /* 1496 */
#define BC250_SCANOUT_CAPS_MAGIC 0x46533242u   /* B2SF */
#define BC250_SCANOUT_CAPS_VERSION 1u
#define BC250_SCANOUT_CAPS_BYTES 24u
#define BC250_SCANOUT_CAPS_TOTAL (BC250_SCANOUT_CAPS_OFFSET+BC250_SCANOUT_CAPS_BYTES) /* 1520 */
// This start admits a client scan-out flip. Absent, or zero, means the answer stays what 0.7.207.1 gave.
#define BC250_SCANOUT_CAPS_DIRECT_FLIP 0x1u

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
