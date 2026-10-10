/* The generated translation unit contains the real GfxCaptureWaves and its
 * helpers. This model exercises lifetime admission and cleanup, not Windows
 * scheduling or hardware behavior during a hang. Escape flags are exactly 8. */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include "wave_snapshot.h"
typedef struct { const char* Name; unsigned long Offset; } BC250_DCN_REG_INFO;
#define BC250_REGS_WITH_TABLES
#include "regs.generated.h"
#include "gc_10_1_0_sh_mask.h"

typedef int NTSTATUS, BOOLEAN;
typedef long LONG;
typedef unsigned long ULONG;
typedef ULONG* PULONG;
typedef unsigned long long ULONGLONG;
typedef struct { long long QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define MAXULONG 0xfffffffful
#define MAXLONG 0x7fffffffl
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#ifndef _In_
#define _In_
#endif
#define STATUS_SUCCESS 0
#define STATUS_ACCESS_DENIED (-1)
#define STATUS_DEVICE_DATA_ERROR (-2)
#define STATUS_DEVICE_NOT_READY (-3)
#define STATUS_INVALID_DEVICE_STATE (-4)
#define STATUS_IO_TIMEOUT (-5)
#define STATUS_MOCK_FAULT (-6)
#define STATUS_MOCK_GART (-7)
#define STATUS_INVALID_PARAMETER (-8)
#define STATUS_INSUFFICIENT_RESOURCES (-9)
#define NT_SUCCESS(s) ((s) >= 0)
#define BC250_KMD_VERSION 0x00070100u
#define BC250_GFX_STAGE_INTERRUPTS 8u

struct amdgpu_device { void* backend; };
typedef struct BC250_GFX {
    int SetUp, PowerSuspended, Failed;
    unsigned StagesDone;
} BC250_GFX;
typedef struct BC250_DEVICE {
    int Started;
    void* Gfx;
    void* GpuMem;
    volatile LONG RetainedPowerPhase;
    volatile LONG WaveRegisterAccess;
    void* PhysicalDeviceObject;
    volatile ULONG* Mmio;
    int MmioGfxEnabled;
    int GfxPagingLock, GartLock;
} BC250_DEVICE;
typedef struct BC250_SEQUENCE {
    const char* Name;
    NTSTATUS (*Read)(const BC250_DEVICE*, ULONG, ULONG*);
    NTSTATUS (*Write)(const BC250_DEVICE*, ULONG, ULONG);
    NTSTATUS Fault;
    ULONG FaultOffset;
    const BC250_DEVICE* Device;
} BC250_SEQUENCE;

enum { ShimComplete, ShimFault, ShimBudget, ShimTruncated, ShimPartial };
typedef struct { char Kind; ULONG Offset, Value; } MMIO_EVENT;
static struct {
    unsigned Accesses, FailAt, MismatchAt, PoisonAt;
    int Irql, Critical, Shared, Mutex, GartCalls, ShimCalls, ShimMode, Enabled, Owner;
    NTSTATUS GartResult;
    ULONG Grbm, SqIndex;
    ULONGLONG InterruptTime, SystemTime;
    MMIO_EVENT Events[32];
    char Locks[16];
    unsigned LockCount;
    unsigned StallCalls, ReleaseSamplerAt, LogCount, HardwareWrites;
    char Logs[16][180];
} model;
static BC250_DEVICE device;
static BC250_GFX testGfx;
static struct amdgpu_device testAdev;
static BC250_WAVE_SNAPSHOT testSnapshot;
static unsigned checks, failures;
static int oldBackend;
static int pdoIdentity;
static const ULONG entryIndex = 0x13579u;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %u: %s\n", __LINE__, #x); } } while (0)
#define NT_ASSERT(x) CHECK(x)
BOOLEAN GfxWaveSampleBegin(BC250_DEVICE* Device);
void GfxWaveSampleEnd(BC250_DEVICE* Device);
BOOLEAN GfxWaveCaptureActive(const BC250_DEVICE* Device);
static void GuardLog(const char* format, ...)
{
    va_list args;
    CHECK(model.LogCount < 16);
    va_start(args, format);
    vsnprintf(model.Logs[model.LogCount++], sizeof(model.Logs[0]), format, args);
    va_end(args);
}

static unsigned int bc250_gfx_wave_broadcast_value(void)
{
    return GRBM_GFX_INDEX__INSTANCE_BROADCAST_WRITES_MASK |
           GRBM_GFX_INDEX__SA_BROADCAST_WRITES_MASK |
           GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK;
}

static void lock_event(char c)
{
    CHECK(model.LockCount + 1 < sizeof(model.Locks));
    model.Locks[model.LockCount++] = c;
    model.Locks[model.LockCount] = 0;
}
static void KeEnterCriticalRegion(void)
{ CHECK(!model.Critical); model.Critical = 1; lock_event('C'); }
static void ExAcquirePushLockShared(int* lock)
{ CHECK(lock == &device.GfxPagingLock && model.Critical && !model.Shared && !model.Mutex); model.Shared = 1; lock_event('S'); }
static void ExAcquireFastMutex(int* lock)
{ CHECK(lock == &device.GartLock && model.Shared && !model.Mutex); model.Mutex = 1; lock_event('M'); }
static void ExReleaseFastMutex(int* lock)
{ CHECK(lock == &device.GartLock && model.Shared && model.Mutex); model.Mutex = 0; lock_event('m'); }
static void ExReleasePushLockShared(int* lock)
{ CHECK(lock == &device.GfxPagingLock && model.Shared && !model.Mutex); model.Shared = 0; lock_event('s'); }
static void KeLeaveCriticalRegion(void)
{ CHECK(model.Critical && !model.Shared && !model.Mutex); model.Critical = 0; lock_event('c'); }
static int KeGetCurrentIrql(void) { return model.Irql; }
static ULONGLONG KeQueryInterruptTime(void) { return model.InterruptTime++; }
/* Any production use of wall time for capture duration is a test failure. */
#define KeQuerySystemTime(now) do { CHECK(0); (now)->QuadPart = model.SystemTime; } while (0)
static void KeStallExecutionProcessor(unsigned usec)
{
    CHECK(usec == 1 && device.WaveRegisterAccess > 0);
    model.StallCalls++; model.InterruptTime += 10000;
    if (model.StallCalls == model.ReleaseSamplerAt) GfxWaveSampleEnd(&device);
}
static LONG InterlockedCompareExchange(volatile LONG* p, LONG value, LONG match)
{ LONG old = *p; if (old == match) *p = value; return old; }
static LONG InterlockedExchange(volatile LONG* p, LONG value)
{ LONG old = *p; *p = value; return old; }
static LONG InterlockedDecrement(volatile LONG* p) { return --*p; }
static int AdapterOwnerIs(void* pdo)
{ CHECK(model.Shared && model.Mutex); return model.Owner && pdo == &pdoIdentity; }

static NTSTATUS GartDevice(BC250_DEVICE* d, struct amdgpu_device** out, BOOLEAN* enabled)
{
    CHECK(d == &device && model.Shared && model.Mutex);
    model.GartCalls++;
    *out = &testAdev;
    *enabled = model.Enabled;
    return model.GartResult;
}
static NTSTATUS mmio_event(const BC250_DEVICE* d, char kind, ULONG offset, ULONG value)
{
    MMIO_EVENT* event;
    CHECK(d == &device && model.Shared && model.Mutex);
    CHECK(GfxWaveCaptureActive(&device) && !GfxWaveSampleBegin(&device));
    CHECK(offset == BC250_REG_GC_GRBM_GFX_INDEX || offset == BC250_REG_GC_SQ_IND_INDEX);
    CHECK(model.Accesses < sizeof(model.Events) / sizeof(model.Events[0]));
    event = &model.Events[model.Accesses++];
    event->Kind = kind; event->Offset = offset; event->Value = value;
    return model.Accesses == model.FailAt ? STATUS_MOCK_FAULT : STATUS_SUCCESS;
}
static NTSTATUS MmioGfxRead(const BC250_DEVICE* d, ULONG offset, ULONG* value)
{
    NTSTATUS status = mmio_event(d, 'R', offset, 0);
    if (!NT_SUCCESS(status)) return status;
    *value = offset == BC250_REG_GC_GRBM_GFX_INDEX ? model.Grbm : model.SqIndex;
    if (model.Accesses == model.MismatchAt) *value ^= 1;
    if (model.Accesses == model.PoisonAt) *value = MAXULONG;
    return status;
}
static NTSTATUS MmioGfxWrite(const BC250_DEVICE* d, ULONG offset, ULONG value)
{
    NTSTATUS status = mmio_event(d, 'W', offset, value);
    if (!NT_SUCCESS(status)) return status;
    if (offset == BC250_REG_GC_GRBM_GFX_INDEX) model.Grbm = value;
    else model.SqIndex = value;
    return status;
}
static void SequenceBegin(BC250_SEQUENCE* s, const BC250_DEVICE* d, BOOLEAN enabled, void* data, ULONG bytes)
{
    CHECK(d == &device && !enabled && !data && !bytes);
    CHECK(s->Read == MmioGfxRead && s->Write != NULL && !strcmp(s->Name, "waves"));
    CHECK(testAdev.backend == &oldBackend && model.Mutex && model.Shared);
    s->Device = d; s->Fault = STATUS_SUCCESS; s->FaultOffset = 0;
}

/* The shim's many individual MMIO reads are tested by run_wave.ps1. This stub
 * leaves a selected bank behind on failure as a faulted sequence would. */
static int bc250_gfx_read_waves(struct amdgpu_device* a, BC250_WAVE_SNAPSHOT* result,
                              unsigned int (*stop)(void*), void* context)
{
    BC250_SEQUENCE* s = a->backend;
    NTSTATUS status;
    CHECK(a == &testAdev && a->backend != &oldBackend && s->Device == &device);
    CHECK(model.Shared && model.Mutex && stop(context) == 0);
    model.ShimCalls++;
    result->Header.Flags = 0;
    /* A wall-clock correction during capture cannot invalidate its duration. */
    model.SystemTime = 0;
    result->Header.WaveCount = 1;
    result->Header.SlotsVisited = 80;
    result->Waves[0].Raw[Bc250WavePcLo] = 0x123400u;
    status = s->Write(s->Device, BC250_REG_GC_GRBM_GFX_INDEX, 1);
    if (!NT_SUCCESS(status) || model.ShimMode == ShimFault) {
        s->Fault = STATUS_MOCK_FAULT;
        s->FaultOffset = BC250_REG_GC_GRBM_GFX_INDEX;
        result->Header.Flags = stop(context);
        CHECK(result->Header.Flags == BC250_WAVE_READ_ERROR);
        return -1;
    }
    if (model.ShimMode == ShimBudget) {
        model.InterruptTime += 100000000ull;
        result->Header.Flags = stop(context);
        CHECK(result->Header.Flags == BC250_WAVE_BUDGET_EXPIRED);
        return -1;
    }
    if (model.ShimMode == ShimTruncated) { result->Header.Flags = BC250_WAVE_TRUNCATED; return -1; }
    if (model.ShimMode == ShimPartial) { result->Header.Flags = BC250_WAVE_READ_ERROR; return -1; }
    result->Header.WaveCount = 2;
    result->Header.Flags = BC250_WAVE_COMPLETE;
    return 0;
}

/* ACTUAL_SOURCE */

/* Check the actual shared GFX write filter, beyond the wave-only backend. */
#define BC250_BAR5_LENGTH 0x80000ul
static ULONG fakeBar[BC250_BAR5_LENGTH / sizeof(ULONG)];
static BOOLEAN InTable(const unsigned long* table, ULONG count, ULONG offset)
{
    ULONG i;
    for (i = 0; i < count; i++) if (table[i] == offset) return TRUE;
    return FALSE;
}
static void HardwareWrite(PULONG pointer, ULONG value)
{ model.HardwareWrites++; *pointer = value; }
#define WRITE_REGISTER_ULONG(pointer, value) HardwareWrite(pointer, value)
/* ACTUAL_MMIO */

/* The first four WDK 10.0.26100 d3dukmdt.h escape flag positions (lines 600-606).
 * The host model does not establish dxgkrnl admission for their combination. */
typedef union {
    struct {
        unsigned HardwareAccess : 1;
        unsigned DeviceStatusQuery : 1;
        unsigned ChangeFrameLatency : 1;
        unsigned NoAdapterSynchronization : 1;
        unsigned Rest : 28;
    };
    unsigned Value;
} D3DDDI_ESCAPEFLAGS;
/* Common escape prefix, independently checked against waves by production. */
typedef struct { ULONG Magic, Command, Status; } BC250_ESCAPE;
#define BC250_ESCAPE_MAGIC 0x30353242u
#define BC250_ESCAPE_STATUS_DONE 0u
#define BC250_ESCAPE_STATUS_REFUSED 2u
#define BC250_ESCAPE_STATUS_NOT_ADMIN 3u
#define RtlZeroMemory(p, n) memset(p, 0, n)
#define RtlCopyMemory(d, s, n) memcpy(d, s, n)
#define C_ASSERT(x) _Static_assert(x, #x)
#define FIELD_OFFSET(t, f) offsetof(t, f)
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_WAVES, Status) == 8);
#define POOL_FLAG_NON_PAGED 1
typedef struct { int Primary, Client, HasClient; } SECURITY_SUBJECT_CONTEXT;
static int primaryAdmin, clientAdmin, hasClient, subjectLive, subjectLocked;
static int failPool, allocatedPool, poolAllocCalls, poolFreeCalls;
static char subjectTrace[16];
static unsigned subjectPos;
static BC250_ESCAPE_WAVES response;
static BC250_WAVE_SNAPSHOT privateSnapshot;

static void subject_event(char event)
{
    CHECK(subjectPos + 1 < sizeof(subjectTrace));
    subjectTrace[subjectPos++] = event; subjectTrace[subjectPos] = 0;
}
static void SeCaptureSubjectContext(SECURITY_SUBJECT_CONTEXT* subject)
{
    CHECK(!subjectLive && !subjectLocked); subjectLive = 1; subject_event('C');
    subject->Primary = primaryAdmin; subject->Client = clientAdmin; subject->HasClient = hasClient;
}
static void SeLockSubjectContext(SECURITY_SUBJECT_CONTEXT* subject)
{ (void)subject; CHECK(subjectLive && !subjectLocked); subjectLocked = 1; subject_event('L'); }
static const int* SeQuerySubjectContextToken(SECURITY_SUBJECT_CONTEXT* subject)
{ CHECK(subjectLive && subjectLocked); subject_event('Q'); return subject->HasClient ? &subject->Client : &subject->Primary; }
static BOOLEAN SeTokenIsAdmin(const int* token)
{ CHECK(subjectLive && subjectLocked); subject_event('A'); return *token; }
static void SeUnlockSubjectContext(SECURITY_SUBJECT_CONTEXT* subject)
{ (void)subject; CHECK(subjectLive && subjectLocked); subjectLocked = 0; subject_event('U'); }
static void SeReleaseSubjectContext(SECURITY_SUBJECT_CONTEXT* subject)
{ (void)subject; CHECK(subjectLive && !subjectLocked); subjectLive = 0; subject_event('R'); }
static void* ExAllocatePool2(int flags, size_t bytes, int tag)
{
    CHECK(flags == POOL_FLAG_NON_PAGED && bytes == sizeof(privateSnapshot) && tag == 'vW2B');
    CHECK(!subjectLive && !allocatedPool); poolAllocCalls++;
    if (failPool) return NULL;
    memset(&privateSnapshot, 0, sizeof(privateSnapshot)); allocatedPool = 1;
    return &privateSnapshot;
}
static void ExFreePoolWithTag(void* p, int tag)
{ CHECK(p == &privateSnapshot && tag == 'vW2B' && allocatedPool); allocatedPool = 0; poolFreeCalls++; }

/* ACTUAL_ESCAPE */

static void reset(void)
{
    memset(&model, 0, sizeof(model)); memset(&device, 0, sizeof(device));
    memset(&testGfx, 0, sizeof(testGfx)); memset(&testSnapshot, 0, sizeof(testSnapshot));
    model.Enabled = 1; model.Grbm = bc250_gfx_wave_broadcast_value(); model.SqIndex = entryIndex;
    model.Owner = 1; device.PhysicalDeviceObject = &pdoIdentity;
    model.InterruptTime = 1000; model.SystemTime = 2000;
    device.Started = 1; device.Gfx = &testGfx; device.GpuMem = &testGfx;
    testGfx.SetUp = 1; testGfx.StagesDone = BC250_GFX_STAGE_INTERRUPTS;
    testAdev.backend = &oldBackend;
    testSnapshot.Header.Kind = BC250_WAVE_KIND_PRE_KILL;
    testSnapshot.Header.Seq = 19; testSnapshot.Header.Fence = 29; testSnapshot.Header.Vmid = 3;
    primaryAdmin = 1; clientAdmin = 0; hasClient = 0;
    subjectLive = subjectLocked = 0; subjectPos = 0; subjectTrace[0] = 0;
    failPool = allocatedPool = poolAllocCalls = poolFreeCalls = 0;
    memset(&response, 0xff, sizeof(response));
    response.AbiVersion = BC250_WAVE_ABI_VERSION; response.Op = 0; response.Reserved = 0;
}
static NTSTATUS capture(void)
{
    NTSTATUS status = GfxCaptureWaves(&device, &testSnapshot);
    CHECK(testAdev.backend == &oldBackend);
    CHECK(!model.Critical && !model.Shared && !model.Mutex);
    CHECK(!strcmp(model.Locks, model.Irql ? "" : "CSMmsc"));
    CHECK(testSnapshot.Header.Result == status);
    CHECK(testSnapshot.Header.AbiVersion == BC250_WAVE_ABI_VERSION);
    CHECK(testSnapshot.Header.DriverVersion == BC250_KMD_VERSION);
    CHECK(testSnapshot.Header.EndTime100ns >= testSnapshot.Header.StartTime100ns);
    CHECK(testSnapshot.Header.StartTime100ns == 1000);
    CHECK(testSnapshot.Header.Kind == BC250_WAVE_KIND_PRE_KILL);
    CHECK(testSnapshot.Header.Seq == 19 && testSnapshot.Header.Fence == 29 && testSnapshot.Header.Vmid == 3);
    CHECK(testSnapshot.Header.Bytes == sizeof(BC250_WAVE_HEADER) + testSnapshot.Header.WaveCount * sizeof(BC250_WAVE_RECORD));
    if (!NT_SUCCESS(status)) CHECK(!(testSnapshot.Header.Flags & BC250_WAVE_COMPLETE));
    return status;
}
static void cleanup_order(unsigned first)
{
    unsigned i, grbmWrite = 32, sqWrite = 32;
    for (i = first; i < model.Accesses; i++) {
        MMIO_EVENT* e = &model.Events[i];
        if (e->Kind != 'W') continue;
        if (e->Offset == BC250_REG_GC_GRBM_GFX_INDEX) grbmWrite = i;
        if (e->Offset == BC250_REG_GC_SQ_IND_INDEX) sqWrite = i;
    }
    CHECK(grbmWrite < sqWrite && sqWrite < model.Accesses);
    CHECK(model.Events[grbmWrite].Value == bc250_gfx_wave_broadcast_value());
    CHECK(model.Events[sqWrite].Value == entryIndex);
}

static void escape(ULONG flags)
{
    WaveEscape(&device, &response, flags);
    CHECK(!strcmp(subjectTrace, "CLQAUR") && !subjectLive && !subjectLocked);
    CHECK(!allocatedPool && poolFreeCalls == (poolAllocCalls && !failPool ? 1 : 0));
    CHECK(testAdev.backend == &oldBackend && !model.Critical && !model.Shared && !model.Mutex);
    CHECK(response.Magic == BC250_ESCAPE_MAGIC && response.Command == BC250_ESCAPE_GET_WAVES);
    CHECK(response.Version == BC250_KMD_VERSION && response.AbiVersion == BC250_WAVE_ABI_VERSION);
    CHECK(response.NtStatus == response.Snapshot.Header.Result);
    CHECK(response.Op == 0 && response.Reserved == 0);
    CHECK(response.Snapshot.Header.Reserved0 == 0 && response.Snapshot.Header.Reserved[3] == 0);
    CHECK(response.Snapshot.Waves[BC250_WAVE_MAX_RECORDS - 1].Raw[Bc250WavePcLo] == 0);
}

static void test_escape(void)
{
    unsigned i;
    reset(); primaryAdmin = 1; hasClient = 1; clientAdmin = 0;
    escape(8);
    CHECK(response.NtStatus == STATUS_ACCESS_DENIED && response.Status == BC250_ESCAPE_STATUS_NOT_ADMIN);
    CHECK(!model.Accesses && !poolAllocCalls && !model.GartCalls);
    reset(); primaryAdmin = 0; hasClient = 1; clientAdmin = 1;
    escape(8);
    CHECK(response.NtStatus == STATUS_SUCCESS && response.Status == BC250_ESCAPE_STATUS_DONE);
    CHECK(model.ShimCalls == 1 && poolAllocCalls == 1 && poolFreeCalls == 1);
    CHECK(response.Snapshot.Header.WaveCount == 2 && response.Snapshot.Header.Kind == BC250_WAVE_KIND_LIVE);
    CHECK(response.Snapshot.Header.Seq == 0 && response.Snapshot.Header.Fence == 0);
    reset(); primaryAdmin = 0;
    escape(8); CHECK(response.NtStatus == STATUS_ACCESS_DENIED && !model.Accesses);

    /* Every other low-bit combination and every extra bit is refused. */
    for (i = 0; i < 32; i++) {
        if (i == 8) continue;
        reset(); escape(i);
        CHECK(response.NtStatus == STATUS_INVALID_PARAMETER && !model.Accesses && !poolAllocCalls);
    }
    for (i = 5; i < 32; i++) {
        reset(); escape(8ul | (1ul << i));
        CHECK(response.NtStatus == STATUS_INVALID_PARAMETER && !model.Accesses && !poolAllocCalls);
    }
    for (i = 0; i < 5; i++) {
        reset();
        switch (i) {
        case 0: response.AbiVersion++; break;
        case 1: response.Op = 1; break;
        case 2: response.Reserved = 1; break;
        case 3: model.Irql = 2; break;
        case 4: failPool = 1; break;
        }
        escape(8);
        CHECK(!model.Accesses && !model.ShimCalls && !poolFreeCalls);
        CHECK(response.Status == BC250_ESCAPE_STATUS_REFUSED);
        CHECK(response.NtStatus == (i == 4 ? STATUS_INSUFFICIENT_RESOURCES :
              i == 3 ? STATUS_INVALID_DEVICE_STATE : STATUS_INVALID_PARAMETER));
        CHECK(response.Snapshot.Header.Bytes == sizeof(BC250_WAVE_HEADER));
    }
    reset(); model.ShimMode = ShimBudget;
    escape(8);
    CHECK(response.NtStatus == STATUS_IO_TIMEOUT && response.Status == BC250_ESCAPE_STATUS_REFUSED);
    CHECK(response.Snapshot.Header.WaveCount == 1 && poolFreeCalls == 1);
    CHECK(response.Snapshot.Header.Flags == BC250_WAVE_BUDGET_EXPIRED);
}

static void test_sampling_admission(void)
{
    unsigned accesses;
    reset();
    CHECK(GfxWaveSampleBegin(&device) && device.WaveRegisterAccess == 1);
    CHECK(GfxWaveSampleBegin(&device) && device.WaveRegisterAccess == 2);
    CHECK(!GfxWaveCaptureActive(&device));
    GfxWaveSampleEnd(&device); CHECK(device.WaveRegisterAccess == 1);
    GfxWaveSampleEnd(&device); CHECK(device.WaveRegisterAccess == 0);
    device.WaveRegisterAccess = MAXLONG;
    CHECK(!GfxWaveSampleBegin(&device) && device.WaveRegisterAccess == MAXLONG);
    device.WaveRegisterAccess = -1;
    CHECK(GfxWaveCaptureActive(&device) && !GfxWaveSampleBegin(&device));
    GfxWaveSamplingReset(&device);
    CHECK(GfxWaveSampleBegin(&device)); GfxWaveSampleEnd(&device);

    /* The old reader retires before the first selector access. New readers are
     * rejected at every MMIO event while the capture owns the selectors. */
    reset(); CHECK(GfxWaveSampleBegin(&device)); model.ReleaseSamplerAt = 2;
    CHECK(capture() == STATUS_SUCCESS && model.StallCalls == 2);
    CHECK(device.WaveRegisterAccess == 0);
    reset(); CHECK(GfxWaveSampleBegin(&device));
    CHECK(capture() == STATUS_IO_TIMEOUT);
    CHECK(testSnapshot.Header.Flags & BC250_WAVE_BUDGET_EXPIRED);
    CHECK(!model.Accesses && !model.ShimCalls && device.WaveRegisterAccess == 1);
    GfxWaveSampleEnd(&device);

    /* A failed GRBM restore quarantines banked sampling until a new start.
     * A verified GRBM restore plus SQ-index failure does not poison sampling. */
    reset(); model.MismatchAt = 4;
    CHECK(capture() == STATUS_DEVICE_DATA_ERROR);
    CHECK(device.WaveRegisterAccess == -1 && !GfxWaveSampleBegin(&device));
    CHECK(strstr(model.Logs[0], "requested") != NULL && strstr(model.Logs[0], "read 1 value") != NULL);
    accesses = model.Accesses;
    model.LockCount = 0;
    CHECK(GfxCaptureWaves(&device, &testSnapshot) == STATUS_DEVICE_DATA_ERROR);
    CHECK(model.Accesses == accesses && device.WaveRegisterAccess == -1);
    CHECK(!model.Shared && !model.Mutex && testAdev.backend == &oldBackend);
    GfxWaveSamplingReset(&device);
    CHECK(GfxWaveSampleBegin(&device)); GfxWaveSampleEnd(&device);
    reset(); model.MismatchAt = 6;
    CHECK(capture() == STATUS_DEVICE_DATA_ERROR && device.WaveRegisterAccess == 0);
    CHECK(GfxWaveSampleBegin(&device)); GfxWaveSampleEnd(&device);
}

static void test_global_write_filter(void)
{
    reset(); memset(fakeBar, 0, sizeof(fakeBar));
    device.Mmio = fakeBar; device.MmioGfxEnabled = TRUE;
    CHECK(ProductionMmioGfxWrite(&device, BC250_REG_GC_SQ_IND_DATA, 7) == STATUS_ACCESS_DENIED);
    CHECK(model.HardwareWrites == 0);
    CHECK(ProductionMmioGfxWrite(&device, BC250_REG_GC_SQ_IND_INDEX, 7) == STATUS_SUCCESS);
    CHECK(model.HardwareWrites == 1 && fakeBar[BC250_REG_GC_SQ_IND_INDEX / sizeof(ULONG)] == 7);
    device.Mmio = NULL; device.MmioGfxEnabled = FALSE;
    CHECK(ProductionMmioGfxWrite(&device, BC250_REG_GC_SQ_IND_DATA, 7) == STATUS_ACCESS_DENIED);
    CHECK(model.HardwareWrites == 1);
}

int main(void)
{
    unsigned i;
    reset(); CHECK(capture() == STATUS_SUCCESS);
    CHECK(model.Accesses == 6 && model.ShimCalls == 1);
    CHECK(testSnapshot.Header.Flags == BC250_WAVE_COMPLETE && testSnapshot.Header.WaveCount == 2);
    CHECK(model.Grbm == bc250_gfx_wave_broadcast_value() && model.SqIndex == entryIndex);
    cleanup_order(2);

    /* Every access can fail, including the bank change inside the shim and each
     * cleanup write/read. Cleanup cannot be short-circuited by the sequence. */
    for (i = 1; i <= 6; i++) {
        reset(); model.FailAt = i;
        CHECK(capture() == STATUS_MOCK_FAULT);
        CHECK(testSnapshot.Header.Flags & BC250_WAVE_READ_ERROR);
        CHECK(device.WaveRegisterAccess == ((i == 3 || i == 4) ? -1 : 0));
        if (i == 1) {
            CHECK(model.ShimCalls == 0 && testSnapshot.Header.WaveCount == 0);
            CHECK(model.Accesses == 3);
            CHECK(model.Events[1].Kind == 'W' && model.Events[1].Offset == BC250_REG_GC_GRBM_GFX_INDEX);
        } else {
            cleanup_order(2);
            CHECK(testSnapshot.Header.WaveCount == (i == 2 ? 1u : 2u));
            CHECK(testSnapshot.Waves[0].Raw[Bc250WavePcLo] == 0x123400u);
        }
    }
    for (i = 4; i <= 6; i += 2) {
        reset(); model.MismatchAt = i;
        CHECK(capture() == STATUS_DEVICE_DATA_ERROR);
        CHECK(testSnapshot.Header.Flags & BC250_WAVE_READ_ERROR); cleanup_order(2);
    }
    reset(); model.PoisonAt = 1;
    CHECK(capture() == STATUS_DEVICE_DATA_ERROR && model.ShimCalls == 0);
    CHECK(model.Accesses == 3 && testSnapshot.Header.WaveCount == 0);

    reset(); model.ShimMode = ShimFault;
    CHECK(capture() == STATUS_MOCK_FAULT); cleanup_order(2);
    CHECK(model.Grbm == bc250_gfx_wave_broadcast_value() && model.SqIndex == entryIndex);
    CHECK(testSnapshot.Header.FaultOffset == BC250_REG_GC_GRBM_GFX_INDEX);
    reset(); model.ShimMode = ShimBudget;
    CHECK(capture() == STATUS_IO_TIMEOUT); cleanup_order(2);
    CHECK(testSnapshot.Header.Flags == BC250_WAVE_BUDGET_EXPIRED && testSnapshot.Header.WaveCount == 1);
    reset(); model.ShimMode = ShimTruncated;
    CHECK(capture() == STATUS_DEVICE_DATA_ERROR && testSnapshot.Header.Flags == BC250_WAVE_TRUNCATED);
    reset(); model.ShimMode = ShimPartial;
    CHECK(capture() == STATUS_DEVICE_DATA_ERROR && testSnapshot.Header.WaveCount == 1);

    /* Cold, stopped and retained-power devices do not touch the registers. */
    for (i = 0; i < 10; i++) {
        reset();
        switch (i) {
        case 0: device.Started = 0; break;
        case 1: device.Gfx = NULL; break;
        case 2: device.GpuMem = NULL; break;
        case 3: device.RetainedPowerPhase = 1; break;
        case 4: model.Enabled = 0; break;
        case 5: testGfx.SetUp = 0; break;
        case 6: testGfx.PowerSuspended = 1; break;
        case 7: testGfx.Failed = 1; break;
        case 8: testGfx.StagesDone = BC250_GFX_STAGE_INTERRUPTS - 1; break;
        case 9: model.GartResult = STATUS_MOCK_GART; break;
        }
        CHECK(capture() == (i == 9 ? STATUS_MOCK_GART : STATUS_DEVICE_NOT_READY));
        CHECK(!model.Accesses && !model.ShimCalls && testSnapshot.Header.WaveCount == 0);
    }
    reset(); model.Irql = 2;
    CHECK(capture() == STATUS_INVALID_DEVICE_STATE);
    CHECK(!model.Accesses && !model.GartCalls && !model.ShimCalls && !model.LockCount);
    reset(); model.Owner = 0;
    CHECK(capture() == STATUS_DEVICE_NOT_READY && !model.GartCalls && !model.Accesses);
    reset(); device.PhysicalDeviceObject = &oldBackend;
    CHECK(capture() == STATUS_DEVICE_NOT_READY && !model.GartCalls && !model.Accesses);

    /* Register data and SQ_CMD are never admitted as a write destination. */
    reset();
    CHECK(GfxWaveWrite(&device, BC250_REG_GC_SQ_IND_DATA, 1) == STATUS_ACCESS_DENIED);
    CHECK(GfxWaveWrite(&device, BC250_REG_GC_SQ_CMD, 1) == STATUS_ACCESS_DENIED);
    CHECK(model.Accesses == 0);
    test_sampling_admission();
    test_global_write_filter();
    test_escape();
    printf("wave access tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
