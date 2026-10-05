// Device-owned mailbox. The caller must complete legacy-writer handover first.
#pragma once
#include "bc250kmd_escape.h"
#include "../shim/include/bc250_smu.h"
#include "../shim/include/bc250_clock.h"
typedef struct _BC250_SMU_OWNER {
    EX_PUSH_LOCK Lock;
    PETHREAD Caller;
    BOOLEAN Online;
    ULONG FirmwareVersion; // GetSmuVersion, established before publishing Online
    volatile ULONG* Registers;
    struct bc250_smu Transport;
} BC250_SMU_OWNER;
// AddDevice before publication; object storage lives with BC250_DEVICE.
void SmuOwnerInitialize(BC250_SMU_OWNER* Owner);
// PASSIVE_LEVEL, unpublished start after legacy writer removal. Requires a
// mapped BAR and proven valid THM access; PnP requires EnableNativeSmu.
NTSTATUS SmuOwnerStart(BC250_SMU_OWNER* Owner, volatile ULONG* Registers);
// Close/join under the same lock, BEFORE MMIO unmap. Waiters see offline.
void SmuOwnerStop(BC250_SMU_OWNER* Owner);
NTSTATUS SmuPrepareClock(BC250_SMU_OWNER* Owner, struct bc250_clock_report* Report);
// A paired read uses the same transaction owner as clock preparation.
NTSTATUS SmuReadClock(BC250_SMU_OWNER* Owner, ULONG* MHz, ULONG* Vid, LONG* TemperatureMc);

// Typed request processor; caller validates payload length before entry. All
// responses initialize output fields, including refusal/offline. READ does not
// require adapter-wide idling; object storage survives until RemoveDevice.
void SmuClockRequest(BC250_SMU_OWNER* Owner, BC250_ESCAPE_CLOCK* Data,
    BOOLEAN Administrator, BOOLEAN HardwareAccess, BOOLEAN NoAdapterSynchronization);

// Cached only, PASSIVE_LEVEL; no SMU message on this query path.
NTSTATUS SmuReadFirmwareVersion(BC250_SMU_OWNER* Owner, ULONG* Version);
