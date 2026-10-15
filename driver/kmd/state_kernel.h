#pragma once
#include <ntddk.h>
#include "../state/state.h"
#include "../state/transfer.h"
NTSTATUS DriverStateInit(PDRIVER_OBJECT driver);
void DriverStateClose(void);
BOOLEAN DriverStateReady(void);
// Returned handles belong to the caller. No handle for P is ever writable.
NTSTATUS DriverStateOpenData(PHANDLE key);
NTSTATUS DriverStateOpenVolatile(PCWSTR child, BOOLEAN create, PHANDLE key);
NTSTATUS DriverStateQuery(PCWSTR child, PCWSTR name, ULONG mode, AMDGPU_STATE_VALUE *value,
    PVOID data, ULONG capacity);
NTSTATUS DriverStateNumber(PCWSTR child,PCWSTR name,BOOLEAN effective,ULONG *value);
NTSTATUS DriverStateSet(PCWSTR child, PCWSTR name, ULONG type, const void *data, ULONG bytes);
NTSTATUS DriverStateClear(PCWSTR child, PCWSTR name);
// Invalid optional inputs close their whole tuning operation before consumers
// read individual fields. Required inputs fail DriverStateInit instead.
BOOLEAN DriverStateGroupEnabled(ULONG group);
NTSTATUS DriverStateReceipt(AMDGPU_TRANSFER_HEADER *receipt);
NTSTATUS DriverStateValidateDevice(PDEVICE_OBJECT device);
// StartDevice only, before persistent diagnostics, admission and hardware I/O.
NTSTATUS DriverStateActivateDevice(PDEVICE_OBJECT device);
// SYSTEM worker only, after the caller resolves the actual PDO identity.
NTSTATUS DriverStateActivate(const unsigned char deviceHash[32]);
NTSTATUS DriverStateConsumeBoardSeed(BOOLEAN *first);
