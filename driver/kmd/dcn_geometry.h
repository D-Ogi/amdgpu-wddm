// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "display_io.h"
#include "../shim/bc250_scaler_plan.h"

enum BC250_DCN_COLOUR { BC250_DCN_RGB, BC250_DCN_YCBCR444, BC250_DCN_YCBCR420 };
#define BC250_DCN_GEOMETRY_WORDS 29
typedef struct BC250_DCN_GEOMETRY {
    BC250_SCALER_PREPARED Scaler;
    unsigned long Words[BC250_DCN_GEOMETRY_WORDS];
    unsigned Ready, ChromaMode, MirrorH, MirrorV;
} BC250_DCN_GEOMETRY;

// PASSIVE preparation into caller-owned private storage, not the kernel stack.
// Progressive scanout only. Colour chooses DSCL processing only, NOT a
// supported format/CSC/HDR capability. The owner must reject interlaced output.
// Caller must admit format, bandwidth, chroma siting and surface configuration.
// All refusal/EMPTY paths clear output. Input/output may overlap.
int Bc250DcnGeometryPrepare(const BC250_SCALER_REQUEST* Request, unsigned Colour,
                            BC250_DCN_GEOMETRY* Plan);

// Pipe 0/1, positively established by the owner to feed OTG0. Requires its
// acknowledged lock, powered scaler memory and an immutable successful Prepare
// result. The owner establishes power readiness before the commit. No allocation,
// coefficient upload/selection, power change, enable, trigger, unlock or stall.
// MirrorH/V must also be applied by the surface owner in this transaction.
// The owner stages Plan.Scaler and selects the bank using Plan.ChromaMode.
// On failure after any attempted write, restore the ENTIRE plane transaction
// while still locked. An unsuccessful MMIO call may have affected hardware.
// Success is programming only, never proof of latch, PresentId or retirement.
long Bc250DcnGeometryApplyLocked(BC250_DISP_IO* Io, unsigned Pipe,
    const BC250_DCN_GEOMETRY* Plan, unsigned* WritesAttempted);
