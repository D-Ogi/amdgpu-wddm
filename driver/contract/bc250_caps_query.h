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
