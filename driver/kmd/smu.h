// Device-owned mailbox. The caller must complete legacy-writer handover first.
#pragma once
#include "bc250kmd_escape.h"
#include "../shim/include/bc250_smu.h"
#include "../shim/include/bc250_clock.h"
#include "../shim/include/bc250_cpu.h"
#include "../shim/include/bc250_smu_metrics.h"
typedef struct _BC250_SMU_OWNER {
    EX_PUSH_LOCK Lock;
    PETHREAD Caller;
    BOOLEAN Online;
    // The firmware's queue 3 (0.7.210, the CPU surface): a second transport instance over the same BAR mapping
    // and the same owner lock, with its own firmware_state, because "check and drain the previous response before
    // the first write" must run once per queue. CpuOnline says the instance exists; it never means queue 3 has
    // answered - driver/kmd/cpu.c owns that gate.
    BOOLEAN CpuOnline;
    // One CPU sequence at a time. A sequence is several messages BC250_CPU_MESSAGE_GAP_MS apart, and the owner
    // lock is taken for one message at a time so the governor's 25 ms tick is never held for the whole sequence;
    // this flag is what keeps two CPU callers from interleaving inside it.
    volatile LONG CpuBusy;
    // Atomic published metadata survives hardware stop; bit32 means valid.
    // A successful new start replaces it; failed starts cannot erase it.
    DECLSPEC_ALIGN(8) volatile LONG64 FirmwareSnapshot;
    volatile ULONG* Registers;
    struct bc250_smu Transport;
    struct bc250_smu CpuTransport;      // the firmware's queue 3; the registers are in include/bc250_cpu.h
    // Set by the DPM governor while it runs (dpm.c): the administrator's SET escape is refused then.
    volatile LONG GovernorActive;
    // The page the firmware was told to write the metrics table into (0.7.215, SmuReadMetrics), or 0 while this
    // owner start has told it nothing. Under the owner lock; SmuOwnerStart clears it, so the address goes out
    // again after every power transition.
    ULONGLONG MetricsTableMc;
} BC250_SMU_OWNER;
// AddDevice before publication; object storage lives with BC250_DEVICE.
void SmuOwnerInitialize(BC250_SMU_OWNER* Owner);
// PASSIVE_LEVEL, unpublished start after legacy writer removal. Requires a
// mapped BAR and proven valid THM access; PnP requires EnableNativeSmu.
NTSTATUS SmuOwnerStart(BC250_SMU_OWNER* Owner, volatile ULONG* Registers);
// Close/join under the same lock, BEFORE MMIO unmap. Waiters see offline.
void SmuOwnerStop(BC250_SMU_OWNER* Owner);
NTSTATUS SmuPrepareClock(BC250_SMU_OWNER* Owner, struct bc250_clock_report* Report);
// Any point of the bc250_clock.h table, through the same transaction (the DPM governor, dpm.c).
NTSTATUS SmuSetPoint(BC250_SMU_OWNER* Owner, ULONG MHz, ULONG Mv, struct bc250_clock_report* Report);
// THM_TCON_CUR_TMP only, under the owner lock, no mailbox message: cheap enough for every governor tick.
NTSTATUS SmuReadTemperature(BC250_SMU_OWNER* Owner, LONG* TemperatureMc);
// A paired read uses the same transaction owner as clock preparation.
NTSTATUS SmuReadClock(BC250_SMU_OWNER* Owner, ULONG* MHz, ULONG* Vid, LONG* TemperatureMc);

// Typed request processor; caller validates payload length before entry. All
// responses initialize output fields, including refusal/offline. READ does not
// require adapter-wide idling; object storage survives until RemoveDevice.
void SmuClockRequest(BC250_SMU_OWNER* Owner, BC250_ESCAPE_CLOCK* Data,
    BOOLEAN Administrator, BOOLEAN HardwareAccess, BOOLEAN NoAdapterSynchronization);

// ---- the CPU domain (0.7.210, driver/kmd/cpu.c, docs/design/tuner.md, ADR 0020) --------------------------------
// One CPU-domain message, on the firmware's queue 0 (BC250_CPU_QUEUE_GFX) or queue 3 (BC250_CPU_QUEUE_CPU).
// PASSIVE_LEVEL. The owner lock is taken for this one message and released, so a sequence of them never holds it;
// SmuCpuBegin/SmuCpuEnd bracket the sequence instead. Refused when:
//   - the message is not on bc250_cpu_message_allowed() for that queue and that direction, or its argument is not
//     on bc250_cpu_argument_allowed(): STATUS_INVALID_PARAMETER, and nothing is written;
//   - the part is at or above BC250_CLOCK_HOT_MC and this is a setter that AllowHot does not cover:
//     STATUS_DEVICE_POWER_FAILURE (the temperature is read before every message, not once per sequence);
//   - the governor reports the GPU at or above BC250_CPU_GPU_BUSY_PERMILLE busy (the caller passes it):
//     STATUS_DEVICE_BUSY.
// AllowHot (0.7.211) is the CPU rail's reading of the rule the GPU clock path already carries: the part must
// always be returnable to the settings it is known to run at. A getter changes nothing, a step that lowers the
// dissipation lowers it at any temperature, and the way back from a trial must run even on a hot part, because
// the alternative is an unvalidated operating point with nothing watching it. driver/kmd/cpu.c passes it for
// every getter, for every step bc250_cpu_plan marked cools, and for every restore. A restore also runs when the
// temperature cannot be read at all; TemperatureValid then says so, and nothing judges the part as cold.
// FirmwareStatus, when given, carries the firmware's own response word for the support report. A timeout is an
// abort and never a retry; the transport's deadline is the queue 0 one.
NTSTATUS SmuCpuMessage(BC250_SMU_OWNER* Owner, ULONG Queue, ULONG Message, ULONG Parameter, BOOLEAN Write,
                       BOOLEAN AllowHot, ULONG BusyPermille, _Out_opt_ ULONG* Value,
                       _Out_opt_ LONG* TemperatureMc, _Out_opt_ ULONG* FirmwareStatus,
                       _Out_opt_ BOOLEAN* TemperatureValid);
// TRUE when this caller took the CPU sequence flag; SmuCpuEnd releases it. FALSE means another sequence runs.
BOOLEAN SmuCpuBegin(BC250_SMU_OWNER* Owner);
void SmuCpuEnd(BC250_SMU_OWNER* Owner);

// ---- the SMU metrics table (0.7.215, driver/kmd/smu_metrics.c, docs/design/dpm.md "Power reading") ----------------
// PASSIVE_LEVEL. One read under the owner lock: the first read of an owner start names TableMc to the firmware
// (SetDriverTableDramAddrHigh, then SetDriverTableDramAddrLow), every read fills the page with
// BC250_SMU_METRICS_POISON, sends TransferTableSmu2Dram with the table id and copies Length bytes of the page into
// Copy. The three messages pass bc250_smu_metrics_message_allowed alone, with the arguments that list fixes for
// TableMc; the clock list admits none of them. Table is the caller's uncached mapping of the page at TableMc;
// Length is a multiple of 4 and at most one page. Returns the transport's result: 0, BC250_SMU_METRICS_OFFLINE
// when the owner is not online (nothing was sent), or the first failure: a firmware answer other than OK, -62 for
// a timeout, -22 for the allowlist. Every message is bounded by the transport's 20 ms poll.
int SmuReadMetrics(BC250_SMU_OWNER* Owner, ULONGLONG TableMc, volatile ULONG* Table, UCHAR* Copy, ULONG Length);

// Last successfully established firmware metadata, not an online/readiness test.
// Cached only, PASSIVE_LEVEL; no mailbox lock or SMU message on this query path.
NTSTATUS SmuReadFirmwareVersion(BC250_SMU_OWNER* Owner, ULONG* Version);
