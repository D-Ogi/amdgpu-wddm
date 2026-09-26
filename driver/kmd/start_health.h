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
} BC250_START_HEALTH_STATE;
