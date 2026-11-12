// One registration on \Callback\PowerState (power_notify.c). Windows sends PO_CB_SYSTEM_STATE_LOCK through it before
// the system set-power IRP of a sleep, a shutdown or a restart (Argument2 0), and again when the system is back in S0
// (Argument2 1). At a shutdown and a restart this is the last point where the registry and the file system are known
// to be up: unit A showed no DxgkDdiSetPowerState line of the adapter in the kept log of a restart, and the
// notification's line was there (BD-369, evidence/windows/2026-11-12-early-restart-marks).
//
// Each module that owns state on disk keeps its own registration, with its own lifetime and its own context: the
// interop session for the life of the image (interop.c), the governor and the CPU surface for one start (dpm.c,
// cpu.c). The function decodes the two arguments itself and must return without work above PASSIVE_LEVEL.
#pragma once

typedef struct _BC250_POWER_NOTIFY {
    PCALLBACK_OBJECT Object;
    PVOID Registration;
} BC250_POWER_NOTIFY;

// PASSIVE_LEVEL. A second call on a registered record changes nothing and answers STATUS_SUCCESS. A failure leaves
// the record empty, and the caller says in its log line what it loses.
NTSTATUS PowerNotifyRegister(BC250_POWER_NOTIFY* Notify, PCALLBACK_FUNCTION Function, PVOID Context);
// PASSIVE_LEVEL, idempotent. Returns after a notification in progress has finished, so the caller must not hold a
// lock that its function takes.
void PowerNotifyUnregister(BC250_POWER_NOTIFY* Notify);
