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
// An owed revert is retried this often until it goes through (0.7.211). A refused revert is the one state this
// surface must not settle in, and the two states that refuse one - the part at or above BC250_CLOCK_HOT_MC and
// the GPU busy - both pass by themselves, so the worker keeps asking. Slower than the poll interval on purpose:
// a retry is a mailbox sequence, and a part that stays hot must not turn into one message every 100 ms.
#define BC250_CPU_REVERT_RETRY_MS 1000u
// BD-297: a start read stage that does not answer is tried again, BC250_CPU_REVERT_RETRY_MS apart, this many times in
// all. The refusal seen on unit A (0x80000011, 3.6 s after boot) is a mailbox that is not ready yet, and a start that
// gives up at once never sends the default cap. After the last attempt the start stays unproven until a readback
// answers, and that readback gives the start back to the worker.
#define BC250_CPU_START_TRIES 30u

// Where the start of the CPU surface is (BD-297). One field, not flags: up to 0.7.216.109 a Proven flag and a
// StartWork flag held this together, and a readback could set Proven while the start apply never ran.
typedef enum _BC250_CPU_START {
    BC250_CPU_START_NONE = 0,               // no start, or stopped: queue 3 not proven, nothing owed
    BC250_CPU_START_OWED,                   // the worker owes the start read stage and the start apply
    BC250_CPU_START_MISSED,                 // the read stage did not answer in BC250_CPU_START_TRIES: the start
                                            // apply did not run; a readback that answers makes it owed again
    BC250_CPU_START_PROVEN                  // the read stage answered and the start apply ran: setters admitted
} BC250_CPU_START;

typedef struct _BC250_CPU_SNAP {
    ULONG Flags;                            // BC250_CPU_FLAG_*
    ULONG VoltageMv, GpuVoltageMv, CapC, Features;
    ULONG CoreMHz[BC250_CPU_CORES], PstateMHz[BC250_CPU_PSTATES];
    ULONG LastQueue, LastMessage, LastStatus, LastParameter;     // the support report's trail
    LONG TemperatureMc;
    BOOLEAN TemperatureValid;               // the last message's temperature was read (0.7.211)
    ULONG Reads, Writes, Refusals, Reverts;
    ULONG RevertRetries, RevertFailures;    // an owed revert's attempts, and how many were refused
} BC250_CPU_SNAP;

typedef struct _BC250_CPU_STATE {
    KMUTEX Lock;                            // every transition and every write: PASSIVE_LEVEL, registry and mailbox
    KSPIN_LOCK SnapLock;                    // Snap, the deadline and the search state
    KEVENT StopEvent;                       // the worker's exit
    KEVENT Wake;                            // a new deadline, or work at start
    PKTHREAD Thread;
    BOOLEAN Created;
    BOOLEAN Enabled;                        // CpuTune 1: without it every write is refused
    BOOLEAN Lab;                            // CpuLab 1: the lab's own bound (BC250_CPU_MAX_MHZ_LAB)
    ULONG OcTier;                           // the overclock tier of this start (oc_tier.c), read at CpuStart
    BOOLEAN Pending, Confirmed;             // the stored settings' two marks
    BOOLEAN CorePending, CoreConfirmed;     // the core mask's two marks
    BC250_CPU_START Start;                  // the start of this surface (BC250_CPU_START_*)
    struct bc250_cpu_settings Applied;      // what this driver has sent (cached; the chip has no "what is set")
    struct bc250_cpu_settings Stored;       // the registry's values
    struct bc250_cpu_settings Baseline;     // what the firmware answered before the first write of this start
    BOOLEAN BaselineValid;
    struct bc250_cpu_baseline BaselineRead; // the two tops the read stages of this start answered (0.7.216.24):
                                            // table_mhz is what a loaded core is judged against with no limit
                                            // applied, boost_mhz is the firmware's own ceiling, and boost_given
                                            // says whether a clock limit can be given back at all (BD-094)
    ULONG BootRecord;                       // BD-295: CPU_BOOT_RECORD_* (cpu.c), what this start did with CpuBoot
    BOOLEAN LeftoverOwed;                  // BD-295: an earlier start of this boot left settings in the chip, and
                                            // the restore to the boot baseline has not gone through: every setter
                                            // is refused and the worker retries it every BC250_CPU_REVERT_RETRY_MS
    struct bc250_cpu_settings TrialBefore;  // what a revert puts back
    ULONG TrialMs;                          // the window a SET gets by default (CpuTrialMs)
    ULONG TrialSerial;                      // one more for every SET, KEEP, CANCEL and revert
    BOOLEAN OnTrial;
    BOOLEAN RevertOwed;                     // a revert was refused: the worker keeps trying (0.7.211)
    ULONGLONG TrialDeadline;                // KeQueryInterruptTime units, under SnapLock
    struct bc250_cpu_search Search;         // the guided undervolt search, under SnapLock
    ULONG CoreMask, CoreMaskStored;
    ULONG Cores, Threads;                   // what Windows reports: all the mask's effect we can observe
    BC250_CPU_SNAP Snap;                    // under SnapLock
    ULONGLONG Generation;                   // start-health generation of this start
    // The joint power arm (0.7.216.7, docs/design/dpm.md "The joint power arm"). The DPM governor's thread decides
    // (driver/shim bc250_joint_step) and writes JointWantMHz; this surface's worker alone sends, under Lock, and
    // publishes the other three for the governor to read. All four are interlocked; nothing else here is shared.
    volatile LONG JointWantMHz;             // the cap the governor asks for, 0 for none (dpm.c writes it)
    volatile LONG JointAppliedMHz;          // the arm's cap in the chip now, 0 for none
    volatile LONG JointReady;               // the surface can take a cap now (CpuJointPublish)
    volatile LONG JointBaseMHz;             // the limit without the arm: JointBefore's, Applied's or the baseline's
    volatile LONG JointSends, JointRefusals;    // the arm's changes that went through, and the refused attempts
    BOOLEAN JointFault;                     // a change of the arm failed its voltage readback: off for this start
    BOOLEAN JointPaused;                    // CpuPause to CpuResume: no cap across a power transition
    struct bc250_cpu_settings JointBefore;  // Applied before the arm's first cap: what its release goes back to
    ULONGLONG JointNextTry;                 // KeQueryInterruptTime units: a refused change is tried again after this
} BC250_CPU_STATE;
// A refused change of the arm is tried again this often, as an owed revert is.
#define BC250_CPU_JOINT_RETRY_MS BC250_CPU_REVERT_RETRY_MS

// BD-295 lab arm: LabCpuRevertFault (REG_DWORD under Parameters, read once at DriverEntry, guard.c) is a bit mask.
// Bit BC250_CPU_LAB_FAULT_STOP_REVERT makes the next revert that CpuStop runs report a refusal and send nothing.
// Bit BC250_CPU_LAB_FAULT_LEFTOVER makes the first restore of a leftover at the next start report a refusal and send
// nothing. Bit BC250_CPU_LAB_FAULT_START_READ (BD-297 arm) makes the first start read stage of the load report a
// refusal and send nothing, so the worker's retry runs. Each bit fires once per load. In the release flavour, and in
// the host tests, the macro is FALSE.
#define BC250_CPU_LAB_FAULT_STOP_REVERT 1u
#define BC250_CPU_LAB_FAULT_LEFTOVER 2u
#define BC250_CPU_LAB_FAULT_START_READ 4u
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
BOOLEAN GuardLabCpuFault(ULONG Bit);
#define BC250_CPU_LAB_FAULT(Bit) GuardLabCpuFault(Bit)
#else
#define BC250_CPU_LAB_FAULT(Bit) FALSE
#endif
