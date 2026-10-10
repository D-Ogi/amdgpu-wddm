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
// Rule 10 means a sustained load: one step pays less into the heavy-time account than the arming time, so no
// single late step (a starved governor thread, a resume) can arm the boost by itself.
C_ASSERT(BC250_FAN_BOOST_STEP_MAX_MS < BC250_FAN_BOOST_ARM_MS);
// And the step itself must stay inside that bound, or every step is a late one: the rise window would open again
// at each step and never close, which switches the temperature's rise signal (WHY_RISE) off without a word. The
// step runs at the hwmon cadence (dpm.c calls FanStep right after HwmonSample, and that period is the dt a step
// without a reading falls back to), so this is the tie between the two files.
C_ASSERT(BC250_HWMON_PERIOD_MS <= BC250_FAN_BOOST_STEP_MAX_MS);

// The governor's load feed for one control step (the load feed-forward, rule 10 of bc250_fan.h). The governor
// thread fills it in dpm.c beside the Tctl reading: the busy share is the mean over the whole step and not one
// 25 ms DPM tick, the clock is the level the governor asks for (its own read-back when it does not govern, never
// a read of the chip in this step), and the power is the SMU metrics table's socket figure.
// A step without a feed (Valid FALSE, or no pointer at all) runs on the temperature curve alone.
typedef struct _BC250_FAN_LOAD {
    ULONG BusyPermille;                     // GPU busy over the step, 0..1000
    ULONG Mhz;                              // the GFX clock the governor asks for, 0 when it is not known
    ULONG SocketMw;                         // the SMU socket power, when PowerValid
    BOOLEAN Valid;
    BOOLEAN PowerValid;
} BC250_FAN_LOAD;

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
    KBUGCHECK_CALLBACK_RECORD BugCheck;
    // The log's memory, so that a state is logged once when it changes and not once a second.
    ULONG LoggedState, LoggedReason, LoggedDoubt, LoggedBoost;
    BC250_FAN_SNAP Snap;                    // under SnapLock
} BC250_FAN_OWNER;
