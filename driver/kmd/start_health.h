// Adapter-owned startup witness. No subsystem pointer is part of the snapshot.
#pragma once
#include "bc250kmd_escape.h"
typedef struct _BC250_START_HEALTH_STATE {
    KMUTEX Lifecycle;
    KSPIN_LOCK Lock;
    EX_RUNDOWN_REF Readers;
    ULONGLONG Generation, Epoch, Completed, LastCompletion, ReadySince;
    ULONGLONG ConfirmedGeneration, ConfirmedEpoch;
    ULONG LastSequence;
    BOOLEAN Full, Engines, Visible, Mode, Closed;
    // BD-120. TRUE while Closed came from a node-0 hardware fence timeout that the OS TDR still owes an answer
    // to. Such a close, and only such a close, is undone by a recovered engine reset. Every other fault clears
    // it, so a fault recorded before the reset is never cleared by the reset.
    BOOLEAN PendingReset;
    // BD-120. TRUE once an engine reset of this device start has recovered node 0, whether the witness was
    // closed at the time or not. The witness itself re-arms; this says that the start needed a GPU reset to get
    // there, which is what keeps a clock/voltage or CU request that hung node 0 from becoming the durable
    // default (StartHealthIsReady and the confirmation tail of StartHealthRequest). Only a new device start
    // clears it: a resume invalidates the witness but does not make the hung request safe.
    BOOLEAN Recovered;
} BC250_START_HEALTH_STATE;
// BD-120. What a recovered engine reset found the witness in, for the caller's log line. The reset itself
// always succeeded; this says what the witness did about it.
typedef enum _BC250_START_HEALTH_RECOVERY {
    BC250_START_HEALTH_RECOVERY_STICKY = 0,     // a fault of another kind is recorded: the witness stays closed
    BC250_START_HEALTH_RECOVERY_REOPENED = 1,   // the timeout latch's own close, undone here on a new epoch
    BC250_START_HEALTH_RECOVERY_REARMED = 2     // never closed (the OS reset arrived first): a new epoch anyway
} BC250_START_HEALTH_RECOVERY;
