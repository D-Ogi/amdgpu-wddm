// GPU DWM interop switches of one adapter (interop.c, docs/design/gpu-dwm-interop-switches.md). The start
// policy is interop_policy.c; this is what the miniport keeps around it: the session marker's life and the
// snapshot the escape and the summary read.
#pragma once
#include "interop_policy.h"

typedef struct _BC250_INTEROP_SNAP {
    ULONG Flags;                            // BC250_INTEROP_FLAG_*
    ULONG Requested, Effective, Reason, ClosedReason;
    ULONG BlitSetting, CddSetting;
    ULONG BootId, SessionBootId;
    ULONG Users, Marks, Unmarks, MarkFailures;
    ULONG PreviousEnd, LastEnd;
    ULONGLONG Generation;
} BC250_INTEROP_SNAP;

typedef struct _BC250_INTEROP_STATE {
    KMUTEX Lock;                            // start, user begin/end, stop: PASSIVE_LEVEL registry transitions
    KSPIN_LOCK SnapLock;                    // Snap, read by the escape and the summary
    BOOLEAN Marked;                         // InteropSession is on disk for this start (under Lock)
    BC250_INTEROP_SNAP Work;                // under Lock
    BC250_INTEROP_SNAP Snap;                // under SnapLock, a copy of Work
} BC250_INTEROP_STATE;
