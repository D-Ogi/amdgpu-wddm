// monfence - positive control for a WDDM 2.0 monitored fence written by the GPU itself (stage A0 of
// scratch\m15\offgpu\wddm-uplift\REPORT.md section 3), on our KMD, before the ICD relies on it (stage A2).
//
// What it proves, through raw D3DKMT calls and nothing else (no ICD, no runtime, no KMD change):
//   (a) a RELEASE_MEM in our own indirect buffer (EOP, DATA_SEL 2 = 64 bits, INT_SEL 0, the KMD fence packet's cache
//       actions) writes N to the fence's FenceValueGPUVirtualAddress, and FenceValueCPUVirtualAddress reads N once
//       the IB has completed;
//   (b) a WaitForSynchronizationObjectFromCpu(N) registered before the submission wakes, with no Signal call at all;
//   (c) a second context's WaitForSynchronizationObjectFromGpu(N) holds the work queued behind it until the GPU
//       write, then releases it;
//   (d) the same from four threads at once, each with its own two contexts and fence, plus one cross-thread wait
//       (thread t's second context also waits for thread t+1's fence).
// Then, for stage A2's decision, a latency series: GPU-written fence (INT_SEL 0 and 3) against the kernel path the
// ICD uses today (SubmitCommand + SignalSynchronizationObjectFromGpu), with the CPU time of each call.
//
// Safety design (see the run report for the KMD analysis):
//   - packet control first: the same RELEASE_MEM into an allocation of our own, a mapping this project has used for
//     months, so a packet problem is never mistaken for a fence problem;
//   - read before write: the fence is CPU-signalled to a distinctive value and the GPU reads it back from
//     FenceValueGPUVirtualAddress (the E27 DMA_DATA readback). Only if the GPU sees exactly that value at that VA is
//     anything ever written there - a PTE pointing at some other physical page cannot pass this, so the write test
//     cannot scribble over foreign memory;
//   - a write that faults goes to the default page (noretry, fault-enable-default; driver/kmd/gart.c:147,
//     driver/shim/bc250_gmc.c:304-308): a clean, bounded failure, which the runner reports from `ih state`;
//   - every wait is bounded, queued GPU waits are released by a CPU signal before teardown, and a watchdog ends the
//     process (exit 4) whatever a D3DKMT call does.
//
// Runs ON THE TARGET, elevated, session 0 is fine. On the development PC: --help and --selftest only.
// Exit codes: monfence_packets.h (the first failed check wins; every check prints its line).
//
// "Ufaj, ale sprawdzaj" - trust, but verify. The fence page is dxgkrnl's; we read it before we write it.

#define main kmtprobe_original_main
#include "../kmtprobe/kmtprobe.c"
#undef main

#include <intrin.h>
#include <stdarg.h>
#include "monfence_blobs.h"
#include "monfence_packets.h"

#define MF_VERSION            1
#define MF_MAX_WORKERS        4u
#define MF_MAX_ROUNDS         16u
#define MF_MAX_ITERS          100u
#define MF_IB_BYTES           0x10000ull       // 64 KB of IB slots per worker
#define MF_RB_BYTES           0x1000ull        // one page of readback per worker
#define MF_SLOT_BYTES         64u              // one IB per 64-byte slot (8 dwords used)

// IB slots, per worker.
#define SLOT_PACKET_S         0u
#define SLOT_READ_S           1u
#define SLOT_WRITE_S          2u
#define SLOT_MARK_S           3u
#define SLOT_VIEW_S           4u
#define SLOT_NOP              5u
#define SLOT_PACKET_M         6u
#define SLOT_READ_M           7u
#define SLOT_VIEW_M           8u
#define SLOT_WRITE_M          16u              // + round
#define SLOT_MARK_M           48u              // + round
#define SLOT_LAT              80u              // + 2 * iteration + (INT_SEL 3 ? 1 : 0)

// Readback layout, per worker (byte offsets into its page).
#define RB_READ_S             0x000u
#define RB_VIEW_S             0x008u
#define RB_READ_M             0x010u
#define RB_VIEW_M             0x018u
#define RB_PACKET_S           0x100u
#define RB_PACKET_M           0x108u
#define RB_MARK_S             0x200u
#define RB_MARK_M             0x208u           // + 8 * round

// Fence value steps (mf_value). Index = worker id for the fences under test.
#define STEP_READ_S           1u
#define STEP_WRITE_S          2u
#define STEP_READ_M           3u
#define STEP_WRITE_M          4u               // + round
#define STEP_LAT              32u              // + 2 * iteration + mode
#define IDX_KERNEL_FENCE      8u               // the kernel-signalled latency fence
#define IDX_PACKET_VALUE      11u              // packet-control values (never a fence)
#define IDX_MARKER            12u              // + worker: marker values (never a fence)

C_ASSERT(SLOT_LAT + 2u * MF_MAX_ITERS <= MF_IB_BYTES / MF_SLOT_BYTES);
C_ASSERT(STEP_LAT + 2u * MF_MAX_ITERS <= MF_STEP_LIMIT);
C_ASSERT(STEP_WRITE_M + MF_MAX_ROUNDS <= STEP_LAT);
C_ASSERT(RB_MARK_M + 8u * MF_MAX_ROUNDS <= MF_RB_BYTES);
C_ASSERT(MF_SLOT_BYTES >= MF_IB_DWORDS * 4u);

typedef struct _MF_OPT {
    const char* Match;
    unsigned IntSel;
    DWORD BoundMs;          // every poll / wait for GPU work
    DWORD GateMs;           // how long a queued GPU wait must hold before the write
    DWORD StaggerMs;        // (d): thread t writes t * StaggerMs after the others are armed
    unsigned Rounds;        // (d) rounds
    unsigned Iters;         // latency iterations per mode
    unsigned Workers;       // threads in (d), 1..4
    unsigned Affinity;      // EngineAffinity for the fences under test
    DWORD WatchdogMs;
    BOOL SkipMt, SkipLatency, SelfTest;
} MF_OPT;

typedef struct _MF_FENCE {
    D3DKMT_HANDLE h;
    volatile UINT64* Cpu;
    UINT64 Gpu;
    UINT64 Expected;        // what this tool last made it (CPU signal, or a GPU write it saw complete)
    volatile LONG64 GpuWaitTop;  // highest value any context was told to wait for (WaitGpuOn); the final rescue
                                 // CPU-signals the fence up to it, which is never above a write this run queued
} MF_FENCE;

typedef struct _MF_PROGRESS {      // a kernel-path fence: SignalSynchronizationObjectFromGpu on a context
    D3DKMT_HANDLE h;
    volatile UINT64* Cpu;
    UINT64 Last;            // last value queued
    HANDLE Event;
} MF_PROGRESS;

typedef struct _MF_WORKER {
    unsigned Id;
    D3DKMT_HANDLE CtxA, CtxB;
    MF_FENCE F;
    MF_PROGRESS PA, PB;
    HANDLE WaitEvent;
    BUFFER Ib, Rb;
    UINT32* IbCpu;
    volatile UINT64* RbCpu;
    int Code;               // first failure of this worker in (d)
    char What[64];
    // (d) statistics
    UINT64 AUs[MF_MAX_ROUNDS], BUs[MF_MAX_ROUNDS], CUs[MF_MAX_ROUNDS];
    unsigned RoundsDone;
} MF_WORKER;

typedef struct _MF_BARRIER {
    volatile LONG Count;
    volatile LONG Gen;
    LONG N;
} MF_BARRIER;

static MF_OPT g_Opt;
static PROBE g_P;
static MF_WORKER g_W[MF_MAX_WORKERS];
static MF_FENCE g_K;                // kernel-signalled latency fence
static HANDLE g_KEvent;
static LARGE_INTEGER g_QpcBase, g_QpcFreq;
static CRITICAL_SECTION g_Print;
static MF_BARRIER g_Bar;
static volatile LONG g_MtAbort;

// ---- small helpers ----------------------------------------------------------------------------------------------

static UINT64 NowUs(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (UINT64)(((t.QuadPart - g_QpcBase.QuadPart) * 1000000ll) / g_QpcFreq.QuadPart);
}

static void Line(const char* Format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, Format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, Format, args);
    va_end(args);
    EnterCriticalSection(&g_Print);
    fputs(text, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    LeaveCriticalSection(&g_Print);
}

static const char* Verdict(BOOL ok) { return ok ? "PASS" : "FAIL"; }

static void FirstCode(int* code, int candidate) { if (*code == 0 && candidate != 0) *code = candidate; }

static UINT64 IbVa(const MF_WORKER* w, unsigned slot) { return w->Ib.MappedVa + (UINT64)slot * MF_SLOT_BYTES; }
static UINT32* IbCpu(const MF_WORKER* w, unsigned slot) { return w->IbCpu + slot * (MF_SLOT_BYTES / 4u); }
static UINT64 RbVa(const MF_WORKER* w, unsigned offset) { return w->Rb.MappedVa + offset; }
static UINT64 RbRead(const MF_WORKER* w, unsigned offset) { return w->RbCpu[offset / 8u]; }

static BOOL EventSet(HANDLE e) { return WaitForSingleObject(e, 0) == WAIT_OBJECT_0; }

// The value range a fence value belongs to (mf_value's fence index), or 0xFF when it is none of ours.
static unsigned ValueOwner(UINT64 v)
{
    UINT32 hi = (UINT32)(v >> 32), lo = (UINT32)v;
    unsigned idx, step;
    if (hi < 0x000BC000u || hi > 0x000BCFFFu) return 0xFFu;
    idx = (hi >> 8) & 0xFu;
    step = hi & 0xFFu;
    return mf_value(idx, step) == ((UINT64)hi << 32 | lo) ? idx : 0xFFu;
}

// ---- D3DKMT wrappers (no printing on the hot path; the caller prints the line) -------------------------------

static NTSTATUS SubmitIb(D3DKMT_HANDLE Ctx, UINT64 Va, UINT32 Bytes, UINT64 FenceVa, UINT64 FenceValue, UINT64* CallUs)
{
    struct bc250_umd_submit_private blob;
    D3DKMT_SUBMITCOMMAND submit;
    UINT64 t0;
    NTSTATUS status;

    ZeroMemory(&submit, sizeof(submit));
    submit.PrivateDriverDataSize = mf_build_submit_blob(&blob, Va, Bytes, FenceVa, FenceValue);
    submit.Commands = Va;
    submit.CommandLength = Bytes;
    submit.BroadcastContextCount = 1;
    submit.BroadcastContext[0] = Ctx;
    submit.pPrivateDriverData = &blob;
    t0 = NowUs();
    status = D3DKMTSubmitCommand(&submit);
    if (CallUs) *CallUs = NowUs() - t0;
    return status;
}

static NTSTATUS SubmitSlot(const MF_WORKER* w, D3DKMT_HANDLE Ctx, unsigned Slot, UINT64 FenceVa, UINT64 FenceValue,
                           UINT64* CallUs)
{
    return SubmitIb(Ctx, IbVa(w, Slot), MF_IB_DWORDS * 4u, FenceVa, FenceValue, CallUs);
}

static NTSTATUS SignalGpu(D3DKMT_HANDLE Ctx, D3DKMT_HANDLE Fence, UINT64 Value, UINT64* CallUs)
{
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU s;
    D3DKMT_HANDLE objects[1];
    UINT64 values[1], t0;
    NTSTATUS status;
    objects[0] = Fence;
    values[0] = Value;
    ZeroMemory(&s, sizeof(s));
    s.hContext = Ctx;
    s.ObjectCount = 1;
    s.ObjectHandleArray = objects;
    s.MonitoredFenceValueArray = values;
    t0 = NowUs();
    status = D3DKMTSignalSynchronizationObjectFromGpu(&s);
    if (CallUs) *CallUs = NowUs() - t0;
    return status;
}

static NTSTATUS WaitGpu(D3DKMT_HANDLE Ctx, D3DKMT_HANDLE Fence, UINT64 Value)
{
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wt;
    D3DKMT_HANDLE objects[1];
    UINT64 values[1];
    objects[0] = Fence;
    values[0] = Value;
    ZeroMemory(&wt, sizeof(wt));
    wt.hContext = Ctx;
    wt.ObjectCount = 1;
    wt.ObjectHandleArray = objects;
    wt.MonitoredFenceValueArray = values;
    return D3DKMTWaitForSynchronizationObjectFromGpu(&wt);
}

// WaitGpu on one of the fences under test, remembering the value so that nothing can stay queued behind it.
static NTSTATUS WaitGpuOn(D3DKMT_HANDLE Ctx, MF_FENCE* Fence, UINT64 Value)
{
    NTSTATUS status = WaitGpu(Ctx, Fence->h, Value);
    if (NT_SUCCESS(status))
    {
        LONG64 seen = Fence->GpuWaitTop;
        while ((UINT64)seen < Value)
        {
            LONG64 prior = InterlockedCompareExchange64(&Fence->GpuWaitTop, (LONG64)Value, seen);
            if (prior == seen) break;
            seen = prior;
        }
    }
    return status;
}

static NTSTATUS SignalCpu(D3DKMT_HANDLE Fence, UINT64 Value)
{
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU s;
    D3DKMT_HANDLE objects[1];
    UINT64 values[1];
    objects[0] = Fence;
    values[0] = Value;
    ZeroMemory(&s, sizeof(s));
    s.hDevice = g_P.hDevice;
    s.ObjectCount = 1;
    s.ObjectHandleArray = objects;
    s.FenceValueArray = values;
    return D3DKMTSignalSynchronizationObjectFromCpu(&s);
}

// Registers a CPU wait that sets Event when Fence >= Value. Never the NULL-event form: that one cannot be bounded.
static NTSTATUS ArmCpuWait(D3DKMT_HANDLE Fence, UINT64 Value, HANDLE Event)
{
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wt;
    D3DKMT_HANDLE objects[1];
    UINT64 values[1];
    ResetEvent(Event);
    objects[0] = Fence;
    values[0] = Value;
    ZeroMemory(&wt, sizeof(wt));
    wt.hDevice = g_P.hDevice;
    wt.ObjectCount = 1;
    wt.ObjectHandleArray = objects;
    wt.FenceValueArray = values;
    wt.hAsyncEvent = Event;
    return D3DKMTWaitForSynchronizationObjectFromCpu(&wt);
}

// Kernel-path progress: queue SignalSynchronizationObjectFromGpu(next) on Ctx behind everything submitted there.
static NTSTATUS QueueProgress(D3DKMT_HANDLE Ctx, MF_PROGRESS* p)
{
    NTSTATUS status = SignalGpu(Ctx, p->h, p->Last + 1, NULL);
    if (NT_SUCCESS(status)) p->Last++;
    return status;
}

// Bounded wait for p->Last. 1 reached, 0 timed out, -1 a call failed. The value is the judge, not the event: a
// registration left over from an earlier timed-out wait (lower value, same event) may set the event early, and
// then the wait is simply registered again.
static int AwaitProgress(MF_PROGRESS* p, DWORD TimeoutMs, UINT64* WaitedUs)
{
    UINT64 t0 = NowUs();
    ULONGLONG deadline = GetTickCount64() + TimeoutMs;
    int result;
    for (;;)
    {
        ULONGLONG now;
        DWORD r;
        if (*p->Cpu >= p->Last) { result = 1; break; }
        if (!NT_SUCCESS(ArmCpuWait(p->h, p->Last, p->Event))) { result = -1; break; }
        now = GetTickCount64();
        r = now >= deadline ? WAIT_TIMEOUT : WaitForSingleObject(p->Event, (DWORD)(deadline - now));
        if (r == WAIT_FAILED) { result = -1; break; }
        if (r == WAIT_TIMEOUT) { result = *p->Cpu >= p->Last ? 1 : 0; break; }
    }
    if (WaitedUs) *WaitedUs = NowUs() - t0;
    return result;
}

// Submit a slot on Ctx, queue progress behind it and wait. 1 done, 0 timeout, -1 call failure (status in *Status).
static int RunSlot(MF_WORKER* w, D3DKMT_HANDLE Ctx, MF_PROGRESS* p, unsigned Slot, UINT64* SubmitUs, UINT64* DoneUs,
                   NTSTATUS* Status)
{
    UINT64 t0 = NowUs();
    NTSTATUS status = SubmitSlot(w, Ctx, Slot, 0, 0, SubmitUs);
    int r;
    if (Status) *Status = status;
    if (!NT_SUCCESS(status)) return -1;
    status = QueueProgress(Ctx, p);
    if (Status) *Status = status;
    if (!NT_SUCCESS(status)) return -1;
    r = AwaitProgress(p, g_Opt.BoundMs, NULL);
    if (DoneUs) *DoneUs = NowUs() - t0;
    return r;
}

// ---- setup ------------------------------------------------------------------------------------------------------

// BC2A version 1 (monfence_blobs.h), mapped and made resident with kmtprobe's own steps.
static BOOL CreateUmdBuffer(PROBE* p, BUFFER* b, UINT heap)
{
    struct bc250_umd_alloc_private blob;
    D3DDDI_ALLOCATIONINFO2 info;
    D3DKMT_CREATEALLOCATION create;
    ZeroMemory(&info, sizeof(info));
    ZeroMemory(&create, sizeof(create));
    mf_build_alloc_blob(&blob, b->Size, heap);
    info.pPrivateDriverData = &blob;
    info.PrivateDriverDataSize = sizeof(blob);
    create.hDevice = p->hDevice;
    create.NumAllocations = 1;
    create.pAllocationInfo2 = &info;
    create.Flags.NonSecure = 1;
    if (!NT_SUCCESS(ReportOn("CreateAllocation2 BC2A", b, D3DKMTCreateAllocation2(&create)))) return FALSE;
    b->hAllocation = info.hAllocation;
    return StepMapVa(p, b) && StepMakeResident(p, b);
}

// BC2C version 2, node 0, as the E27 probe and the ICD create theirs. The KMD refuses a version-1 blob
// (driver/kmd/umd_blob.c:141) and anything not GFX on node 0.
static BOOL CreateContext(PROBE* p, D3DKMT_HANDLE* Out, const char* Name)
{
    struct bc250_umd_context_private blob;
    D3DKMT_CREATECONTEXTVIRTUAL c;
    char label[64];
    ZeroMemory(&c, sizeof(c));
    mf_build_context_blob(&blob);
    c.hDevice = p->hDevice;
    c.NodeOrdinal = 0;
    c.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
    c.pPrivateDriverData = &blob;
    c.PrivateDriverDataSize = sizeof(blob);
    sprintf_s(label, sizeof(label), "CreateContextVirtual BC2C [%s]", Name);
    if (!NT_SUCCESS(Report(label, D3DKMTCreateContextVirtual(&c)))) return FALSE;
    *Out = c.hContext;
    return TRUE;
}

static BOOL CreateFence(PROBE* p, unsigned Affinity, D3DKMT_HANDLE* h, volatile UINT64** Cpu, UINT64* Gpu,
                        const char* Name)
{
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 c;
    char label[64];
    ZeroMemory(&c, sizeof(c));
    c.hDevice = p->hDevice;
    c.Info.Type = D3DDDI_MONITORED_FENCE;
    c.Info.MonitoredFence.InitialFenceValue = 0;
    c.Info.MonitoredFence.EngineAffinity = Affinity;
    sprintf_s(label, sizeof(label), "CreateSynchronizationObject2 [%s]", Name);
    if (!NT_SUCCESS(Report(label, D3DKMTCreateSynchronizationObject2(&c)))) return FALSE;
    *h = c.hSyncObject;
    *Cpu = (volatile UINT64*)c.Info.MonitoredFence.FenceValueCPUVirtualAddress;
    if (Gpu) *Gpu = c.Info.MonitoredFence.FenceValueGPUVirtualAddress;
    return *Cpu != NULL;
}

static void DestroySync(D3DKMT_HANDLE* h, const char* Name)
{
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT d;
    char label[64];
    if (*h == 0) return;
    ZeroMemory(&d, sizeof(d));
    d.hSyncObject = *h;
    sprintf_s(label, sizeof(label), "DestroySynchronizationObject [%s]", Name);
    (void)Report(label, D3DKMTDestroySynchronizationObject(&d));
    *h = 0;
}

static void DestroyCtx(D3DKMT_HANDLE* h, const char* Name)
{
    D3DKMT_DESTROYCONTEXT d;
    char label[64];
    if (*h == 0) return;
    ZeroMemory(&d, sizeof(d));
    d.hContext = *h;
    sprintf_s(label, sizeof(label), "DestroyContext [%s]", Name);
    (void)Report(label, D3DKMTDestroyContext(&d));
    *h = 0;
}

static BOOL SetupWorker(MF_WORKER* w, unsigned Id)
{
    static const char* ibNames[MF_MAX_WORKERS] = { "ib0", "ib1", "ib2", "ib3" };
    static const char* rbNames[MF_MAX_WORKERS] = { "rb0", "rb1", "rb2", "rb3" };
    char name[32];
    w->Id = Id;
    w->Ib.Name = ibNames[Id];
    w->Ib.Size = MF_IB_BYTES;
    w->Rb.Name = rbNames[Id];
    w->Rb.Size = MF_RB_BYTES;
    if (!CreateUmdBuffer(&g_P, &w->Ib, AMDGPU_GEM_DOMAIN_GTT) || !CreateUmdBuffer(&g_P, &w->Rb, AMDGPU_GEM_DOMAIN_GTT))
        return FALSE;
    // Locked for the whole run, the way the ICD keeps its buffers mapped. Every IB is written before the first
    // submission and never touched again; the readback page is only read after the GPU work that wrote it completed.
    if (!LockBuffer(&g_P, &w->Ib) || !LockBuffer(&g_P, &w->Rb)) return FALSE;
    w->IbCpu = (UINT32*)w->Ib.Locked;
    w->RbCpu = (volatile UINT64*)w->Rb.Locked;
    sprintf_s(name, sizeof(name), "w%u.A", Id);
    if (!CreateContext(&g_P, &w->CtxA, name)) return FALSE;
    sprintf_s(name, sizeof(name), "w%u.B", Id);
    if (!CreateContext(&g_P, &w->CtxB, name)) return FALSE;
    sprintf_s(name, sizeof(name), "w%u.F", Id);
    if (!CreateFence(&g_P, g_Opt.Affinity, &w->F.h, &w->F.Cpu, &w->F.Gpu, name)) return FALSE;
    sprintf_s(name, sizeof(name), "w%u.PA", Id);
    if (!CreateFence(&g_P, 1, &w->PA.h, &w->PA.Cpu, NULL, name)) return FALSE;
    sprintf_s(name, sizeof(name), "w%u.PB", Id);
    if (!CreateFence(&g_P, 1, &w->PB.h, &w->PB.Cpu, NULL, name)) return FALSE;
    w->WaitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->PA.Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->PB.Event = CreateEventW(NULL, TRUE, FALSE, NULL);
    w->F.Expected = 0;
    return w->WaitEvent != NULL && w->PA.Event != NULL && w->PB.Event != NULL;
}

// Every IB of the run, written once, before anything is submitted.
static BOOL WriteIbs(MF_WORKER* w)
{
    unsigned r, i, ok = 1, sel = g_Opt.IntSel;
    UINT64 f = w->F.Gpu;
    unsigned id = w->Id;
    ok &= mf_emit_release_mem(IbCpu(w, SLOT_PACKET_S), RbVa(w, RB_PACKET_S), mf_value(IDX_PACKET_VALUE, 1 + id),
                              MF_DATA_SEL_64, sel) == 8;
    ok &= mf_emit_dma_copy(IbCpu(w, SLOT_READ_S), f, RbVa(w, RB_READ_S), 8) == 8;
    ok &= mf_emit_release_mem(IbCpu(w, SLOT_WRITE_S), f, mf_value(id, STEP_WRITE_S), MF_DATA_SEL_64, sel) == 8;
    ok &= mf_emit_release_mem(IbCpu(w, SLOT_MARK_S), RbVa(w, RB_MARK_S), mf_value(IDX_MARKER + id, 1),
                              MF_DATA_SEL_64, sel) == 8;
    ok &= mf_emit_dma_copy(IbCpu(w, SLOT_VIEW_S), f, RbVa(w, RB_VIEW_S), 8) == 8;
    ok &= mf_emit_nop(IbCpu(w, SLOT_NOP)) == 8;
    ok &= mf_emit_release_mem(IbCpu(w, SLOT_PACKET_M), RbVa(w, RB_PACKET_M), mf_value(IDX_PACKET_VALUE, 16 + id),
                              MF_DATA_SEL_64, sel) == 8;
    ok &= mf_emit_dma_copy(IbCpu(w, SLOT_READ_M), f, RbVa(w, RB_READ_M), 8) == 8;
    ok &= mf_emit_dma_copy(IbCpu(w, SLOT_VIEW_M), f, RbVa(w, RB_VIEW_M), 8) == 8;
    for (r = 0; r < MF_MAX_ROUNDS; r++)
    {
        ok &= mf_emit_release_mem(IbCpu(w, SLOT_WRITE_M + r), f, mf_value(id, STEP_WRITE_M + r), MF_DATA_SEL_64, sel) == 8;
        ok &= mf_emit_release_mem(IbCpu(w, SLOT_MARK_M + r), RbVa(w, RB_MARK_M + 8u * r), mf_value(IDX_MARKER + id, 2 + r),
                                  MF_DATA_SEL_64, sel) == 8;
    }
    if (id == 0)
        for (i = 0; i < MF_MAX_ITERS; i++)
        {
            ok &= mf_emit_release_mem(IbCpu(w, SLOT_LAT + 2u * i), f, mf_value(0, STEP_LAT + 2u * i),
                                      MF_DATA_SEL_64, MF_INT_SEL_NONE) == 8;
            ok &= mf_emit_release_mem(IbCpu(w, SLOT_LAT + 2u * i + 1u), f, mf_value(0, STEP_LAT + 2u * i + 1u),
                                      MF_DATA_SEL_64, MF_INT_SEL_DATA_CONFIRM) == 8;
        }
    for (i = 0; i < MF_RB_BYTES / 8u; i++) w->RbCpu[i] = 0;
    // The mappings are write-combined: drain the WC buffers before any submission can fetch these dwords.
    _mm_sfence();
    MemoryBarrier();
    return ok != 0;
}

// ---- (a) (b) (c), one worker, single thread ----------------------------------------------------------------------

typedef struct _MF_POLL {
    BOOL SeenA, SeenB, SeenC, SeenPB, Order, Unexpected;
    UINT64 TA, TB, TC, TPB;
    UINT64 BadValue, NeighbourAtC;
} MF_POLL;

// Poll the fence, the CPU-wait event, the marker and the second context's progress until all four are seen or the
// bound passes. The marker is read before the fences: once it is visible, a fence read that still shows the old value
// proves the work behind the GPU wait ran first (x86 does not reorder loads with loads).
static void Poll(MF_WORKER* w, UINT64 T0, UINT64 Prev, UINT64 N, unsigned MarkOffset, UINT64 Marker,
                 const MF_FENCE* Neighbour, UINT64 NeighbourN, MF_POLL* out)
{
    UINT64 deadline = T0 + (UINT64)g_Opt.BoundMs * 1000ull;
    unsigned spins = 0;
    ZeroMemory(out, sizeof(*out));
    out->Order = TRUE;
    for (;;)
    {
        UINT64 now = NowUs();
        UINT64 mk = RbRead(w, MarkOffset);
        UINT64 f = *w->F.Cpu;
        UINT64 nb = Neighbour ? *Neighbour->Cpu : 0;
        if (f != Prev && f != N && !out->Unexpected) { out->Unexpected = TRUE; out->BadValue = f; }
        if (!out->SeenA && f == N) { out->SeenA = TRUE; out->TA = now - T0; }
        if (!out->SeenC && mk == Marker)
        {
            out->SeenC = TRUE;
            out->TC = now - T0;
            if (f < N || (Neighbour && nb < NeighbourN)) { out->Order = FALSE; out->NeighbourAtC = nb; }
        }
        if (!out->SeenB && EventSet(w->WaitEvent)) { out->SeenB = TRUE; out->TB = now - T0; }
        if (!out->SeenPB && EventSet(w->PB.Event)) { out->SeenPB = TRUE; out->TPB = now - T0; }
        if (out->SeenA && out->SeenB && out->SeenC && out->SeenPB) return;
        if (now > deadline) return;
        if ((++spins & 63u) == 0) SwitchToThread(); else YieldProcessor();
    }
}

// Everything that could still hold a queued GPU wait: CPU-signal the fence up to Value (dxgkrnl's own path,
// context-monitoring.md:49-51) and wait for the second context's queued progress. TRUE when drained.
static BOOL Rescue(MF_WORKER* w, UINT64 Value, const char* Phase)
{
    BOOL signalled = FALSE;
    int r;
    if (*w->F.Cpu < Value)
    {
        NTSTATUS st = SignalCpu(w->F.h, Value);
        signalled = TRUE;
        Line("RESCUE %s w%u fence 0x%016llX < 0x%016llX: CPU signal 0x%08lX", Phase, w->Id, *w->F.Cpu, Value,
             (unsigned long)st);
    }
    if (w->PB.Last == 0 || *w->PB.Cpu >= w->PB.Last) return TRUE;
    r = AwaitProgress(&w->PB, g_Opt.BoundMs, NULL);
    if (r != 1 || signalled)
        Line("RESCUE %s w%u second context progress %llu/%llu -> %s", Phase, w->Id, *w->PB.Cpu, w->PB.Last,
             r == 1 ? "drained" : "STILL QUEUED");
    return r == 1;
}

static int RunCore(MF_WORKER* w)
{
    UINT64 X = mf_value(IDX_PACKET_VALUE, 1 + w->Id);
    UINT64 V0 = mf_value(w->Id, STEP_READ_S);
    UINT64 N = mf_value(w->Id, STEP_WRITE_S);
    UINT64 M = mf_value(IDX_MARKER + w->Id, 1);
    UINT64 submitUs = 0, doneUs = 0, got, cpuNow, aFinal, view, t0, pagingBefore, pagingAfter, dmaUs = 0;
    NTSTATUS st = STATUS_SUCCESS;
    MF_POLL poll;
    BOOL gateHeld, earlyB, late = FALSE, aOk, bOk, cOrder, cRelease, unexpected, neighboursOk = TRUE;
    int r, code = 0;
    unsigned j;

    // S.packet: the packet under test, into memory of our own.
    r = RunSlot(w, w->CtxA, &w->PA, SLOT_PACKET_S, &submitUs, &doneUs, &st);
    got = RbRead(w, RB_PACKET_S);
    Line("CHECK S.packet %s ctx=A target=0x%016llX expect=0x%016llX got=0x%016llX status=0x%08lX done=%s submit_us=%llu done_us=%llu",
         Verdict(r == 1 && got == X), RbVa(w, RB_PACKET_S), X, got, (unsigned long)st,
         r == 1 ? "yes" : r == 0 ? "timeout" : "call-failed", submitUs, doneUs);
    if (r != 1 || got != X) return MF_EXIT_PACKET_CONTROL;

    // S.read: CPU-signal a value nobody else could produce, read it back with the GPU from the fence's GPU VA.
    st = SignalCpu(w->F.h, V0);
    cpuNow = *w->F.Cpu;
    if (!NT_SUCCESS(st) || cpuNow != V0)
    {
        Line("CHECK S.read FAIL cpu-signal status=0x%08lX cpu_value=0x%016llX expect=0x%016llX", (unsigned long)st,
             cpuNow, V0);
        return MF_EXIT_READ_CONTROL;
    }
    w->F.Expected = V0;
    r = RunSlot(w, w->CtxA, &w->PA, SLOT_READ_S, &submitUs, &doneUs, &st);
    got = RbRead(w, RB_READ_S);
    Line("CHECK S.read %s fence_gpu_va=0x%016llX expect=0x%016llX gpu_read=0x%016llX status=0x%08lX done=%s done_us=%llu",
         Verdict(r == 1 && got == V0), w->F.Gpu, V0, got, (unsigned long)st,
         r == 1 ? "yes" : r == 0 ? "timeout" : "call-failed", doneUs);
    if (r != 1 || got != V0)
    {
        Line("NOTE the GPU does not see the CPU-signalled value at FenceValueGPUVirtualAddress: no write is attempted "
             "(unmapped, wrong page, or a non-snooped mapping of a CPU-cached page; see ih state for a fault)");
        return MF_EXIT_READ_CONTROL;
    }

    // Arm (b) and (c) before the write, so that both are real pending waits dxgkrnl has to resolve.
    st = ArmCpuWait(w->F.h, N, w->WaitEvent);
    if (!NT_SUCCESS(st)) { Line("CHECK S.b FAIL arm status=0x%08lX", (unsigned long)st); return MF_EXIT_B_WAKE; }
    st = WaitGpuOn(w->CtxB, &w->F, N);
    if (!NT_SUCCESS(st)) { Line("CHECK S.c FAIL gpu-wait status=0x%08lX", (unsigned long)st); return MF_EXIT_C_RELEASE; }
    st = SubmitSlot(w, w->CtxB, SLOT_MARK_S, 0, 0, NULL);
    if (NT_SUCCESS(st)) st = QueueProgress(w->CtxB, &w->PB);
    if (NT_SUCCESS(st)) st = ArmCpuWait(w->PB.h, w->PB.Last, w->PB.Event);
    if (!NT_SUCCESS(st))
    {
        Line("CHECK S.c FAIL queue-behind-wait status=0x%08lX", (unsigned long)st);
        (void)Rescue(w, N, "S");
        return MF_EXIT_C_RELEASE;
    }
    Sleep(g_Opt.GateMs);
    gateHeld = RbRead(w, RB_MARK_S) != M && !EventSet(w->PB.Event) && *w->PB.Cpu < w->PB.Last;
    earlyB = EventSet(w->WaitEvent);
    Line("CHECK S.c.gate %s held_ms=%lu marker=0x%016llX second_ctx_progress=%llu/%llu fence=0x%016llX",
         Verdict(gateHeld), g_Opt.GateMs, RbRead(w, RB_MARK_S), *w->PB.Cpu, w->PB.Last, *w->F.Cpu);
    if (!gateHeld) FirstCode(&code, MF_EXIT_C_ORDER);
    if (earlyB) Line("CHECK S.b.early FAIL the CPU wait for N fired before N was written");

    // (a): the write.
    pagingBefore = g_P.PagingFence ? *g_P.PagingFence : 0;
    t0 = NowUs();
    st = SubmitSlot(w, w->CtxA, SLOT_WRITE_S, w->F.Gpu, N, &submitUs);
    if (!NT_SUCCESS(st))
    {
        Line("CHECK S.a FAIL submit status=0x%08lX", (unsigned long)st);
        (void)Rescue(w, N, "S");
        return MF_EXIT_A_VALUE;
    }
    Poll(w, t0, V0, N, RB_MARK_S, M, NULL, 0, &poll);
    // The IB's completion, by the kernel path, queued only now so that it cannot have caused (b) or (c).
    st = QueueProgress(w->CtxA, &w->PA);
    r = NT_SUCCESS(st) ? AwaitProgress(&w->PA, g_Opt.BoundMs, &dmaUs) : -1;
    aFinal = *w->F.Cpu;
    if (!poll.SeenB) late = WaitForSingleObject(w->WaitEvent, 300) == WAIT_OBJECT_0;
    // The GPU's own view of the fence word after the write: tells "landed but the CPU sees a stale line" from
    // "never landed".
    (void)RunSlot(w, w->CtxA, &w->PA, SLOT_VIEW_S, NULL, NULL, NULL);
    view = RbRead(w, RB_VIEW_S);
    pagingAfter = g_P.PagingFence ? *g_P.PagingFence : 0;
    for (j = 0; j < g_Opt.Workers; j++)
        if (j != w->Id && *g_W[j].F.Cpu != g_W[j].F.Expected) neighboursOk = FALSE;
    if (g_K.Cpu && *g_K.Cpu != g_K.Expected) neighboursOk = FALSE;
    if (pagingAfter < pagingBefore || ValueOwner(pagingAfter) != 0xFFu) neighboursOk = FALSE;

    aOk = poll.SeenA && r == 1 && aFinal == N;
    bOk = poll.SeenB && !earlyB;
    cOrder = poll.Order && gateHeld;
    cRelease = poll.SeenC && poll.SeenPB;
    unexpected = poll.Unexpected || !neighboursOk;
    Line("CHECK S.a %s n=0x%016llX cpu_after_dma=0x%016llX cpu_seen_us=%lld dma_done=%s gpu_view=0x%016llX submit_us=%llu%s",
         Verdict(aOk), N, aFinal, poll.SeenA ? (long long)poll.TA : -1ll, r == 1 ? "yes" : "no", view, submitUs,
         aOk ? "" : (view == N ? " diagnosis=landed-cpu-stale" : view == V0 ? " diagnosis=not-landed" : " diagnosis=other"));
    Line("CHECK S.b %s wake_us=%lld late_wake_after_next_completion=%s early=%s",
         Verdict(bOk), poll.SeenB ? (long long)poll.TB : -1ll, poll.SeenB ? "n/a" : (late ? "yes" : "no"),
         earlyB ? "yes" : "no");
    Line("CHECK S.c %s gate=%s order=%s released_marker_us=%lld second_ctx_progress_us=%lld marker=0x%016llX",
         Verdict(cOrder && cRelease), gateHeld ? "held" : "BROKEN", poll.Order ? "ok" : "VIOLATED",
         poll.SeenC ? (long long)poll.TC : -1ll, poll.SeenPB ? (long long)poll.TPB : -1ll, RbRead(w, RB_MARK_S));
    Line("CHECK S.unexpected %s torn_or_foreign=0x%016llX neighbours=%s paging_fence=%llu->%llu",
         Verdict(!unexpected), poll.Unexpected ? poll.BadValue : 0ull, neighboursOk ? "intact" : "CHANGED",
         pagingBefore, pagingAfter);
    if (!aOk) FirstCode(&code, MF_EXIT_A_VALUE);
    if (!bOk) FirstCode(&code, MF_EXIT_B_WAKE);
    if (!cOrder) FirstCode(&code, MF_EXIT_C_ORDER);
    if (!cRelease) FirstCode(&code, MF_EXIT_C_RELEASE);
    if (unexpected) FirstCode(&code, MF_EXIT_UNEXPECTED_VALUE);
    if (!Rescue(w, N, "S")) FirstCode(&code, MF_EXIT_RESCUE);
    w->F.Expected = *w->F.Cpu;
    return code;
}

// ---- (d) four threads ------------------------------------------------------------------------------------------

static BOOL BarrierWait(MF_BARRIER* b, DWORD TimeoutMs)
{
    LONG gen = InterlockedCompareExchange(&b->Gen, 0, 0);
    ULONGLONG deadline;
    if (InterlockedIncrement(&b->Count) == b->N)
    {
        InterlockedExchange(&b->Count, 0);
        InterlockedIncrement(&b->Gen);
        return g_MtAbort == 0;
    }
    deadline = GetTickCount64() + TimeoutMs;
    while (InterlockedCompareExchange(&b->Gen, 0, 0) == gen)
    {
        if (g_MtAbort) return FALSE;
        if (GetTickCount64() > deadline) { InterlockedExchange(&g_MtAbort, 1); return FALSE; }
        Sleep(1);
    }
    return g_MtAbort == 0;
}

static void MtFail(MF_WORKER* w, int Code, const char* What)
{
    if (w->Code == 0) { w->Code = Code; strncpy_s(w->What, sizeof(w->What), What, _TRUNCATE); }
}

static DWORD WINAPI MtThread(LPVOID Parameter)
{
    MF_WORKER* w = (MF_WORKER*)Parameter;
    MF_WORKER* nbw = &g_W[(w->Id + 1u) % g_Opt.Workers];
    DWORD barrierMs = g_Opt.BoundMs * 2u + g_Opt.GateMs + g_Opt.StaggerMs * g_Opt.Workers + 5000u;
    UINT64 X = mf_value(IDX_PACKET_VALUE, 16 + w->Id), V0 = mf_value(w->Id, STEP_READ_M), got, doneUs = 0;
    NTSTATUS st = STATUS_SUCCESS;
    unsigned rd;
    int r;

    // Controls, concurrently in all threads.
    r = RunSlot(w, w->CtxA, &w->PA, SLOT_PACKET_M, NULL, &doneUs, &st);
    got = RbRead(w, RB_PACKET_M);
    Line("CHECK M.w%u.packet %s expect=0x%016llX got=0x%016llX status=0x%08lX done_us=%llu", w->Id,
         Verdict(r == 1 && got == X), X, got, (unsigned long)st, doneUs);
    if (r != 1 || got != X) MtFail(w, MF_EXIT_MT_CONTROL, "packet");
    if (w->Code == 0)
    {
        st = SignalCpu(w->F.h, V0);
        if (NT_SUCCESS(st) && *w->F.Cpu == V0) w->F.Expected = V0;
        r = NT_SUCCESS(st) ? RunSlot(w, w->CtxA, &w->PA, SLOT_READ_M, NULL, &doneUs, &st) : -1;
        got = RbRead(w, RB_READ_M);
        Line("CHECK M.w%u.read %s fence_gpu_va=0x%016llX expect=0x%016llX gpu_read=0x%016llX status=0x%08lX done_us=%llu",
             w->Id, Verdict(r == 1 && got == V0), w->F.Gpu, V0, got, (unsigned long)st, doneUs);
        if (r != 1 || got != V0) MtFail(w, MF_EXIT_MT_CONTROL, "read");
    }
    if (w->Code != 0) InterlockedExchange(&g_MtAbort, 1);
    if (!BarrierWait(&g_Bar, barrierMs)) { MtFail(w, MF_EXIT_MT_SYNC, "barrier-controls"); return 0; }

    for (rd = 0; rd < g_Opt.Rounds && !g_MtAbort; rd++)
    {
        UINT64 prev = w->F.Expected;
        UINT64 N = mf_value(w->Id, STEP_WRITE_M + rd);
        UINT64 nbN = mf_value(nbw->Id, STEP_WRITE_M + rd);
        UINT64 M = mf_value(IDX_MARKER + w->Id, 2 + rd);
        unsigned markOff = RB_MARK_M + 8u * rd;
        UINT64 t0, submitUs = 0;
        MF_POLL poll;
        BOOL gateHeld, earlyB, armed = TRUE;
        char what[48];
        unsigned j;

        st = ArmCpuWait(w->F.h, N, w->WaitEvent);
        if (NT_SUCCESS(st)) st = WaitGpuOn(w->CtxB, &w->F, N);
        if (NT_SUCCESS(st)) st = WaitGpuOn(w->CtxB, &nbw->F, nbN);      // the cross-thread wait
        if (NT_SUCCESS(st)) st = SubmitSlot(w, w->CtxB, SLOT_MARK_M + rd, 0, 0, NULL);
        if (NT_SUCCESS(st)) st = QueueProgress(w->CtxB, &w->PB);
        if (NT_SUCCESS(st)) st = ArmCpuWait(w->PB.h, w->PB.Last, w->PB.Event);
        if (!NT_SUCCESS(st)) { armed = FALSE; MtFail(w, MF_EXIT_MT_C_RELEASE, "arm"); InterlockedExchange(&g_MtAbort, 1); }
        if (!BarrierWait(&g_Bar, barrierMs)) { MtFail(w, MF_EXIT_MT_SYNC, "barrier-armed"); break; }

        Sleep(g_Opt.GateMs);
        gateHeld = RbRead(w, markOff) != M && !EventSet(w->PB.Event) && *w->PB.Cpu < w->PB.Last;
        earlyB = EventSet(w->WaitEvent);
        if (!BarrierWait(&g_Bar, barrierMs)) { MtFail(w, MF_EXIT_MT_SYNC, "barrier-gate"); break; }

        Sleep(g_Opt.StaggerMs * w->Id);
        t0 = NowUs();
        st = armed ? SubmitSlot(w, w->CtxA, SLOT_WRITE_M + rd, w->F.Gpu, N, &submitUs) : STATUS_UNSUCCESSFUL;
        if (NT_SUCCESS(st)) Poll(w, t0, prev, N, markOff, M, &nbw->F, nbN, &poll);
        else { ZeroMemory(&poll, sizeof(poll)); poll.Order = TRUE; }
        if (!BarrierWait(&g_Bar, barrierMs)) { MtFail(w, MF_EXIT_MT_SYNC, "barrier-polled"); break; }

        Line("CHECK M.w%u.r%u.a %s n=0x%016llX cpu_seen_us=%lld submit_us=%llu status=0x%08lX", w->Id, rd,
             Verdict(poll.SeenA), N, poll.SeenA ? (long long)poll.TA : -1ll, submitUs, (unsigned long)st);
        Line("CHECK M.w%u.r%u.b %s wake_us=%lld early=%s", w->Id, rd, Verdict(poll.SeenB && !earlyB),
             poll.SeenB ? (long long)poll.TB : -1ll, earlyB ? "yes" : "no");
        Line("CHECK M.w%u.r%u.c %s cross=w%u gate=%s order=%s marker_us=%lld second_ctx_progress_us=%lld neighbour_at_marker=0x%016llX",
             w->Id, rd, Verdict(gateHeld && poll.Order && poll.SeenC && poll.SeenPB), nbw->Id,
             gateHeld ? "held" : "BROKEN", poll.Order ? "ok" : "VIOLATED", poll.SeenC ? (long long)poll.TC : -1ll,
             poll.SeenPB ? (long long)poll.TPB : -1ll, poll.NeighbourAtC);
        sprintf_s(what, sizeof(what), "r%u", rd);
        if (!poll.SeenA) MtFail(w, MF_EXIT_MT_A, what);
        if (!poll.SeenB || earlyB) MtFail(w, MF_EXIT_MT_B, what);
        if (!gateHeld || !poll.Order) MtFail(w, MF_EXIT_MT_C_ORDER, what);
        if (!poll.SeenC || !poll.SeenPB) MtFail(w, MF_EXIT_MT_C_RELEASE, what);
        if (poll.Unexpected)
        {
            Line("CHECK M.w%u.r%u.unexpected FAIL value=0x%016llX", w->Id, rd, poll.BadValue);
            MtFail(w, MF_EXIT_MT_UNEXPECTED, what);
        }
        if (poll.SeenA) { w->AUs[rd] = poll.TA; w->BUs[rd] = poll.TB; w->CUs[rd] = poll.TC; }

        // Release whatever is still queued: own fence first, then wait until every thread did the same, because the
        // previous thread's second context waits for this fence too.
        if (*w->F.Cpu < N) { st = SignalCpu(w->F.h, N); Line("RESCUE M w%u r%u CPU signal 0x%08lX", w->Id, rd, (unsigned long)st); }
        if (!BarrierWait(&g_Bar, barrierMs)) { MtFail(w, MF_EXIT_MT_SYNC, "barrier-rescue"); break; }
        if (w->PB.Last && *w->PB.Cpu < w->PB.Last && AwaitProgress(&w->PB, g_Opt.BoundMs, NULL) != 1)
            MtFail(w, MF_EXIT_MT_C_RELEASE, "drain");
        w->F.Expected = *w->F.Cpu;
        // Every fence on the page still holds a value of its own range.
        for (j = 0; j < g_Opt.Workers; j++)
        {
            UINT64 v = *g_W[j].F.Cpu;
            if (ValueOwner(v) != g_W[j].Id)
            {
                Line("CHECK M.w%u.r%u.foreign FAIL fence w%u holds 0x%016llX", w->Id, rd, j, v);
                MtFail(w, MF_EXIT_MT_UNEXPECTED, what);
            }
        }
        w->RoundsDone = rd + 1;
        if (!BarrierWait(&g_Bar, barrierMs)) { MtFail(w, MF_EXIT_MT_SYNC, "barrier-round"); break; }
    }
    return 0;
}

static void PrintStats(const char* Label, UINT64* v, unsigned n)
{
    unsigned i, j;
    if (n == 0) { Line("%s n=0", Label); return; }
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) { UINT64 t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    Line("%s n=%u min=%llu med=%llu p90=%llu max=%llu", Label, n, v[0], v[n / 2], v[(n * 9u) / 10u], v[n - 1]);
}

static int RunMt(void)
{
    HANDLE threads[MF_MAX_WORKERS] = { 0 };
    DWORD joinMs = (g_Opt.BoundMs * 3u + g_Opt.GateMs + g_Opt.StaggerMs * g_Opt.Workers + 1000u) * (g_Opt.Rounds + 1u) + 10000u;
    UINT64 a[MF_MAX_WORKERS * MF_MAX_ROUNDS], b[MF_MAX_WORKERS * MF_MAX_ROUNDS], c[MF_MAX_WORKERS * MF_MAX_ROUNDS];
    unsigned i, rd, n = 0;
    int code = 0;
    DWORD wr;

    g_Bar.Count = 0;
    g_Bar.Gen = 0;
    g_Bar.N = (LONG)g_Opt.Workers;
    g_MtAbort = 0;
    Line("PHASE M threads=%u rounds=%u gate_ms=%lu stagger_ms=%lu bound_ms=%lu (thread t's second context also waits for thread t+1's fence)",
         g_Opt.Workers, g_Opt.Rounds, g_Opt.GateMs, g_Opt.StaggerMs, g_Opt.BoundMs);
    for (i = 0; i < g_Opt.Workers; i++)
    {
        threads[i] = CreateThread(NULL, 0, MtThread, &g_W[i], 0, NULL);
        if (threads[i] == NULL) { InterlockedExchange(&g_MtAbort, 1); code = MF_EXIT_MT_SYNC; g_Opt.Workers = i; break; }
    }
    wr = g_Opt.Workers ? WaitForMultipleObjects(g_Opt.Workers, threads, TRUE, joinMs) : WAIT_OBJECT_0;
    if (wr == WAIT_TIMEOUT)
    {
        InterlockedExchange(&g_MtAbort, 1);
        Line("PHASE M threads did not finish in %lu ms", joinMs);
        if (WaitForMultipleObjects(g_Opt.Workers, threads, TRUE, 5000) == WAIT_TIMEOUT)
        {
            Line("RESULT FAIL exit=%d first=M.join (threads stuck; process exit cleans up)", MF_EXIT_MT_SYNC);
            SetEvent(g_Done);
            ExitProcess(MF_EXIT_MT_SYNC);
        }
        FirstCode(&code, MF_EXIT_MT_SYNC);
    }
    for (i = 0; i < g_Opt.Workers; i++) if (threads[i]) CloseHandle(threads[i]);
    for (i = 0; i < g_Opt.Workers; i++)
        if (g_W[i].Code) Line("PHASE M w%u first failure %d at %s", i, g_W[i].Code, g_W[i].What);
    // A barrier timeout in one thread is usually the echo of a real failure in another: report the real one.
    for (i = 0; i < g_Opt.Workers; i++)
        if (g_W[i].Code && g_W[i].Code != MF_EXIT_MT_SYNC) FirstCode(&code, g_W[i].Code);
    for (i = 0; i < g_Opt.Workers; i++)
        if (g_W[i].Code) FirstCode(&code, g_W[i].Code);
    for (i = 0; i < g_Opt.Workers; i++)
        for (rd = 0; rd < g_W[i].RoundsDone; rd++)
            if (g_W[i].AUs[rd]) { a[n] = g_W[i].AUs[rd]; b[n] = g_W[i].BUs[rd]; c[n] = g_W[i].CUs[rd]; n++; }
    PrintStats("SUMMARY M.a cpu_seen_us", a, n);
    PrintStats("SUMMARY M.b wake_us", b, n);
    PrintStats("SUMMARY M.c marker_us", c, n);
    return code;
}

// ---- latency series ----------------------------------------------------------------------------------------------

static int RunLatency(MF_WORKER* w)
{
    static UINT64 sub[3][MF_MAX_ITERS], sig[MF_MAX_ITERS], seen[3][MF_MAX_ITERS], wake[3][MF_MAX_ITERS];
    unsigned i, m, n[3] = { 0, 0, 0 };
    int code = 0;
    UINT64 prevK = g_K.Expected;

    Line("PHASE L iterations=%u modes=gpu-int0,gpu-int3,kernel-signal context=w0.A", g_Opt.Iters);
    for (i = 0; i < g_Opt.Iters && code == 0; i++)
    {
        for (m = 0; m < 3u && code == 0; m++)
        {
            UINT64 t0, prev, v, submitUs = 0, signalUs = 0, deadline, tSeen = 0, tWake = 0;
            BOOL seenV = FALSE, seenE = FALSE, bad = FALSE;
            volatile UINT64* cpu;
            HANDLE ev;
            D3DKMT_HANDLE fence;
            NTSTATUS st;
            unsigned spins = 0;
            if (m < 2u) { v = mf_value(0, STEP_LAT + 2u * i + m); prev = w->F.Expected; cpu = w->F.Cpu; ev = w->WaitEvent; fence = w->F.h; }
            else { v = mf_value(IDX_KERNEL_FENCE, 1 + i); prev = prevK; cpu = g_K.Cpu; ev = g_KEvent; fence = g_K.h; }
            st = ArmCpuWait(fence, v, ev);
            t0 = NowUs();
            if (NT_SUCCESS(st))
            {
                if (m < 2u) st = SubmitSlot(w, w->CtxA, SLOT_LAT + 2u * i + m, w->F.Gpu, v, &submitUs);
                else
                {
                    st = SubmitSlot(w, w->CtxA, SLOT_NOP, 0, 0, &submitUs);
                    if (NT_SUCCESS(st)) st = SignalGpu(w->CtxA, g_K.h, v, &signalUs);
                }
            }
            deadline = t0 + (UINT64)g_Opt.BoundMs * 1000ull;
            while (NT_SUCCESS(st))
            {
                UINT64 now = NowUs(), f = *cpu;
                if (f != prev && f != v) bad = TRUE;
                if (!seenV && f == v) { seenV = TRUE; tSeen = now - t0; }
                if (!seenE && EventSet(ev)) { seenE = TRUE; tWake = now - t0; }
                if ((seenV && seenE) || now > deadline) break;
                if ((++spins & 63u) == 0) SwitchToThread(); else YieldProcessor();
            }
            if (!NT_SUCCESS(st) || !seenV || !seenE || bad)
            {
                Line("CHECK L.%u.%s FAIL value=0x%016llX status=0x%08lX seen=%u wake=%u torn=%u current=0x%016llX", i,
                     m == 0 ? "gpu-int0" : m == 1 ? "gpu-int3" : "kernel-signal", v, (unsigned long)st, seenV, seenE,
                     bad, *cpu);
                code = MF_EXIT_LATENCY;
                if (*cpu < v) (void)SignalCpu(fence, v);
            }
            else
            {
                sub[m][n[m]] = submitUs;
                seen[m][n[m]] = tSeen;
                wake[m][n[m]] = tWake;
                if (m == 2u) sig[n[m]] = signalUs;
                n[m]++;
            }
            if (m < 2u) w->F.Expected = *cpu; else { prevK = *cpu; g_K.Expected = prevK; }
        }
    }
    // The context goes idle before anything else uses it.
    if (NT_SUCCESS(QueueProgress(w->CtxA, &w->PA))) (void)AwaitProgress(&w->PA, g_Opt.BoundMs, NULL);
    for (m = 0; m < 3u; m++)
    {
        char label[96];
        const char* name = m == 0 ? "gpu-int0" : m == 1 ? "gpu-int3" : "kernel-signal";
        sprintf_s(label, sizeof(label), "LAT mode=%s submit_call_us", name);
        PrintStats(label, sub[m], n[m]);
        if (m == 2u) { sprintf_s(label, sizeof(label), "LAT mode=%s signal_call_us", name); PrintStats(label, sig, n[m]); }
        sprintf_s(label, sizeof(label), "LAT mode=%s cpu_seen_us", name);
        PrintStats(label, seen[m], n[m]);
        sprintf_s(label, sizeof(label), "LAT mode=%s wake_us", name);
        PrintStats(label, wake[m], n[m]);
    }
    if (n[2])
    {
        UINT64 total = 0;
        for (i = 0; i < n[2]; i++) total += sig[i];
        Line("LAT kernel-signal mean_signal_call_us=%llu (the per-submit CPU cost stage A2 removes, on this thread)", total / n[2]);
    }
    return code;
}

// ---- self-test (development PC, no D3DKMT call) -------------------------------------------------------------------

static int SelfTest(void)
{
    UINT32 dw[8];
    int bad = 0;
    unsigned f, s;
    // Linux amdgpu's own fence on unit A: "c0064900 06603514 22000000" (evidence/linux/2026-09-21-E13-reference-2/
    // boot1-full/rings-after-ib/amdgpu_ring_gfx_0.0.0.txt:326) - the same packet with DATA_SEL 1 and INT_SEL 2.
    if (mf_emit_release_mem(dw, 0x401080ull, 1, MF_DATA_SEL_32, MF_INT_SEL_IRQ_CONFIRM) != 8 ||
        dw[0] != 0xC0064900u || dw[1] != 0x06603514u || dw[2] != 0x22000000u || dw[3] != 0x00401080u) bad |= 1;
    if (mf_emit_release_mem(dw, 0x40040ull, 0x1122334455667788ull, MF_DATA_SEL_64, MF_INT_SEL_NONE) != 8 ||
        dw[0] != 0xC0064900u || dw[1] != 0x06603514u || dw[2] != 0x40000000u || dw[3] != 0x40040u || dw[4] != 0 ||
        dw[5] != 0x55667788u || dw[6] != 0x11223344u || dw[7] != 0) bad |= 2;
    if (mf_emit_release_mem(dw, 0x40044ull, 1, MF_DATA_SEL_64, MF_INT_SEL_NONE) != 0) bad |= 4;   // misaligned
    if (mf_emit_dma_copy(dw, 0x40040ull, 0x200000ull, 8) != 8 || dw[0] != 0xC0055000u || dw[1] != 0x80000000u ||
        dw[6] != 8u || dw[7] != MF_CP_NOP) bad |= 8;
    if (mf_emit_nop(dw) != 8 || dw[0] != 0xC0061000u) bad |= 16;
    for (f = 0; f < MF_FENCE_LIMIT; f++)
        for (s = 1; s < MF_STEP_LIMIT; s++)
            if (mf_value(f, s) <= mf_value(f, s - 1) || ValueOwner(mf_value(f, s)) != f ||
                (UINT32)mf_value(f, s) == (UINT32)mf_value(f, s - 1)) bad |= 32;
    if (ValueOwner(7000) != 0xFFu || ValueOwner(0) != 0xFFu) bad |= 64;
    printf("SELFTEST %s mask=0x%X release_mem_dw1=0x%08X\n", bad ? "FAIL" : "PASS", bad, mf_release_mem_dw1());
    return bad ? 1 : 0;
}

// ---- command line and main ---------------------------------------------------------------------------------------

static void MfUsage(void)
{
    printf(
        "monfence - WDDM 2.0 monitored fence written by the GPU (stage A0), raw D3DKMT on the bc250kmd miniport.\n"
        "  (a) RELEASE_MEM of N to FenceValueGPUVirtualAddress, CPU mapping reads N after completion\n"
        "  (b) WaitForSynchronizationObjectFromCpu(N) registered before the write wakes\n"
        "  (c) a second context's WaitForSynchronizationObjectFromGpu(N) holds, then releases, queued work\n"
        "  (d) (a)-(c) from up to four threads at once, plus one cross-thread wait each\n"
        "  then a latency series: GPU-written (INT_SEL 0 and 3) against SubmitCommand + SignalFromGpu\n"
        "\n"
        "  --match <text>        adapter substring (default bc250)\n"
        "  --int-sel 0|3         INT_SEL of the fence writes under test (default 0)\n"
        "  --bound-ms <ms>       bound of every wait for GPU work, 100..5000 (default 2000)\n"
        "  --gate-ms <ms>        how long a queued GPU wait must hold, 10..1000 (default 100)\n"
        "  --stagger-ms <ms>     (d) write stagger per thread, 0..100 (default 15)\n"
        "  --threads <n>         (d) threads, 1..4 (default 4; 1 skips the cross-thread wait)\n"
        "  --rounds <n>          (d) rounds, 0..16 (default 4)\n"
        "  --iterations <n>      latency iterations per mode, 0..100 (default 32)\n"
        "  --fence-affinity <n>  EngineAffinity of the fences under test (default 1, as the ICD)\n"
        "  --skip-mt, --skip-latency\n"
        "  --watchdog-s <s>      process watchdog, 10..85 (default 55; exit 4)\n"
        "  --selftest            packet and value checks only, no D3DKMT call (development PC)\n"
        "\n"
        "Exit: 0 pass, 2 args, 3 setup, 4 watchdog, 5 no fence GPU VA, 10 packet control, 11 read control,\n"
        "12 (a), 13 (b), 14 (c) order, 15 (c) release, 16 torn/foreign value, 17 latency, 21-27 (d), 30 rescue.\n");
}

static BOOL ParseRange(const char* Text, UINT64 Lo, UINT64 Hi, UINT64* Out)
{
    return ParseU64(Text, Out) && *Out >= Lo && *Out <= Hi;
}

static BOOL MfParseArgs(int argc, char** argv)
{
    int i;
    UINT64 v;
    g_Opt.Match = "bc250";
    g_Opt.IntSel = 0;
    g_Opt.BoundMs = 2000;
    g_Opt.GateMs = 100;
    g_Opt.StaggerMs = 15;
    g_Opt.Rounds = 4;
    g_Opt.Iters = 32;
    g_Opt.Workers = MF_MAX_WORKERS;
    g_Opt.Affinity = 1;
    g_Opt.WatchdogMs = 55000;
    for (i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--skip-mt")) g_Opt.SkipMt = TRUE;
        else if (!strcmp(a, "--skip-latency")) g_Opt.SkipLatency = TRUE;
        else if (!strcmp(a, "--selftest")) g_Opt.SelfTest = TRUE;
        else if (next == NULL) return FALSE;
        else if (!strcmp(a, "--match")) { g_Opt.Match = next; i++; }
        else if (!strcmp(a, "--int-sel") && ParseU64(next, &v) && (v == 0 || v == 3)) { g_Opt.IntSel = (unsigned)v; i++; }
        else if (!strcmp(a, "--bound-ms") && ParseRange(next, 100, 5000, &v)) { g_Opt.BoundMs = (DWORD)v; i++; }
        else if (!strcmp(a, "--gate-ms") && ParseRange(next, 10, 1000, &v)) { g_Opt.GateMs = (DWORD)v; i++; }
        else if (!strcmp(a, "--stagger-ms") && ParseRange(next, 0, 100, &v)) { g_Opt.StaggerMs = (DWORD)v; i++; }
        else if (!strcmp(a, "--threads") && ParseRange(next, 1, MF_MAX_WORKERS, &v)) { g_Opt.Workers = (unsigned)v; i++; }
        else if (!strcmp(a, "--rounds") && ParseRange(next, 0, MF_MAX_ROUNDS, &v)) { g_Opt.Rounds = (unsigned)v; i++; }
        else if (!strcmp(a, "--iterations") && ParseRange(next, 0, MF_MAX_ITERS, &v)) { g_Opt.Iters = (unsigned)v; i++; }
        else if (!strcmp(a, "--fence-affinity") && ParseRange(next, 0, 0xFFFF, &v)) { g_Opt.Affinity = (unsigned)v; i++; }
        else if (!strcmp(a, "--watchdog-s") && ParseRange(next, 10, 85, &v)) { g_Opt.WatchdogMs = (DWORD)v * 1000u; i++; }
        else return FALSE;
    }
    return TRUE;
}

static void TeardownAll(void)
{
    unsigned i;
    char name[32];
    printf("-- teardown: fences, contexts, buffers\n");
    // Fences before contexts (kmtprobe's order): an outstanding CPU wait references the fence, not the context.
    for (i = 0; i < MF_MAX_WORKERS; i++)
    {
        MF_WORKER* w = &g_W[i];
        sprintf_s(name, sizeof(name), "w%u.F", i); DestroySync(&w->F.h, name);
        sprintf_s(name, sizeof(name), "w%u.PA", i); DestroySync(&w->PA.h, name);
        sprintf_s(name, sizeof(name), "w%u.PB", i); DestroySync(&w->PB.h, name);
    }
    DestroySync(&g_K.h, "K");
    for (i = 0; i < MF_MAX_WORKERS; i++)
    {
        sprintf_s(name, sizeof(name), "w%u.A", i); DestroyCtx(&g_W[i].CtxA, name);
        sprintf_s(name, sizeof(name), "w%u.B", i); DestroyCtx(&g_W[i].CtxB, name);
    }
    for (i = 0; i < MF_MAX_WORKERS; i++)
    {
        TeardownBuffer(&g_P, &g_W[i].Ib);
        TeardownBuffer(&g_P, &g_W[i].Rb);
    }
    Teardown(&g_P);     // paging queue, device, adapter
    for (i = 0; i < MF_MAX_WORKERS; i++)
    {
        if (g_W[i].WaitEvent) CloseHandle(g_W[i].WaitEvent);
        if (g_W[i].PA.Event) CloseHandle(g_W[i].PA.Event);
        if (g_W[i].PB.Event) CloseHandle(g_W[i].PB.Event);
    }
    if (g_KEvent) CloseHandle(g_KEvent);
}

int main(int argc, char** argv)
{
    int code = 0;
    unsigned i, setupWorkers;
    const char* first = "none";
    UINT64 started;

    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_Print);
    QueryPerformanceFrequency(&g_QpcFreq);
    QueryPerformanceCounter(&g_QpcBase);
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) { MfUsage(); return 0; }
    if (!MfParseArgs(argc, argv)) { MfUsage(); return MF_EXIT_ARGS; }
    if (g_Opt.SelfTest) return SelfTest() ? MF_EXIT_ARGS : 0;
    if (!StartWatchdog(g_Opt.WatchdogMs)) return MF_EXIT_SETUP;
    started = NowUs();
    Line("MONFENCE version=%d pid=%lu int_sel=%u bound_ms=%lu gate_ms=%lu threads=%u rounds=%u iterations=%u affinity=%u watchdog_ms=%lu",
         MF_VERSION, GetCurrentProcessId(), g_Opt.IntSel, g_Opt.BoundMs, g_Opt.GateMs, g_Opt.Workers, g_Opt.Rounds,
         g_Opt.Iters, g_Opt.Affinity, g_Opt.WatchdogMs);
    if (SelfTest() != 0) { SetEvent(g_Done); return MF_EXIT_SETUP; }

    // Setup. All four workers' resources exist from the start, also for --threads < 4, so that the page holding the
    // fences looks the same in every run; only (d) uses fewer threads.
    g_P.Opt.Match = g_Opt.Match;
    g_P.Opt.FenceTimeoutMs = 5000;
    setupWorkers = MF_MAX_WORKERS;
    if (!StepFindAdapter(&g_P) || !StepOpenAdapter(&g_P) || !StepQueryCaps(&g_P) || !StepCreateDevice(&g_P) ||
        !StepCreatePagingQueue(&g_P)) { code = MF_EXIT_SETUP; first = "setup.device"; goto done; }
    for (i = 0; i < setupWorkers; i++)
        if (!SetupWorker(&g_W[i], i)) { code = MF_EXIT_SETUP; first = "setup.worker"; goto done; }
    if (!CreateFence(&g_P, 1, &g_K.h, &g_K.Cpu, NULL, "K")) { code = MF_EXIT_SETUP; first = "setup.kfence"; goto done; }
    g_KEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_KEvent == NULL) { code = MF_EXIT_SETUP; first = "setup.event"; goto done; }
    for (i = 0; i < setupWorkers; i++)
    {
        MF_WORKER* w = &g_W[i];
        Line("FENCE w%u handle=0x%08lX cpu=%p gpu=0x%016llX page=0x%016llX offset=0x%03llX value=%llu", i,
             (unsigned long)w->F.h, (void*)w->F.Cpu, w->F.Gpu, w->F.Gpu & ~0xFFFull, w->F.Gpu & 0xFFFull, *w->F.Cpu);
        Line("BUFFER w%u ib=0x%016llX rb=0x%016llX ctxA=0x%08lX ctxB=0x%08lX", i, w->Ib.MappedVa, w->Rb.MappedVa,
             (unsigned long)w->CtxA, (unsigned long)w->CtxB);
        if (w->F.Gpu == 0 || (w->F.Gpu & 7u) != 0) { code = MF_EXIT_NO_GPU_VA; first = "setup.fence-gpu-va"; goto done; }
        if (!WriteIbs(w)) { code = MF_EXIT_SETUP; first = "setup.ibs"; goto done; }
    }
    Line("PAGINGFENCE cpu=%p value=%llu", (void*)g_P.PagingFence, g_P.PagingFence ? *g_P.PagingFence : 0ull);

    Line("PHASE S worker=0 contexts=A,B");
    code = RunCore(&g_W[0]);
    if (code) { first = "S"; goto done; }
    if (!g_Opt.SkipMt && g_Opt.Rounds > 0)
    {
        code = RunMt();
        if (code) { first = "M"; goto done; }
    }
    if (!g_Opt.SkipLatency && g_Opt.Iters > 0)
    {
        code = RunLatency(&g_W[0]);
        if (code) { first = "L"; goto done; }
    }

done:
    // Nothing may stay queued behind a GPU wait. First every fence is CPU-signalled up to the highest value a context
    // was told to wait for (never above a write this run queued itself), all of them before any drain, because
    // thread t's second context also waits for thread t+1's fence; then every second context's progress is awaited;
    // then every first context goes idle.
    for (i = 0; i < MF_MAX_WORKERS; i++)
    {
        MF_WORKER* w = &g_W[i];
        UINT64 top = (UINT64)w->F.GpuWaitTop;
        if (w->F.h == 0 || w->F.Cpu == NULL || *w->F.Cpu >= top) continue;
        Line("RESCUE final w%u fence 0x%016llX < 0x%016llX: CPU signal 0x%08lX", i, *w->F.Cpu, top,
             (unsigned long)SignalCpu(w->F.h, top));
    }
    for (i = 0; i < MF_MAX_WORKERS; i++)
    {
        MF_WORKER* w = &g_W[i];
        if (w->PB.h == 0 || w->PB.Last == 0 || *w->PB.Cpu >= w->PB.Last) continue;
        if (AwaitProgress(&w->PB, g_Opt.BoundMs, NULL) != 1)
        {
            Line("RESCUE final w%u second context progress %llu/%llu STILL QUEUED", i, *w->PB.Cpu, w->PB.Last);
            FirstCode(&code, MF_EXIT_RESCUE);
        }
    }
    for (i = 0; i < MF_MAX_WORKERS; i++)
    {
        MF_WORKER* w = &g_W[i];
        // Tidiness only (teardown would wait for idle contexts anyway), so a short bound keeps the worst case small.
        if (w->CtxA && w->PA.h && NT_SUCCESS(QueueProgress(w->CtxA, &w->PA))) (void)AwaitProgress(&w->PA, 500, NULL);
    }
    Line("RESULT %s exit=%d first=%s pid=%lu elapsed_ms=%llu", code ? "FAIL" : "PASS", code, code ? first : "none",
         GetCurrentProcessId(), (NowUs() - started) / 1000ull);
    TeardownAll();
    SetEvent(g_Done);
    DeleteCriticalSection(&g_Print);
    return code;
}
