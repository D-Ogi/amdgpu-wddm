/* Compiles the REAL driver/kmd/fan.c and driver/kmd/hwmon.c on the host: run_hwmon.ps1 replaces their
 * #include "bc250kmd.h" with this header. It is hwmon_native_mock.h (the ports, the spin locks, the registry, the
 * log) plus the primitives only the fan control uses: the watchdog's timer and DPC, the bugcheck callback, the
 * request mutex and the wait in the controller's hold. The timer and the callback only record what they were given,
 * so the test fires the watchdog and the bugcheck itself, at the moments it chooses.
 */
#pragma once
#define NATIVE_FAN_DEVICE 1
#define NATIVE_STALL_ANY 1
/* The context a call runs in, and what it stalls there. A DPC runs at DISPATCH_LEVEL, where the kernel contract
 * says: "DPC routines that call the KeStallExecutionProcessor routine to delay execution must not specify delays
 * of more than 100 microseconds", and a DPC "should run for no more than 100 microseconds each time it is
 * called" (ref/windows-driver-docs/windows-driver-docs-pr/kernel/guidelines-for-writing-dpc-routines.md:31,35,
 * revision 110f60eaf2ac5836e644d320c1e92c1011f2af5e). The test sets native_irql around the calls it makes, and
 * the hook below records the context class, the largest single stall and the stall the driver asked for in all,
 * so the rule is asserted instead of inferred from a comment (audit finding F1). */
#define NATIVE_IRQL_PASSIVE 0
#define NATIVE_IRQL_DISPATCH 2
static unsigned char native_irql = NATIVE_IRQL_PASSIVE;
static unsigned int native_stall_max_us;            /* the largest single stall, any context */
static unsigned int native_stall_dispatch_us;       /* the sum of the stalls asked for at DISPATCH_LEVEL */
static unsigned int native_stall_dispatch_max_us;   /* and the largest single one of them */
static void NativeStall(unsigned int usec);
#define NATIVE_STALL_HOOK(usec) NativeStall(usec)
#include "hwmon_native_mock.h"

static void NativeStall(unsigned int usec)
{
    if (usec > native_stall_max_us) native_stall_max_us = usec;
    if (native_irql < NATIVE_IRQL_DISPATCH) return;
    native_stall_dispatch_us += usec;
    if (usec > native_stall_dispatch_max_us) native_stall_dispatch_max_us = usec;
}

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

/* ---- the work item the watchdog hands the normal handback to (audit finding F1) ---------------------------- */

typedef struct _DEVICE_OBJECT { int Present; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef VOID IO_WORKITEM_ROUTINE(PDEVICE_OBJECT DeviceObject, PVOID Context);
typedef IO_WORKITEM_ROUTINE *PIO_WORKITEM_ROUTINE;
typedef enum _WORK_QUEUE_TYPE { CriticalWorkQueue = 0, DelayedWorkQueue = 1 } WORK_QUEUE_TYPE;
typedef struct _IO_WORKITEM {
    PDEVICE_OBJECT Device;
    PIO_WORKITEM_ROUTINE Routine;
    PVOID Context;
    int Queued;
    int Freed;
} IO_WORKITEM, *PIO_WORKITEM;

/* One device object, one item: the fan control allocates at the start and frees at the stop. */
static IO_WORKITEM native_work_item;
static unsigned int native_work_allocations, native_work_queued, native_work_runs;
static int native_work_refused;             /* 1: IoAllocateWorkItem answers NULL, for the fallback section */

static PIO_WORKITEM IoAllocateWorkItem(PDEVICE_OBJECT Device)
{
    CHECK(Device != NULL && native_irql == NATIVE_IRQL_PASSIVE);
    if (native_work_refused) return NULL;
    memset(&native_work_item, 0, sizeof(native_work_item));
    native_work_item.Device = Device;
    native_work_allocations++;
    return &native_work_item;
}

static void IoFreeWorkItem(PIO_WORKITEM Item)
{
    CHECK(Item != NULL && !Item->Queued);   /* never freed while the system still owns it */
    Item->Freed = 1;
}

static void IoQueueWorkItem(PIO_WORKITEM Item, PIO_WORKITEM_ROUTINE Routine, WORK_QUEUE_TYPE Queue, PVOID Context)
{
    CHECK(Item != NULL && !Item->Freed && !Item->Queued && Routine != NULL && Queue == DelayedWorkQueue);
    Item->Routine = Routine;
    Item->Context = Context;
    Item->Queued = 1;
    native_work_queued++;
}

/* The system worker thread, where the test (or a wait inside the driver) chooses to let it run. Always at
 * PASSIVE_LEVEL, which is the whole point of the item. */
static int NativeRunWorkItems(void)
{
    PIO_WORKITEM_ROUTINE routine;
    unsigned char saved = native_irql;

    if (!native_work_item.Queued) return 0;
    routine = native_work_item.Routine;
    native_work_item.Queued = 0;
    native_work_runs++;
    native_irql = NATIVE_IRQL_PASSIVE;
    routine(native_work_item.Device, native_work_item.Context);
    native_irql = saved;
    return 1;
}

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
    /* The one thing a real wait at PASSIVE_LEVEL also allows: the system worker thread runs the queued item.
     * Without this, FanStop's hold would wait here for a handback that nothing dispatches. */
    if (NativeRunWorkItems()) return STATUS_SUCCESS;
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
    PDEVICE_OBJECT PhysicalDeviceObject;    /* what FanStart allocates the work item against */
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
void FanStep(BC250_DEVICE *Device, LONG TctlMc, BOOLEAN TctlValid);
void FanLogLine(BC250_DEVICE *Device, _In_z_ const char *What);
void FanRequest(BC250_DEVICE *Device, BC250_ESCAPE_FAN *Data, BOOLEAN Admin, ULONG EscapeFlags);
