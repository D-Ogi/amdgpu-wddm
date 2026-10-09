// The CPU surface: a clock limit, an undervolt in firmware curve-scale steps, the firmware's own temperature cap
// and the core-enable mask (docs/design/tuner.md, ADR 0020, docs/hardware.md). The policy is driver/shim/bc250_cpu.c
// and is host-tested; this file is the miniport around it:
//
//   driver/shim/bc250_cpu.c     the allowlists, the ranges, the order of a change, the failure signs, the search
//   driver/kmd/smu.c            the one serialized mailbox owner, with queue 3's transport beside queue 0's
//   this file                   the opt-in setting, the boot guard, the worker thread, the trial, the escape
//
// Three rules it enforces, in this order:
//   1. CpuTune must be 1. Without it the whole surface is read-only: no mailbox message is ever sent, and every
//      write is refused with STATUS_INVALID_DEVICE_STATE. A machine that nobody opted in does not even get a thread.
//   2. No setter runs before this start's read stage has answered (Proven). Queue 3 has never been spoken to on
//      this part, and every range in the shim's header is REPORTED and not measured, so the first thing that
//      happens is a read.
//   3. A SET is a trial. The kernel owns the deadline, so a killed tool, a hung tool, a lost remote session and a
//      bugcheck all end at the settings that were in force before it. Only a KEEP writes the registry.
//
// One more caller since 0.7.216.7: the DPM governor's joint power arm (DpmJointGovernor, default off) asks for a
// lower clock limit while the GPU is bound and held by heat, and the worker sends it under the same rules, with the
// busy gate lifted for that one setter and its voltage readback ("the joint power arm" below).
//
// Why a thread of its own: a trial's revert and the start's read stage both need PASSIVE_LEVEL and both send
// several messages BC250_CPU_MESSAGE_GAP_MS apart, which is not something to do in a timer callback and not
// something to hang on the DPM governor's 25 ms tick. The thread exists only while CpuTune is 1.
//
// Settings, all REG_DWORD under Services\bc250kmd\Parameters:
//   CpuTune           1 (written by the INF from 0.7.216) opens the surface; absent or 0, read-only
//   CpuLab            1 admits BC250_CPU_MAX_MHZ_LAB with an undervolt in force (the owner's own bound)
//   CpuMaxMHz         the stored clock limit, absent for none
//   CpuUvSteps        the stored undervolt, in curve-scale steps
//   CpuTempC          the stored temperature cap
//   CpuTrialMs        the default trial window of a SET
//   CpuPending        the mark of a start that applied the stored values and never became healthy
//   CpuConfirmed      the mark a healthy start wrote
//   CpuLastReason     what this start did with the stored values
//   CoreMask          119 (stock, 6 cores) or 255 (all 8); applied at start, visible after the next restart
//   CoreMaskPending, CoreMaskConfirmed   the same two marks for the mask
#include "bc250kmd.h"
#include "bc250kmd_escape.h"

#define CPU_SETTING_TUNE L"CpuTune"
#define CPU_SETTING_LAB L"CpuLab"
#define CPU_SETTING_MAX L"CpuMaxMHz"
#define CPU_SETTING_UV L"CpuUvSteps"
#define CPU_SETTING_TEMP L"CpuTempC"
#define CPU_SETTING_TRIAL L"CpuTrialMs"
#define CPU_SETTING_PENDING L"CpuPending"
#define CPU_SETTING_CONFIRMED L"CpuConfirmed"
#define CPU_SETTING_REASON L"CpuLastReason"
#define CPU_SETTING_CORE_MASK L"CoreMask"
#define CPU_SETTING_CORE_PENDING L"CoreMaskPending"
#define CPU_SETTING_CORE_CONFIRMED L"CoreMaskConfirmed"

// CpuLastReason, for the tools when the adapter is gone (docs/design/tuner.md).
#define CPU_REASON_NONE 0u              // nothing stored: the firmware's own settings
#define CPU_REASON_OK 1u                // the stored values are applied
#define CPU_REASON_REFUSED 2u           // the stored values are outside the admitted ranges
#define CPU_REASON_UNCONFIRMED 3u       // an earlier start applied them and never became healthy
#define CPU_REASON_REGISTRY 4u          // the pending mark would not reach the disk
#define CPU_REASON_OFF 5u               // CpuTune is 0
#define CPU_REASON_NOT_PROVEN 6u        // queue 3 did not answer: nothing was sent
#define CPU_REASON_HARDWARE 7u          // the firmware refused a message, or the voltage readback did

// The worker waits this long before its first mailbox message, so that the read stage never shares the start
// window with the clock transaction and the first frames.
#define CPU_START_DELAY_MS 2000u

C_ASSERT(sizeof(BC250_ESCAPE_CPU) == 296);        // ABI 1; 272 up to 0.7.210, before the sample's three inputs
C_ASSERT(sizeof(ULONG) == sizeof(unsigned int));  // the read stage's ULONG arrays go to bc250_cpu_baseline_read
// The escape header carries three of the shim's numbers for callers that cannot include the shim
// (tools/win/bc250kmd_cli, bc250control.dll). They are the same numbers or this does not build.
C_ASSERT(BC250_CPU_REQUEST_MASK_STOCK == BC250_CPU_MASK_STOCK);
C_ASSERT(BC250_CPU_REQUEST_MASK_FULL == BC250_CPU_MASK_FULL);
C_ASSERT(BC250_CPU_REQUEST_SEARCH_STEPS == BC250_CPU_SEARCH_MAX_STEPS);
C_ASSERT(BC250_CPU_REQUEST_ERROR_COUNT == BC250_CPU_ERROR_COUNT);
C_ASSERT(BC250_CPU_REQUEST_ERROR_NO_CEILING == BC250_CPU_ERROR_NO_CEILING);
C_ASSERT(BC250_CPU_CORE_SLOTS == BC250_CPU_CORES);
C_ASSERT(BC250_CPU_CORE_SLOTS == BC250_CPU_PSTATES);
C_ASSERT(BC250_CPU_ERROR_COUNT == 7);
C_ASSERT(BC250_CPU_FAIL_COUNT == 6);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_CPU, Status) == FIELD_OFFSET(BC250_ESCAPE, Status) &&
         FIELD_OFFSET(BC250_ESCAPE_CPU, Version) == FIELD_OFFSET(BC250_ESCAPE, Version));

static void CpuLock(BC250_CPU_STATE* S)
{
    KeWaitForSingleObject(&S->Lock, Executive, KernelMode, FALSE, NULL);
}

static void CpuUnlock(BC250_CPU_STATE* S)
{
    KeReleaseMutex(&S->Lock, FALSE);
}

static void CpuWait(ULONG Ms)
{
    LARGE_INTEGER delay;
    delay.QuadPart = -10000ll * (LONGLONG)Ms;
    (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

// The same wait, spent busy on this processor instead of asleep: the boost probe alone uses it (0.7.216.24,
// BD-094). The firmware answers the clock of the moment on message 0x43, so a sleeping thread reads an idle core
// and the probe would measure nothing. KeStallExecutionProcessor takes 50 us at a time, which is the documented
// bound for one call; the thread stays at PASSIVE_LEVEL and preemptible, so this costs one core's time and
// delays nothing else. Nothing but the probe may wait this way: every other wait of this file is a wait for the
// firmware and belongs asleep.
#define CPU_BUSY_SLICE_US 50u
static void CpuBusyWait(ULONG Ms)
{
    ULONG slices = Ms * (1000u / CPU_BUSY_SLICE_US), i;
    for (i = 0; i < slices; i++) KeStallExecutionProcessor(CPU_BUSY_SLICE_US);
}

static BOOLEAN CpuQueryPresent(PCWSTR Name, ULONG* Value)
{
    ULONG value = 0;
    NTSTATUS status = GuardQuerySetting(Name, &value);
    *Value = NT_SUCCESS(status) ? value : 0;
    if (NT_SUCCESS(status)) return TRUE;
    if (status != STATUS_OBJECT_NAME_NOT_FOUND)
        GuardLog("cpu: reading %ws failed 0x%08X, treated as absent", Name, status);
    return FALSE;
}

static void CpuStoreLogged(PCWSTR Name, ULONG Value)
{
    NTSTATUS status = GuardStoreSetting(Name, Value);
    if (!NT_SUCCESS(status)) GuardLog("cpu: writing %ws = %lu failed 0x%08X", Name, Value, status);
}

static void CpuDeleteLogged(PCWSTR Name)
{
    NTSTATUS status = GuardDeleteSetting(Name);
    if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND)
        GuardLog("cpu: deleting %ws failed 0x%08X", Name, status);
}

// One number that names a set of settings, for the guard's marks and the log. Not a checksum of anything the
// firmware holds: the chip has no "what is set now" to compare it with.
static ULONG CpuMark(const struct bc250_cpu_settings* S)
{
    return ((S->max_given ? S->max_mhz : 0u) & 0xFFFFu) | ((S->uv_given ? S->uv_steps : 0u) & 0xFFu) << 16 |
           ((S->temp_given ? S->temp_c : 0u) & 0xFFu) << 24;
}

static void CpuLogSettings(const char* What, const struct bc250_cpu_settings* S)
{
    // A pair of lines, by the rule of the guardlog-width gate: the three "(not given)" markers make one line
    // 200 characters at its widest and a log line holds 159, so the cap would be cut off (BD-070).
    GuardLog("cpu: %s clock %s%lu MHz", What,
             S->max_given ? "" : "(not given) ", S->max_given ? S->max_mhz : 0u);
    GuardLog("cpu: %s undervolt %s%lu steps, cap %s%lu C", What,
             S->uv_given ? "" : "(not given) ", S->uv_given ? S->uv_steps : 0u,
             S->temp_given ? "" : "(not given) ", S->temp_given ? S->temp_c : 0u);
}

// The governor's own last busy share, which SmuCpuMessage refuses a CPU message above. Reading the published
// snapshot costs one spin lock and never a mailbox message.
static ULONG CpuBusyPermille(BC250_DEVICE* Device)
{
    ULONG permille;
    KIRQL irql;
    KeAcquireSpinLock(&Device->Dpm.SnapLock, &irql);
    permille = Device->Dpm.Snap.BusyPermille;
    KeReleaseSpinLock(&Device->Dpm.SnapLock, irql);
    return permille;
}

// No mailbox traffic while the adapter is on its way out of D0 or already there, and none without the owner: the
// governor's own paused flag is the one the power path sets first (pnp.c, DpmPause), and this surface honours it.
static BOOLEAN CpuAdapterDown(BC250_DEVICE* Device)
{
    return (!Device->Smu.Online || !Device->Smu.CpuOnline ||
            InterlockedCompareExchange(&Device->Dpm.Paused, 0, 0) != 0) ? TRUE : FALSE;
}

// One message of an open sequence (the caller holds SmuCpuBegin). Everything the support report wants about it is
// recorded, including a refusal: "what did the driver last send and what came back" is the first question of every
// report about this surface.
// AllowHot is passed on to the owner (smu.h): TRUE for every getter, for a step bc250_cpu_plan marked cools,
// and for every restore, so the part can always be brought back to the settings it is known to run at.
// Joint (0.7.216.7) marks a message of the joint power arm's own change: it goes through SmuCpuJointMessage, which
// lifts the busy gate for the clock limit and the voltage readback alone and refuses every other message.
static NTSTATUS CpuMessageEx(BC250_DEVICE* Device, ULONG Queue, ULONG Message, ULONG Parameter, BOOLEAN Write,
                             BOOLEAN AllowHot, BOOLEAN Joint, _Out_opt_ ULONG* Value)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    ULONG value = 0, firmware = 0;
    LONG temperature = 0;
    BOOLEAN temperatureValid = FALSE;
    NTSTATUS status;
    KIRQL irql;

    if (Value != NULL) *Value = 0;
    if (Joint)
        status = SmuCpuJointMessage(&Device->Smu, Queue, Message, Parameter, Write, AllowHot,
                                    CpuBusyPermille(Device), &value, &temperature, &firmware, &temperatureValid);
    else
        status = SmuCpuMessage(&Device->Smu, Queue, Message, Parameter, Write, AllowHot, CpuBusyPermille(Device),
                               &value, &temperature, &firmware, &temperatureValid);
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->Snap.LastQueue = Queue;
    s->Snap.LastMessage = Message;
    s->Snap.LastParameter = Parameter;
    s->Snap.LastStatus = NT_SUCCESS(status) ? firmware : (ULONG)status;
    // A temperature the owner could not read leaves the old reading alone and clears the validity bit, so
    // that nothing downstream judges an unreadable part as cold (BC250_CPU_FAIL_HOT).
    s->Snap.TemperatureValid = temperatureValid;
    if (temperatureValid) s->Snap.TemperatureMc = temperature;
    if (NT_SUCCESS(status)) {
        if (Write) s->Snap.Writes++;
        else s->Snap.Reads++;
    } else s->Snap.Refusals++;
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (!NT_SUCCESS(status))
        GuardLog("cpu: queue %lu message 0x%02X argument 0x%08X %s refused 0x%08X (firmware 0x%08X)", Queue,
                 Message, Parameter, Write ? "set" : "get", status, firmware);
    if (NT_SUCCESS(status) && Value != NULL) *Value = value;
    return status;
}

static NTSTATUS CpuMessage(BC250_DEVICE* Device, ULONG Queue, ULONG Message, ULONG Parameter, BOOLEAN Write,
                           BOOLEAN AllowHot, _Out_opt_ ULONG* Value)
{
    return CpuMessageEx(Device, Queue, Message, Parameter, Write, AllowHot, FALSE, Value);
}

// The baseline this start restores to, from what the firmware has just answered (0.7.211). Until this exists no
// revert and no reset can name the clock limit, and 0.7.210 invented BC250_CPU_MAX_MHZ in its place, which is a
// raise sold as a restore. Recorded only while this driver has sent nothing in this start:
//   the cap        message 0x40 when it is inside the admitted range, the firmware's own default otherwise
//   the undervolt  always 0 steps: nothing of the curve scale persists in the chip across a boot
//   the clock      the highest clock the firmware itself answered (bc250_cpu_baseline_read): the P-state table of
//                  message 0x3B and the per-core clocks of message 0x43, each inside the admitted band, the result
//                  clamped to the release bound BC250_CPU_MAX_MHZ. With no plausible answer max_given stays 0, and
//                  a restore then says the limit stays.
// Why 0x43 counts (0.7.216.15, K137): it is the firmware's own answer of the boost it gives. On unit A the P-state
// table tops out at 3200 MHz while the per-core clocks read 3500 MHz after a cold boot, so a baseline of the table
// alone made every revert, reset and joint-arm release a 0x8F 3200 that held the processor near 3180 MHz until a
// restart. That is a cut sold as a restore, the mirror image of the 0.7.210 defect. The number is still an answer
// and not a constant this driver chose: the release bound only clamps it.
// Why the table alone is not enough either (0.7.216.24, BD-094): message 0x43 answers the clock of the moment, so
// an idle start reads 900 to 1400 MHz, the band refuses those, and the baseline falls back to the table's 3200 MHz
// - the very cut K137 removed. The lab met exactly that in the b23 validation (arm A of the BD-094 test: a revert
// on an idle machine capped the processor at 3200 MHz until a restart; arm B, with one busy thread during the read
// stage, recorded 3500 MHz and restored it). The read stage therefore makes the firmware answer the ceiling
// itself, with a short busy window (CpuBoostProbe), and a clock limit the start could not give back is refused
// (BC250_CPU_ERROR_NO_CEILING) instead of taken and then cut back to the table.
// The full read stage records the cap and the clock; a later read stage of the same start (READBACK, a search step)
// may raise the clock while the driver has still sent nothing, so the baseline is the highest boost the firmware
// showed before the first setter. Pstate is NULL for a stage that did not read the P-state table.
static void CpuRecordBaseline(BC250_CPU_STATE* s, ULONG Cap, _In_opt_ const ULONG* Pstate, const ULONG* CoreMHz)
{
    struct bc250_cpu_settings b;
    struct bc250_cpu_baseline read;
    ULONG previous;
    if (s->Applied.max_given || s->Applied.uv_given || s->Applied.temp_given) return;
    if (!s->BaselineValid && Pstate == NULL) return;    // the full stage records first: it has the cap and the table
    previous = (s->BaselineValid && s->Baseline.max_given) ? s->Baseline.max_mhz : 0u;
    bc250_cpu_baseline_read((const unsigned int*)Pstate, Pstate != NULL ? BC250_CPU_PSTATES : 0u,
                            (const unsigned int*)CoreMHz, BC250_CPU_CORES, &s->BaselineRead, &read);
    s->BaselineRead = read;
    if (s->BaselineValid) {
        if (read.mhz <= previous) return;
        s->Baseline.max_given = 1;
        s->Baseline.max_mhz = read.mhz;
        GuardLog("cpu: the baseline clock of this start rises to %lu MHz (was %lu): the firmware's own boost",
                 read.mhz, previous);
        return;
    }
    RtlZeroMemory(&b, sizeof(b));
    b.temp_given = 1;
    b.temp_c = (Cap >= BC250_CPU_TEMP_MIN_C && Cap <= BC250_CPU_TEMP_MAX_C) ? Cap : BC250_CPU_TEMP_MAX_C;
    b.uv_given = 1;
    b.uv_steps = 0;
    if (read.mhz) { b.max_given = 1; b.max_mhz = read.mhz; }
    s->Baseline = b;
    s->BaselineValid = TRUE;
    // The clock a loaded core is judged against when no limit is applied stays the P-state table's top
    // (BaselineRead.table_mhz), as up to 0.7.216.14: the boost of one busy core is not what every core reaches
    // under the search's load, and judging stretching against 3500 MHz would fail the first step of every
    // undervolt search on an all-core clock.
    GuardLog("cpu: the baseline of this start is clock %s%lu MHz, undervolt 0 steps, cap %lu C",
             b.max_given ? "" : "(not answered) ", b.max_mhz, b.temp_c);
    GuardLog("cpu: the baseline's answers: P-state table top %lu MHz, per-core (boost) top %s%lu MHz",
             read.table_mhz, read.boost_given ? "" : "(not answered) ", read.boost_mhz);
}

// The boost probe (0.7.216.24, BD-094). The read allowlist of docs/hardware.md carries no message that answers the
// firmware's boost ceiling: 0x3B answers the named P-states (3200 MHz on unit A, under the boost) and 0x43 answers
// the clock of the moment, which on an idle machine is 900 to 1400 MHz. So the stage makes the firmware answer the
// ceiling: it keeps one core busy for BC250_CPU_BOOST_PROBE_MS and reads the per-core clocks again. One busy thread
// is enough - the lab measured 3500 MHz on every core of arm B of the b23 BD-094 test with a single busy thread -
// and the probe stops at the first answer inside the band.
// Cost and bounds: one core of six busy for about a fifth of a second at worst, once per start, and only on a start
// whose cores all read under the band. The thread holds this processor (KeSetSystemAffinityThreadEx, which the user
// thread of a READBACK escape also admits) so the load cannot wander between cores. It stays at PASSIVE_LEVEL and
// preemptible, and it sends nothing: every message here is the
// getter 0x43, which changes nothing in the chip and passes the hot gate. The caller holds the SMU owner lock, so
// the probe adds no mailbox traffic of any other kind and keeps the documented one-getter-per-10-ms rate. The
// stage's slow 100 ms rate is for the FIRST traffic of a start, which has answered 11 messages by now.
// CoreMHz carries the stage's own answers in and the highest of both out: a probe that measures less than the
// stage did changes nothing.
static void CpuBoostProbe(BC250_DEVICE* Device, ULONG* CoreMHz)
{
    ULONG round, i, value, answered = 0, probed = 0, top = 0;
    KAFFINITY affinity = (KAFFINITY)1 << KeGetCurrentProcessorNumber(), previous;

    previous = KeSetSystemAffinityThreadEx(affinity);
    for (round = 0; round < BC250_CPU_BOOST_PROBE_ROUNDS && !answered; round++) {
        CpuBusyWait(BC250_CPU_BOOST_PROBE_MS);
        for (i = 0; i < BC250_CPU_CORES && !answered; i++) {
            value = 0;
            if (NT_SUCCESS(CpuMessage(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CORE_MHZ, i, FALSE, TRUE,
                                      &value))) {
                probed++;
                if (value > CoreMHz[i]) CoreMHz[i] = value;
                if (value >= BC250_CPU_MIN_MHZ && value <= BC250_CPU_MAX_MHZ_LAB) answered = value;
            }
            CpuBusyWait(BC250_CPU_GETTER_GAP_MS);
        }
    }
    KeRevertToUserAffinityThreadEx(previous);
    for (i = 0; i < BC250_CPU_CORES; i++) if (CoreMHz[i] > top) top = CoreMHz[i];
    GuardLog("cpu: the boost probe kept one core busy and read %lu core clocks: %s%lu MHz", probed,
             answered ? "the firmware's own boost is " : "still no clock inside the band, highest ", top);
}

// Whether the stage owes a probe: the firmware's ceiling is still unknown and this driver has sent nothing, so a
// clock limit taken now could only be given back from the P-state table, which is the BD-094 cut. A hot part owes
// nothing either: the probe makes heat, and the only thing it would buy is the clock control, which the start can
// refuse. The next read stage of the same start probes again when the part is cool. Under Lock.
static BOOLEAN CpuBoostUnknown(BC250_CPU_STATE* s, _In_opt_ const ULONG* Pstate, const ULONG* CoreMHz)
{
    struct bc250_cpu_baseline read;
    KIRQL irql;
    LONG temperature;
    BOOLEAN hot;
    if (s->Applied.max_given || s->Applied.uv_given || s->Applied.temp_given) return FALSE;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    temperature = s->Snap.TemperatureMc;
    hot = (s->Snap.TemperatureValid && temperature >= BC250_CLOCK_HOT_MC) ? TRUE : FALSE;
    KeReleaseSpinLock(&s->SnapLock, irql);
    bc250_cpu_baseline_read((const unsigned int*)Pstate, Pstate != NULL ? BC250_CPU_PSTATES : 0u,
                            (const unsigned int*)CoreMHz, BC250_CPU_CORES, &s->BaselineRead, &read);
    if (!bc250_cpu_boost_probe_needed(&read)) return FALSE;
    if (hot) {
        GuardLog("cpu: the boost probe is not run at %ld.%ld C (gate %lu C): the clock control stays refused",
                 temperature / 1000, (temperature < 0 ? -temperature : temperature) % 1000 / 100,
                 (ULONG)(BC250_CLOCK_HOT_MC / 1000));
        return FALSE;
    }
    return TRUE;
}

// The read stage: everything the surface can answer without changing anything. It is also the gate: until it has
// answered, no setter is admitted (rule 2 at the head of this file), and the Full form is what records the
// baseline. GapMs is BC250_CPU_MESSAGE_GAP_MS for the first stage of a start, so that the first traffic this
// project has ever sent on queue 3 keeps the documented one-message-per-100-ms rate, and the getter gap after
// that. Every message here is a getter, so all of them pass the hot gate: a read changes nothing.
static NTSTATUS CpuReadStage(BC250_DEVICE* Device, BOOLEAN Full, ULONG GapMs)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    ULONG mv = 0, gpuMv = 0, cap = 0, features = 0, i;
    ULONG coreMHz[BC250_CPU_CORES], pstate[BC250_CPU_PSTATES];
    NTSTATUS status;
    KIRQL irql;

    RtlZeroMemory(coreMHz, sizeof(coreMHz));
    RtlZeroMemory(pstate, sizeof(pstate));
    if (!SmuCpuBegin(&Device->Smu)) return STATUS_DEVICE_BUSY;
    status = CpuMessage(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CPU_MV, 0, FALSE, TRUE, &mv);
    if (NT_SUCCESS(status)) {
        CpuWait(GapMs);
        (void)CpuMessage(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_GPU_MV, 0, FALSE, TRUE, &gpuMv);
        CpuWait(GapMs);
        (void)CpuMessage(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CAP_C, 0, FALSE, TRUE, &cap);
        for (i = 0; i < BC250_CPU_CORES; i++) {
            CpuWait(GapMs);
            (void)CpuMessage(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CORE_MHZ, i, FALSE, TRUE,
                             &coreMHz[i]);
        }
        if (Full) {
            for (i = 0; i < BC250_CPU_PSTATES; i++) {
                CpuWait(GapMs);
                (void)CpuMessage(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_PSTATE_MHZ, i, FALSE, TRUE,
                                 &pstate[i]);
            }
            CpuWait(GapMs);
            (void)CpuMessage(Device, BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_GET_ENABLED_FEATURES, 0, FALSE, TRUE,
                             &features);
            // The P-state table has answered, so the two tops of this stage are known: with no boost answer among
            // them the stage owes the probe (BD-094), and it runs inside this same sequence.
            if (CpuBoostUnknown(s, pstate, coreMHz)) CpuBoostProbe(Device, coreMHz);
        }
    }
    SmuCpuEnd(&Device->Smu);
    if (!NT_SUCCESS(status)) return status;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->Snap.VoltageMv = mv;
    s->Snap.GpuVoltageMv = gpuMv;
    s->Snap.CapC = cap;
    for (i = 0; i < BC250_CPU_CORES; i++) s->Snap.CoreMHz[i] = coreMHz[i];
    if (Full) {
        for (i = 0; i < BC250_CPU_PSTATES; i++) s->Snap.PstateMHz[i] = pstate[i];
        s->Snap.Features = features;
    }
    KeReleaseSpinLock(&s->SnapLock, irql);
    // The caller holds Lock, so this is the one place the baseline can be recorded from a complete answer. A stage
    // without the P-state table may only raise the clock of a baseline the full stage recorded (CpuRecordBaseline).
    CpuRecordBaseline(s, cap, Full ? pstate : NULL, coreMHz);
    return STATUS_SUCCESS;
}

// What the shim judges a trial by. WHEA events and the load's own wrong answers are not visible from here: the
// caller that drives the load counts them and passes them in on the escape (0.7.211), and it also says whether
// it was loading the part at all, because an idle core sits a gigahertz under any limit and clock stretching is
// judged over a loaded sample alone. Target names the clock the cores should reach: the applied limit, or, with
// no limit applied, the baseline the read stage recorded - the firmware's own ceiling is what stretching is
// measured against, and 0.7.210 passed 0 there, which turned the sign off in every undervolt-only search.
static void CpuSample(BC250_DEVICE* Device, const struct bc250_cpu_settings* Target, BOOLEAN Loaded,
                      ULONG Whea, ULONG Checksum, struct bc250_cpu_sample* Sample)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    ULONG i;
    KIRQL irql;
    RtlZeroMemory(Sample, sizeof(*Sample));
    KeAcquireSpinLock(&s->SnapLock, &irql);
    Sample->voltage_mv = s->Snap.VoltageMv;
    for (i = 0; i < BC250_CPU_CORES; i++) Sample->core_mhz[i] = s->Snap.CoreMHz[i];
    Sample->temperature_mc = s->Snap.TemperatureMc;
    Sample->temperature_valid = s->Snap.TemperatureValid ? 1 : 0;
    KeReleaseSpinLock(&s->SnapLock, irql);
    Sample->cores = BC250_CPU_CORES;
    Sample->target_mhz = Target->max_given ? Target->max_mhz : (s->BaselineValid ? s->BaselineRead.table_mhz : 0u);
    Sample->loaded = Loaded ? 1 : 0;
    Sample->whea_events = Whea;
    Sample->checksum_errors = Checksum;
}

// The one place a change reaches the chip: the shim's plan, in the shim's order, one message per gap, with the
// voltage read back after it. A refusal in the middle leaves what the earlier steps did in place and says so;
// Applied then names exactly the steps that went through, because that is what the chip has.
//
// Restore (0.7.211) marks the way back: every step then passes the hot gate, and the owner sends it even when
// the part's temperature cannot be read, because a trial left in the chip with nothing watching it is the worse
// of the two states. An ordinary request passes the gate only for the steps bc250_cpu_plan marked cools.
//
// The voltage is also read between the steps whenever a step that lowers it is followed by one that raises it
// (the undervolt before a lab clock): the reading that matters there is the one taken BEFORE the clock goes up.
// Joint (0.7.216.7): the joint power arm's own change. Its plan must be the clock limit alone, one step, or nothing
// is sent; its two messages go through SmuCpuJointMessage (CpuMessageEx).
static NTSTATUS CpuApplyEx(BC250_DEVICE* Device, const struct bc250_cpu_settings* To, const char* Why,
                           enum bc250_cpu_error* Error, BOOLEAN Restore, BOOLEAN Joint)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    struct bc250_cpu_plan plan;
    struct bc250_cpu_settings applied;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i, mv = 0;
    enum bc250_cpu_error error;

    RtlZeroMemory(&plan, sizeof(plan));
    applied = s->Applied;
    error = bc250_cpu_plan(&applied, To, s->Lab ? 1 : 0, &plan);
    if (Error != NULL) *Error = error;
    if (error == BC250_CPU_ERROR_NOTHING) {
        // Nothing to send is not a failure, and it is never silent: a revert whose target is already in force
        // must say so, or a receipt records a change that did not happen (0.7.210 reverted this way on the
        // first trial of every start, and the undervolt stayed in the chip).
        GuardLog("cpu: %s sends nothing: the chip already has what it asks for", Why);
        return STATUS_SUCCESS;
    }
    if (error != BC250_CPU_OK) return STATUS_INVALID_PARAMETER;
    // Never take a control this start could not give back (0.7.216.24, BD-094). A clock limit is given back from
    // the baseline, and a baseline without a per-core answer names the P-state table's top alone, which is UNDER
    // the firmware's own boost: sending it back is the cut that held unit A at 3200 MHz until a restart. The read
    // stage makes the firmware answer the ceiling (CpuBoostProbe); when even that failed, the limit is refused
    // here instead, and the processor keeps the clocks its own firmware chooses. A restore always goes out: it is
    // the way back out of the chip, and nothing else would take a change out of it.
    if (!Restore) {
        for (i = 0; i < plan.count; i++) {
            if (plan.step[i].kind != BC250_CPU_STEP_CLOCK) continue;
            if (s->BaselineValid && s->BaselineRead.boost_given) break;
            if (Error != NULL) *Error = BC250_CPU_ERROR_NO_CEILING;
            GuardLog("cpu: %s asks for a clock limit of %lu MHz and this start does not know the firmware's own "
                     "ceiling", Why, To->max_given ? To->max_mhz : 0u);
            GuardLog("cpu: %s is refused: the way back could only be the P-state top %lu MHz, under the boost "
                     "(BD-094)", Why, s->BaselineValid ? s->BaselineRead.table_mhz : 0u);
            return STATUS_INVALID_DEVICE_STATE;
        }
    }
    if (Joint && (plan.count != 1 || plan.step[0].kind != BC250_CPU_STEP_CLOCK)) {
        GuardLog("cpu: %s is not the clock limit alone (%lu steps): nothing is sent", Why, plan.count);
        return STATUS_INVALID_PARAMETER;
    }
    if (!SmuCpuBegin(&Device->Smu)) return STATUS_DEVICE_BUSY;
    for (i = 0; i < plan.count; i++) {
        if (i) CpuWait(BC250_CPU_MESSAGE_GAP_MS);
        status = CpuMessageEx(Device, plan.step[i].queue, plan.step[i].message, plan.step[i].parameter, TRUE,
                              Restore || plan.step[i].cools ? TRUE : FALSE, Joint, NULL);
        if (!NT_SUCCESS(status)) break;
        // The chip has this step now, whatever happens to the next one.
        if (plan.step[i].kind == BC250_CPU_STEP_CLOCK) { applied.max_given = 1; applied.max_mhz = To->max_mhz; }
        else if (plan.step[i].kind == BC250_CPU_STEP_UV) { applied.uv_given = 1; applied.uv_steps = To->uv_steps; }
        else if (plan.step[i].kind == BC250_CPU_STEP_TEMP) { applied.temp_given = 1; applied.temp_c = To->temp_c; }
        // The undervolt is in place and another step follows: read the voltage now and stop here if it is not a
        // voltage this driver understands, so that a raise never goes out over an operating point nobody knows.
        // The plan puts the undervolt step before a clock raise exactly so that this reading comes first.
        if (plan.step[i].kind == BC250_CPU_STEP_UV && i + 1 < plan.count) {
            CpuWait(BC250_CPU_MESSAGE_GAP_MS);
            status = CpuMessage(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CPU_MV, 0, FALSE, TRUE, &mv);
            if (!NT_SUCCESS(status)) break;
            if (mv > BC250_CPU_REFUSE_MV || mv < BC250_CPU_PLAUSIBLE_MIN_MV || mv > BC250_CPU_PLAUSIBLE_MAX_MV) {
                GuardLog("cpu: %s read %lu mV back after the undervolt, outside %lu..%lu (refusal line %lu)",
                         Why, mv, (ULONG)BC250_CPU_PLAUSIBLE_MIN_MV, (ULONG)BC250_CPU_PLAUSIBLE_MAX_MV,
                         (ULONG)BC250_CPU_REFUSE_MV);
                GuardLog("cpu: %s the steps that raise the voltage are NOT sent", Why);
                status = STATUS_DEVICE_CONFIGURATION_ERROR;
                break;
            }
        }
    }
    if (NT_SUCCESS(status)) {
        CpuWait(BC250_CPU_MESSAGE_GAP_MS);
        status = CpuMessageEx(Device, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CPU_MV, 0, FALSE, TRUE, Joint, &mv);
    }
    SmuCpuEnd(&Device->Smu);
    s->Applied = applied;
    if (NT_SUCCESS(status)) {
        KIRQL irql;
        KeAcquireSpinLock(&s->SnapLock, &irql);
        s->Snap.VoltageMv = mv;
        KeReleaseSpinLock(&s->SnapLock, irql);
        // The hard rule of this surface: a voltage above the refusal line, or one that is not a voltage at all,
        // fails the change, and the caller's own way back undoes it (every caller of a setter has one). The
        // reported bricking ceiling is 1325 mV and nothing here goes near it. A restore has no further way back,
        // so it says that instead of claiming an undo it cannot do.
        if (mv > BC250_CPU_REFUSE_MV || mv < BC250_CPU_PLAUSIBLE_MIN_MV || mv > BC250_CPU_PLAUSIBLE_MAX_MV) {
            GuardLog("cpu: %s read %lu mV back, outside %lu..%lu (refusal line %lu)", Why, mv,
                     (ULONG)BC250_CPU_PLAUSIBLE_MIN_MV, (ULONG)BC250_CPU_PLAUSIBLE_MAX_MV,
                     (ULONG)BC250_CPU_REFUSE_MV);
            GuardLog("cpu: %s voltage refusal: %s", Why,
                     Restore ? "this WAS the way back, and the next cold boot is the last one"
                             : "the settings before it come back now");
            status = STATUS_DEVICE_CONFIGURATION_ERROR;
        }
    }
    CpuLogSettings(Why, &applied);
    GuardLog("cpu: %s sent %lu of %lu steps, %lu mV back: 0x%08X", Why, i, plan.count, mv, status);
    return status;
}

static NTSTATUS CpuApply(BC250_DEVICE* Device, const struct bc250_cpu_settings* To, const char* Why,
                         enum bc250_cpu_error* Error, BOOLEAN Restore)
{
    return CpuApplyEx(Device, To, Why, Error, Restore, FALSE);
}

// The way back to a named state (0.7.211). *Before is what the caller wants back - the state before a trial, or
// nothing given at all for a reset - and bc250_cpu_restore_target names every control this driver has sent, out
// of *Before, the recorded baseline or the firmware's own default. Without that step a target that gives nothing
// inherits what is applied (bc250_cpu_plan), the plan is empty, and the chip keeps the trial while the log says
// it came back: that was the defect of 0.7.210, and it is the reason this function exists.
static NTSTATUS CpuRestoreTo(BC250_DEVICE* Device, const struct bc250_cpu_settings* Before, const char* Why)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    struct bc250_cpu_settings back;
    RtlZeroMemory(&back, sizeof(back));
    if (!bc250_cpu_restore_target(&s->Applied, Before, s->BaselineValid ? &s->Baseline : NULL, &back)) {
        GuardLog("cpu: %s cannot name the clock limit to go back to - no clock answered this start", Why);
        GuardLog("cpu: %s the applied limit of %lu MHz STAYS in the chip until a restart", Why,
                 s->Applied.max_mhz);
    }
    return CpuApply(Device, &back, Why, NULL, TRUE);
}

// A revert does not fail halfway: it sends the plan back to the state before the trial and, when a step is
// refused, says so loudly and stays owed. RevertOwed keeps the worker trying every BC250_CPU_REVERT_RETRY_MS,
// because the one thing that must not happen is a trial nobody takes back: a hot part, a busy GPU and a mailbox
// timeout are all states that pass by themselves, and 0.7.210 cleared the trial at the first refusal and never
// looked again.
static void CpuRevert(BC250_DEVICE* Device, const char* Why)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    KIRQL irql;
    NTSTATUS status = CpuRestoreTo(Device, &s->TrialBefore, Why);
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->Snap.Reverts++;
    s->OnTrial = FALSE;
    s->TrialDeadline = 0;
    s->TrialSerial++;
    s->RevertOwed = NT_SUCCESS(status) ? FALSE : TRUE;
    if (!NT_SUCCESS(status)) s->Snap.RevertFailures++;
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (!NT_SUCCESS(status)) {
        GuardLog("cpu: the revert (%s) did NOT go through 0x%08X; it stays owed and runs again every %lu ms",
                 Why, status, (ULONG)BC250_CPU_REVERT_RETRY_MS);
        KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
    }
}

// One attempt at an owed revert, from the worker alone and under Lock. The target cannot drift: Applied names
// what the chip has and TrialBefore what it must go back to, both unchanged until a revert succeeds.
static void CpuRevertRetry(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    KIRQL irql;
    NTSTATUS status;
    ULONG attempts;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    attempts = ++s->Snap.RevertRetries;
    KeReleaseSpinLock(&s->SnapLock, irql);
    status = CpuRestoreTo(Device, &s->TrialBefore, "the owed revert");
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->RevertOwed = NT_SUCCESS(status) ? FALSE : TRUE;
    if (!NT_SUCCESS(status)) s->Snap.RevertFailures++;
    KeReleaseSpinLock(&s->SnapLock, irql);
    // One line for the first attempt and then one in ten: a part that stays hot must not fill the log ring.
    if (NT_SUCCESS(status)) GuardLog("cpu: the owed revert went through at attempt %lu", attempts);
    else if (attempts == 1 || attempts % 10 == 0)
        GuardLog("cpu: the owed revert is still refused 0x%08X at attempt %lu", status, attempts);
}

// ---- the joint power arm (0.7.216.7) ----------------------------------------------------------------------------
// The DPM governor's thread decides (bc250_joint_step, behind DpmJointGovernor, default off) and writes the cap it
// wants into JointWantMHz; the worker alone sends it, under Lock, through CpuApplyEx with Joint set. The arm therefore
// has every rule any other change has - the plan's order, the voltage readback with its refusal line, the hot gate,
// the sequence flag, the allowlist and the argument range - and one rule lifted: the busy gate, for its two messages
// alone (smu.h, SmuCpuJointMessage). Applied names the arm's cap while it is in force, because that is what the chip
// has; JointBefore names what was applied before it, and the release restores exactly that through
// bc250_cpu_restore_target, as a revert does. The operator comes first: a SET, a RESET and a search release the cap
// before they run, and the arm takes no cap while a trial, a search or an owed revert is open.

C_ASSERT(BC250_JOINT_MIN_MHZ == BC250_CPU_MIN_MHZ);

// What the governor reads each tick: whether the surface can take a cap now, and the limit the cap is measured from.
// Under Lock.
static void CpuJointPublish(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    const struct bc250_cpu_settings* from =
        InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0) ? &s->JointBefore : &s->Applied;
    ULONG base = 0;
    BOOLEAN ready;
    KIRQL irql;
    if (from->max_given) base = from->max_mhz;
    else if (s->BaselineValid && s->Baseline.max_given) base = s->Baseline.max_mhz;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    ready = s->Enabled && s->Created && s->Proven && !s->OnTrial && !s->RevertOwed && !s->Search.running;
    KeReleaseSpinLock(&s->SnapLock, irql);
    // The arm takes no cap it could only release as the P-state table's top (0.7.216.24, BD-094): with no limit
    // applied the release goes back to the baseline, and a baseline without a per-core answer is under the
    // firmware's own boost. CpuApplyEx refuses such a cap as well; this keeps the governor from asking.
    if (!from->max_given && !(s->BaselineValid && s->BaselineRead.boost_given)) ready = FALSE;
    if (s->JointFault || s->JointPaused || CpuAdapterDown(Device) || base < BC250_CPU_MIN_MHZ) ready = FALSE;
    InterlockedExchange(&s->JointBaseMHz, (LONG)base);
    InterlockedExchange(&s->JointReady, ready ? 1 : 0);
}

// The cap out of the chip: back to JointBefore, as a restore (every step passes the hot gate). JointAppliedMHz is
// cleared once the chip has the limit back, whatever the readback after it said. Under Lock.
static NTSTATUS CpuJointRelease(BC250_DEVICE* Device, const char* Why)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    struct bc250_cpu_settings back;
    NTSTATUS status;
    if (!InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0)) return STATUS_SUCCESS;
    RtlZeroMemory(&back, sizeof(back));
    if (!bc250_cpu_restore_target(&s->Applied, &s->JointBefore, s->BaselineValid ? &s->Baseline : NULL, &back)) {
        // Not reachable: the arm takes no cap without a base it can name (CpuJointPublish).
        GuardLog("cpu: %s cannot name the limit to go back to; the cap of %lu MHz stays", Why, s->Applied.max_mhz);
        return STATUS_INVALID_DEVICE_STATE;
    }
    status = CpuApplyEx(Device, &back, Why, NULL, TRUE, TRUE);
    if (s->Applied.max_given == back.max_given && s->Applied.max_mhz == back.max_mhz) {
        InterlockedExchange(&s->JointAppliedMHz, 0);
        RtlZeroMemory(&s->JointBefore, sizeof(s->JointBefore));
    }
    return status;
}

// One piece of the arm's work, from the worker alone, under Lock: the cap the governor wants into the chip, or out of
// it. A refusal is tried again after BC250_CPU_JOINT_RETRY_MS (the hot gate refuses a step up on a hot part, a
// timeout passes); a cap whose voltage readback fails turns the arm off for this start and goes out again.
static void CpuJointService(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    ULONG want, applied, base, refusals;
    ULONGLONG now = KeQueryInterruptTime();
    NTSTATUS status;

    CpuJointPublish(Device);
    if (CpuAdapterDown(Device)) return;
    want = (ULONG)InterlockedCompareExchange(&s->JointWantMHz, 0, 0);
    applied = (ULONG)InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0);
    base = (ULONG)InterlockedCompareExchange(&s->JointBaseMHz, 0, 0);
    if (!InterlockedCompareExchange(&s->JointReady, 0, 0)) want = 0;        // not ready: a release, or nothing
    if (want && (want < BC250_CPU_MIN_MHZ || want >= base)) want = 0;      // the policy never asks for this
    if (want == applied || now < s->JointNextTry) return;
    if (!want) status = CpuJointRelease(Device, "the joint arm's release");
    else {
        struct bc250_cpu_settings to;
        if (!applied) s->JointBefore = s->Applied;
        RtlZeroMemory(&to, sizeof(to));
        to.max_given = 1;
        to.max_mhz = want;
        // A lower limit cools and passes the hot gate (bc250_cpu_plan marks it); a step back up waits for a cool part.
        status = CpuApplyEx(Device, &to, applied && want > applied ? "the joint arm's step up" : "the joint arm's cap",
                            NULL, FALSE, TRUE);
        if (s->Applied.max_given && s->Applied.max_mhz == want) {
            InterlockedExchange(&s->JointAppliedMHz, (LONG)want);
            if (!NT_SUCCESS(status)) {
                s->JointFault = TRUE;
                GuardLog("cpu: the joint arm's cap of %lu MHz failed its readback 0x%08X: the arm is off for this start",
                         want, status);
                (void)CpuJointRelease(Device, "the joint arm's fault");
            }
        } else if (!applied) RtlZeroMemory(&s->JointBefore, sizeof(s->JointBefore));
    }
    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&s->JointSends);
        s->JointNextTry = 0;
    } else {
        refusals = (ULONG)InterlockedIncrement(&s->JointRefusals);
        s->JointNextTry = now + 10000ull * BC250_CPU_JOINT_RETRY_MS;
        if (refusals == 1 || refusals % 10 == 0)
            GuardLog("cpu: the joint arm's change to %lu MHz is refused 0x%08X (refusal %lu), again in %lu ms", want,
                     status, refusals, (ULONG)BC250_CPU_JOINT_RETRY_MS);
    }
    CpuJointPublish(Device);
}

// TRUE while the worker owes the arm a change it can make: the poll keeps running until it is made.
static BOOLEAN CpuJointOwed(BC250_CPU_STATE* S)
{
    ULONG want = (ULONG)InterlockedCompareExchange(&S->JointWantMHz, 0, 0);
    if (S->JointPaused) return FALSE;       // the power path owns the mailbox; CpuResume clears the cap's record
    if (!InterlockedCompareExchange(&S->JointReady, 0, 0)) want = 0;
    return want != (ULONG)InterlockedCompareExchange(&S->JointAppliedMHz, 0, 0) ? TRUE : FALSE;
}

// The operator's SET, RESET and SEARCH_BEGIN take the cap out first, so that what they record as "before" is the
// operator's own state and never the arm's. Under Lock. FALSE when the cap could not go: the operation is refused.
static BOOLEAN CpuJointYield(BC250_DEVICE* Device, ULONG Op)
{
    NTSTATUS status;
    if (Op != BC250_CPU_OP_SET && Op != BC250_CPU_OP_RESET && Op != BC250_CPU_OP_SEARCH_BEGIN) return TRUE;
    if (!InterlockedCompareExchange(&Device->Cpu.JointAppliedMHz, 0, 0)) return TRUE;
    status = CpuJointRelease(Device, "the operator's request");
    if (InterlockedCompareExchange(&Device->Cpu.JointAppliedMHz, 0, 0)) {
        GuardLog("cpu: the joint arm's cap did not go 0x%08X: the operator's op %lu is refused", status, Op);
        return FALSE;
    }
    return TRUE;
}

// ---- the worker thread ------------------------------------------------------------------------------------------
// It owns the start sequence (the read stage, the stored values, the core mask) and every trial's deadline. Nothing
// else in this file sends a message without holding Lock, and the thread holds it for one piece of work at a time.

static KSTART_ROUTINE CpuThread;
static void CpuThread(_In_ PVOID Context)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_CPU_STATE* s = &device->Cpu;
    PVOID objects[2];
    LARGE_INTEGER timeout;
    ULONGLONG nextRetry = 0;        // when an owed revert may be tried again

    objects[0] = &s->StopEvent;
    objects[1] = &s->Wake;
    timeout.QuadPart = -10000ll * (LONGLONG)CPU_START_DELAY_MS;
    if (KeWaitForSingleObject(&s->StopEvent, Executive, KernelMode, FALSE, &timeout) != STATUS_TIMEOUT) {
        PsTerminateSystemThread(STATUS_SUCCESS);
        return;
    }
    for (;;) {
        BOOLEAN work, onTrial, over = FALSE, owed, joint;
        KIRQL irql;
        CpuLock(s);
        // While the adapter is paused or the owner is gone, the worker does nothing at all: the start work stays
        // owed and a deadline that passes is served at the next wake-up with the mailbox back.
        work = s->StartWork && !CpuAdapterDown(device);
        if (work) s->StartWork = FALSE;
        if (work) {
            // The read stage first, always: it is what admits every setter of this start, and it is also what
            // records the baseline. The first queue 3 traffic of a start keeps the slow rate.
            NTSTATUS status = CpuReadStage(device, TRUE, BC250_CPU_MESSAGE_GAP_MS);
            s->Proven = NT_SUCCESS(status) ? TRUE : FALSE;
            GuardLog("cpu: the read stage %s (0x%08X): %lu mV, GPU %lu mV, cap %lu C",
                     s->Proven ? "answered" : "did NOT answer", status, device->Cpu.Snap.VoltageMv,
                     device->Cpu.Snap.GpuVoltageMv, device->Cpu.Snap.CapC);
            if (!s->Proven) {
                CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_NOT_PROVEN);
            } else {
                // CpuReadStage has recorded the baseline from what the firmware answered. Applied stays empty:
                // this driver has sent nothing yet, and saying otherwise would report settings the chip has
                // from its own firmware as ours.
                if (s->Stored.max_given || s->Stored.uv_given || s->Stored.temp_given) {
                    enum bc250_cpu_error error = BC250_CPU_OK;
                    struct bc250_cpu_settings stored = s->Stored;
                    // A clock limit this start could not give back is left OUT of the plan, and the rest of the
                    // stored settings still go in (0.7.216.24, BD-094). CpuApplyEx refuses a whole plan that
                    // carries such a step, which is right for a request an operator sent and wrong here: the
                    // undervolt and the temperature cap of the registry have nothing to do with the ceiling, and
                    // losing them would turn one refused control into three. s->Stored itself does not move, so
                    // the surface still reports what is on disk.
                    if (stored.max_given && !(s->BaselineValid && s->BaselineRead.boost_given)) {
                        GuardLog("cpu: the stored clock limit of %lu MHz is left out: this start does not know "
                                 "the firmware's own ceiling (BD-094); the rest of the stored settings go in",
                                 stored.max_mhz);
                        stored.max_given = 0;
                        stored.max_mhz = 0;
                    }
                    if (!stored.max_given && !stored.uv_given && !stored.temp_given)
                        GuardLog("cpu: the stored settings were that clock limit alone: nothing is sent");
                    else
                        status = CpuApply(device, &stored, "the stored settings", &error, FALSE);
                    if (NT_SUCCESS(status)) CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_OK);
                    else {
                        struct bc250_cpu_settings none;
                        CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_HARDWARE);
                        // CpuApply stops in the middle of its plan, so the steps before the refusal ARE in the
                        // chip. Saying "the firmware's own values stand" was false; this undoes them instead,
                        // and the log then names what is left if even that does not go through.
                        GuardLog("cpu: the stored settings did not go through 0x%08X (error %d); the steps that "
                                 "did are undone now", status, (int)error);
                        RtlZeroMemory(&none, sizeof(none));
                        (void)CpuRestoreTo(device, &none, "the stored settings failed");
                    }
                }
                // The core mask is the CU mode's shape: sent now, visible after the next restart.
                if (s->CoreMask && s->CoreMask != BC250_CPU_MASK_STOCK) {
                    if (SmuCpuBegin(&device->Smu)) {
                        status = CpuMessage(device, BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_SET_CORE_ENABLE_MASK,
                                            s->CoreMask, TRUE, FALSE, NULL);
                        SmuCpuEnd(&device->Smu);
                        GuardLog("cpu: core mask 0x%02X (%lu cores) sent: 0x%08X; the count changes at the next "
                                 "restart", s->CoreMask, bc250_cpu_mask_cores(s->CoreMask), status);
                    }
                }
            }
        }
        KeAcquireSpinLock(&s->SnapLock, &irql);
        onTrial = s->OnTrial;
        owed = s->RevertOwed;
        if (onTrial && KeQueryInterruptTime() >= s->TrialDeadline) over = TRUE;
        KeReleaseSpinLock(&s->SnapLock, irql);
        if (over && !CpuAdapterDown(device)) {
            GuardLog("cpu: the trial window is over; the settings before it come back");
            CpuRevert(device, "trial over");
            nextRetry = KeQueryInterruptTime() + 10000ull * BC250_CPU_REVERT_RETRY_MS;
        } else if (owed && !CpuAdapterDown(device) && KeQueryInterruptTime() >= nextRetry) {
            // The one state this surface must not settle in: a change in the chip that the driver has said it
            // took back. The part cools, the GPU goes quiet, and then this goes through (0.7.211).
            CpuRevertRetry(device);
            nextRetry = KeQueryInterruptTime() + 10000ull * BC250_CPU_REVERT_RETRY_MS;
        }
        // The joint power arm (0.7.216.7) after the operator's own work: a trial's deadline and an owed revert come
        // first, and the arm takes no cap while either is open (CpuJointPublish). With DpmJointGovernor 0 nothing
        // ever writes JointWantMHz, so this publishes the readiness and sends nothing.
        CpuJointService(device);
        KeAcquireSpinLock(&s->SnapLock, &irql);
        owed = s->RevertOwed;
        KeReleaseSpinLock(&s->SnapLock, irql);
        joint = CpuJointOwed(s);
        CpuUnlock(s);
        timeout.QuadPart = -10000ll * (LONGLONG)BC250_CPU_POLL_MS;
        if (KeWaitForMultipleObjects(2, objects, WaitAny, Executive, KernelMode, FALSE,
                                     onTrial || over || owed || joint ? &timeout : NULL, NULL) == STATUS_WAIT_0)
            break;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ---- the start, the stop and the confirmation -------------------------------------------------------------------

void CpuInitialize(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    RtlZeroMemory(s, sizeof(*s));
    KeInitializeMutex(&s->Lock, 0);
    KeInitializeSpinLock(&s->SnapLock);
    KeInitializeEvent(&s->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&s->Wake, SynchronizationEvent, FALSE);
    s->TrialMs = BC250_CPU_TRIAL_MS;
    s->CoreMask = BC250_CPU_MASK_STOCK;
}

// StartDevice, after DpmStart: the registry, the guard's marks and, if this machine opted in, the worker thread.
// No mailbox message runs here; the thread sends the first one CPU_START_DELAY_MS later. PASSIVE_LEVEL.
void CpuStart(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    OBJECT_ATTRIBUTES attributes;
    HANDLE handle;
    NTSTATUS status;
    ULONG tune = 0, lab = 0, value = 0, pending = 0, confirmed = 0, mask = 0, corePending = 0, coreConfirmed = 0;
    BOOLEAN pendingPresent, corePendingPresent, maskPresent;

    CpuLock(s);
    if (s->Created) { CpuUnlock(s); return; }
    RtlZeroMemory(&s->Applied, sizeof(s->Applied));
    RtlZeroMemory(&s->Stored, sizeof(s->Stored));
    RtlZeroMemory(&s->Baseline, sizeof(s->Baseline));
    RtlZeroMemory(&s->BaselineRead, sizeof(s->BaselineRead));
    RtlZeroMemory(&s->Search, sizeof(s->Search));
    s->BaselineValid = s->Proven = s->OnTrial = s->RevertOwed = FALSE;
    s->Pending = s->Confirmed = s->CorePending = s->CoreConfirmed = FALSE;
    s->TrialDeadline = 0;
    s->Generation = Device->StartHealth.Generation;
    s->CoreMask = BC250_CPU_MASK_STOCK;
    s->CoreMaskStored = 0;
    // The joint power arm starts with no cap and no fault; the governor's thread may already run (DpmStart is first)
    // and reads JointReady 0 until this start's read stage has answered.
    InterlockedExchange(&s->JointWantMHz, 0);
    InterlockedExchange(&s->JointAppliedMHz, 0);
    InterlockedExchange(&s->JointReady, 0);
    InterlockedExchange(&s->JointBaseMHz, 0);
    InterlockedExchange(&s->JointSends, 0);
    InterlockedExchange(&s->JointRefusals, 0);
    s->JointFault = s->JointPaused = FALSE;
    RtlZeroMemory(&s->JointBefore, sizeof(s->JointBefore));
    s->JointNextTry = 0;
    KeClearEvent(&s->StopEvent);
    (void)CpuQueryPresent(CPU_SETTING_TUNE, &tune);
    (void)CpuQueryPresent(CPU_SETTING_LAB, &lab);
    s->Enabled = (tune == 1) && Device->FullWddm && Device->Smu.Online && Device->Smu.CpuOnline;
    s->Lab = lab == 1;
    if (CpuQueryPresent(CPU_SETTING_TRIAL, &value) && value) {
        if (value < BC250_CPU_TRIAL_MIN_MS) value = BC250_CPU_TRIAL_MIN_MS;
        if (value > BC250_CPU_TRIAL_MAX_MS) value = BC250_CPU_TRIAL_MAX_MS;
        s->TrialMs = value;
    } else s->TrialMs = BC250_CPU_TRIAL_MS;
    if (!s->Enabled) {
        GuardLog("cpu: the CPU surface is off (CpuTune %lu, WDDM %u, SMU %u, queue 3 %u): read-only, no thread, "
                 "no message", tune, Device->FullWddm ? 1u : 0u, Device->Smu.Online ? 1u : 0u,
                 Device->Smu.CpuOnline ? 1u : 0u);
        if (tune == 1) CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_OFF);
        CpuUnlock(s);
        return;
    }
    // The stored settings, all or nothing: a value outside its range refuses the whole set, because a half-applied
    // set is a state nobody chose.
    s->Stored.max_given = CpuQueryPresent(CPU_SETTING_MAX, &value) ? 1 : 0;
    s->Stored.max_mhz = s->Stored.max_given ? value : 0;
    s->Stored.uv_given = CpuQueryPresent(CPU_SETTING_UV, &value) ? 1 : 0;
    s->Stored.uv_steps = s->Stored.uv_given ? value : 0;
    s->Stored.temp_given = CpuQueryPresent(CPU_SETTING_TEMP, &value) ? 1 : 0;
    s->Stored.temp_c = s->Stored.temp_given ? value : 0;
    pendingPresent = CpuQueryPresent(CPU_SETTING_PENDING, &pending);
    (void)CpuQueryPresent(CPU_SETTING_CONFIRMED, &confirmed);
    if (s->Stored.max_given || s->Stored.uv_given || s->Stored.temp_given) {
        enum bc250_cpu_error error = bc250_cpu_settings_check(&s->Stored, s->Lab ? 1 : 0);
        if (pendingPresent) {
            GuardLog("cpu: the stored settings (mark 0x%08X) ran a start that never became healthy: they are "
                     "deleted and the firmware's own values stand", pending);
            CpuDeleteLogged(CPU_SETTING_MAX);
            CpuDeleteLogged(CPU_SETTING_UV);
            CpuDeleteLogged(CPU_SETTING_TEMP);
            CpuDeleteLogged(CPU_SETTING_PENDING);
            CpuDeleteLogged(CPU_SETTING_CONFIRMED);
            CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_UNCONFIRMED);
            RtlZeroMemory(&s->Stored, sizeof(s->Stored));
        } else if (error != BC250_CPU_OK) {
            GuardLog("cpu: the stored settings are refused (error %d): the firmware's own values stand", (int)error);
            CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_REFUSED);
            RtlZeroMemory(&s->Stored, sizeof(s->Stored));
        } else if (!NT_SUCCESS(GuardStoreSetting(CPU_SETTING_PENDING, CpuMark(&s->Stored)))) {
            GuardLog("cpu: the pending mark is not durable: the stored settings are not applied this start");
            CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_REGISTRY);
            RtlZeroMemory(&s->Stored, sizeof(s->Stored));
        } else {
            s->Pending = TRUE;
            s->Confirmed = confirmed == CpuMark(&s->Stored);
            CpuLogSettings("stored", &s->Stored);
        }
    } else if (pendingPresent) {
        CpuDeleteLogged(CPU_SETTING_PENDING);
    }
    // The core mask, with its own two marks: a mask that the firmware took and the machine then did not survive
    // comes back as the stock mask.
    maskPresent = CpuQueryPresent(CPU_SETTING_CORE_MASK, &mask);
    corePendingPresent = CpuQueryPresent(CPU_SETTING_CORE_PENDING, &corePending);
    (void)CpuQueryPresent(CPU_SETTING_CORE_CONFIRMED, &coreConfirmed);
    s->CoreMaskStored = maskPresent ? mask : 0;
    if (maskPresent && bc250_cpu_mask_allowed(mask) && mask != BC250_CPU_MASK_STOCK) {
        if (corePendingPresent) {
            GuardLog("cpu: core mask 0x%02X ran a start that never became healthy: the stock mask comes back",
                     corePending);
            CpuStoreLogged(CPU_SETTING_CORE_MASK, BC250_CPU_MASK_STOCK);
            CpuDeleteLogged(CPU_SETTING_CORE_PENDING);
            CpuDeleteLogged(CPU_SETTING_CORE_CONFIRMED);
            s->CoreMask = BC250_CPU_MASK_STOCK;
        } else if (!NT_SUCCESS(GuardStoreSetting(CPU_SETTING_CORE_PENDING, mask))) {
            GuardLog("cpu: the core mask's pending mark is not durable: the stock mask runs");
            s->CoreMask = BC250_CPU_MASK_STOCK;
        } else {
            s->CorePending = TRUE;
            s->CoreConfirmed = coreConfirmed == mask;
            s->CoreMask = mask;
        }
    } else if (corePendingPresent && !maskPresent) {
        CpuDeleteLogged(CPU_SETTING_CORE_PENDING);
    }
    s->Cores = (ULONG)KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    s->Threads = s->Cores;
    s->StartWork = TRUE;
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &attributes, NULL, NULL, CpuThread, Device);
    if (NT_SUCCESS(status)) {
        status = ObReferenceObjectByHandle(handle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID*)&s->Thread, NULL);
        if (!NT_SUCCESS(status)) {
            KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
            (void)ZwWaitForSingleObject(handle, FALSE, NULL);
            s->Thread = NULL;
        }
        ZwClose(handle);
    }
    if (NT_SUCCESS(status)) s->Created = TRUE;
    else GuardLog("cpu: the worker thread did NOT start 0x%08X: the surface stays read-only", status);
    // A pair of lines: one is 171 characters at its widest, over the 159 a log line holds (guardlog-width).
    GuardLog("cpu: the surface is on (CpuTune 1%s), trial window %lu ms",
             s->Lab ? ", CpuLab 1" : "", s->TrialMs);
    GuardLog("cpu: the surface sees %lu processors, core mask 0x%02X%s", s->Cores, s->CoreMask,
             s->Created ? "" : " - without a worker thread");
    CpuUnlock(s);
}

// StopDevice (before SmuOwnerStop) and RemoveDevice. A trial never outlives the driver that watches it: the
// settings before it come back here, while the mailbox is still ours. Settings somebody kept stay in the chip and
// on disk: the next start reads them back. PASSIVE_LEVEL, idempotent.
void CpuStop(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    BOOLEAN onTrial, owed;
    KIRQL irql;

    CpuLock(s);
    if (!s->Created) { CpuUnlock(s); CpuLogSummary(Device); return; }
    CpuUnlock(s);
    KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
    (void)KeWaitForSingleObject(s->Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(s->Thread);
    CpuLock(s);
    s->Thread = NULL;
    s->Created = FALSE;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    onTrial = s->OnTrial;
    owed = s->RevertOwed;
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (onTrial) CpuRevert(Device, "stop");
    else if (owed) CpuRevertRetry(Device);          // the thread is gone: this is the last attempt there is
    KeAcquireSpinLock(&s->SnapLock, &irql);
    owed = s->RevertOwed;
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (owed)
        GuardLog("cpu: a revert is still owed at the stop; the applied settings stay in the chip until the "
                 "next cold boot");
    // The joint arm's cap does not outlive the driver that set it (0.7.216.7). A trial cannot be open at the same
    // time (the arm takes no cap during one), so this is the only change left to take back.
    if (InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0) && !CpuAdapterDown(Device)) {
        NTSTATUS status = CpuJointRelease(Device, "stop");
        if (InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0))
            GuardLog("cpu: the joint arm's cap of %lu MHz stays in the chip after the stop 0x%08X; a lower limit "
                     "is the safe side", (ULONG)InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0), status);
    }
    s->Proven = FALSE;
    CpuJointPublish(Device);
    CpuUnlock(s);
    CpuLogSummary(Device);
}

// Before a transition out of D0 (pnp.c, before DpmPause): a trial does not cross a power transition. The revert
// runs here, while the mailbox is still ours; the worker then sends nothing until the adapter is back, because
// every one of its messages goes through the governor's paused flag below. PASSIVE_LEVEL.
void CpuPause(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    BOOLEAN onTrial, owed;
    KIRQL irql;
    if (!s->Created) return;
    CpuLock(s);
    KeAcquireSpinLock(&s->SnapLock, &irql);
    onTrial = s->OnTrial;
    owed = s->RevertOwed;
    RtlZeroMemory(&s->Search, sizeof(s->Search));
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (onTrial) {
        GuardLog("cpu: the trial ends with the power transition; the settings before it come back");
        CpuRevert(Device, "power down");
    } else if (owed) CpuRevertRetry(Device);        // the worker sends nothing in D3: try while the mailbox is ours
    // The joint arm's cap goes too, while the mailbox is ours, and no new one is taken until CpuResume (0.7.216.7).
    s->JointPaused = TRUE;
    if (InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0)) (void)CpuJointRelease(Device, "power down");
    CpuJointPublish(Device);
    CpuUnlock(s);
}

// After a successful return to D0 (pnp.c, beside DpmResume). Nothing of this surface survives the transition in
// the chip: the firmware is back at its own values, so the cached Applied, the proven mark and the recorded
// baseline all describe a state that is gone (0.7.211; without this the driver reported an undervolt the chip
// did not have, and re-applying it was refused as "this setting is in use already"). The worker therefore owes
// the whole start sequence again: the read stage, the new baseline and the stored settings. PASSIVE_LEVEL.
void CpuResume(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    KIRQL irql;
    if (!s->Created) return;
    CpuLock(s);
    RtlZeroMemory(&s->Applied, sizeof(s->Applied));
    RtlZeroMemory(&s->Baseline, sizeof(s->Baseline));
    RtlZeroMemory(&s->BaselineRead, sizeof(s->BaselineRead));
    s->BaselineValid = s->Proven = FALSE;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    s->OnTrial = FALSE;
    s->TrialDeadline = 0;
    s->RevertOwed = FALSE;          // what it would have put back is gone from the chip with the power
    RtlZeroMemory(&s->TrialBefore, sizeof(s->TrialBefore));
    RtlZeroMemory(&s->Search, sizeof(s->Search));
    KeReleaseSpinLock(&s->SnapLock, irql);
    s->StartWork = TRUE;
    // The joint arm's cap is gone from the chip with the rest; the arm waits for the read stage again (0.7.216.7).
    InterlockedExchange(&s->JointAppliedMHz, 0);
    RtlZeroMemory(&s->JointBefore, sizeof(s->JointBefore));
    s->JointNextTry = 0;
    s->JointPaused = FALSE;
    CpuJointPublish(Device);
    CpuUnlock(s);
    KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
    GuardLog("cpu: the power transition is over; the read stage and the stored settings run again");
}

// A durably confirmed start confirms this surface's two guards, in the guard's order: the pending mark away first,
// the confirmed mark second. Its own failure costs a reconfirmation at the next start, never this confirmation.
NTSTATUS CpuConfirm(BC250_DEVICE* Device, _In_z_ const char* Why)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    NTSTATUS status = STATUS_SUCCESS;
    CpuLock(s);
    if (s->Pending) {
        ULONG mark = CpuMark(&s->Stored);
        status = GuardDeleteSetting(CPU_SETTING_PENDING);
        if (NT_SUCCESS(status)) {
            s->Pending = FALSE;
            status = GuardStoreSetting(CPU_SETTING_CONFIRMED, mark);
            if (NT_SUCCESS(status)) s->Confirmed = TRUE;
        }
        GuardLog("cpu: the stored settings (mark 0x%08X) confirmed by %s: 0x%08X", mark, Why, status);
    }
    if (s->CorePending) {
        NTSTATUS core = GuardDeleteSetting(CPU_SETTING_CORE_PENDING);
        if (NT_SUCCESS(core)) {
            s->CorePending = FALSE;
            core = GuardStoreSetting(CPU_SETTING_CORE_CONFIRMED, s->CoreMask);
            if (NT_SUCCESS(core)) s->CoreConfirmed = TRUE;
        }
        GuardLog("cpu: core mask 0x%02X confirmed by %s: 0x%08X", s->CoreMask, Why, core);
    }
    CpuUnlock(s);
    return status;
}

void CpuLogSummary(BC250_DEVICE* Device)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    BC250_CPU_SNAP snap;
    KIRQL irql;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    snap = s->Snap;
    KeReleaseSpinLock(&s->SnapLock, irql);
    if (!s->Enabled && !snap.Reads && !snap.Writes) return;     // nothing to say on a machine that opted out
    // Four lines, by the rule of the guardlog-width gate: the state and voltages line is 207 characters at its
    // widest and the counter line 166, and a log line holds 159 (BD-070). Every line begins "cpu: summary", so
    // a reader takes the block and not one line of it.
    GuardLog("cpu: summary %s%s%s %lu mV (GPU %lu mV)",
             s->Enabled ? "on" : "off", s->Proven ? ", queue 3 proven" : ", queue 3 silent",
             s->OnTrial ? ", on trial" : "", snap.VoltageMv, snap.GpuVoltageMv);
    GuardLog("cpu: summary cap %lu C, cores %lu/%lu, mask 0x%02X", snap.CapC,
             bc250_cpu_mask_cores(s->CoreMask), (ULONG)BC250_CPU_CORES, s->CoreMask);
    CpuLogSettings("summary applied", &s->Applied);
    GuardLog("cpu: summary reads %lu writes %lu refusals %lu reverts %lu",
             snap.Reads, snap.Writes, snap.Refusals, snap.Reverts);
    GuardLog("cpu: summary last queue %lu message 0x%02X argument 0x%08X status 0x%08X",
             snap.LastQueue, snap.LastMessage, snap.LastParameter, snap.LastStatus);
    // The joint power arm (0.7.216.7), only when it ever acted: a start with DpmJointGovernor 0 logs what 0.7.216.6 did.
    if (InterlockedCompareExchange(&s->JointSends, 0, 0) || InterlockedCompareExchange(&s->JointRefusals, 0, 0) ||
        InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0) || s->JointFault)
        GuardLog("cpu: summary joint cap %lu MHz (base %lu), %lu changes, %lu refused%s",
                 (ULONG)InterlockedCompareExchange(&s->JointAppliedMHz, 0, 0),
                 (ULONG)InterlockedCompareExchange(&s->JointBaseMHz, 0, 0),
                 (ULONG)InterlockedCompareExchange(&s->JointSends, 0, 0),
                 (ULONG)InterlockedCompareExchange(&s->JointRefusals, 0, 0),
                 s->JointFault ? ", off after a failed readback" : "");
}

// ---- the escape ------------------------------------------------------------------------------------------------
// BC250_ESCAPE_RUN_CPU. Unlike every other escape of this driver, the write operations send mailbox messages, so
// display.c admits them only with HardwareAccess=1 (the Level Two exclusion) and an administrator, exactly as
// RUN_CLOCK's SET. READ is software state and takes NoAdapterSynchronization=1 alone.
//
// KEEP is the one write that sends nothing (0.7.213), so it takes NoAdapterSynchronization=1 as well, and that is
// what the CLI and the DLL now send for it: it copies Applied into Stored, writes seven values of the Parameters
// key - each one flushed to the disk (GuardStoreSetting) - and ends the trial under SnapLock. Seven registry
// flushes with the GPU scheduler suspended is what HardwareAccess asked the OS for, and this operation never
// needed it. The old flag word stays admitted for one release, for a CLI or DLL of 0.7.212 or older.
//
// Lock order: Readers (no wait) -> Lock -> SnapLock, with GuardLog and every registry write outside SnapLock. The
// worker thread takes Lock -> SnapLock and nothing else, so there is no cycle. A sequence of messages holds Lock
// but not the SMU owner lock, which SmuCpuMessage takes and releases per message, so the DPM governor's tick is
// never blocked for a whole sequence.
void CpuRequest(BC250_DEVICE* Device, BC250_ESCAPE_CPU* Data, ULONG Size, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_CPU_STATE* s = &Device->Cpu;
    D3DDDI_ESCAPEFLAGS expectedRead = {0}, expectedWrite = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    const ULONG op = Data->Op;
    const BOOLEAN write = op != BC250_CPU_OP_READ;
    const BOOLEAN keep = op == BC250_CPU_OP_KEEP;       // the one write that sends no mailbox message
    const ULONG given = Data->Given;
    // Read once: the buffer belongs to the caller, so nothing here reads an input twice.
    const BOOLEAN loaded = Data->Loaded ? TRUE : FALSE;
    const ULONG whea = Data->WheaEvents, checksum = Data->ChecksumErrors;
    const ULONGLONG expected = Data->ExpectedGeneration;
    struct bc250_cpu_settings request, applied, stored, baseline;
    struct bc250_cpu_sample sample;
    enum bc250_cpu_error error = BC250_CPU_OK;
    BC250_CPU_SNAP snap;
    ULONG trialMs = 0, remaining = 0, serial, i, searchStep = 0, searchBest = 0, searchFail = 0, searchTested = 0;
    BOOLEAN onTrial, searching = FALSE, revertOwed = FALSE, boostKnown = FALSE;
    KIRQL irql;

    RtlZeroMemory(&request, sizeof(request));
    request.max_given = (given & BC250_CPU_GIVEN_MAX) ? 1 : 0;
    request.max_mhz = Data->MaxMHz;
    request.uv_given = (given & BC250_CPU_GIVEN_UV) ? 1 : 0;
    request.uv_steps = Data->UvSteps;
    request.temp_given = (given & BC250_CPU_GIVEN_TEMP) ? 1 : 0;
    request.temp_c = Data->TempC;
    expectedRead.NoAdapterSynchronization = 1;
    expectedWrite.HardwareAccess = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)status;
    Data->Flags = Data->Error = 0;
    // READ: expectedRead alone. KEEP: expectedRead, or expectedWrite for one release. Every other operation:
    // expectedWrite alone. Any other flag word is refused, per operation.
    if (Data->AbiVersion != BC250_CPU_ABI || Size != sizeof(BC250_ESCAPE_CPU) || op > BC250_CPU_OP_SEARCH_STEP ||
        Data->Reserved[0] || Data->Reserved[1] ||
        (!write && EscapeFlags != expectedRead.Value) ||
        (write && !keep && EscapeFlags != expectedWrite.Value) ||
        (keep && EscapeFlags != expectedRead.Value && EscapeFlags != expectedWrite.Value)) return;
    // Every operation that sends a mailbox message needs an administrator, READBACK included (0.7.211). Reading
    // a voltage is not a privilege, but holding CpuLock and the SMU owner lock for 19 messages is: an
    // unprivileged loop of readbacks would delay the worker's revert and put mailbox traffic under a compute
    // load, against ADR 0014's single privileged owner. The cached values stay open to every caller through
    // READ, which sends nothing.
    if (write && !Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    status = STATUS_SUCCESS;
    if (write) {
        CpuLock(s);
        if (expected != s->Generation) status = STATUS_RETRY;
        else if (!s->Enabled || !s->Created) status = STATUS_INVALID_DEVICE_STATE;
        else if (CpuAdapterDown(Device)) status = STATUS_DEVICE_NOT_READY;
        else if (op != BC250_CPU_OP_READBACK && !s->Proven) status = STATUS_DEVICE_NOT_READY;
        else if (!CpuJointYield(Device, op)) status = STATUS_DEVICE_BUSY;
        else if (op == BC250_CPU_OP_READBACK) {
            // The first full stage of a start keeps the slow rate, whether the worker's own stage answered or
            // not: it is what records the baseline, and it is the first queue 3 traffic this part ever sees.
            status = CpuReadStage(Device, TRUE, s->BaselineValid ? BC250_CPU_GETTER_GAP_MS
                                                                : BC250_CPU_MESSAGE_GAP_MS);
            if (NT_SUCCESS(status)) s->Proven = TRUE;
        } else if (op == BC250_CPU_OP_SET) {
            ULONG window = Data->TrialMs ? Data->TrialMs : s->TrialMs;
            if (window < BC250_CPU_TRIAL_MIN_MS) window = BC250_CPU_TRIAL_MIN_MS;
            if (window > BC250_CPU_TRIAL_MAX_MS) window = BC250_CPU_TRIAL_MAX_MS;
            error = bc250_cpu_settings_check(&request, s->Lab ? 1 : 0);
            if (error != BC250_CPU_OK) status = STATUS_INVALID_PARAMETER;
            else {
                // A second SET inside a window keeps the first window's revert target: a chain of trials never
                // leaves a candidate behind as the thing the deadline falls back to.
                KeAcquireSpinLock(&s->SnapLock, &irql);
                if (!s->OnTrial) s->TrialBefore = s->Applied;
                KeReleaseSpinLock(&s->SnapLock, irql);
                status = CpuApply(Device, &request, "on trial", &error, FALSE);
                if (!NT_SUCCESS(status)) CpuRevert(Device, "the trial did not go through");
                else {
                    KeAcquireSpinLock(&s->SnapLock, &irql);
                    s->OnTrial = TRUE;
                    s->TrialDeadline = KeQueryInterruptTime() + 10000ull * window;
                    s->TrialSerial++;
                    KeReleaseSpinLock(&s->SnapLock, irql);
                    KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
                    GuardLog("cpu: the trial runs for %lu ms; the settings before it come back by themselves",
                             window);
                }
            }
        } else if (op == BC250_CPU_OP_KEEP) {
            KeAcquireSpinLock(&s->SnapLock, &irql);
            onTrial = s->OnTrial;
            KeReleaseSpinLock(&s->SnapLock, irql);
            if (!onTrial) status = STATUS_INVALID_DEVICE_STATE;
            else {
                ULONG mark;
                s->Stored = s->Applied;
                mark = CpuMark(&s->Stored);
                if (s->Stored.max_given) CpuStoreLogged(CPU_SETTING_MAX, s->Stored.max_mhz);
                else CpuDeleteLogged(CPU_SETTING_MAX);
                if (s->Stored.uv_given) CpuStoreLogged(CPU_SETTING_UV, s->Stored.uv_steps);
                else CpuDeleteLogged(CPU_SETTING_UV);
                if (s->Stored.temp_given) CpuStoreLogged(CPU_SETTING_TEMP, s->Stored.temp_c);
                else CpuDeleteLogged(CPU_SETTING_TEMP);
                CpuDeleteLogged(CPU_SETTING_PENDING);
                s->Pending = FALSE;
                CpuStoreLogged(CPU_SETTING_CONFIRMED, mark);
                CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_OK);
                s->Confirmed = TRUE;
                KeAcquireSpinLock(&s->SnapLock, &irql);
                s->OnTrial = FALSE;
                s->TrialDeadline = 0;
                s->TrialSerial++;
                KeReleaseSpinLock(&s->SnapLock, irql);
                CpuLogSettings("kept", &s->Stored);
                GuardLog("cpu: the settings are kept, mark 0x%08X; the next start applies them again and marks "
                         "them pending until it is healthy", mark);
            }
        } else if (op == BC250_CPU_OP_CANCEL) {
            KeAcquireSpinLock(&s->SnapLock, &irql);
            onTrial = s->OnTrial;
            KeReleaseSpinLock(&s->SnapLock, irql);
            if (!onTrial) status = STATUS_INVALID_DEVICE_STATE;
            else CpuRevert(Device, "cancelled");
        } else if (op == BC250_CPU_OP_RESET) {
            // The recorded baseline back into the chip, and the stored values off the disk. Every control this
            // driver has sent is named by bc250_cpu_restore_target out of the baseline or the firmware's own
            // default, and a clock limit the baseline cannot name is reported and left: 0.7.210 substituted
            // BC250_CPU_MAX_MHZ here, which raised the ceiling to 3600 MHz and called it a restore.
            struct bc250_cpu_settings none;
            RtlZeroMemory(&none, sizeof(none));
            status = CpuRestoreTo(Device, &none, "reset");
            CpuDeleteLogged(CPU_SETTING_MAX);
            CpuDeleteLogged(CPU_SETTING_UV);
            CpuDeleteLogged(CPU_SETTING_TEMP);
            CpuDeleteLogged(CPU_SETTING_PENDING);
            CpuDeleteLogged(CPU_SETTING_CONFIRMED);
            CpuStoreLogged(CPU_SETTING_REASON, CPU_REASON_NONE);
            RtlZeroMemory(&s->Stored, sizeof(s->Stored));
            s->Pending = s->Confirmed = FALSE;
            KeAcquireSpinLock(&s->SnapLock, &irql);
            s->OnTrial = FALSE;
            s->TrialDeadline = 0;
            RtlZeroMemory(&s->Search, sizeof(s->Search));
            KeReleaseSpinLock(&s->SnapLock, irql);
            GuardLog("cpu: reset to the recorded baseline: 0x%08X", status);
        } else if (op == BC250_CPU_OP_CORES) {
            ULONG mask = Data->CoreMask;
            if (!bc250_cpu_mask_allowed(mask)) status = STATUS_INVALID_PARAMETER;
            else {
                // The mask is a restart's business: it is written to the disk and sent now, and the processor
                // count moves only after the next Windows restart. Writing the stock mask back is the way out,
                // which is why this driver uses the AMD-named queue 0 message and not the community's SMN write.
                CpuStoreLogged(CPU_SETTING_CORE_MASK, mask);
                s->CoreMaskStored = mask;
                if (mask != BC250_CPU_MASK_STOCK) {
                    if (NT_SUCCESS(GuardStoreSetting(CPU_SETTING_CORE_PENDING, mask))) s->CorePending = TRUE;
                } else {
                    CpuDeleteLogged(CPU_SETTING_CORE_PENDING);
                    CpuDeleteLogged(CPU_SETTING_CORE_CONFIRMED);
                    s->CorePending = s->CoreConfirmed = FALSE;
                }
                if (SmuCpuBegin(&Device->Smu)) {
                    status = CpuMessage(Device, BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_SET_CORE_ENABLE_MASK, mask,
                                        TRUE, FALSE, NULL);
                    SmuCpuEnd(&Device->Smu);
                } else status = STATUS_DEVICE_BUSY;
                if (NT_SUCCESS(status)) s->CoreMask = mask;
                GuardLog("cpu: core mask 0x%02X (%lu cores) requested: 0x%08X; the count changes at the next "
                         "restart", mask, bc250_cpu_mask_cores(mask), status);
            }
        } else if (op == BC250_CPU_OP_SEARCH_BEGIN) {
            ULONG steps = Data->UvSteps ? Data->UvSteps : BC250_CPU_SEARCH_MAX_STEPS;
            ULONG window = Data->TrialMs ? Data->TrialMs : s->TrialMs;
            if (steps > BC250_CPU_SEARCH_MAX_STEPS) status = STATUS_INVALID_PARAMETER;
            else {
                if (window < BC250_CPU_TRIAL_MIN_MS) window = BC250_CPU_TRIAL_MIN_MS;
                if (window > BC250_CPU_TRIAL_MAX_MS) window = BC250_CPU_TRIAL_MAX_MS;
                KeAcquireSpinLock(&s->SnapLock, &irql);
                // The baseline clock is what a stretched core is judged against, so with no limit applied it is
                // the firmware's own ceiling from the read stage and not 0 (0.7.210 passed 0 and the sign died).
                bc250_cpu_search_begin(&s->Search, steps, BC250_CPU_SEARCH_LOAD_MS, s->Snap.VoltageMv,
                                       s->Applied.max_given ? s->Applied.max_mhz
                                       : (s->BaselineValid ? s->BaselineRead.table_mhz : 0u));
                if (!s->OnTrial) s->TrialBefore = s->Applied;
                KeReleaseSpinLock(&s->SnapLock, irql);
                (void)bc250_cpu_search_next(&s->Search, NULL);       // asks for step 1
                request = s->Applied;
                request.uv_given = 1;
                request.uv_steps = s->Search.step;
                status = CpuApply(Device, &request, "search step", &error, FALSE);
                if (!NT_SUCCESS(status)) CpuRevert(Device, "the search step did not go through");
                else {
                    KeAcquireSpinLock(&s->SnapLock, &irql);
                    s->OnTrial = TRUE;
                    s->TrialDeadline = KeQueryInterruptTime() + 10000ull * window;
                    s->TrialSerial++;
                    KeReleaseSpinLock(&s->SnapLock, irql);
                    KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
                    GuardLog("cpu: the undervolt search begins at step %lu of at most %lu, %lu ms of load a step",
                             s->Search.step, steps, (ULONG)BC250_CPU_SEARCH_LOAD_MS);
                }
            }
        } else {
            // SEARCH_STEP: judge the step the caller has just loaded, then the next one or the baseline back.
            if (!s->Search.running) status = STATUS_INVALID_DEVICE_STATE;
            else {
                status = CpuReadStage(Device, FALSE, BC250_CPU_GETTER_GAP_MS);
                if (NT_SUCCESS(status)) {
                    enum bc250_cpu_search_action action;
                    CpuSample(Device, &s->Applied, loaded, whea, checksum, &sample);
                    action = bc250_cpu_search_next(&s->Search, &sample);
                    if (action == BC250_CPU_SEARCH_APPLY) {
                        request = s->Applied;
                        request.uv_given = 1;
                        request.uv_steps = s->Search.step;
                        status = CpuApply(Device, &request, "search step", &error, FALSE);
                        if (!NT_SUCCESS(status)) CpuRevert(Device, "the search step did not go through");
                    } else {
                        CpuRevert(Device, "search done");
                        GuardLog("cpu: the undervolt search stops: %lu steps tested, the deepest that passed is "
                                 "%lu, the one that failed did so with sign %lu", s->Search.tested, s->Search.best,
                                 s->Search.fail);
                    }
                }
            }
        }
        if (!NT_SUCCESS(status))
            GuardLog("cpu: op %lu refused 0x%08X (error %d)", op, status, (int)error);
        // A trial, a search or a reset changes what the joint arm may do: the governor reads it at its next tick,
        // and the worker looks at the arm again now.
        CpuJointPublish(Device);
        KeSetEvent(&s->Wake, IO_NO_INCREMENT, FALSE);
        CpuUnlock(s);
    }
    // The reply: what is applied, stored and recorded, what the chip answered, and where a trial or a search is.
    applied = s->Applied;
    stored = s->Stored;
    baseline = s->Baseline;
    boostKnown = (s->BaselineValid && s->BaselineRead.boost_given) ? TRUE : FALSE;
    trialMs = s->TrialMs;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    snap = s->Snap;
    onTrial = s->OnTrial;
    serial = s->TrialSerial;
    searching = s->Search.running ? TRUE : FALSE;
    revertOwed = s->RevertOwed;
    searchStep = s->Search.step;
    searchBest = s->Search.best;
    searchFail = s->Search.fail;
    searchTested = s->Search.tested;
    remaining = 0;
    if (onTrial) {
        ULONGLONG now = KeQueryInterruptTime();
        remaining = now >= s->TrialDeadline ? 0u : (ULONG)((s->TrialDeadline - now) / 10000ull);
    }
    KeReleaseSpinLock(&s->SnapLock, irql);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);
    Data->TrialMs = trialMs;
    Data->TrialRemainingMs = remaining;
    Data->Serial = serial;
    Data->Given = given;
    Data->MaxMHz = applied.max_given ? applied.max_mhz : 0;
    Data->UvSteps = applied.uv_given ? applied.uv_steps : 0;
    Data->TempC = applied.temp_given ? applied.temp_c : 0;
    Data->AppliedMaxMHz = Data->MaxMHz;
    Data->AppliedUvSteps = Data->UvSteps;
    Data->AppliedTempC = Data->TempC;
    Data->StoredMaxMHz = stored.max_given ? stored.max_mhz : 0;
    Data->StoredUvSteps = stored.uv_given ? stored.uv_steps : 0;
    Data->StoredTempC = stored.temp_given ? stored.temp_c : 0;
    Data->BaselineMaxMHz = baseline.max_given ? baseline.max_mhz : 0;
    Data->BaselineUvSteps = baseline.uv_given ? baseline.uv_steps : 0;
    Data->BaselineTempC = baseline.temp_given ? baseline.temp_c : 0;
    Data->VoltageMv = snap.VoltageMv;
    Data->GpuVoltageMv = snap.GpuVoltageMv;
    Data->CapC = snap.CapC;
    Data->Features = snap.Features;
    for (i = 0; i < BC250_CPU_CORE_SLOTS; i++) {
        Data->CoreMHz[i] = snap.CoreMHz[i];
        Data->PstateMHz[i] = snap.PstateMHz[i];
    }
    Data->Cores = s->Cores;
    Data->Threads = s->Threads;
    Data->CoreMask = s->CoreMask;
    Data->CoreMaskStored = s->CoreMaskStored;
    Data->LastQueue = snap.LastQueue;
    Data->LastMessage = snap.LastMessage;
    Data->LastStatus = snap.LastStatus;
    Data->LastParameter = snap.LastParameter;
    Data->TemperatureMc = snap.TemperatureMc;
    Data->SearchStep = searchStep;
    Data->SearchBest = searchBest;
    Data->SearchFail = searchFail;
    Data->SearchTested = searchTested;
    Data->Reads = snap.Reads;
    Data->Writes = snap.Writes;
    Data->Refusals = snap.Refusals;
    Data->Reverts = snap.Reverts;
    Data->RevertRetries = snap.RevertRetries;
    Data->RevertFailures = snap.RevertFailures;
    Data->Generation = s->Generation;
    Data->Error = (ULONG)error;
    Data->Flags = (Device->FullWddm ? BC250_CPU_FLAG_VALID : 0) |
                  (onTrial ? BC250_CPU_FLAG_ON_TRIAL : 0) |
                  ((stored.max_given || stored.uv_given || stored.temp_given) ? BC250_CPU_FLAG_STORED : 0) |
                  (s->Pending ? BC250_CPU_FLAG_PENDING : 0) |
                  (s->Confirmed ? BC250_CPU_FLAG_CONFIRMED : 0) |
                  (s->Proven ? BC250_CPU_FLAG_QUEUE3_PROVEN : 0) |
                  (s->Enabled ? BC250_CPU_FLAG_TUNE_ON : 0) |
                  (searching ? BC250_CPU_FLAG_SEARCHING : 0) |
                  (s->CorePending ? BC250_CPU_FLAG_CORE_PENDING : 0) |
                  (s->CoreConfirmed ? BC250_CPU_FLAG_CORE_CONFIRMED : 0) |
                  (status == STATUS_DEVICE_BUSY ? BC250_CPU_FLAG_BUSY : 0) |
                  (revertOwed ? BC250_CPU_FLAG_REVERT_OWED : 0) |
                  (snap.TemperatureValid ? BC250_CPU_FLAG_TEMP_VALID : 0) |
                  (boostKnown ? BC250_CPU_FLAG_BOOST_KNOWN : 0);
    Data->NtStatus = (ULONG)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
