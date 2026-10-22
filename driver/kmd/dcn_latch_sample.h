// Read-only brackets for the BD-223 lock experiment, not a retirement proof.
#pragma once
#include "display_io.h"

enum bc250_latch_word {
    Bc250LatchFrameBefore, Bc250LatchSelectBefore, Bc250LatchLockBefore,
    Bc250LatchHighBefore, Bc250LatchLow, Bc250LatchHighAfter,
    Bc250LatchLockAfter, Bc250LatchSelectAfter, Bc250LatchFrameAfter,
    Bc250LatchWordCount
};
typedef struct _BC250_LATCH_SAMPLE {
    unsigned long Word[Bc250LatchWordCount];
    unsigned long ValidMask;
    unsigned long BracketsMatch;
    unsigned long LockHeld;
    unsigned long LockClear;
    unsigned long long Physical;
} BC250_LATCH_SAMPLE;

// Exactly nine reads on success, no writes, waits or retries. Partial raw words
// remain available on read failure, but all derived fields stay zero. Mismatch
// returns BUSY. Matching brackets are only a candidate observation: the caller
// must retain both surfaces and exclude other writers, reset and power changes.
// In particular, equal endpoints do not exclude an intervening A->B->A change.
// LockHeld/LockClear describe both endpoints; neither proves continuity.
// Both are zero for partial request/ack states and for any rejected sample.
long Bc250LatchSampleRead(BC250_DISP_IO* Io, BC250_LATCH_SAMPLE* Sample);
