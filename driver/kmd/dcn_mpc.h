// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "display_io.h"

typedef struct BC250_MPC_PLANE {
    unsigned Enabled, ZOrder; // ZOrder 0/1, larger is above; unique if both on.
    unsigned PerPixelAlpha, PremultipliedAlpha;
    unsigned GlobalAlphaEnabled, GlobalAlpha; // 0..255
} BC250_MPC_PLANE;
typedef struct BC250_MPC_REQUEST { BC250_MPC_PLANE Plane[2]; } BC250_MPC_REQUEST;
#define BC250_MPC_WORDS_PER_PLANE 11
typedef struct BC250_MPC_PLAN {
    unsigned long Words[2 * BC250_MPC_WORDS_PER_PLANE];
    unsigned Output, EnabledMask, Ready;
} BC250_MPC_PLAN;

// CPU-only plan for reserved DPP0->MPCC0 and DPP1->MPCC1 feeding OPP0.
// Black outside planes, including the bottom plane's transparent pixels.
// Input and output can overlap. Failure clears output. No format/caps admission.
int Bc250MpcPrepare(const BC250_MPC_REQUEST* Request, BC250_MPC_PLAN* Plan);

// Caller owns both MPCCs, all display writers and the acknowledged OTG0 lock.
// Plan is a private, immutable successful Prepare result, not caller-supplied
// register data. The owner keeps it alive for the complete transaction.
// BOTH MPCC_UPDATE_LOCK_SEL must already name OPP0 (set while quiesced, before
// acquiring the OTG lock), including disconnected but reserved instances.
// All-disabled requires the owner to establish and retain black OTG blanking;
// OutputBlankConfirmed records that external precondition, not a request to blank.
// Never changes lock selectors, enables HUBP, releases locks, waits or allocates.
// Restores no state: any attempted write failure requires whole-transaction
// restoration before unlock. Success is not latch/PresentId retirement.
long Bc250MpcApplyLocked(BC250_DISP_IO* Io, const BC250_MPC_PLAN* Plan,
    unsigned OutputBlankConfirmed, unsigned* WritesAttempted);
