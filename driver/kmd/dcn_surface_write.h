// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "dcn_surface_layout.h"
#include "display_io.h"

typedef struct BC250_SURFACE_WRITE_REQUEST {
    BC250_SURFACE_LAYOUT_REQUEST Surface;
    // Trusted device/allocator topology, not values guessed from a surface.
    // Counts are powers of two; PipeInterleave is the AMD encoded field.
    unsigned NumPipes, NumBanks, NumShaderEngines, NumRbPerSe;
    unsigned MaxCompressedFrags, PipeInterleave;
} BC250_SURFACE_WRITE_REQUEST;
#define BC250_SURFACE_WRITE_WORDS 13
typedef struct BC250_SURFACE_WRITE_PLAN {
    BC250_SURFACE_LAYOUT Layout;
    unsigned long Words[BC250_SURFACE_WRITE_WORDS];
    unsigned Ready;
} BC250_SURFACE_WRITE_PLAN;

// Captures AMD's selected HUBP1 DCC/tiling/address sequence and checked layout.
// Linear, uncompressed, non-TMZ, mono, frame-boundary flip. No format admission.
// Input/output may overlap. Failure clears output. No hardware accesses.
int Bc250SurfaceWritePrepare(const BC250_SURFACE_WRITE_REQUEST* Request,
                            BC250_SURFACE_WRITE_PLAN* Plan);

// Private immutable successful Prepare result; no untrusted register data.
// Caller owns all display writers, residency, matching CNVC/CSC, geometry,
// DML/requestor programming and OTG0 lock. HUBP must already select VTG0 and
// acknowledge the master lock; no earlier surface flip may remain pending.
// Triple buffering and a separate SURFACE_UPDATE_LOCK must be disabled.
// All reads precede writes. Address low is last. No enable/lock/unlock/wait.
// Any attempted-write failure needs whole-transaction restoration by caller
// before unlock. Success is not latch, scanout retirement or an HLK pass.
long Bc250SurfaceWriteApplyLocked(BC250_DISP_IO* Io, unsigned Pipe,
    const BC250_SURFACE_WRITE_PLAN* Plan, unsigned* WritesAttempted);
