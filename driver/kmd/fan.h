// The case fan control as the miniport keeps it (fan.c, Part B of docs/design/fan.md): the gate, the controller,
// the watchdog, the bugcheck record and the published snapshot. The policy is driver/shim/bc250_fan.c, which has no
// Windows in it; this is the part that does.
#pragma once
#include "bc250_fan.h"
#include "bc250kmd_escape.h"
#include "hwmon.h"

#define BC250_FAN_WATCHDOG_PERIOD_MS 1000u  // the watchdog's timer
#define BC250_FAN_WATCHDOG_FORCE_MS 6000u   // a controller held this long by a step: the blind restore
#define BC250_FAN_CUSTOMER_UNIT_A 0x162Bu   // the EC customer ID M803 read on unit A

C_ASSERT(BC250_FAN_CURVE_SLOTS == BC250_FAN_POINTS_MAX);
C_ASSERT(BC250_FAN_WATCHDOG_MS > BC250_FAN_WATCHDOG_PERIOD_MS);

// What the escape reads. Written under SnapLock by whoever holds the controller, after every change.
typedef struct _BC250_FAN_SNAP {
    struct bc250_fan_ctl Ctl;
    ULONG Flags;                            // BC250_FAN_FLAG_* that are not in Ctl
    ULONG Gate;
    ULONG ReadbackRaw, Rpm;
    ULONGLONG WatchdogFires;
    ULONGLONG Generation;
} BC250_FAN_SNAP;

typedef struct _BC250_FAN_OWNER {
    // The controller (Ctl) has one holder at a time: the governor thread's step, an exit path, or the watchdog's
    // DPC. Busy is that hold. It is an interlocked flag and not a lock, because the DPC must never wait: it tries
    // once and comes back a second later.
    volatile LONG Busy;
    KSPIN_LOCK SnapLock;                    // Snap and the pending request
    KMUTEX RequestLock;                     // escape writers: one registry update at a time, still at PASSIVE_LEVEL
    BOOLEAN Configured;                     // FanStart ran and FanStop has not
    BOOLEAN Enabled;                        // this start may drive the chip
    BOOLEAN Paused;                         // out of D0
    BOOLEAN TimerArmed, BugCheckRegistered;
    BOOLEAN BlindDone;                      // the watchdog's forced blind restore ran in this start
    BOOLEAN Stored;                         // FanMode is in the registry
    ULONG StoredMode, StoredProfile;
    ULONG Gate;                             // BC250_FAN_GATE_*
    BC250_HWMON_OWNER* Hwmon;               // the window and the reader's last sample
    struct bc250_fan_ctl Ctl;               // under Busy; the restore record in it survives a stop and a start
    // The escape's request, taken by the next step. Under SnapLock.
    BOOLEAN PendingSet, PendingRenew;
    struct bc250_fan_request Pending;
    ULONG PendingLeaseMs;
    // The watchdog. LastStepAt is written by the step and read by the DPC, both through Interlocked*64.
    volatile LONG64 LastStepAt;
    ULONGLONG WatchdogRetryAt;
    ULONGLONG WatchdogFires;
    KTIMER Timer;
    KDPC Dpc;
    // The watchdog's normal handback runs here and not in the DPC (audit finding F1). The handshake polls the
    // chip with 250-microsecond stalls, up to 200 of them per phase (BC250_FAN_POLL_US, BC250_FAN_POLL_MAX),
    // and a DPC "must not specify delays of more than 100 microseconds"
    // (ref/windows-driver-docs/windows-driver-docs-pr/kernel/guidelines-for-writing-dpc-routines.md:35). The
    // DPC therefore takes the hold, queues this item and returns; the item gives the fan back at
    // PASSIVE_LEVEL and releases the hold. WorkerQueued is the one-at-a-time guard and WorkerQueuedAt says
    // since when, so a worker that never runs cannot keep the blind restore away for ever.
    PIO_WORKITEM Worker;
    volatile LONG WorkerQueued;
    ULONGLONG WorkerQueuedAt;
    KBUGCHECK_CALLBACK_RECORD BugCheck;
    // The log's memory, so that a state is logged once when it changes and not once a second.
    ULONG LoggedState, LoggedReason, LoggedDoubt;
    BC250_FAN_SNAP Snap;                    // under SnapLock
} BC250_FAN_OWNER;
