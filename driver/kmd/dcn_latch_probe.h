// Lab-only, fixed-mode address-latch experiment. Not a presentation DDI.
#pragma once
#include "dcn_latch_sample.h"

enum bc250_latch_arm {
    Bc250LatchUnlocked, Bc250LatchLockBeforeProgram, Bc250LatchLockAfterProgram,
    Bc250LatchArmCount
};
typedef struct _BC250_LATCH_UNLOCK_SAMPLE {
    // Reads counts successful raw reads. LastSetQpc is the last observation
    // with either request or ack set, not proof of a fully held lock.
    unsigned long Attempted, Reads, LastValue, ClearObserved;
    long Status;
    unsigned long Reserved;
    unsigned long long StartQpc, LastQpc, LastSetQpc;
} BC250_LATCH_UNLOCK_SAMPLE;
typedef struct _BC250_LATCH_PROBE_RESULT {
    unsigned long Arm, Generation, Samples, WritesAttempted;
    long Status, UnlockStatus, RestoreStatus, EndStatus;
    unsigned long BoundaryObserved, Restored;
    unsigned long long AddressA, AddressB;
    BC250_LATCH_SAMPLE Before, Armed, Boundary, AfterUnlock;
    unsigned long long QpcFrequency;
    BC250_LATCH_UNLOCK_SAMPLE Unlock[2]; // normal operation, then cleanup
} BC250_LATCH_PROBE_RESULT;

// Caller: lab gate, administrator, exact HardwareAccess synchronization,
// PASSIVE_LEVEL. No worker may outlive the synchronized call. No raw address
// input: both surfaces come from the existing driver-owned reservation.
// Result records observations only; the controls must be compared externally.
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
struct _BC250_DEVICE;
long DcnLatchProbe(struct _BC250_DEVICE* Device, unsigned long Arm,
                  BC250_LATCH_PROBE_RESULT* Result);
#endif
