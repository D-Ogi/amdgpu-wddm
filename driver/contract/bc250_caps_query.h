// SPDX-License-Identifier: MIT
#pragma once
#include "bc250_scanout_caps.h"

// Private caps have three published extents, including partial optional trailers.
// A larger buffer is not evidence of a caller that understands our wire format:
// AGS uses a different vendor-private format and interprets our byte count as an
// adapter count. Refuse foreign extents before publishing any bytes. Add a new
// extent here only together with its versioned reader and compatibility tests.
static inline int Bc250CapsQueryExtentSupported(unsigned int bytes)
{
    return bytes <= BC250_SCANOUT_CAPS_TOTAL;
}

// AMD's own 608-byte block, which the FidelityFX upscaler and frame-generation DLLs ask for (family at 0x30,
// external revision at 0x34; docs/design/fsr4-identity.md). It is a separate answer, not an extent of our caps:
// no reader of ours asks this size, and the block has no BC25 signature. The KMD answers it before the
// BUFFER_TOO_SMALL check of our caps (driver/kmd/amd_identity.c).
#define BC250_AMD_IDENTITY_BYTES 608u
#define BC250_AMD_IDENTITY_FAMILY_OFFSET 0x30u
#define BC250_AMD_IDENTITY_EREV_OFFSET 0x34u
static inline int Bc250AmdIdentityExtent(unsigned int bytes)
{
    return bytes == BC250_AMD_IDENTITY_BYTES;
}
