// CPU state of one adapter (cpu.c, docs/design/tuner.md, ADR 0020). The policy is driver/shim/bc250_cpu.c, which
// knows the allowlists, the ranges, the order a change is sent in and the three failure signs; this is what the
// miniport keeps around it: the opt-in setting, the guard's marks, the worker thread that owns every trial's
// deadline, the cached readbacks and the snapshot the escape reads.
//
// Nothing here runs on a machine that did not ask for it: CpuTune is absent by default, and then this file reads
// four registry values at start, writes one log line and stops. No thread, no mailbox message, no timer.
#pragma once
#include "../shim/include/bc250_cpu.h"

// The trial window, in the same shape as the V/F curve's (dpm.h): the kernel owns the deadline, so a killed or
// hung tool cannot leave a candidate running.
#define BC250_CPU_TRIAL_MS 25000u
#define BC250_CPU_TRIAL_MIN_MS 10000u
#define BC250_CPU_TRIAL_MAX_MS 180000u
#define BC250_CPU_POLL_MS 100u              // the worker's wake-up while a trial runs

typedef struct _BC250_CPU_SNAP {
    ULONG Flags;                            // BC250_CPU_FLAG_*
    ULONG VoltageMv, GpuVoltageMv, CapC, Features;
    ULONG CoreMHz[BC250_CPU_CORES], PstateMHz[BC250_CPU_PSTATES];
    ULONG LastQueue, LastMessage, LastStatus, LastParameter;     // the support report's trail
    LONG TemperatureMc;
    ULONG Reads, Writes, Refusals, Reverts;
} BC250_CPU_SNAP;

typedef struct _BC250_CPU_STATE {
    KMUTEX Lock;                            // every transition and every write: PASSIVE_LEVEL, registry and mailbox
    KSPIN_LOCK SnapLock;                    // Snap, the deadline and the search state
    KEVENT StopEvent;                       // the worker's exit
    KEVENT Wake;                            // a new deadline, or work at start
    PKTHREAD Thread;
    BOOLEAN Created;
    BOOLEAN Enabled;                        // CpuTune 1: without it every write is refused
    BOOLEAN Lab;                            // CpuLab 1: the owner's own bound (BC250_CPU_MAX_MHZ_LAB)
    BOOLEAN Proven;                         // queue 3 answered a getter in this start: setters are admitted
    BOOLEAN Pending, Confirmed;             // the stored settings' two marks
    BOOLEAN CorePending, CoreConfirmed;     // the core mask's two marks
    BOOLEAN StartWork;                      // the worker still owes the start readback and the stored apply
    struct bc250_cpu_settings Applied;      // what this driver has sent (cached; the chip has no "what is set")
    struct bc250_cpu_settings Stored;       // the registry's values
    struct bc250_cpu_settings Baseline;     // what the firmware answered before the first write of this start
    BOOLEAN BaselineValid;
    struct bc250_cpu_settings TrialBefore;  // what a revert puts back
    ULONG TrialMs;                          // the window a SET gets by default (CpuTrialMs)
    ULONG TrialSerial;                      // one more for every SET, KEEP, CANCEL and revert
    BOOLEAN OnTrial;
    ULONGLONG TrialDeadline;                // KeQueryInterruptTime units, under SnapLock
    struct bc250_cpu_search Search;         // the guided undervolt search, under SnapLock
    ULONG CoreMask, CoreMaskStored;
    ULONG Cores, Threads;                   // what Windows reports: all the mask's effect we can observe
    BC250_CPU_SNAP Snap;                    // under SnapLock
    ULONGLONG Generation;                   // start-health generation of this start
} BC250_CPU_STATE;
