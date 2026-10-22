// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "dcn_surface_layout.h"

enum BC250_INPUT_COLOR {
    BC250_INPUT_RGB_FULL,
    BC250_INPUT_601_FULL, BC250_INPUT_601_LIMITED,
    BC250_INPUT_709_FULL, BC250_INPUT_709_LIMITED,
    BC250_INPUT_COLOR_COUNT
};
typedef struct BC250_CNVC_REQUEST {
    unsigned Format; // BC250_LINEAR_FORMAT, matched to the surface plan by owner.
    unsigned Color;  // Explicit: RGB_FULL for RGB; one of the four YUV choices.
} BC250_CNVC_REQUEST;
typedef struct BC250_CNVC_PLAN {
    unsigned long FormatControl, PixelFormat, AlphaLut;
    unsigned short Matrix[12]; // Original AMD signed S2.13 coefficient encodings.
    unsigned Format, Color, CscRequired, HasPixelAlpha, WriteAlphaLut, Ready;
} BC250_CNVC_PLAN;

// CPU-only preparation through the selected AMD DCN201 function. No MMIO or
// bank choice, no format/capability admission. Includes zero expansion as used
// by dcn20_program_pipe, not a proof of physical sample normalization. RGB CSC
// is bypassed. Packed YUV is internal AMD_AYCRCB8, not verified DXGI AYUV.
// Owner must supply matching HUBP/DSCL/MPC state, OBUF power and proven CM
// normalization/bypass state, preserve bank lifetime, and atomically commit all
// plane state. DCN201 has no selected bias-and-scale callback; do not assume
// the generic BNS implementation provides a missing normalization correction.
// Values describe writable fields, not permission to replace whole registers.
// Rejects unknown/mismatched spaces. Input/output may overlap; failure clears.
int Bc250CnvcPrepare(const BC250_CNVC_REQUEST* Request, BC250_CNVC_PLAN* Plan);
