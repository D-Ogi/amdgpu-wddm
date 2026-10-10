/* Compiles the REAL driver/kmd/cpu.c on the host: run_cpu.ps1 replaces its #include "bc250kmd.h" with this
 * header. Only the Windows kernel primitives and the one mailbox owner below it are replaced, so the binding
 * under test is the one that ships and not a copy of it. The same pattern as smu_native_mock.h, hwmon_native_mock.h
 * and fan_native_mock.h, for the same reason.
 *
 * What this exists for (audit finding F3, 2026-10-10): the boost probe's rule lives in a loop inside
 * CpuBoostProbe, and a pure-shim check of bc250_cpu_boost_probe_more cannot reach it. With
 * BC250_CPU_BOOST_PROBE_ROUNDS = 2 the only call that loop makes is bc250_cpu_boost_probe_more(0, ...), which the
 * bound alone answers, so flipping that helper changes no outcome. The sweep's real behaviour - every window of
 * every round is read, and the highest reply per core is kept - is only observable by driving the stage against a
 * firmware that answers DIFFERENT clocks in the two windows. That is what native_firmware below is.
 *
 * THE FIRMWARE MODEL answers message 0x43 (BC250_CPU_MSG_READ_CORE_MHZ) from a schedule of windows. The probe
 * separates its windows with CpuBusyWait, which calls KeStallExecutionProcessor, so the mock's clock advances on
 * the stall and the model steps to the next window exactly when the driver's busy window ends. Nothing in the
 * model depends on real time.
 */
#pragma once
#include <windows.h>
#include <stdio.h>
#include <string.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

static volatile LONG native_checks, native_failures;
#define CHECK(x) do { InterlockedIncrement(&native_checks); if (!(x)) { InterlockedIncrement(&native_failures); \
    printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#define NT_ASSERT(x) CHECK(x)
#ifndef MAXULONG
#define MAXULONG 0xffffffffUL
#endif
#define PASSIVE_LEVEL 0
#define ALL_PROCESSOR_GROUPS 0xffff
#define OBJ_KERNEL_HANDLE 0x00000200L
#define IO_NO_INCREMENT 0

typedef UCHAR KIRQL;
typedef ULONG_PTR KSPIN_LOCK;
typedef KSPIN_LOCK *PKSPIN_LOCK;
typedef LONG EX_RUNDOWN_REF;
typedef ULONG_PTR KAFFINITY;

/* D3DDDI_ESCAPEFLAGS, field for field as d3dukmdt.h declares it (the escape path of cpu.c reads
 * NoAdapterSynchronization). The same declaration as hwmon_native_mock.h. */
typedef union _D3DDDI_ESCAPEFLAGS {
    struct {
        UINT HardwareAccess : 1;
        UINT DeviceStatusQuery : 1;
        UINT ChangeFrameLatency : 1;
        UINT NoAdapterSynchronization : 1;
        UINT Reserved : 1;
        UINT VirtualMachineData : 1;
        UINT DriverKnownEscape : 1;
        UINT DriverCommonEscape : 1;
        UINT Reserved2 : 24;
    };
    UINT Value;
} D3DDDI_ESCAPEFLAGS;

/* ---- the clock -------------------------------------------------------------------------------------------- */

/* KeQueryInterruptTime units, 100 ns. Every wait and every stall of cpu.c moves it, and nothing else does, so the
 * test's view of time is exactly the time the driver asked for. */
static ULONG64 native_time;
static ULONG64 native_stall_us;          /* microseconds spent in KeStallExecutionProcessor, any context */
static ULONG64 native_sleep_ms;          /* milliseconds spent in KeDelayExecutionThread */
static unsigned int native_stalls, native_sleeps;

static ULONG64 KeQueryInterruptTime(void) { return native_time; }

/* ---- the mailbox owner and the firmware model ------------------------------------------------------------- */

#include "bc250_cpu.h"
/* The joint power arm's own bound, which cpu.c C_ASSERTs against BC250_CPU_MIN_MHZ. */
#include "bc250_dpm.h"

/* One window of the model: what message 0x43 answers for each core while this window is open, and how long it
 * stays open. The model opens the next window once the driver's stalls have spent `us` microseconds in this one,
 * which is how the probe's BC250_CPU_BOOST_PROBE_MS busy window ends. */
#define NATIVE_WINDOWS 6
struct native_window {
    unsigned int core_mhz[BC250_CPU_CORES];
    ULONG64 us;                             /* 0 for the last window, which never closes */
};

static struct {
    struct native_window window[NATIVE_WINDOWS];
    unsigned int windows;                   /* how many of them are filled */
    unsigned int at;                        /* the open one */
    ULONG64 spent_us;                       /* stall microseconds inside the open window */
    unsigned int pstate_mhz[BC250_CPU_PSTATES];
    unsigned int cpu_mv, gpu_mv, cap_c, features;
    LONG temperature_mc;
    BOOLEAN temperature_valid;
    unsigned int core_requests;             /* message 0x43 attempts, answered or refused */
    unsigned int core_reads;                /* message 0x43 answers given, all cores, all windows */
    unsigned int core_reads_per_window[NATIVE_WINDOWS];
    unsigned int sequences;                 /* SmuCpuBegin that were taken */
    int sequence_open;
    int writes;                             /* any setter: the probe must send none */
    int refuse_core_reads;                  /* 1: message 0x43 is refused, for the no-answer arm */
} native_firmware;

static void NativeFirmwareStall(ULONG64 usec)
{
    struct native_window *open;

    if (native_firmware.windows == 0) return;
    open = &native_firmware.window[native_firmware.at];
    native_firmware.spent_us += usec;
    /* The last window never closes, so a probe that reads more rounds than the test described still answers. */
    while (open->us != 0 && native_firmware.spent_us >= open->us &&
           native_firmware.at + 1u < native_firmware.windows) {
        native_firmware.spent_us -= open->us;
        native_firmware.at++;
        open = &native_firmware.window[native_firmware.at];
    }
}

static unsigned int NativeFirmwareCoreMHz(unsigned int core)
{
    CHECK(core < BC250_CPU_CORES);
    if (core >= BC250_CPU_CORES) return 0;
    native_firmware.core_reads++;
    if (native_firmware.at < NATIVE_WINDOWS) native_firmware.core_reads_per_window[native_firmware.at]++;
    return native_firmware.window[native_firmware.at].core_mhz[core];
}

/* ---- kernel primitives ------------------------------------------------------------------------------------ */

static void KeInitializeSpinLock(PKSPIN_LOCK lock) { *lock = 0; }

static void KeAcquireSpinLock(PKSPIN_LOCK lock, KIRQL *irql)
{
    CHECK(*lock == 0);                      /* a leaf lock, held once: no recursion and no lock order to change */
    *lock = 1;
    *irql = 2;                              /* DISPATCH_LEVEL */
}

static void KeReleaseSpinLock(PKSPIN_LOCK lock, KIRQL irql)
{
    CHECK(*lock == 1 && irql == 2);
    *lock = 0;
}

typedef struct _KMUTEX { int Held; } KMUTEX;
typedef struct _KEVENT { int Signalled; int Notification; } KEVENT;
typedef void *PKTHREAD;
typedef void *POBJECT_TYPE;
#define KernelMode 0
#define Executive 0
#define WaitAny 1
typedef enum _EVENT_TYPE { NotificationEvent = 0, SynchronizationEvent = 1 } EVENT_TYPE;

static void KeInitializeMutex(KMUTEX *mutex, ULONG level)
{
    UNREFERENCED_PARAMETER(level);
    mutex->Held = 0;
}

static LONG KeReleaseMutex(KMUTEX *mutex, BOOLEAN wait)
{
    UNREFERENCED_PARAMETER(wait);
    CHECK(mutex->Held);
    mutex->Held = 0;
    return 0;
}

static void KeInitializeEvent(KEVENT *event, EVENT_TYPE type, BOOLEAN state)
{
    event->Signalled = state ? 1 : 0;
    event->Notification = type == NotificationEvent ? 1 : 0;
}

static LONG KeSetEvent(KEVENT *event, LONG increment, BOOLEAN wait)
{
    LONG was = event->Signalled;

    UNREFERENCED_PARAMETER(increment);
    UNREFERENCED_PARAMETER(wait);
    event->Signalled = 1;
    return was;
}

static void KeClearEvent(KEVENT *event) { event->Signalled = 0; }

/* The one wait this test drives is CpuLock's wait on the request mutex, which is never contended here: the test
 * is single-threaded and the worker thread is never created. A timed wait on an event is the worker's, and it
 * answers STATUS_TIMEOUT so a caller that does run sees the timeout it asked for. */
static NTSTATUS KeWaitForSingleObject(PVOID object, int reason, int mode, BOOLEAN alertable,
                                      PLARGE_INTEGER timeout)
{
    KMUTEX *mutex = (KMUTEX *)object;

    UNREFERENCED_PARAMETER(reason);
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(alertable);
    if (timeout != NULL) {
        CHECK(timeout->QuadPart < 0);
        native_time += (ULONG64)(-timeout->QuadPart);
        return STATUS_TIMEOUT;
    }
    CHECK(!mutex->Held);                    /* never recursive in this test */
    mutex->Held = 1;
    return STATUS_SUCCESS;
}

static NTSTATUS KeWaitForMultipleObjects(ULONG count, PVOID objects[], int type, int reason, int mode,
                                         BOOLEAN alertable, PLARGE_INTEGER timeout, PVOID blocks)
{
    UNREFERENCED_PARAMETER(count);
    UNREFERENCED_PARAMETER(objects);
    UNREFERENCED_PARAMETER(type);
    UNREFERENCED_PARAMETER(reason);
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(alertable);
    UNREFERENCED_PARAMETER(blocks);
    if (timeout != NULL) native_time += (ULONG64)(-timeout->QuadPart);
    return STATUS_WAIT_0;
}

/* Every wait of cpu.c but the probe's is a sleep: it moves the clock and nothing else. */
static NTSTATUS KeDelayExecutionThread(int mode, BOOLEAN alertable, PLARGE_INTEGER interval)
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(alertable);
    CHECK(interval != NULL && interval->QuadPart < 0);
    native_sleeps++;
    native_sleep_ms += (ULONG64)(-interval->QuadPart) / 10000ull;
    native_time += (ULONG64)(-interval->QuadPart);
    return STATUS_SUCCESS;
}

/* The probe's busy wait. The kernel contract bounds one call at 50 us
 * (ref/windows-driver-docs/windows-driver-docs-pr/kernel/kestallexecutionprocessor-and-timers.md), and the
 * driver's CPU_BUSY_SLICE_US is exactly that, so the bound is asserted here instead of trusted. The stall is what
 * steps the firmware model's windows. */
static void KeStallExecutionProcessor(unsigned usec)
{
    CHECK(usec <= 50u);
    native_stalls++;
    native_stall_us += usec;
    native_time += (ULONG64)usec * 10ull;
    NativeFirmwareStall(usec);
}

/* The probe pins itself to the processor it runs on. The test asserts that it reverts, and that it never asks for
 * an affinity of more than one processor. */
static ULONG native_processor;
static KAFFINITY native_affinity;
static int native_affinity_depth;
static unsigned int native_affinity_sets, native_affinity_reverts;

static ULONG KeGetCurrentProcessorNumber(void) { return native_processor; }

static KAFFINITY KeSetSystemAffinityThreadEx(KAFFINITY affinity)
{
    KAFFINITY was = native_affinity;

    CHECK(affinity != 0 && (affinity & (affinity - 1)) == 0);   /* one processor, never a set of them */
    CHECK(affinity == ((KAFFINITY)1 << native_processor));
    native_affinity = affinity;
    native_affinity_depth++;
    native_affinity_sets++;
    return was;
}

static void KeRevertToUserAffinityThreadEx(KAFFINITY affinity)
{
    native_affinity = affinity;
    native_affinity_depth--;
    native_affinity_reverts++;
}

static ULONG KeQueryActiveProcessorCountEx(USHORT group)
{
    UNREFERENCED_PARAMETER(group);
    return BC250_CPU_CORES;
}

/* The adapter's rundown protection, as hwmon_native_mock.h models it. */
static int native_rundown = 1;
static int native_rundown_held;

static BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF *ref)
{
    UNREFERENCED_PARAMETER(ref);
    if (!native_rundown) return FALSE;
    native_rundown_held++;
    return TRUE;
}

static void ExReleaseRundownProtection(EX_RUNDOWN_REF *ref)
{
    UNREFERENCED_PARAMETER(ref);
    native_rundown_held--;
}

/* The worker thread. The probe runs inside CpuReadStage, which the test calls on its own thread, so nothing here
 * ever runs; they exist because cpu.c must compile whole or the test is of a copy. */
typedef struct _OBJECT_ATTRIBUTES { ULONG Length; ULONG Attributes; } OBJECT_ATTRIBUTES;
#define InitializeObjectAttributes(p, name, attributes, root, security) do { \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES); (p)->Attributes = (attributes); \
    UNREFERENCED_PARAMETER(name); UNREFERENCED_PARAMETER(root); UNREFERENCED_PARAMETER(security); } while (0)
typedef void KSTART_ROUTINE(PVOID StartContext);
typedef KSTART_ROUTINE *PKSTART_ROUTINE;
static POBJECT_TYPE native_thread_type;
static POBJECT_TYPE *PsThreadType = &native_thread_type;
static int native_threads_created;

static NTSTATUS PsCreateSystemThread(PHANDLE handle, ULONG access, OBJECT_ATTRIBUTES *attributes, HANDLE process,
                                     PVOID id, PKSTART_ROUTINE routine, PVOID context)
{
    UNREFERENCED_PARAMETER(access);
    UNREFERENCED_PARAMETER(attributes);
    UNREFERENCED_PARAMETER(process);
    UNREFERENCED_PARAMETER(id);
    UNREFERENCED_PARAMETER(routine);
    UNREFERENCED_PARAMETER(context);
    native_threads_created++;
    *handle = (HANDLE)(ULONG_PTR)1;
    return STATUS_SUCCESS;
}

static void PsTerminateSystemThread(NTSTATUS status) { UNREFERENCED_PARAMETER(status); }

static NTSTATUS ObReferenceObjectByHandle(HANDLE handle, ULONG access, POBJECT_TYPE type, int mode,
                                          PVOID *object, PVOID information)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(access);
    UNREFERENCED_PARAMETER(type);
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(information);
    *object = (PVOID)(ULONG_PTR)1;
    return STATUS_SUCCESS;
}

static void ObDereferenceObject(PVOID object) { UNREFERENCED_PARAMETER(object); }
static NTSTATUS ZwClose(HANDLE handle) { UNREFERENCED_PARAMETER(handle); return STATUS_SUCCESS; }

static NTSTATUS ZwWaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout)
{
    UNREFERENCED_PARAMETER(handle);
    UNREFERENCED_PARAMETER(alertable);
    UNREFERENCED_PARAMETER(timeout);
    return STATUS_SUCCESS;
}

/* ---- the driver log and the registry ---------------------------------------------------------------------- */

#define NATIVE_LOG_LINES 128
static char native_log[NATIVE_LOG_LINES][256];
static int native_lines;

static void GuardLog(_In_z_ const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    if (native_lines < NATIVE_LOG_LINES) {
        vsnprintf(native_log[native_lines], sizeof(native_log[0]), format, arguments);
        /* The driver's ring truncates at BC250_LOG_TEXT (160 bytes), so a line that does not fit there would lose
         * its tail on the lab. Caught here instead of on the machine (BD-070). */
        CHECK(strlen(native_log[native_lines]) < 160);
        native_lines++;
    }
    va_end(arguments);
}

static int native_log_has(const char *needle)
{
    int i;

    for (i = 0; i < native_lines; i++)
        if (strstr(native_log[i], needle) != NULL) return 1;
    return 0;
}

#define NATIVE_SETTINGS 32
static struct { const WCHAR *Name; ULONG Value; } native_settings[NATIVE_SETTINGS];

static void NativeSetSetting(const WCHAR *name, ULONG value)
{
    int i;

    for (i = 0; i < NATIVE_SETTINGS; i++)
        if (native_settings[i].Name == NULL || wcscmp(native_settings[i].Name, name) == 0) {
            native_settings[i].Name = name;
            native_settings[i].Value = value;
            return;
        }
    CHECK(0);
}

static void NativeClearSettings(void) { memset(native_settings, 0, sizeof(native_settings)); }

static NTSTATUS GuardQuerySetting(const WCHAR *name, ULONG *value)
{
    int i;

    for (i = 0; i < NATIVE_SETTINGS; i++)
        if (native_settings[i].Name != NULL && wcscmp(native_settings[i].Name, name) == 0) {
            *value = native_settings[i].Value;
            return STATUS_SUCCESS;
        }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

static NTSTATUS GuardStoreSetting(const WCHAR *name, ULONG value)
{
    NativeSetSetting(name, value);
    return STATUS_SUCCESS;
}

static NTSTATUS GuardDeleteSetting(const WCHAR *name)
{
    int i;

    for (i = 0; i < NATIVE_SETTINGS; i++)
        if (native_settings[i].Name != NULL && wcscmp(native_settings[i].Name, name) == 0)
            native_settings[i].Name = L"";
    return STATUS_SUCCESS;
}

static ULONG GuardReadSetting(const WCHAR *name, ULONG fallback)
{
    ULONG value = 0;

    return NT_SUCCESS(GuardQuerySetting(name, &value)) ? value : fallback;
}

/* ---- the device ------------------------------------------------------------------------------------------- */

#include "bc250kmd_escape.h"
#include "cpu.h"

/* Only the fields cpu.c reaches for. The real structures carry much more, and none of it belongs in a CPU test. */
typedef struct _BC250_START_HEALTH_NATIVE {
    EX_RUNDOWN_REF Readers;
    ULONGLONG Generation;
} BC250_START_HEALTH_NATIVE;

typedef struct _BC250_SMU_OWNER_NATIVE {
    BOOLEAN Online;
    BOOLEAN CpuOnline;
} BC250_SMU_OWNER_NATIVE;

typedef struct _BC250_DPM_OWNER_NATIVE {
    KSPIN_LOCK SnapLock;
    struct { ULONG BusyPermille; } Snap;
    volatile LONG Paused;
} BC250_DPM_OWNER_NATIVE;

typedef struct _BC250_DEVICE {
    BC250_START_HEALTH_NATIVE StartHealth;
    BC250_SMU_OWNER_NATIVE Smu;
    BC250_DPM_OWNER_NATIVE Dpm;
    BC250_CPU_STATE Cpu;
    BOOLEAN FullWddm;
} BC250_DEVICE;

/* The mailbox owner, as cpu.c sees it. The sequence flag is the real one's contract: SmuCpuBegin takes it or
 * answers FALSE, and SmuCpuEnd releases it, so a stage that forgot its bracket is caught here. */
static BOOLEAN SmuCpuBegin(BC250_SMU_OWNER_NATIVE *owner)
{
    UNREFERENCED_PARAMETER(owner);
    if (native_firmware.sequence_open) return FALSE;
    native_firmware.sequence_open = 1;
    native_firmware.sequences++;
    return TRUE;
}

static void SmuCpuEnd(BC250_SMU_OWNER_NATIVE *owner)
{
    UNREFERENCED_PARAMETER(owner);
    CHECK(native_firmware.sequence_open);
    native_firmware.sequence_open = 0;
}

/* One message, answered from the model. Everything the probe sends is the getter 0x43 on queue 3; the read stage
 * around it also asks for the two voltages, the temperature cap, the P-state table and the feature word. A setter
 * is counted and refused: the read stage must send none. */
static NTSTATUS SmuCpuMessage(BC250_SMU_OWNER_NATIVE *owner, ULONG queue, ULONG message, ULONG parameter,
                              BOOLEAN write, BOOLEAN allow_hot, ULONG busy_permille, _Out_opt_ ULONG *value,
                              _Out_opt_ LONG *temperature_mc, _Out_opt_ ULONG *firmware_status,
                              _Out_opt_ BOOLEAN *temperature_valid)
{
    ULONG answer = 0;

    UNREFERENCED_PARAMETER(owner);
    UNREFERENCED_PARAMETER(busy_permille);
    CHECK(native_firmware.sequence_open);   /* every message of cpu.c runs inside a Begin/End bracket */
    if (value != NULL) *value = 0;
    if (firmware_status != NULL) *firmware_status = 0;
    if (temperature_mc != NULL) *temperature_mc = native_firmware.temperature_mc;
    if (temperature_valid != NULL) *temperature_valid = native_firmware.temperature_valid;
    if (write) {
        native_firmware.writes++;
        return STATUS_NOT_SUPPORTED;
    }
    CHECK(allow_hot);                       /* every getter of cpu.c passes the hot gate */
    if (queue == BC250_CPU_QUEUE_CPU && message == BC250_CPU_MSG_READ_CORE_MHZ) {
        native_firmware.core_requests++;
        if (native_firmware.refuse_core_reads) return STATUS_IO_DEVICE_ERROR;
        answer = NativeFirmwareCoreMHz(parameter);
    } else if (queue == BC250_CPU_QUEUE_CPU && message == BC250_CPU_MSG_READ_PSTATE_MHZ) {
        CHECK(parameter < BC250_CPU_PSTATES);
        answer = parameter < BC250_CPU_PSTATES ? native_firmware.pstate_mhz[parameter] : 0u;
    } else if (queue == BC250_CPU_QUEUE_CPU && message == BC250_CPU_MSG_READ_CPU_MV) {
        answer = native_firmware.cpu_mv;
    } else if (queue == BC250_CPU_QUEUE_CPU && message == BC250_CPU_MSG_READ_GPU_MV) {
        answer = native_firmware.gpu_mv;
    } else if (queue == BC250_CPU_QUEUE_CPU && message == BC250_CPU_MSG_READ_CAP_C) {
        answer = native_firmware.cap_c;
    } else if (queue == BC250_CPU_QUEUE_GFX && message == BC250_CPU_MSG_GET_ENABLED_FEATURES) {
        answer = native_firmware.features;
    } else {
        return STATUS_NOT_SUPPORTED;
    }
    if (value != NULL) *value = answer;
    return STATUS_SUCCESS;
}

static NTSTATUS SmuCpuJointMessage(BC250_SMU_OWNER_NATIVE *owner, ULONG queue, ULONG message, ULONG parameter,
                                   BOOLEAN write, BOOLEAN allow_hot, ULONG busy_permille, _Out_opt_ ULONG *value,
                                   _Out_opt_ LONG *temperature_mc, _Out_opt_ ULONG *firmware_status,
                                   _Out_opt_ BOOLEAN *temperature_valid)
{
    return SmuCpuMessage(owner, queue, message, parameter, write, allow_hot, busy_permille, value, temperature_mc,
                         firmware_status, temperature_valid);
}

/* What cpu.c publishes to the miniport. */
void CpuInitialize(BC250_DEVICE *Device);
void CpuStart(BC250_DEVICE *Device);
void CpuStop(BC250_DEVICE *Device);
void CpuPause(BC250_DEVICE *Device);
void CpuResume(BC250_DEVICE *Device);
NTSTATUS CpuConfirm(BC250_DEVICE *Device, _In_z_ const char *Why);
void CpuLogSummary(BC250_DEVICE *Device);
void CpuRequest(BC250_DEVICE *Device, BC250_ESCAPE_CPU *Data, ULONG Size, BOOLEAN Admin, ULONG EscapeFlags);
