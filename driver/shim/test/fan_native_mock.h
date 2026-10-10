/* Compiles the REAL driver/kmd/fan.c and driver/kmd/hwmon.c on the host: run_hwmon.ps1 replaces their
 * #include "bc250kmd.h" with this header. It is hwmon_native_mock.h (the ports, the spin locks, the registry, the
 * log) plus the primitives only the fan control uses: the watchdog's timer and DPC, the bugcheck callback, the
 * request mutex and the wait in the controller's hold. The timer and the callback only record what they were given,
 * so the test fires the watchdog and the bugcheck itself, at the moments it chooses.
 */
#pragma once
#define NATIVE_FAN_DEVICE 1
#define NATIVE_STALL_ANY 1
#include "hwmon_native_mock.h"

/* ---- the watchdog's timer and DPC ------------------------------------------------------------------------- */

typedef struct _KDPC *PKDPC;
typedef VOID KDEFERRED_ROUTINE(PKDPC Dpc, PVOID Context, PVOID Argument1, PVOID Argument2);
typedef KDEFERRED_ROUTINE *PKDEFERRED_ROUTINE;
typedef struct _KDPC {
    PKDEFERRED_ROUTINE Routine;
    PVOID Context;
} KDPC;

typedef enum _TIMER_TYPE { NotificationTimer, SynchronizationTimer } TIMER_TYPE;
typedef struct _KTIMER {
    int Armed;
    LONG Period;
    PKDPC Dpc;
} KTIMER;

static void KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE Routine, PVOID Context)
{
    Dpc->Routine = Routine;
    Dpc->Context = Context;
}

static void KeInitializeTimerEx(KTIMER *Timer, TIMER_TYPE Type)
{
    UNREFERENCED_PARAMETER(Type);
    memset(Timer, 0, sizeof(*Timer));
}

static BOOLEAN KeSetTimerEx(KTIMER *Timer, LARGE_INTEGER Due, LONG Period, PKDPC Dpc)
{
    BOOLEAN was = Timer->Armed != 0;

    CHECK(Due.QuadPart < 0 && Period > 0);
    Timer->Armed = 1;
    Timer->Period = Period;
    Timer->Dpc = Dpc;
    return was;
}

static BOOLEAN KeCancelTimer(KTIMER *Timer)
{
    BOOLEAN was = Timer->Armed != 0;

    Timer->Armed = 0;
    return was;
}

static unsigned int native_dpc_flushes;
static void KeFlushQueuedDpcs(void) { native_dpc_flushes++; }

/* ---- the bugcheck callback -------------------------------------------------------------------------------- */

typedef VOID KBUGCHECK_CALLBACK_ROUTINE(PVOID Buffer, ULONG Length);
typedef KBUGCHECK_CALLBACK_ROUTINE *PKBUGCHECK_CALLBACK_ROUTINE;
typedef struct _KBUGCHECK_CALLBACK_RECORD {
    PKBUGCHECK_CALLBACK_ROUTINE Routine;
    PVOID Buffer;
    ULONG Length;
    int Registered;
} KBUGCHECK_CALLBACK_RECORD, *PKBUGCHECK_CALLBACK_RECORD;

static void KeInitializeCallbackRecord(PKBUGCHECK_CALLBACK_RECORD Record) { memset(Record, 0, sizeof(*Record)); }

static BOOLEAN KeRegisterBugCheckCallback(PKBUGCHECK_CALLBACK_RECORD Record, PKBUGCHECK_CALLBACK_ROUTINE Routine,
                                          PVOID Buffer, ULONG Length, PUCHAR Component)
{
    CHECK(!Record->Registered && Component != NULL);
    Record->Routine = Routine;
    Record->Buffer = Buffer;
    Record->Length = Length;
    Record->Registered = 1;
    return TRUE;
}

static BOOLEAN KeDeregisterBugCheckCallback(PKBUGCHECK_CALLBACK_RECORD Record)
{
    BOOLEAN was = Record->Registered != 0;

    Record->Registered = 0;
    return was;
}

/* ---- the request mutex and the hold's wait ---------------------------------------------------------------- */

typedef struct _KMUTEX { int Held; } KMUTEX;
#define KernelMode 0
#define Executive 0

static void KeInitializeMutex(KMUTEX *Mutex, ULONG Level)
{
    UNREFERENCED_PARAMETER(Level);
    Mutex->Held = 0;
}

static NTSTATUS KeWaitForSingleObject(KMUTEX *Mutex, int Reason, int Mode, BOOLEAN Alertable, PLARGE_INTEGER Timeout)
{
    UNREFERENCED_PARAMETER(Reason);
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(Alertable);
    CHECK(Timeout == NULL && !Mutex->Held);     /* one escape at a time in this test: never recursive */
    Mutex->Held = 1;
    return STATUS_SUCCESS;
}

static LONG KeReleaseMutex(KMUTEX *Mutex, BOOLEAN Wait)
{
    UNREFERENCED_PARAMETER(Wait);
    CHECK(Mutex->Held);
    Mutex->Held = 0;
    return 0;
}

/* The controller's hold waits here while another holder runs. This test is single-threaded, so a wait is a hold
 * nobody will ever release: counted, and after a bound the test fails instead of spinning for ever. */
static unsigned int native_delays;
static volatile LONG *native_busy;
static NTSTATUS KeDelayExecutionThread(int Mode, BOOLEAN Alertable, PLARGE_INTEGER Interval)
{
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(Alertable);
    UNREFERENCED_PARAMETER(Interval);
    native_delays++;
    /* A hold nobody releases: fail, and release it so that the test goes on to its other sections. */
    CHECK(native_delays <= 1000u);
    if (native_delays > 1000u && native_busy != NULL) *native_busy = 0;
    return STATUS_SUCCESS;
}

/* ---- the registry writes ---------------------------------------------------------------------------------- */

static NTSTATUS GuardStoreSetting(const WCHAR *Name, ULONG Value)
{
    NativeSetSetting(Name, Value);
    return STATUS_SUCCESS;
}

static NTSTATUS GuardDeleteSetting(const WCHAR *Name)
{
    NativeDeleteSetting(Name);
    return STATUS_SUCCESS;
}

/* ---- the device ------------------------------------------------------------------------------------------ */

#include "fan.h"

typedef struct _BC250_DEVICE {
    BC250_START_HEALTH_NATIVE StartHealth;
    BC250_HWMON_OWNER Hwmon;
    BC250_FAN_OWNER Fan;
} BC250_DEVICE;

void HwmonInitialize(BC250_HWMON_OWNER *Owner);
void HwmonStart(BC250_DEVICE *Device);
void HwmonStop(BC250_HWMON_OWNER *Owner);
void HwmonSample(BC250_DEVICE *Device);
void HwmonLogLine(BC250_DEVICE *Device, _In_z_ const char *What);
void HwmonRequest(BC250_DEVICE *Device, BC250_ESCAPE_HWMON *Data, ULONG EscapeFlags);
void FanInitialize(BC250_DEVICE *Device);
void FanStart(BC250_DEVICE *Device);
void FanStop(BC250_DEVICE *Device, ULONG Reason);
void FanPause(BC250_DEVICE *Device);
void FanResume(BC250_DEVICE *Device);
void FanResetDevice(BC250_DEVICE *Device);
void FanDriverUnload(void);
void FanStep(BC250_DEVICE *Device, LONG TctlMc, BOOLEAN TctlValid, const BC250_FAN_LOAD *Load);
void FanLogLine(BC250_DEVICE *Device, _In_z_ const char *What);
void FanRequest(BC250_DEVICE *Device, BC250_ESCAPE_FAN *Data, BOOLEAN Admin, ULONG EscapeFlags);
