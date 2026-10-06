// DPM state of one adapter (dpm.c, docs/design/dpm.md). The policy is driver/shim/bc250_dpm.c; this is
// what the miniport keeps around it: the governor thread, the busy accounting the submit path feeds, the
// guard's persistence and the telemetry snapshot.
#pragma once
#include "bc250_dpm.h"

#define BC250_DPM_TICK_MS 25u               // the governor's period; the timer resolution may stretch it
#define BC250_DPM_VERIFY_MS 1000u           // SMU readback of clock and VID
#define BC250_DPM_LOG_MS 5000u              // one telemetry line in the driver log
#define BC250_DPM_ERROR_LIMIT 3u            // failed transitions in a row before the governor gives up

typedef struct _BC250_DPM_SNAP {
    ULONG Flags;                            // BC250_DPM_FLAG_*
    ULONG Mode, Requested, Reason, Throttle;
    ULONG MaxMHz, CapMHz, TargetMHz, WantMHz, CurrentMHz, CurrentMv, ObservedMHz, ObservedVid;
    LONG TemperatureMc;
    ULONG BusyPermille, BusyAvgPermille;    // the governor's input (GRBM when BusySource says so), its average
    ULONG SubmitBusyPermille, SdmaBusyPermille, BusySource, HwSamples;
    ULONG Raises, Lowers, ThermalEvents, Errors, Resyncs;
    ULONGLONG Ticks, BusyTime100ns, UptimeMs, Generation;
    ULONG TuneApplied, FloorTicks;          // 0.7.185: the tune serial the governor runs with; Gov.floor_ticks
    ULONG SoftReleases;                     // 0.7.197: Gov.soft_releases
    ULONG WarmHolds;                        // 0.7.200: Gov.warm_holds, driver log only (not in the escape)
    ULONG RampHolds;                        // 0.7.203: Gov.ramp_holds, driver log only (not in the escape)
    // 0.7.207, the idle state. IdleMHz is the point in force, 0 when the state is off for this start
    // (bc250_dpm_idle_mhz: DpmIdleMHz 0, a refused setting, or a point the firmware refused).
    ULONG IdleMHz, IdleHoldMs, IdleBusyPermille;
    ULONG IdleEntries, IdleExits, IdleRefusals;
    ULONGLONG IdleMs;                       // time held at the idle point
} BC250_DPM_SNAP;

typedef struct _BC250_DPM_STATE {
    KMUTEX Lock;                            // Start/Stop/Confirm: PASSIVE_LEVEL registry transitions
    KMUTEX TickLock;                        // held by the thread for one tick; Pause waits on it
    KSPIN_LOCK SnapLock;                    // Snap, read by the escape
    KEVENT StopEvent;
    PKTHREAD Thread;
    BOOLEAN Created;                        // the thread exists
    BOOLEAN Pending, Confirmed;             // the guard's marks of this start
    volatile LONG Paused;                   // a power transition: no SMU traffic
    volatile LONG Resync;                   // read the hardware's clock back before the next decision
    volatile LONG Stable;                   // SetStablePowerState(TRUE)
    // Fed by gfx.c at submit and at fence arrival, interlocked only (DpmBusyBegin/DpmBusyEnd). Both in
    // KeQueryInterruptTime units. BusySince is 0 while the ring is idle.
    volatile LONG64 BusySince;
    volatile LONG64 BusyAccum;
    // The hardware sampler (dpm.c DpmHwSample): a high-resolution timer every BC250_DPM_HW_SAMPLE_US reads
    // GRBM_STATUS and SDMA0_STATUS_REG and counts; each tick takes the counts (InterlockedExchange).
    PEX_TIMER HwTimer;
    volatile LONG HwSamples, HwGfxActive, HwSdmaActive;
    ULONGLONG Generation;                   // start-health generation of this start
    struct bc250_dpm_decision Decision;
    // The thread's alone while it runs.
    struct bc250_dpm_governor Gov;
    struct bc250_dpm_session Session;
    ULONG ErrorsInRow;
    BOOLEAN GaveUp;
    ULONG TuneTaken, TuneRefused;           // the Tune serial last copied into Gov, or refused by it (the thread's, like Gov)
    BC250_DPM_SNAP Snap;                    // under SnapLock
    // Runtime tuning (0.7.185, BC250_ESCAPE_RUN_DPM_TUNE). Written by the escape under Lock and SnapLock, read by the
    // thread under SnapLock at the start of a governing tick, reset to the defaults by DpmStart. Never persisted.
    struct bc250_dpm_tune Tune;             // under SnapLock
    ULONG TuneSerial;                       // under SnapLock: one more for every change
} BC250_DPM_STATE;

// The submit path (gfx.c SubmitIbLocked, after the IB is committed): the ring went, or stays, busy.
// Any IRQL; a CAS so that the earliest outstanding submission's time is kept.
FORCEINLINE void DpmBusyBegin(_Inout_ BC250_DPM_STATE* S)
{
    (void)InterlockedCompareExchange64(&S->BusySince, (LONG64)KeQueryInterruptTime(), 0);
}
// Fence arrival (gfx.c GfxFenceArrivedAccess, after its CAS cleared the last outstanding sequence).
// A race with a new submit costs at most one tick of accuracy: the governor's sampler heals both ways.
FORCEINLINE void DpmBusyEnd(_Inout_ BC250_DPM_STATE* S)
{
    LONG64 since = InterlockedExchange64(&S->BusySince, 0);
    LONG64 now = (LONG64)KeQueryInterruptTime();
    if (since != 0 && now > since) InterlockedAdd64(&S->BusyAccum, now - since);
}
