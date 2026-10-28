// BD-122: the start record of guard.c (Parameters\StartRecord) and the start path that writes it, compiled on the
// host against a registry model, a log-ring model and a pool that can refuse.
//
// What it holds the driver to, requirement by requirement (the defect's text is in DEFECTS.md BD-122):
//   1. every start attempt that does not reach a started device leaves a record, with no gate: each step of
//      Bc250StartDevice by name, the sub-step of WddmStart where there is one, the NTSTATUS, the stage, a UTC
//      time, the UnconfirmedStarts count and the last ring lines, bounded to 64 lines of at most 159 characters;
//      the boot-loop guard's refusal says it was the guard;
//   2. the record always belongs to the newest attempt: "attempt" is written and flushed before any step can
//      fail, a started device writes "started" and drops the lines, and a failure of an earlier boot can never
//      be read as the current one;
//   4. the write cannot make a start fail or hang: every registry and pool failure leaves the step's own status,
//      one log line each, and the existing cleanup order; nothing is written above PASSIVE_LEVEL.
//
// The mutations of run_start_record.ps1 are the negative controls, one per requirement; each must fail here.
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

typedef unsigned long ULONG;
typedef long NTSTATUS;
typedef long LONG;
typedef unsigned char BOOLEAN;
typedef unsigned char UCHAR;
typedef unsigned short USHORT;
typedef short CSHORT;
typedef wchar_t WCHAR;
typedef wchar_t* PWSTR;
typedef const wchar_t* PCWSTR;
typedef void* HANDLE;
typedef void* PVOID;
typedef ULONG* PULONG;
typedef size_t SIZE_T;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;

#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0L
#define STATUS_ACCESS_DENIED (-1L)
#define STATUS_OBJECT_NAME_NOT_FOUND (-2L)
#define STATUS_REGISTRY_IO_FAILED (-4L)
#define STATUS_DEVICE_CONFIGURATION_ERROR (-5L)
#define STATUS_DEVICE_HARDWARE_ERROR (-6L)
#define STATUS_DEVICE_BUSY (-7L)
#define STATUS_INSUFFICIENT_RESOURCES (-8L)
#define STATUS_GRAPHICS_NO_VIDEO_MEMORY (-9L)
#define STATUS_UNSUCCESSFUL (-10L)
#define STATUS_DEVICE_NOT_READY (-11L)
#define STATUS_GRAPHICS_STALE_MODESET (-12L)
#define STATUS_NOT_SUPPORTED (-13L)
#define NT_SUCCESS(s) ((s)>=0)
#define PASSIVE_LEVEL 0
#define DISPATCH_LEVEL 2
#define REG_SZ 1u
#define REG_MULTI_SZ 7u
#define REG_DWORD 4u
#define REG_OPTION_NON_VOLATILE 0u
#define KEY_READ 1u
#define KEY_WRITE 2u
#define OBJ_KERNEL_HANDLE 0x200u
#define OBJ_CASE_INSENSITIVE 0x40u
#define POOL_FLAG_PAGED 1u
#define BC250_TAG 0x3035324Bu
#define BC250_KMD_VERSION 0x000700D8u
#define BC250_LOG_TEXT 160
#define BC250_MAX_UNCONFIRMED_STARTS 2u
#define StageStartEnter 30u
#define StageStartGuardPassed 31u
#define StageStartDeviceInfo 32u
#define StageStartPostDisplayAcquired 33u
#define StageStartFramebufferMapped 34u
#define StageStartMmioDone 35u
#define StageStartDone 39u
#define StageRefusedByGuard 90u
#define StageStartFailed 91u
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
// sal.h and stdlib.h are already in through <stdio.h>: take these names over rather than redefine them.
#undef _In_
#undef _In_z_
#undef _In_opt_z_
#undef _Out_
#undef _Inout_
#undef _Out_writes_
#undef _Out_writes_z_
#undef _Out_writes_to_
#undef _Outptr_
#undef min
#define _In_
#define _In_z_
#define _In_opt_z_
#define _Out_
#define _Inout_
#define _Out_writes_(n)
#define _Out_writes_z_(n)
#define _Out_writes_to_(a,b)
#define _Outptr_
#define min(a,b) ((a)<(b)?(a):(b))

typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING;
typedef struct { CSHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday; } TIME_FIELDS;
typedef struct { ULONG Length; HANDLE RootDirectory; UNICODE_STRING* ObjectName; ULONG Attributes; PVOID a, b; } OBJECT_ATTRIBUTES;
typedef struct { ULONG Sequence, Milliseconds; char Text[BC250_LOG_TEXT]; } BC250_LOG_LINE;

static void RtlInitUnicodeString(UNICODE_STRING* s, PCWSTR t)
{ s->Buffer=(PWSTR)t; s->Length=(USHORT)(wcslen(t)*sizeof(WCHAR)); s->MaximumLength=(USHORT)(s->Length+sizeof(WCHAR)); }
#define InitializeObjectAttributes(a,n,f,r,s) do{(a)->Length=(ULONG)sizeof(*(a));(a)->ObjectName=(n);(a)->Attributes=(f);(a)->RootDirectory=(HANDLE)(r);(void)(s);}while(0)
#define RtlZeroMemory(d,n) memset((d),0,(n))
#define RtlCopyMemory(d,s,n) memcpy((d),(s),(n))

static int g_Irql = PASSIVE_LEVEL;
static int KeGetCurrentIrql(void) { return g_Irql; }
static int stateReady; static int DriverStateReady(void){return stateReady;}

// ---- the GuardLog ring, recorded so the self-check line can be asserted ---------------------------------------
#define MAXLOGS 64
static char logs[MAXLOGS][240];
static unsigned logCount;
static void GuardLog(const char* format, ...)
{
    va_list a;
    va_start(a, format);
    if (logCount < MAXLOGS) vsprintf_s(logs[logCount], sizeof(logs[0]), format, a);
    va_end(a);
    logCount++;
}
static unsigned LoggedTimes(const char* needle)
{
    unsigned i, n = 0;
    for (i = 0; i < logCount && i < MAXLOGS; i++) if (strstr(logs[i], needle)) n++;
    return n;
}

// ---- the log ring the record copies from ----------------------------------------------------------------------
static ULONG ringTotal;
static int ringLongLines;       // every line 159 characters, the widest GuardLog can write
static long ringEmptyAt;        // one line the ring holds empty, which a REG_MULTI_SZ must not end on
static ULONG ringGrowOnStop;    // lines the teardown of a started GPU writes after the failure (WddmStop and the rest)
static void GuardLogStats(ULONG* Total, ULONG* Lost, ULONG* Above) { *Total = ringTotal; *Lost = 0; *Above = 0; }
static ULONG GuardLogSequence(void) { return ringTotal; }
static ULONG GuardLogRead(ULONG From, BC250_LOG_LINE* Lines, ULONG Max, ULONG* Next)
{
    ULONG n = 0;
    while (n < Max && From < ringTotal) {
        memset(&Lines[n], 0, sizeof(Lines[n]));
        Lines[n].Sequence = From;
        if ((long)From == ringEmptyAt) Lines[n].Text[0] = '\0';
        else if (ringLongLines) { memset(Lines[n].Text, 'x', 159); Lines[n].Text[159] = '\0'; }
        else sprintf_s(Lines[n].Text, BC250_LOG_TEXT, "ring line %lu", From);
        From++; n++;
    }
    *Next = From;
    return n;
}
static ULONG lastStage;
static ULONG GuardLastStage(void) { return lastStage; }

// ---- the registry model ---------------------------------------------------------------------------------------
// Handle 1 is Parameters, handle 2 is Parameters\StartRecord. Every write is counted so a failure can be injected
// at exactly the n-th one, and the order of the writes is kept as a trace: the rule of requirement 2 is an order.
#define MAXVAL 16
#define MAXDATA 21000
static struct { int key; WCHAR name[40]; ULONG type; ULONG bytes; unsigned char data[MAXDATA]; int present; } vals[MAXVAL];
static ULONG unconfirmed;
static NTSTATUS openStatus, createStatus, writeStatus, flushStatus;
static unsigned writeOps, failAt, opens, creates, closes, flushes;
static int durable;
static char trace[2048];
static int poolRefused;

static void Trace(const char* what) { strcat_s(trace, sizeof(trace), what); strcat_s(trace, sizeof(trace), ","); }
static void Narrow(PCWSTR wide, char* out, size_t room)
{ size_t i; for (i = 0; i + 1 < room && wide[i]; i++) out[i] = (char)wide[i]; out[i] = 0; }
static int Slot(int key, PCWSTR name)
{
    int i, free_ = -1;
    for (i = 0; i < MAXVAL; i++) {
        if (vals[i].present && vals[i].key == key && !wcscmp(vals[i].name, name)) return i;
        if (!vals[i].present && free_ < 0) free_ = i;
    }
    return free_;
}
static NTSTATUS Store(int key, PCWSTR name, ULONG type, const void* data, ULONG bytes)
{
    int i;
    writeOps++;
    if (failAt ? (writeOps == failAt) : !NT_SUCCESS(writeStatus)) return writeStatus;
    i = Slot(key, name);
    if (i < 0 || bytes > MAXDATA) return STATUS_INSUFFICIENT_RESOURCES;
    vals[i].present = 1; vals[i].key = key; wcscpy_s(vals[i].name, RTL_NUMBER_OF(vals[i].name), name);
    vals[i].type = type; vals[i].bytes = bytes; memcpy(vals[i].data, data, bytes);
    return STATUS_SUCCESS;
}
static int Find(int key, PCWSTR name)
{
    int i;
    for (i = 0; i < MAXVAL; i++) if (vals[i].present && vals[i].key == key && !wcscmp(vals[i].name, name)) return i;
    return -1;
}
static PCWSTR Str(PCWSTR name)
{
    int i = Find(2, name);
    return (i < 0 || vals[i].type != REG_SZ) ? NULL : (PCWSTR)vals[i].data;
}
static int Dword(PCWSTR name, ULONG* out)
{
    int i = Find(2, name);
    if (i < 0 || vals[i].type != REG_DWORD) return 0;
    memcpy(out, vals[i].data, sizeof(*out));
    return 1;
}

static NTSTATUS OpenParameters(HANDLE* Key) { opens++; *Key = (HANDLE)1; return openStatus; }
static NTSTATUS ReadDword(HANDLE Key, PCWSTR Name, ULONG* Value)
{
    (void)Key;
    *Value = 0;
    if (!wcscmp(Name, L"UnconfirmedStarts")) { *Value = unconfirmed; return STATUS_SUCCESS; }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}
static NTSTATUS WriteDword(HANDLE Key, PCWSTR Name, ULONG Value)
{
    char utf[64];
    NTSTATUS s = Store((int)(size_t)Key, Name, REG_DWORD, &Value, sizeof(Value));
    if (NT_SUCCESS(s)) { Narrow(Name, utf, sizeof(utf)); Trace(utf); }
    return s;
}
static NTSTATUS ZwSetValueKey(HANDLE Key, UNICODE_STRING* Name, ULONG Title, ULONG Type, PVOID Data, ULONG Bytes)
{
    char utf[96], value[24];
    NTSTATUS s;
    (void)Title;
    s = Store((int)(size_t)Key, Name->Buffer, Type, Data, Bytes);
    if (!NT_SUCCESS(s)) return s;
    Narrow(Name->Buffer, utf, sizeof(utf));
    if (Type == REG_SZ) { Narrow((PCWSTR)Data, value, sizeof(value)); strcat_s(utf, sizeof(utf), "="); strcat_s(utf, sizeof(utf), value); }
    Trace(utf);
    return s;
}
static NTSTATUS ZwDeleteValueKey(HANDLE Key, UNICODE_STRING* Name)
{
    char utf[64];
    int slot = Find((int)(size_t)Key, Name->Buffer);
    Narrow(Name->Buffer, utf, sizeof(utf));
    strcat_s(trace, sizeof(trace), "-"); Trace(utf);
    if (slot < 0) return STATUS_OBJECT_NAME_NOT_FOUND;
    vals[slot].present = 0;
    return STATUS_SUCCESS;
}
static NTSTATUS ZwCreateKey(HANDLE* Key, ULONG Access, PVOID Attributes, ULONG Index, PVOID Class, ULONG Options,
                            PVOID Disposition)
{
    (void)Access; (void)Attributes; (void)Index; (void)Class; (void)Options; (void)Disposition;
    creates++;
    *Key = (HANDLE)2;
    return createStatus;
}
static NTSTATUS ZwFlushKey(HANDLE Key)
{ (void)Key; flushes++; Trace("flush"); if (NT_SUCCESS(flushStatus)) durable = 1; return flushStatus; }
static void ZwClose(HANDLE Key) { (void)Key; closes++; }
static void KeQuerySystemTime(LARGE_INTEGER* t) { t->QuadPart = 1; }
static void RtlTimeToTimeFields(LARGE_INTEGER* t, TIME_FIELDS* f)
{ (void)t; f->Year=2026; f->Month=10; f->Day=11; f->Hour=12; f->Minute=34; f->Second=56; f->Milliseconds=789; f->Weekday=0; }
static NTSTATUS RtlStringCchPrintfW(PWSTR Buffer, SIZE_T Chars, PCWSTR Format, ...)
{
    va_list a; int r;
    va_start(a, Format);
    r = _vsnwprintf_s(Buffer, Chars, Chars - 1, Format, a);
    va_end(a);
    return r < 0 ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}
static NTSTATUS RtlStringCchVPrintfW(PWSTR Buffer, SIZE_T Chars, PCWSTR Format, va_list a)
{
    int r = _vsnwprintf_s(Buffer, Chars, _TRUNCATE, Format, a);
    return r < 0 ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;   // the kernel says STATUS_BUFFER_OVERFLOW, kept truncated
}
static PVOID ExAllocatePool2(ULONG Flags, SIZE_T Bytes, ULONG Tag)
{ (void)Flags; (void)Tag; return poolRefused ? NULL : malloc(Bytes); }
static void ExFreePoolWithTag(PVOID P, ULONG Tag) { (void)Tag; free(P); }
static ULONG keepLog, enableNativeSmu;
static ULONG GuardReadSetting(PCWSTR Name, ULONG Default)
{
    if (!wcscmp(Name, L"KeepLog")) return keepLog;
    if (!wcscmp(Name, L"EnableNativeSmu")) return enableNativeSmu;
    return Default;
}

#include "record_actual.inc"

// ---- the start path -------------------------------------------------------------------------------------------
typedef struct { ULONG Width, Height, Pitch, ColorFormat, TargetId; LARGE_INTEGER PhysicAddress; } FAKE_POST;
typedef struct FAKE_IF {
    ULONG Size;
    void* DeviceHandle;
    NTSTATUS (*DxgkCbGetDeviceInformation)(void*, ULONG*);
    NTSTATUS (*DxgkCbAcquirePostDisplayOwnership)(void*, FAKE_POST*);
} FAKE_IF;
typedef struct {
    void* PhysicalDeviceObject;
    BOOLEAN InheritedSignalValid, GpuStopUnconfirmed, StopDone, Started, FullWddm;
    ULONG StartInfo, DeviceInfo, Smu;
    FAKE_IF Dxgk;
    FAKE_POST Post;
    void* Mmio;
    LONG RetainedPowerPhase;
    unsigned long long DisplayReservationEnd,RuntimeFramebufferLength;
    LONG RuntimePostValid,RuntimeDisplayChanged,RuntimePlaneBlank;
    BOOLEAN SourceVisible, DcnWriteEnabled, PostDisplayStopAttempted;
    LONG StartVisibilityPending;
    NTSTATUS PostDisplayStopStatus;
} BC250_DEVICE;
#define PDXGK_START_INFO ULONG*
#define PDXGKRNL_INTERFACE FAKE_IF*

static int ownedPdo;
static void* ownerIdentity;
// The inherited output starts visible even when the software cache says FALSE.
// This model is separate from the visibility implementation: this fixture owns
// StartDevice's sequencing, result and unwind, while the DCN gate tests MMIO.
enum START_EVENT { EVENT_MMIO = 1, EVENT_BLANK, EVENT_LOGO, EVENT_GPU,
                   EVENT_CONNECTOR, EVENT_READY, EVENT_STOP };
static enum START_EVENT startEvents[32];
static unsigned startEventCount;
static unsigned blankCalls, logoCalls, gpuCalls, pendingAtStop, blankWithoutHold;
static unsigned originalCauseAtStop;
static unsigned admissionCalls, mmioStops;
static BOOLEAN mmioAvailable, hardwareVisible, connectorUnblanks;
static BOOLEAN blankChangesBeforeError, restoreAttempt, restoreWithMmio;
static BOOLEAN forceRestoreAttempt;
static BOOLEAN visibleAtLogo, visibleAtGpu, visibleAtReady, firstSoftwareVisible;
static NTSTATUS blankStatus[2], restoreStatus;
static NTSTATUS admissionStatus;
static void StartEvent(enum START_EVENT event)
{
    if (startEventCount < RTL_NUMBER_OF(startEvents)) startEvents[startEventCount++] = event;
}
static int EventBefore(enum START_EVENT first, enum START_EVENT last)
{
    unsigned i;
    for (i = 0; i < startEventCount; ++i) {
        if (startEvents[i] == last) return 0;
        if (startEvents[i] == first) {
            for (++i; i < startEventCount; ++i) if (startEvents[i] == last) return 1;
            return 0;
        }
    }
    return 0;
}
static int AdapterOwnerIs(void* id) { return id && id == ownerIdentity; }
// State activation failures are covered by the actual state-device fixture.
static NTSTATUS DriverStateActivateDevice(void* id) { return id ? STATUS_SUCCESS : STATUS_DEVICE_BUSY; }
static void GuardStage(ULONG stage)
{
    char t[16];
    lastStage = stage;
    if (stage == StageStartFailed || stage == StageRefusedByGuard || stage == StageStartDone)
    { sprintf_s(t, sizeof(t), "stage%lu", stage); Trace(t); }
}
static void GuardLogKeepEpisode(PCWSTR label) { (void)label; }
static BOOLEAN WddmFullTableSelected(void) { return TRUE; }
static NTSTATUS guardStatus;
static NTSTATUS GuardCheckAndCountStart(BOOLEAN durableNeeded) { (void)durableNeeded; return guardStatus; }
static void StartHealthBegin(BC250_DEVICE* d, BOOLEAN f) { (void)d; (void)f; }
static void StartHealthClose(BC250_DEVICE* d) { (void)d; Trace("hclose"); }
static unsigned readyCalls;
static void StartHealthReady(BC250_DEVICE* d, BOOLEAN r)
{ (void)d; (void)r; readyCalls++; visibleAtReady = hardwareVisible; StartEvent(EVENT_READY); }
static void CuModeBegin(BC250_DEVICE* d) { (void)d; }
static void BoardMemoryIdentityClear(BC250_DEVICE* d) { (void)d; if (lastStage) Trace("bmclear"); }
static void BoardMemoryIdentityCapture(BC250_DEVICE* d) { (void)d; }
static void BoardMemoryStart(BC250_DEVICE* d) { (void)d; }
static void BoardMemoryStop(BC250_DEVICE* d) { (void)d; Trace("bmstop"); }
static void BoardProviderBind(BC250_DEVICE* d) { (void)d; }
static NTSTATUS fbStatus, wddmStatus, timingStatus, smuStatus, connectorStartStatus;
static unsigned connectorPrepares,connectorStarts,connectorDirty;
static NTSTATUS ConnectorBufferPrepare(BC250_DEVICE* d) {
    connectorPrepares++;
    if(d->DisplayReservationEnd || d->RuntimeFramebufferLength || d->RuntimePostValid ||
       d->RuntimeDisplayChanged || d->RuntimePlaneBlank) connectorDirty++;
    return STATUS_SUCCESS;
}
static NTSTATUS ConnectorStart(BC250_DEVICE* d)
{
    (void)d; connectorStarts++; StartEvent(EVENT_CONNECTOR);
    if (connectorUnblanks) hardwareVisible = TRUE;
    return connectorStartStatus;
}
// BD-169: whether a connector refusal leaves the static inherited path with a
// display. The real predicate is connector.c's; the display-timing host test
// owns its decision, and this test owns what the start does with it.
static int connectorRefusalKeeps;
static BOOLEAN ConnectorRefusalKeepsDisplay(const BC250_DEVICE* d)
{ (void)d;return connectorRefusalKeeps ? TRUE : FALSE; }
// What the record says at this point of the start, for the case where the machine never reaches the end of it, and
// for the case where the write of the record was torn: nothing of the last start may still stand here.
static WCHAR resultInsideStart[16], stepInsideStart[64];
static int logInsideStart, watchInsideStart;
static void Snap(PCWSTR name, PWSTR out, SIZE_T room)
{
    int i = Find(2, name);
    wcscpy_s(out, room, (i >= 0 && vals[i].type == REG_SZ) ? (PCWSTR)vals[i].data : L"absent");
}
static NTSTATUS DisplayMapFramebuffer(BC250_DEVICE* d)
{
    (void)d;
    if (watchInsideStart) {
        Snap(L"Result", resultInsideStart, RTL_NUMBER_OF(resultInsideStart));
        Snap(L"Step", stepInsideStart, RTL_NUMBER_OF(stepInsideStart));
        logInsideStart = Find(2, L"Log") >= 0;
    }
    return fbStatus;
}
static void DisplayUnmapFramebuffer(BC250_DEVICE* d) { (void)d; }
static void DisplayLogFramebufferSample(BC250_DEVICE* d) { (void)d; }
static void DisplayDrawBootLogo(BC250_DEVICE* d)
{ (void)d; logoCalls++; visibleAtLogo = hardwareVisible; StartEvent(EVENT_LOGO); }
// GitHub #38: the real step names its registers through GuardStartDetail when it refuses (display_timing_refused.h).
// BD-169: the inherited pipe's own refusal is written by this step even when the step succeeds, because an
// equal-size pipe this driver cannot classify passes it and the start fails later, in ConnectorStart. The text is
// ModesetPrepareInherited's (whose composition display_timing_test.c checks against the real gate names).
static int inheritedGateDetail;
static NTSTATUS DisplayPrepareInheritedTiming(BC250_DEVICE* d)
{
    (void)d;
    if (!NT_SUCCESS(timingStatus)) GuardStartDetail(L"timing 0x%08X post %ux%u otg0 ctl %08X", timingStatus, 800u, 600u, 0u);
    else if (inheritedGateDetail)
        GuardStartDetail(L"inherited source 2720x1481 pitch 10880 target 2720x1481 total 3000x1500 pixel 483326400"
                         L" support 00000000 pipe C00000BB valid 007FFFFF gate 14 tiling word 00000080");
    return timingStatus;
}
static void NoteInterruptResource(BC250_DEVICE* d) { (void)d; }
static void MmioStart(BC250_DEVICE* d)
{ d->Mmio = mmioAvailable ? d : NULL; StartEvent(EVENT_MMIO); }
static void MmioStop(BC250_DEVICE* d) { d->Mmio = NULL; mmioStops++; }
static NTSTATUS DcnSetVisibility(BC250_DEVICE* d, BOOLEAN visible)
{
    NTSTATUS status = blankStatus[blankCalls ? 1 : 0];
    if (!blankCalls) firstSoftwareVisible = d->SourceVisible;
    blankCalls++;
    if (!d->StartVisibilityPending) blankWithoutHold++;
    StartEvent(EVENT_BLANK);
    if (d->Mmio == NULL || !d->DcnWriteEnabled) return STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status) || blankChangesBeforeError) hardwareVisible = visible;
    return status;
}
static NTSTATUS DcnBeginStartVisibility(BC250_DEVICE* d, BOOLEAN* touched)
{
    NTSTATUS status;
    admissionCalls++; *touched = FALSE;
    firstSoftwareVisible = d->SourceVisible;
    if (d->Mmio == NULL || !d->DcnWriteEnabled) return STATUS_DEVICE_NOT_READY;
    if (!NT_SUCCESS(admissionStatus)) return admissionStatus;
    status = DcnSetVisibility(d, FALSE);
    *touched = NT_SUCCESS(status) || blankChangesBeforeError;
    return status;
}
static void HwmonStart(BC250_DEVICE* d) { (void)d; }
static NTSTATUS SmuOwnerStart(ULONG* s, void* mmio) { (void)s; (void)mmio; return smuStatus; }
static void VramStart(BC250_DEVICE* d) { (void)d; }
static void GartStart(BC250_DEVICE* d) { (void)d; }
static void PspStart(BC250_DEVICE* d) { (void)d; }
static void GpuMemStart(BC250_DEVICE* d) { (void)d; }
static void GfxStart(BC250_DEVICE* d) { (void)d; }
static void IhStart(BC250_DEVICE* d) { (void)d; }
// WddmStart and GpuStartupInitialize name sub-steps of their own; here one stands in for the phase that failed.
static PCWSTR wddmSubStep;
static NTSTATUS WddmStart(BC250_DEVICE* d)
{
    (void)d; gpuCalls++; visibleAtGpu = hardwareVisible; StartEvent(EVENT_GPU);
    if (wddmSubStep) GuardStartSubStep(wddmSubStep);
    return wddmStatus;
}
static void ModesetStart(BC250_DEVICE* d) { (void)d; }
static void HdmiLinkStartAudio(BC250_DEVICE* d, BOOLEAN resume) { (void)d; (void)resume; }
static BOOLEAN GfxStartupResources(BC250_DEVICE* d, BOOLEAN live) { (void)d; (void)live; return TRUE; }
static void HangDetectorStart(BC250_DEVICE* d) { (void)d; }
static void FanStart(BC250_DEVICE* d) { (void)d; }
static void SmuMetricsStart(BC250_DEVICE* d) { (void)d; }
static void OcTierStart(BC250_DEVICE* d) { (void)d; }
static void DpmStart(BC250_DEVICE* d) { (void)d; }
static void CpuStart(BC250_DEVICE* d) { (void)d; }
static void InterlockedExchange(LONG* t, LONG v) { *t = v; }
static unsigned stops;
#include "stop_visibility_actual.inc"
// The real teardown logs through its own subsystems: the ring grows while it runs, which is why the record marks
// the failure point before the cleanup rather than taking the newest lines when it writes.
static NTSTATUS Bc250StopDevice(PVOID context)
{
    BC250_DEVICE* d = (BC250_DEVICE*)context;
    (void)StartTestStopPrelude(context);
    if (LoggedTimes("start: failing step status ")) originalCauseAtStop++;
    if (d->StartVisibilityPending) pendingAtStop++;
    d->PostDisplayStopAttempted = restoreAttempt && (blankCalls != 0 || forceRestoreAttempt);
    d->PostDisplayStopStatus = restoreStatus;
    restoreWithMmio = d->Mmio != NULL;
    if (d->PostDisplayStopAttempted && NT_SUCCESS(restoreStatus)) hardwareVisible = TRUE;
    stops++; ringTotal += ringGrowOnStop; Trace("stop"); StartEvent(EVENT_STOP);
    return STATUS_SUCCESS;
}

#include "pnp_actual.inc"

static unsigned checks, failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)

static NTSTATUS GetDeviceInfo(void* h, ULONG* info) { (void)h; (void)info; return STATUS_SUCCESS; }
static NTSTATUS GetDeviceInfoFails(void* h, ULONG* info) { (void)h; (void)info; return STATUS_ACCESS_DENIED; }
static NTSTATUS AcquirePost(void* h, FAKE_POST* p)
{
    (void)h;
    p->Width = 1920; p->Height = 1080; p->Pitch = 7680; p->ColorFormat = 1; p->TargetId = 1;
    p->PhysicAddress.QuadPart = 0x10000000;
    return STATUS_SUCCESS;
}
static NTSTATUS AcquirePostFails(void* h, FAKE_POST* p) { (void)h; memset(p, 0, sizeof(*p)); return STATUS_DEVICE_NOT_READY; }
static NTSTATUS AcquirePostEmpty(void* h, FAKE_POST* p) { (void)h; memset(p, 0, sizeof(*p)); return STATUS_SUCCESS; }

static BC250_DEVICE dev;
static FAKE_IF iface;
static ULONG startInfo;
static ULONG outputSources, outputChildren;

static void Reset(void)
{
    memset(vals, 0, sizeof(vals));
    memset(&dev, 0, sizeof(dev));
    dev.DisplayReservationEnd=1;dev.RuntimeFramebufferLength=1;
    dev.RuntimePostValid=dev.RuntimeDisplayChanged=dev.RuntimePlaneBlank=1;
    dev.DcnWriteEnabled = TRUE;
    memset(&iface, 0, sizeof(iface));
    trace[0] = 0; logCount = 0; lastStage = 0; durable = 0;
    opens = creates = closes = flushes = writeOps = stops = readyCalls = 0;
    failAt = 0; openStatus = createStatus = writeStatus = flushStatus = STATUS_SUCCESS;
    fbStatus = wddmStatus = timingStatus = smuStatus = guardStatus = connectorStartStatus = STATUS_SUCCESS;
    startEventCount = blankCalls = logoCalls = gpuCalls = pendingAtStop = blankWithoutHold = 0;
    originalCauseAtStop = 0;
    admissionCalls = mmioStops = 0;
    blankStatus[0] = blankStatus[1] = restoreStatus = STATUS_SUCCESS;
    admissionStatus = STATUS_SUCCESS;
    mmioAvailable = hardwareVisible = restoreAttempt = TRUE;
    connectorUnblanks = blankChangesBeforeError = restoreWithMmio = FALSE;
    forceRestoreAttempt = FALSE;
    visibleAtLogo = visibleAtGpu = visibleAtReady = firstSoftwareVisible = FALSE;
    connectorRefusalKeeps = 0; inheritedGateDetail = 0;
    unconfirmed = 1; keepLog = 0; enableNativeSmu = 0; poolRefused = 0;
    ringTotal = 12; ringLongLines = 0; ringEmptyAt = -1; ringGrowOnStop = 0;
    wddmSubStep = NULL; g_Irql = PASSIVE_LEVEL;
    stateReady = 1;
    ownerIdentity = &ownedPdo;
    dev.PhysicalDeviceObject = &ownedPdo;
    iface.Size = (ULONG)sizeof(iface);
    iface.DeviceHandle = &iface;
    iface.DxgkCbGetDeviceInformation = GetDeviceInfo;
    iface.DxgkCbAcquirePostDisplayOwnership = AcquirePost;
    GuardStartStep(NULL);
}
static NTSTATUS Start(void)
{
    outputSources = outputChildren = 99;
    return Bc250StartDevice(&dev, &startInfo, &iface, &outputSources, &outputChildren);
}
// The lines of the Log value, as a reader walks a REG_MULTI_SZ.
static ULONG LogLines(SIZE_T* widest)
{
    int i = Find(2, L"Log");
    PCWSTR p;
    ULONG n = 0;
    if (widest) *widest = 0;
    if (i < 0 || vals[i].type != REG_MULTI_SZ) return 0;
    p = (PCWSTR)vals[i].data;
    while (*p) { SIZE_T len = wcslen(p); if (widest && len > *widest) *widest = len; n++; p += len + 1; }
    return n;
}
// The n-th line of the Log value, counted from the oldest, or NULL when there are fewer than that.
static PCWSTR LogLineAt(ULONG n)
{
    int i = Find(2, L"Log");
    PCWSTR p;
    if (i < 0 || vals[i].type != REG_MULTI_SZ) return NULL;
    p = (PCWSTR)vals[i].data;
    while (*p && n) { p += wcslen(p) + 1; n--; }
    return *p ? p : NULL;
}
static int RecordIs(PCWSTR result, PCWSTR step, PCWSTR sub, ULONG status, ULONG stage)
{
    ULONG v;
    PCWSTR s;
    if ((s = Str(L"Result")) == NULL || wcscmp(s, result)) return 0;
    if ((s = Str(L"Step")) == NULL || wcscmp(s, step)) return 0;
    if ((s = Str(L"SubStep")) == NULL || wcscmp(s, sub)) return 0;
    if (!Dword(L"Status", &v) || v != status) return 0;
    if (!Dword(L"Stage", &v) || v != stage) return 0;
    if (!Dword(L"Version", &v) || v != BC250_KMD_VERSION) return 0;
    if ((s = Str(L"TimeUtc")) == NULL || wcscmp(s, L"2026-10-11T12:34:56.789Z")) return 0;
    return 1;
}

int main(void)
{
    // StartDevice must return with each source invisible. The inherited output
    // is visible here and SourceVisible initially says FALSE: trusting that
    // cache would let startup drawing reach the screen before the OS permits it.
    {
        unsigned full;
        for (full = 0; full < 2; ++full) {
            Reset(); dev.FullWddm = (BOOLEAN)full;
            CHECK(hardwareVisible && !dev.SourceVisible);
            CHECK(Start() == STATUS_SUCCESS);
            CHECK(admissionCalls == 1 && blankCalls == 2 && blankWithoutHold == 0);
            CHECK(!hardwareVisible && !dev.SourceVisible && dev.StartVisibilityPending == 1);
            CHECK(!firstSoftwareVisible && !visibleAtLogo && !visibleAtGpu && !visibleAtReady);
            CHECK(EventBefore(EVENT_MMIO, EVENT_BLANK) && EventBefore(EVENT_BLANK, EVENT_LOGO));
            CHECK(EventBefore(EVENT_BLANK, EVENT_GPU) && EventBefore(EVENT_CONNECTOR, EVENT_READY));
            CHECK(logoCalls == 1 && gpuCalls == 1 && readyCalls == 1 && stops == 0);
            CHECK(outputSources == 1 && outputChildren == 1);
        }
    }
    // Reusing a device context must clear the previous OS visibility request.
    Reset(); dev.SourceVisible = TRUE;
    CHECK(Start() == STATUS_SUCCESS);
    CHECK(!firstSoftwareVisible && !dev.SourceVisible && dev.StartVisibilityPending == 1);

    // A startup callback can change the hardware after the initial blank. Both
    // connector success and the static-inherited fallback need final readback
    // before health readiness and success are published.
    {
        unsigned fallback;
        for (fallback = 0; fallback < 2; ++fallback) {
            Reset(); connectorUnblanks = TRUE;
            if (fallback) { connectorStartStatus = STATUS_DEVICE_NOT_READY; connectorRefusalKeeps = 1; }
            CHECK(Start() == STATUS_SUCCESS);
            CHECK(blankCalls == 2 && !hardwareVisible && !visibleAtReady);
            CHECK(startEventCount >= 3 && startEvents[startEventCount - 3] == EVENT_CONNECTOR &&
                  startEvents[startEventCount - 2] == EVENT_BLANK &&
                  startEvents[startEventCount - 1] == EVENT_READY);
            CHECK(dev.StartVisibilityPending == 1 && outputSources == 1 && outputChildren == 1);
        }
    }
    // A missing MMIO mapping, a closed write gate, or a failed initial blank
    // cannot be treated as a successful start. No boot-logo or GPU work runs.
    {
        unsigned mode;
        for (mode = 0; mode < 3; ++mode) {
            NTSTATUS expected = mode == 2 ? STATUS_ACCESS_DENIED : STATUS_DEVICE_NOT_READY;
            Reset();
            if (mode == 0) mmioAvailable = FALSE;
            if (mode == 1) dev.DcnWriteEnabled = FALSE;
            if (mode == 2) { blankStatus[0] = expected; blankChangesBeforeError = TRUE; }
            CHECK(Start() == expected);
            CHECK(RecordIs(L"failed", L"StartDisplayBlank", L"-", (ULONG)expected, StageStartFailed));
            CHECK(admissionCalls == 1 && blankCalls == (mode == 2 ? 1u : 0u) &&
                  logoCalls == 0 && gpuCalls == 0 && readyCalls == 0);
            CHECK(mmioStops == (mode == 2 ? 0u : 1u));
            CHECK(stops == 1 && pendingAtStop == 0 && dev.StartVisibilityPending == 0);
            CHECK(outputSources == 0 && outputChildren == 0);
        }
    }
    // Read-only admission refuses inactive or unsupported pipes and unreadable
    // state. No hardware blank was attempted. Detach MMIO before common cleanup
    // so stop cannot write into an output that this start never owned.
    {
        NTSTATUS refused[] = { STATUS_DEVICE_NOT_READY, STATUS_NOT_SUPPORTED, STATUS_ACCESS_DENIED };
        unsigned i;
        for (i = 0; i < RTL_NUMBER_OF(refused); ++i) {
            Reset(); admissionStatus = refused[i]; restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
            forceRestoreAttempt = TRUE;
            CHECK(Start() == refused[i]);
            CHECK(RecordIs(L"failed", L"StartDisplayBlank", L"-", (ULONG)refused[i], StageStartFailed));
            CHECK(admissionCalls == 1 && blankCalls == 0 && logoCalls == 0 && gpuCalls == 0 && readyCalls == 0);
            CHECK(mmioStops == 1 && !restoreWithMmio && dev.Mmio == NULL);
            CHECK(dev.PostDisplayStopAttempted && dev.PostDisplayStopStatus == STATUS_DEVICE_HARDWARE_ERROR);
            CHECK(dev.StartVisibilityPending == 0 && pendingAtStop == 0 && outputSources == 0 && outputChildren == 0);
        }
    }
    // An admitted pipe can refuse its first write without touching hardware.
    // That is also an ordinary start refusal, even if cleanup reports failure.
    Reset(); blankStatus[0] = STATUS_ACCESS_DENIED; restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_ACCESS_DENIED);
    CHECK(blankCalls == 1 && mmioStops == 1 && !restoreWithMmio && dev.Mmio == NULL);
    // The last blank can fail even though the first one succeeded. It must not
    // publish source/child counts or health readiness, and must restore with
    // MMIO still mapped after releasing the startup-only visibility hold.
    Reset(); connectorUnblanks = TRUE; blankStatus[1] = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(RecordIs(L"failed", L"StartDisplayBlankConfirm", L"-",
                   (ULONG)STATUS_DEVICE_HARDWARE_ERROR, StageStartFailed));
    CHECK(blankCalls == 2 && readyCalls == 0 && stops == 1 && restoreWithMmio);
    CHECK(pendingAtStop == 0 && dev.StartVisibilityPending == 0);
    CHECK(outputSources == 0 && outputChildren == 0);

    // Failure after the initial blank uses the common cleanup while BAR5 still
    // exists. In particular, an SMU refusal must not unmap it before restoring
    // the output. Check the other exits that used to stop the device early too.
    {
        unsigned step;
        for (step = 0; step < 4; ++step) {
            Reset();
            if (step == 0) { enableNativeSmu = 1; smuStatus = STATUS_ACCESS_DENIED; }
            if (step == 1) wddmStatus = STATUS_ACCESS_DENIED;
            if (step == 2) timingStatus = STATUS_ACCESS_DENIED;
            if (step == 3) connectorStartStatus = STATUS_ACCESS_DENIED;
            CHECK(Start() == STATUS_ACCESS_DENIED);
            CHECK(blankCalls == 1 && stops >= 1 && restoreWithMmio && hardwareVisible);
            CHECK(originalCauseAtStop == 1);
            CHECK(pendingAtStop == 0 && dev.StartVisibilityPending == 0 && readyCalls == 0);
            CHECK(outputSources == 0 && outputChildren == 0);
        }
    }
    // If rollback cannot restore a changed display, the caller needs the stale
    // modeset status. The start record retains the original failing step and
    // reports the status that the caller receives; the kept log has the cause.
    Reset(); connectorStartStatus = STATUS_ACCESS_DENIED; restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_GRAPHICS_STALE_MODESET);
    CHECK(RecordIs(L"failed", L"ConnectorStart", L"-", (ULONG)STATUS_GRAPHICS_STALE_MODESET, StageStartFailed));
    CHECK(originalCauseAtStop == 1 &&
          LoggedTimes("start: failing step status 0xFFFFFFFF before display handoff") == 1);
    CHECK(dev.PostDisplayStopAttempted && dev.PostDisplayStopStatus == STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(pendingAtStop == 0 && dev.StartVisibilityPending == 0 && outputSources == 0 && outputChildren == 0);
    // A partial initial blank has the same rollback requirement.
    Reset(); blankStatus[0] = STATUS_ACCESS_DENIED; blankChangesBeforeError = TRUE;
    restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_GRAPHICS_STALE_MODESET);
    CHECK(RecordIs(L"failed", L"StartDisplayBlank", L"-", (ULONG)STATUS_GRAPHICS_STALE_MODESET, StageStartFailed));
    CHECK(!hardwareVisible && dev.StartVisibilityPending == 0 && pendingAtStop == 0);
    CHECK(mmioStops == 0 && restoreWithMmio);
    // Without an attempted restore, an unrelated stale saved status is not
    // enough to change the caller's failure classification.
    Reset(); connectorStartStatus = STATUS_ACCESS_DENIED; restoreAttempt = FALSE;
    restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_ACCESS_DENIED && !dev.PostDisplayStopAttempted);
    Reset(); fbStatus = STATUS_ACCESS_DENIED; restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_ACCESS_DENIED && blankCalls == 0 && !dev.PostDisplayStopAttempted);
    // A blank refusal without MMIO ownership does not make a stale stop-status
    // field a display transition owned by this start.
    Reset(); mmioAvailable = FALSE; restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_DEVICE_NOT_READY);
    CHECK(dev.StartVisibilityPending == 0 && outputSources == 0 && outputChildren == 0);
    Reset(); dev.DcnWriteEnabled = FALSE; restoreStatus = STATUS_DEVICE_HARDWARE_ERROR;
    CHECK(Start() == STATUS_DEVICE_NOT_READY);
    CHECK(dev.StartVisibilityPending == 0 && outputSources == 0 && outputChildren == 0);

    // ---- requirement 1: every failing step names itself, with no gate ----------------------------------------
    // The step table of Bc250StartDevice: what makes it fail, and the step the record must name.
    {
        struct { const char* what; PCWSTR step; ULONG status; } table[] = {
            {"device information", L"DxgkCbGetDeviceInformation", (ULONG)STATUS_ACCESS_DENIED},
            {"POST display", L"AcquirePostDisplayOwnership", (ULONG)STATUS_DEVICE_NOT_READY},
            {"POST display geometry", L"PostDisplayGeometry", (ULONG)STATUS_GRAPHICS_NO_VIDEO_MEMORY},
            {"framebuffer map", L"DisplayMapFramebuffer", (ULONG)STATUS_ACCESS_DENIED},
            {"native SMU owner", L"SmuOwnerStart", (ULONG)STATUS_ACCESS_DENIED},
            {"WDDM start", L"WddmStart", (ULONG)STATUS_DEVICE_NOT_READY},
            {"inherited timing", L"DisplayPrepareInheritedTiming", (ULONG)STATUS_UNSUCCESSFUL},
            {"connector admission", L"ConnectorStart", (ULONG)STATUS_DEVICE_NOT_READY},
        };
        unsigned i;
        for (i = 0; i < RTL_NUMBER_OF(table); i++) {
            Reset();
            if (i == 0) iface.DxgkCbGetDeviceInformation = GetDeviceInfoFails;
            if (i == 1) iface.DxgkCbAcquirePostDisplayOwnership = AcquirePostFails;
            if (i == 2) iface.DxgkCbAcquirePostDisplayOwnership = AcquirePostEmpty;
            if (i == 3) fbStatus = STATUS_ACCESS_DENIED;
            if (i == 4) { enableNativeSmu = 1; dev.Mmio = &dev; smuStatus = STATUS_ACCESS_DENIED; }
            if (i == 5) wddmStatus = STATUS_DEVICE_NOT_READY;
            if (i == 6) timingStatus = STATUS_UNSUCCESSFUL;
            if (i == 7) connectorStartStatus = STATUS_DEVICE_NOT_READY;
            CHECK((ULONG)Start() == table[i].status);
            // KeepLog is 0, the release value: the record is written all the same. That is the defect.
            CHECK(RecordIs(L"failed", table[i].step, L"-", table[i].status, StageStartFailed));
            CHECK(durable == 1);
            CHECK(readyCalls == 0);
            CHECK(outputSources == 0 && outputChildren == 0 && dev.StartVisibilityPending == 0);
            CHECK(LogLines(NULL) == 12);
            if (failures) { printf("  (step case: %s)\n", table[i].what); break; }
        }
    }
    // ---- BD-169: a connector refusal that keeps a display completes the start ---------------------------
    // ConnectorStart programs no output, so its refusal leaves the firmware's own
    // stream on the wire. The case above is the other half: when nothing can be
    // presented the step still fails and names itself (#38's scaled tuple).
    {
        unsigned starts;
        Reset(); connectorStartStatus = STATUS_DEVICE_NOT_READY; connectorRefusalKeeps = 1;
        starts = connectorStarts;                       // Reset() does not clear this counter
        CHECK(Start() == STATUS_SUCCESS);
        CHECK(RecordIs(L"started", L"-", L"-", 0, StageStartDone));
        CHECK(connectorStarts == starts + 1 && readyCalls == 1 && stops == 0);
    }
    // BD-169: the check that refused the firmware pipe reaches the record of the step that fails. The classifier
    // runs in the display step, which an equal-size pipe passes whatever its verdict; the start then fails in
    // ConnectorStart, and that record is the file a tester sends us. Detail is written once, by the step that
    // measured it, so the later failure must still carry the gate number, its name and the word it read.
    {
        PCWSTR detail;
        Reset(); inheritedGateDetail = 1; connectorStartStatus = STATUS_DEVICE_NOT_READY;
        CHECK((ULONG)Start() == (ULONG)STATUS_DEVICE_NOT_READY);
        CHECK(RecordIs(L"failed", L"ConnectorStart", L"-", (ULONG)STATUS_DEVICE_NOT_READY, StageStartFailed));
        detail = Str(L"Detail");
        CHECK(detail != NULL && wcsstr(detail, L"gate 14 tiling word 00000080") != NULL);
        CHECK(detail != NULL && wcsstr(detail, L"support 00000000") != NULL);
        // A start that keeps the display is not a failure, so no record carries that line as a reason.
        Reset(); inheritedGateDetail = 1; connectorStartStatus = STATUS_DEVICE_NOT_READY; connectorRefusalKeeps = 1;
        CHECK(Start() == STATUS_SUCCESS);
        CHECK(RecordIs(L"started", L"-", L"-", 0, StageStartDone));
        CHECK(Find(2, L"Detail") < 0);
    }

    // The sub-step half: WddmStart and GpuStartupInitialize name the phase inside them.
    Reset(); wddmStatus = STATUS_DEVICE_NOT_READY; wddmSubStep = L"psp-initialize";
    CHECK(Start() == STATUS_DEVICE_NOT_READY);
    CHECK(RecordIs(L"failed", L"WddmStart", L"psp-initialize", (ULONG)STATUS_DEVICE_NOT_READY, StageStartFailed));
    // A later step with no sub-step of its own does not inherit the last one.
    Reset(); wddmSubStep = L"gfx-initialize"; timingStatus = STATUS_UNSUCCESSFUL;
    CHECK(Start() == STATUS_UNSUCCESSFUL);
    CHECK(RecordIs(L"failed", L"DisplayPrepareInheritedTiming", L"-", (ULONG)STATUS_UNSUCCESSFUL, StageStartFailed));
    // GitHub #38: the refusing step's measured values go with the failure as Detail.
    CHECK(Str(L"Detail") != NULL && !wcscmp(Str(L"Detail"), L"timing 0xFFFFFFF6 post 800x600 otg0 ctl 00000000"));

    // The boot-loop guard's refusal says it was the guard, and carries the count.
    Reset(); guardStatus = STATUS_DEVICE_CONFIGURATION_ERROR; unconfirmed = 2;
    CHECK(Start() == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(RecordIs(L"guard", L"BootLoopGuard", L"-", (ULONG)STATUS_DEVICE_CONFIGURATION_ERROR, StageRefusedByGuard));
    CHECK(Find(2, L"Detail") < 0);   // the attempt cleared the previous start's Detail: it never pairs with this one
    { ULONG v = 0; CHECK(Dword(L"UnconfirmedStarts", &v) && v == 2 && v >= BC250_MAX_UNCONFIRMED_STARTS); }
    CHECK(LogLines(NULL) == 12 && durable == 1);

    // A previous stop that was never confirmed refuses the start before the guard, and is a step as well. It fails
    // the start, so it leaves stage 91 like every other failure: a reader of LastStage, and the record's own
    // documented "Stage 91 for a failure", must not miss this one.
    Reset(); dev.GpuStopUnconfirmed = TRUE;
    CHECK(Start() == STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(RecordIs(L"failed", L"PreviousStopUnconfirmed", L"-", (ULONG)STATUS_DEVICE_HARDWARE_ERROR, StageStartFailed));
    CHECK(lastStage == StageStartFailed);

    // A device object this driver does not own writes nothing at all.
    Reset(); dev.PhysicalDeviceObject = NULL;
    CHECK(Start() == STATUS_DEVICE_BUSY && opens == 0 && creates == 0 && writeOps == 0 && Find(2, L"Result") < 0);

    // The lines are bounded: 64 of them at most, each at most 159 characters.
    Reset(); fbStatus = STATUS_ACCESS_DENIED; ringTotal = 500; ringLongLines = 1;
    CHECK(Start() == STATUS_ACCESS_DENIED);
    { SIZE_T widest = 0; CHECK(LogLines(&widest) == 64 && widest == 159); }
    { int i = Find(2, L"Log"); CHECK(i >= 0 && vals[i].type == REG_MULTI_SZ); }
    // The newest lines, oldest first: they must run up to the failure, not start at the driver's load.
    Reset(); fbStatus = STATUS_ACCESS_DENIED; ringTotal = 500;
    CHECK(Start() == STATUS_ACCESS_DENIED);
    { int i = Find(2, L"Log"); CHECK(i >= 0 && !wcscmp((PCWSTR)vals[i].data, L"ring line 436")); }
    // A shorter ring gives what it has, and an empty line in it does not end the sequence early.
    Reset(); fbStatus = STATUS_ACCESS_DENIED; ringTotal = 5; ringEmptyAt = 2;
    CHECK(Start() == STATUS_ACCESS_DENIED && LogLines(NULL) == 4);

    // The lines lead UP TO the failure, not through the unwind that follows it. The record is written after the
    // cleanup (requirement 4 keeps that order), and the teardown of a GPU that had started writes dozens of lines
    // of its own: without the failure mark the 64 kept lines would be the unwind, which names no step.
    Reset(); timingStatus = STATUS_UNSUCCESSFUL; ringTotal = 500; ringGrowOnStop = 120;
    CHECK(Start() == STATUS_UNSUCCESSFUL);
    CHECK(stops == 1 && ringTotal == 620);              // the common failed: unwind runs once
    CHECK(LogLines(NULL) == 64);
    CHECK(LogLineAt(0) != NULL && !wcscmp(LogLineAt(0), L"ring line 436"));
    CHECK(LogLineAt(63) != NULL && !wcscmp(LogLineAt(63), L"ring line 499"));   // the last line before the teardown
    CHECK(LogLineAt(64) == NULL);
    // The same where the teardown writes far more lines than the record keeps, and the failure point is lower than
    // the 64-line window: what is kept is still the start's own lines and none of the unwind's.
    Reset(); wddmStatus = STATUS_DEVICE_NOT_READY; ringTotal = 30; ringGrowOnStop = 400;
    CHECK(Start() == STATUS_DEVICE_NOT_READY);
    CHECK(LogLines(NULL) == 30 && LogLineAt(29) != NULL && !wcscmp(LogLineAt(29), L"ring line 29"));
    // A refusal that runs no cleanup at all has no mark, and keeps the newest lines the ring holds.
    Reset(); guardStatus = STATUS_DEVICE_CONFIGURATION_ERROR; ringTotal = 70;
    CHECK(Start() == STATUS_DEVICE_CONFIGURATION_ERROR);
    CHECK(LogLines(NULL) == 64 && LogLineAt(63) != NULL && !wcscmp(LogLineAt(63), L"ring line 69"));

    // ---- requirement 2: the record belongs to the newest attempt ----------------------------------------------
    // "attempt" is written and flushed before any step of this start can fail: Result is the first value written,
    // and the real result is the last one before the flush.
    Reset(); fbStatus = STATUS_ACCESS_DENIED;
    CHECK(Start() == STATUS_ACCESS_DENIED);
    CHECK(strstr(trace, "Result=attempt,") == trace);
    CHECK(strstr(trace, "Result=failed,flush,") != NULL);
    // A failure, then a start that succeeds: the record says started and the failure's lines are gone, so no
    // reader can pair this boot's result with the other boot's reason.
    Reset(); fbStatus = STATUS_ACCESS_DENIED;
    CHECK(Start() == STATUS_ACCESS_DENIED &&
          RecordIs(L"failed", L"DisplayMapFramebuffer", L"-", (ULONG)STATUS_ACCESS_DENIED, StageStartFailed));
    trace[0] = 0; logCount = 0; fbStatus = STATUS_SUCCESS; dev.StopDone = TRUE;
    CHECK(Start() == STATUS_SUCCESS);
    CHECK(RecordIs(L"started", L"-", L"-", 0, StageStartDone));
    CHECK(LogLines(NULL) == 0 && Find(2, L"Log") < 0 && Find(2, L"Detail") < 0);
    { const char* a = strstr(trace, "Result=attempt,"); const char* b = strstr(trace, "Result=started,flush,");
      CHECK(a != NULL && b != NULL && a < b); }
    // A failure, then a boot that dies inside the start (bugcheck, hang, AC loss): the attempt record of that
    // boot already replaced the reason, and Step "-" with no lines says the start got nowhere.
    {
        static unsigned char keep[sizeof(vals)];
        Reset(); fbStatus = STATUS_ACCESS_DENIED;
        CHECK(Start() == STATUS_ACCESS_DENIED);
        memcpy(keep, vals, sizeof(vals));
        Reset(); fbStatus = STATUS_ACCESS_DENIED;
        memcpy(vals, keep, sizeof(vals));
        CHECK(Str(L"Result") != NULL && !wcscmp(Str(L"Result"), L"failed"));    // the old boot's record survives
        // From inside the next start, before it can fail or finish, the record must already be this attempt's.
        // That is the moment a bugcheck or a hang freezes, and it is what a tester then reads.
        watchInsideStart = 1; resultInsideStart[0] = 0;
        CHECK(Start() == STATUS_ACCESS_DENIED);
        CHECK(!wcscmp(resultInsideStart, L"attempt"));
        watchInsideStart = 0;
        // The same record read on its own: Step "-" and no lines say the start got nowhere.
        Reset(); memcpy(vals, keep, sizeof(vals));
        GuardRecordStartAttempt();
        CHECK(RecordIs(L"attempt", L"-", L"-", 0, 0) && Find(2, L"Log") < 0 && durable == 1);

        // A torn write, on the kind of machine this record is for: one value of the record cannot be written. The
        // value that failed is REMOVED, never left at the last start's content, and Result stays "attempt" until
        // every value of this one is in. Here the Step write of the attempt record fails and the start then
        // succeeds: from inside that start nothing of the old failure stands any more.
        Reset(); memcpy(vals, keep, sizeof(vals));
        failAt = 2; writeStatus = STATUS_REGISTRY_IO_FAILED;    // the attempt record's Step write
        watchInsideStart = 1; resultInsideStart[0] = 0; stepInsideStart[0] = 0; logInsideStart = 1;
        CHECK(Start() == STATUS_SUCCESS);
        CHECK(!wcscmp(resultInsideStart, L"attempt"));          // not the old boot's "failed"
        CHECK(!wcscmp(stepInsideStart, L"absent"));             // and not the old boot's step either
        CHECK(logInsideStart == 0);                             // nor its log lines
        watchInsideStart = 0;
        // And a hive that answers nothing at all: every value goes, so the record reads as no record, which is the
        // truth. A reader must never find the previous boot's reason with this boot's Result beside it.
        Reset(); memcpy(vals, keep, sizeof(vals));
        writeStatus = STATUS_REGISTRY_IO_FAILED;
        CHECK(Start() == STATUS_SUCCESS);
        CHECK(Find(2, L"Result") < 0 && Find(2, L"Step") < 0 && Find(2, L"Status") < 0 && Find(2, L"Log") < 0);
        CHECK(flushes == 0 && durable == 0);                    // nothing claimed to be durable
    }

    // ---- requirement 4: the record can never fail or hang a start ---------------------------------------------
    // The failure path's cleanup order is the one it had before this change, and the record comes after all of it.
    Reset(); fbStatus = STATUS_ACCESS_DENIED;
    CHECK(Start() == STATUS_ACCESS_DENIED);
    CHECK(strstr(trace, "stop,bmstop,bmclear,hclose,stage91,Result=attempt,") != NULL);
    // Every registry and pool failure, one at a time: the start keeps the step's own status and says so, once per
    // record, and the cleanup order does not move.
    {
        unsigned i;
        for (i = 0; i < 12; i++) {
            Reset(); fbStatus = STATUS_ACCESS_DENIED;
            if (i == 0) openStatus = STATUS_ACCESS_DENIED;
            else if (i == 1) createStatus = STATUS_ACCESS_DENIED;
            else if (i == 2) flushStatus = STATUS_REGISTRY_IO_FAILED;
            else if (i == 3) poolRefused = 1;
            else { failAt = i - 3; writeStatus = STATUS_REGISTRY_IO_FAILED; }
            CHECK(Start() == STATUS_ACCESS_DENIED);     // the step's status, never the registry's
            CHECK(stops == 1);
            CHECK(strstr(trace, "stop,bmstop,bmclear,hclose,stage91,") != NULL);
            CHECK(logCount <= 12);                      // a line, not a storm
            if (i == 0) CHECK(LoggedTimes("Parameters open") == 2);     // the attempt's and the failure's
            if (i == 1) CHECK(LoggedTimes("subkey create") == 2);
            if (i == 3) CHECK(LoggedTimes("log lines not kept") == 1);  // only a failure keeps lines
            if (failures) { printf("  (registry failure case %u)\n", i); break; }
        }
    }
    // Nothing is written above PASSIVE_LEVEL, and nothing before GuardInit took the Parameters path.
    Reset(); g_Irql = DISPATCH_LEVEL;
    GuardRecordStartFailed(STATUS_ACCESS_DENIED);
    CHECK(opens == 0 && creates == 0 && writeOps == 0 && LoggedTimes("IRQL or no Parameters path") == 1);
    Reset(); stateReady = 0;
    GuardRecordStartFailed(STATUS_ACCESS_DENIED);
    CHECK(opens == 0 && creates == 0 && writeOps == 0 && LoggedTimes("IRQL or no Parameters path") == 1);
    // A start that works answers exactly as before: no stop, two records, two flushes, nothing else.
    Reset();
    CHECK(Start() == STATUS_SUCCESS && stops == 0 && flushes == 2 && creates == 2 && closes == 4);

    // ---- requirement 6: the self-check line, inside the ring's 159 characters ---------------------------------
    Reset(); fbStatus = STATUS_ACCESS_DENIED;
    CHECK(Start() == STATUS_ACCESS_DENIED);
    {
        unsigned i, found = 0;
        for (i = 0; i < logCount && i < MAXLOGS; i++)
            if (strstr(logs[i], "start record failed DisplayMapFramebuffer - 0x") &&
                strstr(logs[i], "12 lines, write 0x00000000")) { found++; CHECK(strlen(logs[i]) <= 159); }
        CHECK(found == 1);
    }

    CHECK(connectorPrepares>0 && connectorStarts>0 && connectorDirty==0);
    printf("BD-122 start record: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
