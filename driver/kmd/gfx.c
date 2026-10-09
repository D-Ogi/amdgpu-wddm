// RLC, CP, KIQ, the queues, the ring tests and SDMA (milestone M5 second part, ADR 0002 and 0007). The sequence is
// driver/shim/bc250_gfx.c, bc250_sdma.c and bc250_nbio.c: amdgpu's gfx_v10_0_hw_init() and sdma_v5_0_hw_init()
// transcribed against AMD's imported tables and structures, which a host test replays against amdgpu's recorded
// register traffic of unit A. This file is the command around it; the memory is gpumem.c.
//
//   <service key>\Parameters
//     EnableGfx        REG_DWORD  1 = allow the GFX command. Needs EnableMmio, EnableVram, EnableGart, EnablePsp. Default 0.
//     EnableGpuSubmit  REG_DWORD  1 = GfxSubmitIb() may write the gfx ring (ADR 0008 stage C). Needs EnableGfx and a
//                                 bring-up that reached stage 8, i.e. EnableIh as well. Default 0.
//     EnableVmidPool   REG_DWORD  0 = every WDDM job at VMID 1, a job of another root waits for the ring to drain
//                                 (0.7.213.1). Absent or 1 = a VMID per page-table root from a pool (vmid_pool.h,
//                                 docs/design/gfx-submit-root-serialization.md). Default 1 from 0.7.214.1.
//
// One escape (BC250_ESCAPE_RUN_GFX), four operations:
//   PLAN    set up (allocates memory and zeroes it, the GART table untouched; reads registers), run stages 1..LastStage against the real registers
//           without executing a register or doorbell write, tear down again. What would be written is returned. From
//           the first poll on a plan and the hardware part ways (nothing answers a write that was not made), so a
//           plan ends where the first ring test would.
//   RUN     needs the GART enabled and the firmware loaded through the PSP in this driver instance. Runs the stages
//           that have not run yet, up to LastStage, and stops at the first that fails.
//   FINI    amdgpu's hw_fini order: SDMA, then CP and MEC halted; memory given back if the engines read halted.
//   STATE   the out fields only.
// Stage 8 (M6) enables the interrupt sources as amdgpu does at 1.56 s of its init; it needs ih.c's ring enabled first,
// which is amdgpu's order. A second escape (BC250_ESCAPE_RUN_FENCE) emits fences on one ring: the smallest submission
// that writes a value we can read and raises an end-of-pipe interrupt (experiment E12 part C). Its two newest modes are
// ADR 0008 stage C: BC250_FENCE_MODE_IB submits the ring test as an indirect buffer the driver builds itself, and
// BC250_FENCE_MODE_IB_AT submits the caller's, through the same GfxSubmitIb() that wddm.c uses.
// The driver does FINI by itself when the device stops, before the PSP unload and the GART restore.
//
// Registers only through g_MmioGfxAllow: what amdgpu itself read or wrote on unit A in these steps (E03 trace).
#include "bc250kmd.h"
#include "bc250_fence_order.h"
#include "hang_recovery.h"
#include "vmid_pool.h"
#include "bc250_sdma_virtual_ptes.h"
#include "paging_intervals.h"
#include "paging_permutation.h"
#include "log_rate.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "bc250_gmc.h"
#include "bc250_gfx.h"
#include "bc250_sdma.h"
#include "bc250_sdma_paging.h"
#include "bc250_pte.h"
#include "paging_mc.h"
#include "paging_stream.h"
#include "paging_window.h"
#include "bc250_gart.h"
#include "paging_private.h"
#include "bc250_dispatch.h"
#include "bc250_nbio.h"
#include "bc250_irq.h"

#define BC250_GFX_TAG 'xG2B'

// The halt bits as the firmware leaves them and as hw_fini leaves them again (MEASURED: CP_ME_CNTL 0x15000000,
// CP_MEC_CNTL 0x50000000, SDMAn_F32_CNTL 1 in every sweep of E10 before a load).
#define BC250_CP_ME_HALTED      0x15000000ul
#define BC250_CP_MEC_HALTED     0x50000000ul
#define BC250_SDMA_HALTED       0x00000001ul

typedef int (*BC250_GFX_STAGE_FUNCTION)(struct amdgpu_device* adev);

static int StageDoorbellAperture(struct amdgpu_device* adev) { bc250_nbio_enable_doorbell_aperture(adev, true); return 0; }
static int StageCamProbe(struct amdgpu_device* adev) { bool remapped = false; return bc250_gfx_grbm_cam_probe(adev, &remapped); }

// The E03 trace's 35 writes of 1.560 to 1.562 s, in the order the host replay compares them in (driver/shim/test).
static int StageInterrupts(struct amdgpu_device* adev)
{
    int result = bc250_irq_init_mec_pipes(adev);

    if (result == 0) result = bc250_irq_hw_init(adev);
    if (result == 0) result = bc250_nbio_enable_doorbell_selfring_aperture(adev, true);     // refuses a doorbell base of 0
    if (result == 0) result = bc250_irq_late_init(adev);
    return result;
}

static int StageSdma(struct amdgpu_device* adev);       // below: it needs BC250_GFX

// Stage numbers are part of the escape's interface (bc250kmd_cli prints the names).
static const struct { const char* Name; BC250_GFX_STAGE_FUNCTION Run; } g_Stages[] = {
    { NULL, NULL },
    { "doorbell aperture", StageDoorbellAperture },             // 1  nv_common_hw_init(): nbio_v2_3_enable_doorbell_aperture
    { "golden registers", bc250_gfx_init_golden_registers },    // 2  gfx_v10_0_init_golden_registers
    { "GRBM CAM probe", StageCamProbe },                        // 3  gfx_v10_0_check_grbm_cam_remapping
    { "constants", bc250_gfx_constants_init },                  // 4  gfx_v10_0_constants_init
    { "RLC", bc250_gfx_rlc_resume },                            // 5  gfx_v10_0_rlc_resume
    { "CP", bc250_gfx_cp_resume },                              // 6  gfx_v10_0_cp_resume: KIQ, MEC, queues, ring tests
    { "SDMA", StageSdma },                                      // 7  sdma_v5_0_hw_init, with the ring tests upstream ends it with
    { "interrupt sources", StageInterrupts },                   // 8  amdgpu_fence_driver_hw_init, gfx_v10_0_late_init, amdkfd
};
#define BC250_GFX_STAGE_COUNT (RTL_NUMBER_OF(g_Stages) - 1)
#define BC250_GFX_STAGE_CP 6
#define BC250_GFX_STAGE_SDMA 7
#define BC250_GFX_STAGE_INTERRUPTS 8
#define BC250_FENCE_BUDGET_US 1000000     // all fences of one call
#define BC250_FENCE_TIMEOUT_US 100000ul
// ADR 0008 stage C. FenceRing() gives the fence escape slots 0..9, one per CP ring, so the submission path takes one
// of the six the page has left (BC250_GFX_FENCE_SLOTS is 16). The poll budget is the `ib` escape verb's alone:
// GfxSubmitIb() never waits, and wddm.c will hear about the fence from the interrupt.
#define BC250_SUBMIT_FENCE_SLOT 10u
#define BC250_SUBMIT_POLL_US 500000ul
// M15.12 stage 1: how long GfxSoftRecover re-issues the wave kill while waiting for the newest sequence to retire.
// amdgpu_ring_soft_recovery uses 10000 us (10 ms); we match it. Bounded so DxgkDdiResetEngine returns promptly.
#define BC250_SOFT_RECOVER_US 10000ul

// ADR 0008 stage D (docs/design/paging-node.md): node 1, DXGK_ENGINE_TYPE_COPY on SDMA0, the paging node.
// Its own fence slot on the SDMA fence page (BC250_SDMA_FENCE_SLOTS = 16, bc250_sdma.h): 0/1 are the ring
// tests', 2/3 are SdmaCopyEscape's and GfxFenceEscape's SDMA arm's - one past what the escapes use.
#define BC250_PAGING_FENCE_SLOT 4u
#define BC250_PAGING_MARKER_SLOT 5u // private CPU-reset marker; never the OS completion fence
#define BC250_PAGING_POLL_US 500000ul       // node 1's own watchdog budget, same shape as BC250_SUBMIT_POLL_US
// OS command-buffer capacity; live-ring reservation further limits each batch.
#define BC250_GFX_PAGING_BUFFER_BYTES 0x10000ul

typedef struct _BC250_GFX {
    BC250_SEQUENCE Sequence;
    BOOLEAN PagingCpuBootstrap;     // one-way close before first RUN, under GfxPagingLock
    BOOLEAN SetUp;                  // bc250_gfx_setup and bc250_sdma_setup have allocated
    BOOLEAN PowerSuspended;         // hardware halted, owners retained; under GartLock
    BOOLEAN Failed;                 // a stage failed on the hardware: only FINI from here
    BOOLEAN FencePage;              // bc250_gfx_fence_page_alloc has allocated
    BOOLEAN SdmaFencePage;          // bc250_sdma_fence_page_alloc has
    BOOLEAN Dispatch;               // bc250_gfx_dispatch_setup has
    // volatile LONG, not ULONG: ADR 0008 stage D added a writer (GfxSubmitPaging) that runs at DISPATCH_LEVEL,
    // outside GartLock - review 23 MUST-FIX. Every increment goes through InterlockedIncrement now, so the
    // PASSIVE_LEVEL/GartLock-serialized writers (GfxSubmitIb, GfxFenceEscape, SdmaCopyEscape) and the new
    // DISPATCH_LEVEL one share the counter safely instead of racing a plain "++" across two IRQL domains.
    volatile LONG FenceSeq;         // last fence value emitted
    // Per BC250_FENCE_RING_*: the value a submission that timed out still owes its slot, 0 when none. The shim's
    // amdgpu_ring_alloc() does not look at the read pointer (upstream's scheduler bounds what is in flight); here the
    // synchronous poll does, and a timeout would break that. A ring that owes a fence takes no new submission.
    ULONG RingOwes[BC250_FENCE_RING_SDMA0 + 2];
    ULONG RingOwesSlot[BC250_FENCE_RING_SDMA0 + 2];
    ULONG StagesDone;               // last stage attempted on hardware in this driver instance
    ULONG CpStepDone;               // completed startup CP checkpoint, under GartLock
    // ---- stage C: one indirect buffer at a time on the gfx ring (ADR 0008) ----
    BOOLEAN SubmitGate;             // EnableGpuSubmit, read once at GfxStart
    // D5 (C48 review of the log ring): three uncapped guard-log calls per node-0 submit, inside GartLock, wrote
    // 3871 lines a second on the lab, of which 75 % was overwritten before anything read it - the cost of
    // DbgPrintEx and a global lock paid in the hot path, and the evidence lost anyway. HotSubmitLog brings them
    // back for a run that wants them (a fault hunt reads exactly these three lines), and the summary counts what
    // was left out, so a quiet log is never mistaken for a quiet ring.
    BOOLEAN HotSubmitLog;           // HotSubmitLog, read once at GfxStart; default off
    volatile LONG HotSubmitLinesSkipped;
    BOOLEAN IbPage;                 // bc250_gfx_ib_page_alloc has allocated
    volatile LONG SubmitFailed;     // sticky: nothing goes to the ring through GfxSubmitIb again this device start
    volatile LONG PipelineSamples;     // first 16 submissions only; no hot-path printf after that
    ULONG SubmitVmid;                  // last submitted VMID; diagnostic VMID0 cannot overlap jobs
    volatile LONG SubmitInFlight;   // latest outstanding sequence; CAS protects a newer producer from an old DPC
    ULONG SubmitSeq;                // its sequence number, 0 when none was ever emitted
    // The DPC reads the fence slot without the lock, so it needs a device pointer it can use there. It is this
    // sequence's own adev, set under GartLock at the submission and never freed before GfxStop: pnp.c stops ih.c
    // first, and IhStop() clears Active and drains the DPCs, so by the time TearDown() releases the fence page no
    // consumer of this field can still be running.
    struct amdgpu_device* SubmitAdev;
    // The VMID pool (vmid_pool.h, docs/design/gfx-submit-root-serialization.md). Vmid.Root is the page directory
    // root each VMID was last given (the VmidRoot array of 0.7.213.1); the rest of Vmid says which job last ran at
    // each VMID and for whom. The MMIO invalidation still runs on every job. Index 0 is unused: VMID 0 is the GART
    // aperture and has no root of ours. Every change happens under GartLock; a change of Root, of a tenant field or
    // of VmidHistory also takes VmidLock, which is what the DISPATCH_LEVEL readers (the fault and timeout reports)
    // and the FLUSH_TLB builder (GfxPagingLock, never GartLock) take to read them. Both resets of the table run
    // with GfxAccessClose done and GfxPagingLock held exclusively, so they need no VmidLock.
    BOOLEAN VmidPoolGate;           // EnableVmidPool, read once at GfxStart; absent = on
    BOOLEAN VmidProbed;             // the bring-up read ran; once per device start, a teardown does not clear it
    USHORT VmidMembers;             // the pool: VMIDs 1 and 3..15 less what the bring-up read found programmed
    USHORT VmidExcluded;            // the VMIDs that read found programmed by something else
    KSPIN_LOCK VmidLock;
    BC250_VMID_TABLE Vmid;
    BC250_VMID_HISTORY_RING VmidHistory;    // tenancies that ended, for fault attribution; survives a teardown
    volatile LONG VmidClaims;       // a VMID given a root it did not hold
    volatile LONG VmidReuses;       // a job at the VMID that already held its root
    volatile LONG VmidBusy;         // the pool had no VMID free: STATUS_DEVICE_BUSY, the old wait
    volatile LONG VmidRuleRefusals; // the rule check refused a root change of a live VMID (must stay 0)
    volatile LONG VmidRuleLogged;
    volatile LONG VmidFlushes;      // FLUSH_TLB operations built with the pool open
    volatile LONG VmidFlushVmids;   // the VMID invalidations they carried
    // ADR 0013: the two VRAM scratch regions BC250_ESCAPE_RUN_SDMACOPY copies between. Allocated once from the
    // same VRAM pool the rest of this file's memory comes from (gpumem.c), on the first call, and freed with
    // everything else in TearDown - not by the escape itself, so that two calls in a row need not pay for the
    // allocation twice.
    BOOLEAN SdmaCopyRegions;
    struct bc250_mem SdmaCopySrc, SdmaCopyDst;
    struct bc250_mem SdmaCopyIb; // retained until retirement, including after a diagnostic timeout
    struct bc250_mem SdmaVaTables, SdmaVaData; // unpublished VMID2 control backing

    // ---- ADR 0008 stage D: node 1, the paging node on SDMA0 (docs/design/paging-node.md) ----
    BOOLEAN PagingGate;                   // EnablePagingNode, read once at GfxStart
    // GfxEscape's RUN arm has captured PagingRing/PagingDevicePtr. Read with no lock by
    // GfxPagingBuild (PASSIVE_LEVEL, BuildPagingBuffer - under GfxPagingLock, this field's own
    // publish/clear stays under GartLock) and by the DISPATCH_LEVEL trio (GfxSubmitPaging/GfxPagingFenceArrived/
    // GfxPagingSubmitReady), the ih.c DpcAdev shape.
    struct bc250_mem PagingCopyStaging; // private VRAM page, retained across copy submissions
    PAGING_WINDOW PagingWindow;
    BOOLEAN PagingWindowReady;
    BOOLEAN PagingReady;
    struct amdgpu_ring* PagingRing;       // &adev->sdma.instance[0].ring: a pointer into the live, persistent adev,
                                           // never a copy (section 4: forking .wptr into two counters is the bug this avoids)
    struct amdgpu_device* PagingDevicePtr; // the same adev; GfxPagingFenceArrived reads bc250_sdma_fence_read through it,
                                           // with no lock, the same shape as SubmitAdev above
    KSPIN_LOCK Sdma0RingLock;             // every direct writer of the live SDMA0 ring takes this narrowly (section 4)
    // Device->GfxPagingLock protects PASSIVE_LEVEL builders against all engine
    // setup/teardown and the lifetime of this object. Unlike GartLock it preserves
    // PASSIVE_LEVEL while VidMmTranslate maps page tables. Lock order: this lock
    // first, GartLock second; builders never acquire GartLock.
    volatile LONG PagingSubmitFailed;     // sticky, like SubmitFailed, but independent: node 1 fails on its own hardware
    volatile LONG PagingSubmitInFlight;   // SDMA paging retains its independent one-in-flight rule
    ULONG PagingSubmitSeq;
    // BD-097: the one line a successful paging submit writes, rate limited (log_rate.h). An idle desktop submits
    // paging work about twice a second, and the uncapped line was 708 of the ring's 768 rotating lines in the
    // b23 lab read: the mode sets of an hour before were gone. Its own spin lock, because the decision runs at
    // DISPATCH_LEVEL on any processor, outside GartLock and outside Sdma0RingLock (the log call itself happens
    // after the lock is released, as it did before). A few instructions under the lock, no GuardLog inside it.
    KSPIN_LOCK PagingLogLock;
    BC250_LOG_RATE PagingLogRate;
} BC250_GFX;

// DPC-safe CPU lifetime references. Admission and pointer lookup share a device
// spin lock; users release it before touching rings/fences. No reference holder
// waits for GPU progress or acquires GartLock. Lifecycle writers already own
// GfxPagingLock then GartLock, close admission and wait at <= APC_LEVEL for these
// bounded CPU accesses before changing/freeing the engine state. This is not a
// hardware-DMA drain and does not replace halt/reset or retained backing pages.
static BC250_GFX* GfxAccessAcquire(_In_ const BC250_DEVICE* Device)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Device;
    BC250_GFX* gfx = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&device->GfxAccessLock, &irql);
    if (!device->GfxAccessClosed && device->Gfx != NULL)
    {
        gfx = (BC250_GFX*)device->Gfx;
        if (device->GfxAccessUsers++ == 0) KeClearEvent(&device->GfxAccessDrained);
    }
    KeReleaseSpinLock(&device->GfxAccessLock, irql);
    return gfx;
}

static void GfxAccessRelease(_In_ const BC250_DEVICE* Device)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Device;
    KIRQL irql;
    KeAcquireSpinLock(&device->GfxAccessLock, &irql);
    NT_ASSERT(device->GfxAccessUsers != 0);
    if (--device->GfxAccessUsers == 0) KeSetEvent(&device->GfxAccessDrained, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&device->GfxAccessLock, irql);
}

static void GfxAccessClose(_Inout_ BC250_DEVICE* Device)
{
    KIRQL irql;
    KeAcquireSpinLock(&Device->GfxAccessLock, &irql);
    Device->GfxAccessClosed = TRUE;
    KeReleaseSpinLock(&Device->GfxAccessLock, irql);
    (void)KeWaitForSingleObject(&Device->GfxAccessDrained, Executive, KernelMode, FALSE, NULL);
}

static void GfxAccessOpen(_Inout_ BC250_DEVICE* Device)
{
    KIRQL irql;
    KeAcquireSpinLock(&Device->GfxAccessLock, &irql);
    Device->GfxAccessClosed = FALSE;
    KeReleaseSpinLock(&Device->GfxAccessLock, irql);
}

// sdma_v5_0_gfx_resume_instance() ends with amdgpu_ring_test_helper(ring); the shim leaves that to the miniport, because an
// SDMA ring test touches no register (bc250_sdma.h). Without it stage 7 proves that registers can be written and nothing
// more: in E15 run 001 both engines were dead after a re-init behind an rc 0 in 56 us (facts M59). Not in a plan, where
// nothing executes and the slot would never change.
static int StageSdma(struct amdgpu_device* adev)
{
    BC250_GFX* gfx = CONTAINING_RECORD(adev->backend, BC250_GFX, Sequence);
    int result = bc250_sdma_hw_init(adev), i;

    // A faulted sequence executes nothing any more: two ring tests could only time out, with the GART lock held.
    if (result != 0 || gfx->Sequence.Plan || !NT_SUCCESS(gfx->Sequence.Fault)) return result;
    result = bc250_sdma_fence_page_alloc(adev);
    if (result != 0) return result;
    gfx->SdmaFencePage = TRUE;
    // Sdma0RingLock, narrowly around the push, exactly as every other writer of the live SDMA0 ring takes it
    // (the ring-test escape, GfxFenceEscape's SDMA arm, SdmaCopyEscape, GfxSubmitPaging) - docs/design/
    // paging-node.md section 4 already claimed this stage took it and it did not (E24 run 001, "What run 001
    // hung on"). Node 1 cannot be ready this early in the same RUN (PagingReady is only ever set after stage 8,
    // below this stage in GfxEscape's loop), so nothing contends for it here today; taking it anyway is what
    // makes "every writer, no exceptions" true by construction instead of by an argument about call order that
    // the next change to this file might quietly break.
    for (i = 0; i < adev->sdma.num_instances && result == 0; i++)
    {
        struct amdgpu_ring* ring = &adev->sdma.instance[i].ring;
        volatile u32* slotCpu = NULL;
        KIRQL irql;

        KeAcquireSpinLock(&gfx->Sdma0RingLock, &irql);
        result = bc250_sdma_ring_test_submit(ring, &slotCpu);
        KeReleaseSpinLock(&gfx->Sdma0RingLock, irql);
        if (result == 0) result = bc250_sdma_ring_test_wait(adev, slotCpu);
    }
    return result;
}

static ULONG Microseconds(LARGE_INTEGER From, LARGE_INTEGER Frequency)
{
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);

    return (ULONG)(((now.QuadPart - From.QuadPart) * 1000000ll) / Frequency.QuadPart);
}

// Called before any engine stage can reference the page. The VRAM allocator's
// reserved pool is outside the VidMm segment and has its own teardown ownership.
static int PagingCopyStorageInit(_Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev)
{
    int result;
    if (!Gfx->PagingGate) return 0;
    if (Gfx->PagingCopyStaging.size!=0) return 0;
    result=bc250_shim_mem_alloc(Adev,BC250_MEM_VRAM,PAGE_SIZE,PAGE_SIZE,&Gfx->PagingCopyStaging);
    if (result!=0) return result;
    if (Gfx->PagingCopyStaging.size<PAGE_SIZE || (Gfx->PagingCopyStaging.mc & (PAGE_SIZE-1))!=0 ||
        Gfx->PagingCopyStaging.mc>~(u64)0-(PAGE_SIZE-1)) {
        bc250_shim_mem_free(Adev,&Gfx->PagingCopyStaging);
        return BC250_EINVAL;
    }
    return 0;
}

// Engine halt/retirement is the enclosing Fini/GpuMemRelease responsibility.
// The shared builder lock and CPU-access drain alone do not prove GPU quiescence.
static void PagingCopyStorageFree(_Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev)
{
    if (Gfx->PagingCopyStaging.size!=0) bc250_shim_mem_free(Adev,&Gfx->PagingCopyStaging);
}

static int SetUp(_Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev)
{
    struct bc250_gfx_inputs inputs;
    int result;

    if (Gfx->SetUp) return 0;
    // amdgpu's kernel log on unit A (E03 dmesg): "SE 2, SH per SE 2, CU per SH 10". Backends per SE is not in the log
    // and in no register amdgpu touched; it only scales a software mask (bc250_gfx.h). 2 is the value of every other
    // GC 10.1 part with this SE/SH layout; docs/linux-session-wishlist.md asks for the discovery table.
    inputs.max_shader_engines = BC250_SHADER_ENGINES;
    inputs.max_sh_per_se = BC250_SH_PER_SE;
    inputs.max_cu_per_sh = BC250_MAX_CU_PER_SH;    // cumode.h: the CU mode needs the same number
    inputs.max_backends_per_se = 2;
    inputs.async_gfx_ring = true;       // the trace: no CP_RB0 programming, a KIQ MAP_QUEUES for the gfx queue
    // Full WDDM uses AMD's no-GFXOFF startup policy until its power lifecycle
    // is implemented. This selects the original RLC-SMU handshake-off branch;
    // diagnostic replay keeps the E03 feature-mask policy for trace comparison.
    inputs.pp_gfxoff = !Gfx->Sequence.Device->FullWddm;
    GuardLog("gfx: RLC startup policy pp_gfxoff %u",(ULONG)inputs.pp_gfxoff);
    result = bc250_gfx_setup(Adev, &inputs);
    if (result != 0) return result;
    result = bc250_sdma_setup(Adev);
    if (result != 0) { bc250_gfx_teardown(Adev); return result; }
    result=PagingCopyStorageInit(Gfx,Adev);
    if (result!=0) {
        bc250_sdma_teardown(Adev);
        bc250_gfx_teardown(Adev);
        return result;
    }
    // For the self-ring aperture of stage 8: the doorbell BAR's bus address, which is its CPU physical address here
    // (facts M37). 0 if unknown, and then the shim refuses the stage.
    Adev->doorbell.base = GpuMemDoorbellBase(Gfx->Sequence.Device);
    Gfx->SetUp = TRUE;
    return 0;
}

static void TearDown(_Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev)
{
    GfxAccessClose(Gfx->Sequence.Device);
    if (!Gfx->SetUp) return;
    Gfx->SubmitAdev = NULL;             // first: GfxFenceArrived reads the fence page through it, without a lock
    // The caller holds GfxPagingLock exclusively before touching engine state.
    // All admitted builders have exited and no new builder can enter. The DPC
    // readers require their separate interrupt/timer rundown; this lock is passive.
    Gfx->PagingReady = FALSE;
    Gfx->PagingRing = NULL;
    Gfx->PagingDevicePtr = NULL;
    PagingCopyStorageFree(Gfx,Adev);
    if (Gfx->FencePage) { bc250_gfx_fence_page_free(Adev); Gfx->FencePage = FALSE; }
    if (Gfx->SdmaFencePage) { bc250_sdma_fence_page_free(Adev); Gfx->SdmaFencePage = FALSE; }
    if (Gfx->IbPage) { bc250_gfx_ib_page_free(Adev); Gfx->IbPage = FALSE; }
    if (Gfx->SdmaVaTables.size != 0) bc250_shim_mem_free(Adev, &Gfx->SdmaVaTables);
    if (Gfx->SdmaVaData.size != 0) bc250_shim_mem_free(Adev, &Gfx->SdmaVaData);
    if (Gfx->SdmaCopyIb.size != 0) bc250_shim_mem_free(Adev, &Gfx->SdmaCopyIb);
    if (Gfx->SdmaCopyRegions)
    {
        bc250_sdma_copy_regions_free(Adev, &Gfx->SdmaCopySrc, &Gfx->SdmaCopyDst);
        Gfx->SdmaCopyRegions = FALSE;
    }
    RtlZeroMemory(Gfx->RingOwes, sizeof(Gfx->RingOwes));          // the rings go with the pages
    RtlZeroMemory(Gfx->RingOwesSlot, sizeof(Gfx->RingOwesSlot));
    // The fence page is gone, so nothing may read a slot in it any more, and the rings are gone, so no VMID root this
    // instance programmed describes anything the next one will submit. SubmitFailed is NOT cleared: it is sticky for
    // the whole device start by design, and a teardown is not a reason to trust the path again.
    Gfx->SubmitSeq = 0;
    InterlockedExchange(&Gfx->SubmitInFlight, 0);
    // PagingSubmitFailed is left exactly like SubmitFailed above, for the same reason and on its own hardware.
    Gfx->PagingSubmitSeq = 0;
    InterlockedExchange(&Gfx->PagingSubmitInFlight, 0);
    // GfxAccessClose ran first and the caller holds GfxPagingLock exclusively: no reader of the table is left.
    Bc250VmidResetAll(&Gfx->Vmid, &Gfx->VmidHistory);
    if (Gfx->Dispatch) { bc250_gfx_dispatch_teardown(Adev); Gfx->Dispatch = FALSE; }
    bc250_sdma_teardown(Adev);
    bc250_gfx_teardown(Adev);
    Gfx->SetUp = FALSE;
}

// Halted as the firmware had them? Read through the sequence's table, like everything else.
static BOOLEAN EnginesHalted(_In_ const BC250_DEVICE* Device)
{
    ULONG me = 0, mec = 0, sdma0 = 0, sdma1 = 0;

    if (!NT_SUCCESS(MmioGfxRead(Device, BC250_REG_GC_CP_ME_CNTL, &me)) || !NT_SUCCESS(MmioGfxRead(Device, BC250_REG_GC_CP_MEC_CNTL, &mec)) ||
        !NT_SUCCESS(MmioGfxRead(Device, BC250_REG_GC_SDMA0_F32_CNTL, &sdma0)) || !NT_SUCCESS(MmioGfxRead(Device, BC250_REG_GC_SDMA1_F32_CNTL, &sdma1)))
        return FALSE;
    GuardLog("gfx: CP_ME_CNTL 0x%08X, CP_MEC_CNTL 0x%08X, SDMA0/1_F32_CNTL 0x%X 0x%X", me, mec, sdma0, sdma1);
    return (me & BC250_CP_ME_HALTED) == BC250_CP_ME_HALTED && (mec & BC250_CP_MEC_HALTED) == BC250_CP_MEC_HALTED &&
           (sdma0 & BC250_SDMA_HALTED) != 0 && (sdma1 & BC250_SDMA_HALTED) != 0;
}

// Startup exclusively owns this unpublished device. No engine may consume
// new GTT mappings until the stage5 visibility commit completes.
NTSTATUS GfxBeginTranslationBootstrap(BC250_DEVICE* Device)
{
    BC250_GFX* gfx=(BC250_GFX*)Device->Gfx;
    if (Device->Started || Device->Wddm || !Device->FullWddm ||
        Device->GfxTlbBootstrap || !gfx || gfx->SetUp || gfx->StagesDone ||
        !EnginesHalted(Device)) return STATUS_INVALID_DEVICE_STATE;
    Device->GfxTlbBootstrap=TRUE;
    GuardLog("gfx: unpublished translation bootstrap entered with engines halted");
    return STATUS_SUCCESS;
}

// Runs under GfxPagingLock/GartLock. The barrier is inside the stage loop, so
// both incremental traced startup and a single full RUN obey the same order.
static int RunEngineStage(BC250_DEVICE* Device, struct amdgpu_device* Adev, ULONG Stage,
                          bc250_gfx_checkpoint_fn Checkpoint)
{
    BC250_SEQUENCE* sequence=(BC250_SEQUENCE*)Adev->backend;
    int result;
    if (Device->GfxTlbBootstrap && Stage>=BC250_GFX_STAGE_CP) return BC250_EINVAL;
    if (Checkpoint && Stage==BC250_GFX_STAGE_CP-1) Checkpoint("before-rlc-resume");
    result=g_Stages[Stage].Run(Adev);
    if (Checkpoint && Stage==BC250_GFX_STAGE_CP-1) Checkpoint("after-rlc-resume");
    if (result==0 && NT_SUCCESS(sequence->Fault) &&
        Stage==BC250_GFX_STAGE_CP-1 && Device->GfxTlbBootstrap) {
        if (Checkpoint) Checkpoint("before-gfx-visibility");
        result=GpuMemCompleteGfxBootstrap(Adev);
        if (Checkpoint) Checkpoint("after-gfx-visibility");
    }
    return result;
}

// Diagnostic observation, not a DMA-retirement predicate. AMD gfx_v10_0_soft_reset
// inspects GRBM_STATUS2.RLC_BUSY; a cleared RLC_ENABLE alone does not provide it.
// Existing safe-read allow-list only; failures remain explicit in the log.
void GfxTraceRlcState(_In_ const BC250_DEVICE* Device, _In_ const char* Phase)
{
    ULONG control=0, status2=0;
    NTSTATUS controlStatus=MmioRead(Device,BC250_REG_GC_RLC_CNTL,&control);
    NTSTATUS busyStatus=MmioRead(Device,BC250_REG_GC_GRBM_STATUS2,&status2);
    GuardLog("gfx: RLC %s CNTL 0x%08X read 0x%08X STATUS2 0x%08X read 0x%08X",
             Phase,control,(ULONG)controlStatus,status2,(ULONG)busyStatus);
    // AMD gfx_v10_0_soft_reset also classifies CP/GFX from GRBM_STATUS.
    // Keep the raw input and access status; this observation authorizes no reset.
    {
        ULONG status = 0;
        NTSTATUS readStatus = MmioRead(Device,BC250_REG_GC_GRBM_STATUS,&status);
        GuardLog("gfx: GRBM %s STATUS 0x%08X read 0x%08X",Phase,status,(ULONG)readStatus);
    }
}

// Experimental, default off. Only the unpublished automatic startup calls this;
// it is not a generic escape or an admission/retirement predicate.
NTSTATUS GfxPreparePspReload(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN enabled = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    int result = 0;
    ULONG mode = GuardReadSetting(L"EnableRlcReloadReset",0);
    if (!Device->FullWddm || (mode != 1 && mode != 2)) return STATUS_SUCCESS;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (Device->Wddm != NULL || gfx == NULL || gfx->SetUp || gfx->StagesDone != 0)
        status = STATUS_INVALID_DEVICE_STATE;
    else {
        status = GartDevice(Device,&adev,&enabled);
        if (NT_SUCCESS(status) && !enabled) status = STATUS_DEVICE_NOT_READY;
        if (NT_SUCCESS(status)) {
            void* previousBackend = adev->backend;
            adev->backend = &gfx->Sequence;
            SequenceBegin(&gfx->Sequence,Device,FALSE,NULL,0);
            result = mode == 2 ? bc250_gfx_rlc_reload_reset_readback(adev) : bc250_gfx_rlc_reload_reset(adev);
            status = gfx->Sequence.Fault;
            if (NT_SUCCESS(status) && result < 0) status = STATUS_IO_DEVICE_ERROR;
            adev->backend = previousBackend;
            GfxTraceRlcState(Device,"after-reload-reset-check");
        }
    }
    GuardLog("gfx: opt-in RLC reload reset mode %u result %d status 0x%08X",mode,result,status);
    ExReleaseFastMutex(&Device->GartLock);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// A stage that stopped between nv_grbm_select(me, pipe, queue) and nv_grbm_select(0, 0, 0, 0) leaves GRBM_GFX_CNTL selecting
// a queue, and every later read of a per-queue register, the witness's sweeps included, would be of that queue. After a
// failed stage: back to 0, the value amdgpu leaves there after every selection (gfx_v10_0.c, nv.c nv_grbm_select). Outside the
// sequence, because a faulted sequence writes nothing any more; the register is in the sequence's table.
static void GrbmSelectDefault(_In_ const BC250_DEVICE* Device)
{
    ULONG select = 0;

    if (!NT_SUCCESS(MmioGfxRead(Device, BC250_REG_GC_GRBM_GFX_CNTL, &select))) return;
    if (select == 0) return;
    GuardLog("gfx: GRBM_GFX_CNTL was left at 0x%08X, back to 0: 0x%08X", select, MmioGfxWrite(Device, BC250_REG_GC_GRBM_GFX_CNTL, 0));
}

// Hardware halt only, SDMA before GFX. Storage ownership is unchanged. Returns whether engines read halted; Undo gets what
// the shim's undo returned (BC250_ETIME: the MEC did not let go of the KIQ's queue, facts M44; BC250_EBUSY: an SDMA
// engine halted with its read pointer still behind its write pointer, so it stopped holding packets that name pages of
// ours). GFX first if both failed, because the KIQ is the one whose recovery the next bring-up has a branch for.
static BOOLEAN HaltEngines(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev, _Out_ long* Undo)
{
    BOOLEAN quiet;
    long sdma = 0;

    *Undo = 0;
    GfxAccessClose(Device);

    if (Gfx->StagesDone >= BC250_GFX_STAGE_CP)
    {
        sdma = bc250_sdma_hw_fini(Adev);
        // Disables the three fault sources of late_init. The end-of-pipe enables of stage 8 stay set: harmless with the
        // queues unmapped and both CPs halted, but a second run to stage 8 meets them enabled.
        *Undo = bc250_gfx_hw_fini(Adev);
        if (*Undo == 0) *Undo = sdma;
        // nv_common_hw_fini(): the self-ring aperture goes last.
        if (Gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS) (void)bc250_nbio_enable_doorbell_selfring_aperture(Adev, false);
        // The undo selects queues as well (the KIQ's dequeue), and a sequence that faults in between writes nothing any more.
        GrbmSelectDefault(Device);
        GfxTraceRlcState(Device,"after-stop");
    }
    // Before the CP stage no engine was released by us and no queue was mapped: nothing of ours is in use. (The PSP
    // releases SDMA by itself, facts M35, but an SDMA engine without a ring has no address of ours.)
    quiet = (Gfx->StagesDone < BC250_GFX_STAGE_CP) || EnginesHalted(Device);
    return quiet;
}

// PnP retirement of fully initialized engines: invalidate this owner's GTT
// while RLC, its VRAM CSB and PSP firmware remain owned. No backing is freed.
// Diagnostic Fini and pre-CP unwind keep their existing sequence.
static BOOLEAN HaltForMappingRetirement(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx,
                                       _In_ struct amdgpu_device* Adev, _Out_ long* Undo)
{
    long sdma;
    BOOLEAN quiet;
    if (Gfx->StagesDone < BC250_GFX_STAGE_CP) return HaltEngines(Device,Gfx,Adev,Undo);
    GfxAccessClose(Device);
    sdma = bc250_sdma_hw_fini(Adev);
    *Undo = bc250_gfx_hw_fini_keep_rlc(Adev);
    if (*Undo == 0) *Undo = sdma;
    if (Gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS)
        (void)bc250_nbio_enable_doorbell_selfring_aperture(Adev,false);
    GrbmSelectDefault(Device);
    quiet = EnginesHalted(Device) && *Undo == 0 && NT_SUCCESS(Gfx->Sequence.Fault);
    GfxTraceRlcState(Device,"after-cp-stop-before-mapping-retirement");
    if (quiet) {
        *Undo=bc250_sdma_reset_for_reload(Adev);
        quiet=*Undo==0 && NT_SUCCESS(Gfx->Sequence.Fault);
        GuardLog("gfx: SDMA reload reset result %ld fault 0x%08X",*Undo,Gfx->Sequence.Fault);
        GfxTraceRlcState(Device,"after-sdma-reload-reset");
    }
    if (quiet) {
        Gfx->Sequence.TraceRlcRetirement = TRUE;
        *Undo = GpuMemRetireGttMappings(Adev);
        Gfx->Sequence.TraceRlcRetirement = FALSE;
        quiet = *Undo == 0 && NT_SUCCESS(Gfx->Sequence.Fault);
        GfxTraceRlcState(Device,"after-owner-mapping-retirement");
    }
    // Even when mapping retirement failed, attempt the original RLC stop.
    // A faulted backend preserves its no-further-writes rule; caller retains
    // all owners on failure and never treats this attempt as a quiet verdict.
    bc250_gfx_rlc_stop(Adev);
    GfxTraceRlcState(Device,"after-stop");
    return quiet && NT_SUCCESS(Gfx->Sequence.Fault);
}

// Storage remains owned after HaltEngines. The caller must establish the
// applicable consumer/translation retirement before reaching this phase.
// Fini retains the original combined diagnostic behavior. PnP and startup
// unwind reach this helper only after PSP and GART hardware retirement.
static void ReleaseStoppedStorage(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx,
                                  _In_ struct amdgpu_device* Adev, BOOLEAN Quiet, long Undo)
{
    Gfx->Sequence.TraceRlcRetirement = !Gfx->Sequence.Plan && Gfx->StagesDone >= BC250_GFX_STAGE_CP;
    if (Gfx->Sequence.TraceRlcRetirement) GfxTraceRlcState(Device,"before-gfx-teardown");
    TearDown(Gfx, Adev);
    if (Gfx->Sequence.TraceRlcRetirement) GfxTraceRlcState(Device,"before-gfx-memory-release");
    Gfx->Sequence.TraceRlcRetirement = FALSE;
    // An undo that failed leaves an engine that may still hold an address of ours: its pages stay (they go back with a
    // later undo that succeeds, or never), but the state is reset all the same, because the way out of this is the next
    // bring-up's recovery branch (bc250_kiq_init_register), not a second undo on a halted MEC.
    Device->GfxStopQuiet=Quiet && Undo==0 && NT_SUCCESS(Gfx->Sequence.Fault);
    GpuMemRelease(Device, &Gfx->Sequence, Device->GfxStopQuiet);
    if (Gfx->StagesDone >= BC250_GFX_STAGE_CP) GfxTraceRlcState(Device,"after-gfx-memory-release");
    if (Quiet) { Gfx->StagesDone = 0; Gfx->CpStepDone = 0; Gfx->Failed = FALSE; }
}

static BOOLEAN Fini(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev, _Out_ long* Undo)
{
    BOOLEAN quiet = HaltEngines(Device, Gfx, Adev, Undo);
    ReleaseStoppedStorage(Device, Gfx, Adev, quiet, *Undo);
    return quiet;
}

// Diagnostic-only synchronous snapshots during unpublished CP1 startup. The
// caller retains both locks at PASSIVE_LEVEL, with special APCs enabled.
static void GfxStartupCheckpoint(const char* Phase)
{
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL && !KeAreAllApcsDisabled());
    GuardLog("startup: CP1 checkpoint %s", Phase);
    GuardLogKeep();
}

// Same PASSIVE_LEVEL/special-APC contract as the CP1 checkpoint above.
static void GfxRlcCheckpoint(const char* Phase)
{
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL && !KeAreAllApcsDisabled());
    GuardLog("startup: RLC boundary %s", Phase);
    GuardLogKeep();
}

static void GfxExecute(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GFX* Data, ULONG CpStep)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE, plan = (Data->Op == BC250_GFX_OP_PLAN);
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER frequency, start;
    ULONG stage, first;
    long result = 0;
    BOOLEAN persistKiq = CpStep == BC250_CP_KIQ_INIT &&
                         KeGetCurrentIrql() == PASSIVE_LEVEL && !KeAreAllApcsDisabled();
    BOOLEAN persistRlc = !CpStep && Data->Op == BC250_GFX_OP_RUN &&
                         Device->FullWddm && !Device->Started &&
                         Data->LastStage == BC250_GFX_STAGE_CP-1 &&
                         KeGetCurrentIrql() == PASSIVE_LEVEL && !KeAreAllApcsDisabled();
    BOOLEAN persistStartup = persistKiq || persistRlc;

    Data->Version = BC250_KMD_VERSION;
    Data->Result = 0;
    Data->FailedStage = 0;
    Data->FaultOffset = 0;
    Data->StagesDone = 0;
    Data->WriteCount = 0;
    Data->DoorbellCount = 0;
    Data->StageCount = 0;
    Data->VramBytes = 0;
    Data->GttBytes = 0;
    RtlZeroMemory(Data->Stages, sizeof(Data->Stages));
    RtlZeroMemory(Data->Doorbells, sizeof(Data->Doorbells));
    RtlZeroMemory(Data->Writes, sizeof(Data->Writes));
    KeQueryPerformanceCounter(&frequency);

    if (persistKiq) GfxStartupCheckpoint("acquire-paging-lock");
    if (persistRlc) GfxRlcCheckpoint("acquire-paging-lock");
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    if (persistStartup) {
        if (persistKiq) GfxStartupCheckpoint("acquire-gart-lock");
        if (persistRlc) GfxRlcCheckpoint("acquire-gart-lock");
        // ExAcquireFastMutexUnsafe is permitted inside KeEnterCriticalRegion.
        // It preserves PASSIVE_LEVEL and special APC delivery for Zw file I/O.
        // The same mutex and lock order remain held throughout the sequence.
        ExAcquireFastMutexUnsafe(&Device->GartLock);
        if (persistKiq) GfxStartupCheckpoint("locks-acquired");
        if (persistRlc) GfxRlcCheckpoint("locks-acquired");
    } else ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (Data->Op > BC250_GFX_OP_STATE) status = STATUS_INVALID_PARAMETER;
    else if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else if (gfx->PowerSuspended && Data->Op != BC250_GFX_OP_STATE) status = STATUS_INVALID_DEVICE_STATE;
    else if (Data->Op <= BC250_GFX_OP_RUN && (Data->LastStage == 0 || Data->LastStage > BC250_GFX_STAGE_COUNT)) status = STATUS_INVALID_PARAMETER;
    // CpStep is internal to unpublished StartDevice, never supplied by an escape.
    if (NT_SUCCESS(status) && !CpStep && Data->Op == BC250_GFX_OP_RUN &&
        gfx->CpStepDone != 0 && gfx->CpStepDone < BC250_CP_COMPUTE_TEST)
        status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status) && CpStep &&
        (Device->GfxTlbBootstrap || Data->Op != BC250_GFX_OP_RUN || Data->LastStage != BC250_GFX_STAGE_CP ||
         CpStep > BC250_CP_COMPUTE_TEST || CpStep != gfx->CpStepDone + 1 ||
         gfx->StagesDone != (ULONG)(CpStep == BC250_CP_KIQ_INIT ? BC250_GFX_STAGE_CP - 1 : BC250_GFX_STAGE_CP)))
        status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status)) status = GartDevice(Device, &adev, &gartEnabled);
    if (NT_SUCCESS(status) && Data->Op != BC250_GFX_OP_STATE)
    {
        previousBackend = adev->backend;
        adev->backend = &gfx->Sequence;
        SequenceBegin(&gfx->Sequence, Device, plan, Data->Writes, BC250_GFX_MAX_WRITES);
        GpuMemBeginSequence(Device, Data->Doorbells, BC250_GFX_MAX_DOORBELLS);

        switch (Data->Op)
        {
        case BC250_GFX_OP_PLAN:
        case BC250_GFX_OP_RUN:
            // A plan is about hardware this driver instance has not touched; a run needs what amdgpu had at this point.
            // Interrupt sources only behind an enabled IH ring (amdgpu's order: navi10_ih_irq_init at 0.25 s, the sources at 1.56 s).
            if (plan ? (gfx->StagesDone != 0 || gfx->SetUp) : (!gartEnabled || !PspIsLoaded(Device) || gfx->Failed || (!CpStep && Data->LastStage <= gfx->StagesDone) ||
                                                              (Data->LastStage >= BC250_GFX_STAGE_INTERRUPTS && !IhIsActive(Device))))
            {
                status = STATUS_INVALID_DEVICE_STATE;
                break;
            }
            if (!plan) gfx->PagingCpuBootstrap = FALSE;
            result = SetUp(gfx, adev);
            if (result != 0) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            // OS submission needs its completion page before adapter admission.
            // The diagnostic IB page is unrelated: OS jobs carry their own IB.
            if (!plan && gfx->SubmitGate && !gfx->FencePage) {
                result=bc250_gfx_fence_page_alloc(adev);
                if (result!=0) { status=STATUS_INSUFFICIENT_RESOURCES; break; }
                gfx->FencePage=TRUE;
            }
            first = CpStep ? BC250_GFX_STAGE_CP : (plan ? 1 : gfx->StagesDone + 1);
            for (stage = first; stage <= Data->LastStage; stage++)
            {
                BC250_ESCAPE_GFX_STAGE* got = &Data->Stages[Data->StageCount++];

                got->Stage = stage;
                got->FirstWrite = gfx->Sequence.WriteCount;
                start = KeQueryPerformanceCounter(NULL);
                if (!plan) gfx->StagesDone = stage;         // from its first write on, the stage has touched the hardware
                gfx->Sequence.TraceBootstrapTlb = persistRlc;
                got->Result = CpStep ? bc250_gfx_cp_resume_step_traced(adev, CpStep,
                    persistKiq ? GfxStartupCheckpoint : NULL) : RunEngineStage(Device,adev,stage,
                        persistRlc ? GfxRlcCheckpoint : NULL);
                gfx->Sequence.TraceBootstrapTlb = FALSE;
                if (CpStep && got->Result == 0 && NT_SUCCESS(gfx->Sequence.Fault)) gfx->CpStepDone = CpStep;
                got->Microseconds = Microseconds(start, frequency);
                if (CpStep) GuardLog("gfx: CP step %lu: rc %d, %u writes, %u us", CpStep,
                                    got->Result, gfx->Sequence.WriteCount, got->Microseconds);
                else GuardLog("gfx: stage %u (%s)%s: rc %d, %u writes so far, %u us", stage, g_Stages[stage].Name, plan ? " planned" : "",
                              got->Result, gfx->Sequence.WriteCount, got->Microseconds);
                if (got->Result != 0 || !NT_SUCCESS(gfx->Sequence.Fault))
                {
                    result = got->Result;
                    Data->FailedStage = stage;
                    if (!plan) { gfx->Failed = TRUE; GrbmSelectDefault(Device); }
                    break;
                }
            }
            if (plan)
            {
                TearDown(gfx, adev);
                // The GPU never heard of this memory: no register or doorbell write was executed, and a PLAN's pages
                // are not entered into the GART table (gpumem.c).
                GpuMemRelease(Device, &gfx->Sequence, TRUE);
            }
            else
            {
            // ControlInterrupt often arms VUPDATE_NO_LOCK during StartDevice, before this bring-up's IH
            // ring exists, so the enable bit is set and nothing is delivered (the picture run's 0 acks).
            // Writing it again once stage 8 has enabled the sources is idempotent if the bit survived.
            // Independent of the paging-node capture below: a run opens both gates.
            if (Device->VidPnFlipEnabled && Device->DcnVsyncArmed &&
                gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS && !gfx->Failed)
            {
                (void)DcnVsyncEnable(Device, TRUE);
                GuardLog("gfx: hardware vsync re-armed after the interrupt sources");
            }
            // ADR 0008 stage D (docs/design/paging-node.md section 4): the first RUN to reach stage 8 with the
            // gate open captures what GfxSubmitPaging needs at DISPATCH_LEVEL - a live ring pointer and the
            // engine state - the same place as ih.c's DpcAdev. Once, not
            // on every call: PagingReady stays set until the next TearDown, which clears it first of everything
            // (matching SubmitAdev's own comment above).
            if (gfx->PagingGate && gfx->PagingCopyStaging.size>=PAGE_SIZE && !gfx->PagingReady && gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS &&
                     !gfx->Failed && NT_SUCCESS(gfx->Sequence.Fault))
            {
                gfx->PagingRing = &adev->sdma.instance[0].ring;
                gfx->PagingDevicePtr = adev;
                gfx->PagingWindowReady = adev->gart.bo != NULL &&
                    PagingWindowInit(adev->gmc.gart_start, adev->gmc.gart_size,
                                     adev->gart.bo->gpu_addr, adev->gart.table_size, &gfx->PagingWindow);
                gfx->PagingReady = TRUE;
                GuardLog("gfx: paging node ready, SDMA0 doorbell 0x%X, per-buffer private commands",
                         gfx->PagingRing->doorbell_index);
            }
            }
            break;

        case BC250_GFX_OP_FINI:
            if (gfx->StagesDone == 0 && !gfx->SetUp) { status = STATUS_INVALID_DEVICE_STATE; break; }
            // Review 13: this teardown, unlike GfxStop's, runs with the IH DPC alive, and wddm.c's fence check reads
            // the page it frees. Not while a submission is on the ring; after a watchdog failure nobody looks any more.
            WddmGpuFence(Device);           // a fence that has arrived is reported as one, not by the watchdog later
            if (gfx->SubmitInFlight != 0 && gfx->SubmitFailed == 0 && !GfxFenceArrived(Device, gfx->SubmitSeq))
            {
                GuardLog("gfx: fini refused, submission %lu is still on the ring", gfx->SubmitSeq);
                status = STATUS_DEVICE_BUSY;
                break;
            }
            if (!Fini(Device, gfx, adev, &result)) status = STATUS_IO_DEVICE_ERROR;
            break;
        }
        if (NT_SUCCESS(status)) status = gfx->Sequence.Fault;
        Data->FaultOffset = gfx->Sequence.FaultOffset;
        Data->WriteCount = gfx->Sequence.WriteCount;
        Data->DoorbellCount = GpuMemEndSequence(Device, &Data->VramBytes, &Data->GttBytes);
        gfx->Sequence.Writes = NULL;            // the caller's buffer goes away with this call
        gfx->Sequence.MaxWrites = 0;
        adev->backend = previousBackend;
    }
    if (Data->Op == BC250_GFX_OP_RUN && NT_SUCCESS(status) && gfx != NULL && !gfx->Failed &&
        gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS) GfxAccessOpen(Device);
    if (gfx != NULL) Data->StagesDone = gfx->StagesDone;
    GuardLog("gfx: op %u to stage %u -> 0x%08X, result %d, %u writes, %u doorbells", Data->Op, Data->LastStage, status, result,
             Data->WriteCount, Data->DoorbellCount);
    if (persistStartup) ExReleaseFastMutexUnsafe(&Device->GartLock);
    else ExReleaseFastMutex(&Device->GartLock);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();

    Data->Result = result;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// Diagnostic commands and device startup share the same implementation.
void GfxEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GFX* Data)
{
    GfxExecute(Device,Data,0);
}

// Opt-in hardware control before WDDM publication, never alongside OS submissions.
// Keep the external full-WDDM escape allow-list observational. A failure takes
// the ordinary startup unwind, which owns all resources until engine retirement.
static NTSTATUS SdmaVaStartupControl(BC250_DEVICE* Device);

static NTSTATUS SdmaIbStartupControl(BC250_DEVICE* Device)
{
    BC250_ESCAPE_SDMACOPY control;
    ULONG trial;
    if (GuardReadSetting(L"EnableSdmaIbControl",0)==0 &&
        GuardReadSetting(L"EnableSdmaVaControl",0)==0) return STATUS_SUCCESS;
    if (!Device->FullWddm || Device->Started || Device->Wddm != NULL)
        return STATUS_INVALID_DEVICE_STATE;
    for (trial=0;trial<4;trial++) {
        RtlZeroMemory(&control,sizeof(control));
        control.Magic=BC250_ESCAPE_MAGIC;
        control.Command=(trial==0 || trial==3) ? BC250_ESCAPE_RUN_SDMACOPY : BC250_ESCAPE_RUN_SDMAIB;
        control.Bytes=trial<2 ? BC250_SDMACOPY_DEFAULT_BYTES : BC250_SDMACOPY_MAX_BYTES;
        GuardLog("startup: SDMA control %lu command %lu bytes %lu begin",trial,control.Command,control.Bytes);
        GuardLogKeep();
        SdmaCopyEscape(Device,&control);
        GuardLog("startup: SDMA control %lu status %lu NT 0x%08lX result %ld fence %lu/%lu compare %lu matched %lu",
                 trial,control.Status,control.NtStatus,control.Result,control.LastSeq,control.LastValue,
                 control.BytesCompared,control.Matched);
        GuardLogKeep();
        if (!NT_SUCCESS((NTSTATUS)control.NtStatus)) return (NTSTATUS)control.NtStatus;
        if (control.Status!=BC250_ESCAPE_STATUS_DONE || control.Result!=0 ||
            control.LastSeq==0 || control.LastValue!=control.LastSeq ||
            control.BytesCompared!=control.Bytes || !control.Matched) return STATUS_IO_DEVICE_ERROR;
    }
    if (GuardReadSetting(L"EnableSdmaVaControl",0)!=0) {
        NTSTATUS status;
        GuardLog("startup: SDMA VMID2 translated control begin");
        GuardLogKeep();
        status=SdmaVaStartupControl(Device);
        GuardLogKeep();
        return status;
    }
    return STATUS_SUCCESS;
}

// PASSIVE_LEVEL. Caller owns nonpaged report storage through this synchronous
// call. Keep detailed partial-progress output for the eventual startup unwind.
NTSTATUS GfxInitializeHardware(BC250_DEVICE* Device, BC250_ESCAPE_GFX* Report)
{
    NTSTATUS status;
    ULONG stage, first, cp;
    BOOLEAN trace;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_GFX;
    Report->Op=BC250_GFX_OP_RUN;
    Report->LastStage=BC250_GFX_STAGE_INTERRUPTS;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        trace=GuardReadSetting(L"KeepLog",0)!=0;
        first=trace?1:BC250_GFX_STAGE_INTERRUPTS;
        status=STATUS_SUCCESS;
        // Startup owns the unpublished device. Incremental RUN already preserves
        // setup and completed stages. Outer snapshots run outside GfxExecute's
        // locks. Selected CP1/RLC boundaries keep PASSIVE_LEVEL and special APCs
        // inside the same locks via the unsafe-fast-mutex path.
        // In trace mode Report describes the last attempted
        // stage; the persisted snapshots retain earlier detailed stage reports.
        for (stage=first;stage<=BC250_GFX_STAGE_INTERRUPTS;stage++) {
            Report->LastStage=stage;
            if (trace) {
                GuardLog("startup: entering GFX stage %lu",stage);
                GuardLogKeep();
            }
            if (trace && stage == BC250_GFX_STAGE_CP) {
                for (cp=BC250_CP_KIQ_INIT;cp<=BC250_CP_COMPUTE_TEST;cp++) {
                    GuardLog("startup: entering CP step %lu",cp);
                    GuardLogKeep();
                    GfxExecute(Device,Report,cp);
                    GuardLog("startup: CP step %lu returned 0x%08X result %ld",cp,Report->NtStatus,Report->Result);
                    GuardLogKeep();
                    if (!NT_SUCCESS((NTSTATUS)Report->NtStatus) || Report->Status!=BC250_ESCAPE_STATUS_DONE ||
                        Report->Result!=0 || Report->FailedStage) break;
                }
            } else GfxExecute(Device,Report,0);
            status=(NTSTATUS)Report->NtStatus;
            if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
                Report->FailedStage || Report->StagesDone<stage)) status=STATUS_IO_DEVICE_ERROR;
            if (trace) GuardLogKeep();
            if (!NT_SUCCESS(status)) break;
        }
    }
    if (NT_SUCCESS(status)) status=SdmaIbStartupControl(Device);
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}


// ---- stage C: one indirect buffer on the gfx ring (ADR 0008) --------------------------------------------------------------
//
// The ring side of a submission, so that wddm.c keeps its distance from the shim's types. Everything it does is in
// driver/shim: bc250_gmc_set_vmid_pd() points a VMID's page directory at the submitting process's root and invalidates
// it by MMIO, bc250_gfx_submit_ib() writes PACKET3_INDIRECT_BUFFER and an interrupting RELEASE_MEM into the gfx ring
// and rings the doorbell. What is here is the policy around them.
//
// GFX jobs now reserve aligned space against the CP read pointer. The completion
// queue in wddm.c owns scheduler fences; this layer keeps a cumulative HW sequence.
// VMID root changes still drain prior work because the flush is CPU MMIO.
//
// Nothing here waits for the GPU. GfxSubmitIb() returns as soon as the doorbell is rung; the completion is the
// end-of-pipe interrupt, which ih.c's DPC turns into a GfxFenceArrived() call.

static BOOLEAN GfxSubmitArmed(_In_ const BC250_GFX* gfx)
{
    // Stage 8 and not 6: the fence carries AMDGPU_FENCE_FLAG_INT, and without the interrupt sources of stage 8 the
    // completion would never be reported, only polled. Without the IH ring stage 8 cannot have run at all (GfxEscape).
    return gfx != NULL && gfx->SubmitGate && gfx->SetUp && !gfx->PowerSuspended && !gfx->Failed && gfx->SubmitFailed == 0 &&
           gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS;
}

static BOOLEAN GfxSubmitReadyAccess(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    return GfxSubmitArmed(gfx) && gfx->SubmitInFlight == 0;
}

BOOLEAN GfxSubmitReady(_In_ const BC250_DEVICE* Device)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxSubmitReadyAccess(Device);
    GfxAccessRelease(Device);
    return result;
}

static BOOLEAN GfxSubmitBusyAccess(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    // Outstanding GPU work. The UMD path may still enqueue into available ring
    // and completion slots, or wait for capacity/root-switch retirement.
    return GfxSubmitArmed(gfx) && gfx->SubmitInFlight != 0;
}

BOOLEAN GfxSubmitBusy(_In_ const BC250_DEVICE* Device)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxSubmitBusyAccess(Device);
    GfxAccessRelease(Device);
    return result;
}

static void GfxSubmitFailAccess(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    if (gfx == NULL) return;
    // Once, however often the caller says it: a watchdog that fires twice should not fill the log ring. The write
    // pointer is left where it stands, because facts M59/M60 say a hardware pointer only counts up and a re-init has
    // to adopt it, so abandoning the ring is the safe act and rewinding it is not.
    if (InterlockedExchange(&gfx->SubmitFailed, 1) == 0)
        GuardLog("gfx: submission path failed, no further ring writes this device start (seq %lu in flight, slot 0x%X)",
                 gfx->SubmitSeq, gfx->SubmitAdev != NULL ? (ULONG)bc250_gfx_fence_read(gfx->SubmitAdev, BC250_SUBMIT_FENCE_SLOT) : 0);
    // KMD196: unconditionally, not only on the first call. A held submission is waiting for a fence that will
    // now never come; GfxSubmitArmed is false from here, so it wakes, fails its submission and lets the OS TDR
    // path have the fence. Before this it sat out its whole 500 ms bound after the ring was already abandoned.
    GfxRetireSignal(Device);
}

// D5: the count of per-submit lines HotSubmitLog left out. Read through the same access gate as every other
// answer about gfx state, so a stop in flight gives 0 rather than touching freed state.
ULONG GfxHotSubmitLinesSkipped(_In_ const BC250_DEVICE* Device)
{
    BC250_GFX* gfx = GfxAccessAcquire(Device);
    ULONG skipped;

    if (gfx == NULL) return 0;
    skipped = (ULONG)InterlockedCompareExchange(&gfx->HotSubmitLinesSkipped, 0, 0);
    GfxAccessRelease(Device);
    return skipped;
}

// BD-097: the paging submit line's own tally - submits, lines left out and summary lines - for the log summary,
// through the same access gate. Read under PagingLogLock, so the three numbers are one moment.
void GfxPagingLogCounts(_In_ const BC250_DEVICE* Device, _Out_ ULONG* Submits, _Out_ ULONG* Skipped,
                        _Out_ ULONG* Summaries)
{
    BC250_GFX* gfx = GfxAccessAcquire(Device);
    KIRQL irql;

    *Submits = *Skipped = *Summaries = 0;
    if (gfx == NULL) return;
    KeAcquireSpinLock(&gfx->PagingLogLock, &irql);
    *Submits = gfx->PagingLogRate.Calls;
    *Skipped = gfx->PagingLogRate.Skipped;
    *Summaries = gfx->PagingLogRate.Summaries;
    KeReleaseSpinLock(&gfx->PagingLogLock, irql);
    GfxAccessRelease(Device);
}

void GfxSubmitFail(_Inout_ BC250_DEVICE* Device)
{
    StartHealthFault(Device);
    if (GfxAccessAcquire(Device) == NULL)
    {
        return;
    }
    GfxSubmitFailAccess(Device);
    GfxAccessRelease(Device);
}

// KMD196. The generation is bumped before the event is set, so a waiter that cleared the event and then found
// the generation moved retests instead of sleeping through its own wake, and a second waiter's clear cannot
// swallow the first one's. IO_NO_INCREMENT: a submit thread owes nothing to the DPC that woke it.
void GfxRetireSignal(_Inout_ BC250_DEVICE* Device)
{
    InterlockedIncrement(&Device->GfxRetireGeneration);
    KeSetEvent(&Device->GfxRetireEvent, IO_NO_INCREMENT, FALSE);
}

// M15.12 stage 1 (docs/design/hang-recovery.md). The one place that undoes GfxSubmitFail, and only
// when the ring has provably drained: this is called from DxgkDdiResetEngine (PASSIVE_LEVEL, GPU-scheduler class,
// so no concurrent submit) under HangRecoveryMode, never on the normal path, after the caller has put a pending
// record on the disk (GuardRecordHangRecovery).
//
// amdgpu_ring_soft_recovery's loop (hang_recovery.h Bc250HangKillLoop): while the fence has not signalled and the
// 10 ms budget lasts, issue amdgpu's gfx_v10_0_ring_soft_recovery (bc250_gfx_soft_recover_vmid: SQ_CMD KILL
// broadcast on Vmid). Like upstream it looks before it kills, so a sequence that retired on its own gets no kill at
// all. The sequence waited for is the newest one emitted: the ring runs in order, so its retiring means every job
// before it retired too - the end-of-pipe behind the killed waves fired and the ring drained to idle, and only then
// may the node take new work. Deviation from upstream: a 100 us stall between kills (umr's retry cadence) where
// amdgpu spins; same 10 ms bound.
//
// The register path. Every MMIO access of the shim goes through adev->backend, and the backend checks the offset
// against its sequence's table. SQ_CMD is in the GFX table only (gen_regs.py), so the kill runs the way every other
// GFX operation runs (GfxSubmitIb): under GartLock, with the gfx sequence installed as the backend for the loop and
// put back after it. 0.7.216.16 issued the kill through whatever backend was installed, which is the GART
// sequence: lab trial D1 (2026-10-07) logged "gart: register 0x08DEC refused (0xC0000022), sequence stopped" at
// the first kill and 95 kills of which none reached the register. A kill counts only when the sequence has
// recorded no fault after it, and the first refusal ends the loop with its own log line.
//
// Vmid is the hung job's VMID, chosen by the caller out of the completion-queue entry and checked against
// Bc250KillVmidValid: with the VMID pool (0.7.214, vmid_pool.h) there is no single application VMID any more, so
// this function must not assume one. On ALREADY_RETIRED or DRAINED it clears SubmitInFlight (the ring is idle),
// un-sticks SubmitFailed (the gate reopens), ends the DPM busy share the way GfxFenceArrived would have (DpmBusyEnd
// is a no-op if that already ran) and signals the retire waiters, because a submission held in GfxSubmitWait is
// waiting for exactly this gate to reopen (KMD196). On NOT_DRAINED it changes nothing: the sticky state stands and
// the caller falls back to today's refusal.
typedef struct _GFX_KILL_CONTEXT {
    BC250_GFX* Gfx;
    struct amdgpu_device* Adev;
    ULONG Seq;
    ULONG Vmid;
    ULONGLONG Deadline;
} GFX_KILL_CONTEXT;

static int GfxKillRetired(void* Context)
{
    GFX_KILL_CONTEXT* c = (GFX_KILL_CONTEXT*)Context;
    return bc250_fence_reached((ULONG)bc250_gfx_fence_read(c->Adev, BC250_SUBMIT_FENCE_SLOT), c->Seq);
}

// KeQueryInterruptTime is 100 ns units. KeStallExecutionProcessor, not KeDelayExecutionThread: the kill must be
// re-issued in a tight loop and the total is bounded to 10 ms, which a stall at APC_LEVEL (GartLock) may hold.
static int GfxKillExpired(void* Context)
{
    return KeQueryInterruptTime() >= ((GFX_KILL_CONTEXT*)Context)->Deadline;
}

// The fence page is memory, not a register: GfxKillRetired reads it whatever the backend. The kill is a register
// write, and the gfx sequence's Fault says whether it reached the register (sequence.c drops every write after
// the first refusal).
static int GfxKillIssue(void* Context)
{
    GFX_KILL_CONTEXT* c = (GFX_KILL_CONTEXT*)Context;
    bc250_gfx_soft_recover_vmid(c->Adev, c->Vmid);
    return NT_SUCCESS(c->Gfx->Sequence.Fault);
}

static void GfxKillStall(void* Context)
{
    UNREFERENCED_PARAMETER(Context);
    KeStallExecutionProcessor(100);     // 100 us, like umr's retry cadence
}

static const BC250_HANG_KILL_OPS g_GfxKillOps = { GfxKillRetired, GfxKillExpired, GfxKillIssue, GfxKillStall };

ULONG GfxSoftRecover(_Inout_ BC250_DEVICE* Device, ULONG Vmid, _Out_ ULONG* Seq, _Out_ ULONG* Kills,
                     _Out_ ULONG* Micros)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    void* previousBackend;
    BOOLEAN gartEnabled = FALSE;
    GFX_KILL_CONTEXT kill;
    NTSTATUS status, fault;
    ULONG faultOffset;
    ULONGLONG start;
    unsigned kills = 0;
    int refused = 0;
    ULONG verdict;

    *Seq = 0;
    *Kills = 0;
    *Micros = 0;
    // The caller checked this, but the kill is a broadcast register write: never issue one on a VMID this file
    // cannot name as an application's (0 is the GART domain, 2 is SDMA paging's). Unreachable from wddm.c, whose
    // pre-kill verdict refuses first; the same verdict is returned so the record cannot read as a failed kill.
    if (!Bc250KillVmidValid(Vmid))
    {
        GuardLog("gfx: soft recovery refused: vmid %lu is not an application VMID; nothing changed", Vmid);
        return BC250_HANG_VERDICT_VMID_GUARD;
    }
    // GfxSubmitIb's preamble. GartLock, not GfxAccessAcquire: lifecycle writers hold GartLock while they change
    // Device->Gfx, and a GfxAccess holder may not take GartLock (the rule above GfxAccessAcquire).
    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else status = GartDevice(Device, &adev, &gartEnabled);
    if (NT_SUCCESS(status) && !gartEnabled) status = STATUS_INVALID_DEVICE_STATE;
    if (!NT_SUCCESS(status))
    {
        ExReleaseFastMutex(&Device->GartLock);
        GuardLog("gfx: soft recovery found no ring to kill on (0x%08lX); nothing changed", (ULONG)status);
        return BC250_HANG_VERDICT_NOT_DRAINED;
    }
    *Seq = gfx->SubmitSeq;
    // SubmitAdev is the adev of the last submission, which SubmitIbLocked got from GartDevice: the same object.
    if (gfx->SubmitSeq == 0 || gfx->SubmitAdev != adev)
    {
        ExReleaseFastMutex(&Device->GartLock);
        GuardLog("gfx: soft recovery found no submission to retire (seq %lu); nothing changed", *Seq);
        return BC250_HANG_VERDICT_NOT_DRAINED;
    }

    kill.Gfx = gfx;
    kill.Adev = adev;
    kill.Seq = gfx->SubmitSeq;
    kill.Vmid = Vmid;
    // amdgpu's own 10 ms budget (amdgpu_ring_soft_recovery: ktime_add_us(.., 10000)).
    start = KeQueryInterruptTime();
    kill.Deadline = start + (ULONGLONG)BC250_SOFT_RECOVER_US * 10ull;
    previousBackend = adev->backend;
    adev->backend = &gfx->Sequence;
    SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
    verdict = Bc250HangKillLoop(&g_GfxKillOps, &kill, &kills, &refused);
    fault = gfx->Sequence.Fault;
    faultOffset = gfx->Sequence.FaultOffset;
    adev->backend = previousBackend;
    *Kills = kills;
    *Micros = (ULONG)((KeQueryInterruptTime() - start) / 10ull);

    if (verdict != BC250_HANG_VERDICT_NOT_DRAINED) {
        // The ring went idle: clear the in-flight marker if it is still this sequence, and reopen the gate.
        LONG pending = InterlockedCompareExchange(&gfx->SubmitInFlight, 0, 0);
        if (pending != 0 && bc250_fence_reached((ULONG)bc250_gfx_fence_read(adev, BC250_SUBMIT_FENCE_SLOT), (ULONG)pending))
            InterlockedCompareExchange(&gfx->SubmitInFlight, 0, pending);
        InterlockedExchange(&gfx->SubmitFailed, 0);     // the one un-stick, only on the proven-drained path
        DpmBusyEnd(&Device->Dpm);                       // match the retirement the normal fence path would have reported
        GfxRetireSignal(Device);                        // KMD196: a held submission is waiting for this gate
    }
    ExReleaseFastMutex(&Device->GartLock);

    // A refusal says which register and why, because a refused kill and a kill the waves outlived look the same
    // in the record (verdict 2): only Kills tells them apart.
    if (refused)
        GuardLog("gfx: soft recovery kill refused: register 0x%05lX (0x%08lX) after %lu kill(s) of VMID %lu",
                 faultOffset, (ULONG)fault, *Kills, Vmid);
    if (verdict != BC250_HANG_VERDICT_NOT_DRAINED)
        GuardLog("gfx: soft recovery: seq %lu retired after %lu kill(s) of VMID %lu waves in %lu us; ring reopened",
                 *Seq, *Kills, Vmid, *Micros);
    else
        GuardLog("gfx: soft recovery of seq %lu did not drain: %lu kill(s) of VMID %lu in %lu us; ring stays closed",
                 *Seq, *Kills, Vmid, *Micros);
    return verdict;
}

static BOOLEAN GfxFenceArrivedAccess(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    // One read of a GTT page the CP writes. The outer lifetime reference protects
    // the device and fence page against concurrent FINI or Stop. SubmitAdev is only ever non-NULL between a submission and the teardown that frees
    // the page, and pnp.c drains ih.c's DPCs before that teardown runs (see the field's comment).
    if (gfx == NULL || gfx->SubmitAdev == NULL || Seq == 0) return FALSE;
    {
        ULONG observed = (ULONG)bc250_gfx_fence_read(gfx->SubmitAdev, BC250_SUBMIT_FENCE_SLOT);
        LONG pending = InterlockedCompareExchange(&gfx->SubmitInFlight, 0, 0);
        if (!bc250_fence_reached(observed, Seq)) return FALSE;
        // Store the outstanding sequence, not a boolean. An old DPC may not
        // clear a newer producer's marker after observing an earlier fence.
        if (pending && bc250_fence_reached(observed, (ULONG)pending) &&
            InterlockedCompareExchange(&gfx->SubmitInFlight, 0, pending) == pending)
        {
            DpmBusyEnd(&Device->Dpm);     // the ring went idle: the DPM governor's busy share (dpm.h)
            // KMD196: this CAS is the one place that turns "a job is in flight" into "none is", which is exactly
            // the condition SubmitIbLocked refuses a foreign root on. Whoever observed the fence - the IH DPC,
            // the watchdog, a submit - the waiters are woken from here.
            GfxRetireSignal(Device);
        }
    }
    return TRUE;
}

BOOLEAN GfxFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxFenceArrivedAccess(Device, Seq);
    GfxAccessRelease(Device);
    return result;
}

// The bring-up read's reader. The gfx table holds the page-table base pairs of VMIDs 1..15 and the GART table the
// pair of VMID 0, which gart.c programs; both lists are generated from the register headers, and the offsets come
// from the shim's hub table (bc250_gmc_get_vmid_pd), so nothing here names an address.
static NTSTATUS GfxVmidProbeRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    if (NT_SUCCESS(MmioGfxRead(Device, Offset, Value))) return STATUS_SUCCESS;
    return MmioGartRead(Device, Offset, Value);
}

// Once per device start and before this driver writes the page-table base of any VMID in the pool: the first
// SubmitIbLocked with the pool open is that point, because nothing else in this driver programs VMIDs 1 or 3..15
// (VMID 0 is the GART aperture, programmed by bc250_gmc_gart_enable; VMID 2 is node 1's, programmed per paging
// buffer). Reads only. A VMID in 3..15 that reads non-zero was programmed by something that is not this instance
// of the driver (firmware, or an earlier start in the same boot), and it stays out of the pool. VMID 1 stays in:
// it is the single-VMID driver's own and every earlier start of ours wrote it. A pair that cannot be read is
// treated as programmed. The caller holds GartLock and the gfx sequence is Adev->backend with no fault recorded;
// a refused read would record one, so each is undone here - a read changes nothing on the hardware.
static void GfxVmidProbe(_Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev)
{
    BC250_SEQUENCE* sequence = (BC250_SEQUENCE*)Adev->backend;
    NTSTATUS (*read)(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value);
    unsigned long long value[BC250_VMID_COUNT];
    unsigned excluded = 0, members, set = 0, unread = 0, v;
    KIRQL irql;

    if (Gfx->VmidProbed || sequence == NULL || !NT_SUCCESS(sequence->Fault)) return;
    Gfx->VmidProbed = TRUE;
    read = sequence->Read;
    sequence->Read = GfxVmidProbeRead;
    for (v = 0; v < BC250_VMID_COUNT; v++)
    {
        u64 pair = 0;
        if (bc250_gmc_get_vmid_pd(Adev, v, &pair) != 0 || !NT_SUCCESS(sequence->Fault))
        {
            pair = 0;
            unread |= 1u << v;
            sequence->Fault = STATUS_SUCCESS;
            sequence->FaultOffset = 0;
        }
        value[v] = pair;
        if (pair != 0) set |= 1u << v;
    }
    sequence->Read = read;
    members = (Bc250VmidPoolFromProbe(value, &excluded) & ~unread) | (1u << BC250_VMID_LEGACY);
    excluded |= unread & BC250_VMID_CANDIDATES & ~(1u << BC250_VMID_LEGACY);
    KeAcquireSpinLock(&Gfx->VmidLock, &irql);
    Gfx->VmidMembers = (USHORT)members;
    Gfx->VmidExcluded = (USHORT)excluded;
    KeReleaseSpinLock(&Gfx->VmidLock, irql);
    GuardLog("gfx: VMID bring-up read: non-zero 0x%04lX unread 0x%04lX, pool 0x%04lX (%lu VMIDs), excluded 0x%04lX",
             (ULONG)set, (ULONG)unread, (ULONG)members, (ULONG)Bc250VmidCount(members), (ULONG)excluded);
    for (v = 0; v < BC250_VMID_COUNT; v++)
        if (value[v] != 0)
            GuardLog("gfx: VMID %lu base 0x%llX at bring-up: %s", (ULONG)v, value[v],
                     v == BC250_VMID_GART ? "GART aperture, reserved" :
                     v == BC250_VMID_SDMA_PAGING ? "SDMA paging, reserved" :
                     v == BC250_VMID_LEGACY ? "VMID 1, kept" : "excluded from the pool");
}

// With GartLock held, the gfx sequence installed as adev->backend and a GpuMem sequence open. GfxSubmitIb is this plus
// all three; GfxFenceEscape's IB_AT mode calls it directly, because it already holds them.
//
// Vmid is BC250_VMID_AUTO from the WDDM path and an explicit 0..15 from the IB_AT escape. Which VMID the job runs
// at, and whether it may run now, is Bc250VmidAdmit's decision (vmid_pool.h). With EnableVmidPool 0, AUTO is VMID 1
// and the decision is the predicate of 0.7.213.1, checked at the same point as before. With the pool, a root keeps
// its VMID while it keeps submitting, and a new root takes the least recently used VMID whose last job has retired.
// *VmidUsed is the VMID that the IB packet carries, 0 when nothing was submitted.
static NTSTATUS SubmitIbLocked(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev,
                               ULONG Vmid, ULONGLONG RootPhysical, ULONGLONG GpuAddress, ULONG SizeBytes,
                               _In_opt_ const BC250_GFX_SUBMIT_IDENTITY* Identity, _Out_ ULONG* Seq,
                               _Out_opt_ ULONG* VmidUsed)
{
    struct amdgpu_ring* ring = &Adev->gfx.gfx_ring[0];      // BC250_FENCE_RING_GFX; the only ring that takes an IB here
    ULONG seq, previousSeq, observed;
    LONG previousPending;
    BOOLEAN inFlight;
    BC250_VMID_DECISION decision;
    KIRQL irql;
    u64 address;
    long result;

    *Seq = 0;
    if (VmidUsed != NULL) *VmidUsed = 0;
    // VMID2 belongs to SDMA paging; graphics and IB_AT must not change its root.
    if (Vmid == BC250_SDMA_PAGING_VMID) return STATUS_ACCESS_DENIED;
    if (Gfx->SubmitFailed) return STATUS_DEVICE_HARDWARE_ERROR;
    if (!Gfx->SubmitGate) return STATUS_ACCESS_DENIED;
    if (Gfx->Failed || !Gfx->SetUp || Gfx->StagesDone < BC250_GFX_STAGE_INTERRUPTS) return STATUS_INVALID_DEVICE_STATE;
    // The shim refuses the same three things, but a size in bytes is this file's unit, so the division is checked here:
    // an odd length would otherwise become a shorter IB rather than an error.
    if (SizeBytes == 0 || (SizeBytes & 3) != 0 || (Vmid != BC250_VMID_AUTO && Vmid >= BC250_VMID_COUNT))
        return STATUS_INVALID_PARAMETER;

    // Whether a job is still running. GfxFenceArrived also clears the in-flight mark when the newest job retired.
    inFlight = Gfx->SubmitInFlight != 0 && !GfxFenceArrived(Device, Gfx->SubmitSeq);
    // EnableVmidPool 0, or an explicit VMID: 0.7.213.1's refusal, at 0.7.213.1's point. Queue only jobs sharing the
    // current VMID1 root. A CPU MMIO root change must never redirect an earlier job still using that VMID. VMID0
    // diagnostics remain exclusive. Bc250VmidAdmit below repeats this predicate for these callers.
    if ((Vmid != BC250_VMID_AUTO || !Gfx->VmidPoolGate) && inFlight &&
        ((Vmid != BC250_VMID_AUTO && Vmid != BC250_VMID_LEGACY) || Gfx->SubmitVmid != BC250_VMID_LEGACY ||
         Gfx->Vmid.Root[BC250_VMID_LEGACY] != RootPhysical)) return STATUS_DEVICE_BUSY;
    // GFX10 writeback is a 32-bit dword pointer; use a full aligned slot for
    // either frame. Do this before root writes or sequence publication.
    if (!bc250_ring_has_space(ring, ring->funcs->align_mask + 1u)) return STATUS_DEVICE_BUSY;
    // Before the first write of a pool VMID's root, and before the fence page: the reads go first in the sequence.
    if (Vmid == BC250_VMID_AUTO && Gfx->VmidPoolGate) GfxVmidProbe(Gfx, Adev);
    ring->track_rptr = true;

    if (!Gfx->FencePage)
    {
        result = bc250_gfx_fence_page_alloc(Adev);
        if (result != 0) return STATUS_INSUFFICIENT_RESOURCES;
        Gfx->FencePage = TRUE;
    }
    address = bc250_gfx_fence_addr(Adev, BC250_SUBMIT_FENCE_SLOT);
    if (address == 0) return STATUS_INSUFFICIENT_RESOURCES;

    // The VMID. One read of the fence slot answers "has the last job of VMID v retired" for every v, because the
    // fence is global and in order (vmid_pool.h).
    observed = (ULONG)bc250_gfx_fence_read(Adev, BC250_SUBMIT_FENCE_SLOT);
    Bc250VmidSweep(&Gfx->Vmid, observed);
    decision = Bc250VmidAdmit(&Gfx->Vmid, Gfx->VmidMembers, Gfx->VmidPoolGate, Vmid, RootPhysical, inFlight,
                              Gfx->SubmitVmid, observed);
    if (decision.Verdict == BC250_VMID_REFUSE_PARAM) return STATUS_INVALID_PARAMETER;
    if (decision.Verdict == BC250_VMID_REFUSE_BUSY)
    {
        if (decision.Pool) InterlockedIncrement(&Gfx->VmidBusy);
        return STATUS_DEVICE_BUSY;
    }
    if (decision.Verdict == BC250_VMID_REFUSE_RULE)
    {
        // The rule of the pool, checked on every path that can write a root: the root of a VMID whose last job has
        // not retired is never rewritten. Nothing the chooser picks gets here (the host test model-checks it).
        // Refused before the write, logged once, counted always; the caller sees the old wait.
        InterlockedIncrement(&Gfx->VmidRuleRefusals);
        if (InterlockedExchange(&Gfx->VmidRuleLogged, 1) == 0)
            GuardLog("gfx: VMID %lu root 0x%llX -> 0x%llX REFUSED: its job %lu has not retired (fence %lu)",
                     (ULONG)decision.Vmid, Gfx->Vmid.Root[decision.Vmid], RootPhysical,
                     (ULONG)Gfx->Vmid.LiveSeq[decision.Vmid], observed);
        return STATUS_DEVICE_BUSY;
    }
    Vmid = decision.Vmid;

    // VMID 0 is the GART aperture, whose root bc250_gmc_gart_enable() programmed and which bc250_gmc_set_vmid_pd()
    // refuses to touch; a caller submitting at VMID 0 is submitting out of the driver's own GTT pages. A job
    // flushes its VMID on every submit, same root or not. Remembering the root misses a leaf change under the
    // same root, and the invalidation is what a real job's VM flush is for. It polls for up to 100 ms.
    if (Vmid != 0)
    {
        ProgressEnterInput(ProgressSiteVmFlush, (LONG)Vmid);
        result = bc250_gmc_set_vmid_pd(Adev, Vmid, RootPhysical, 0);
        ProgressExit(ProgressSiteVmFlush, (LONG)result);
        // D5: the flush of a submit that worked is the third hot line. A flush that did not work keeps its line
        // whatever the gate says - that one is not noise, it is the fault.
        if (Gfx->HotSubmitLog || result != 0 || !NT_SUCCESS(Gfx->Sequence.Fault))
            GuardLog("gfx: VMID %lu root 0x%llX flush -> %d", Vmid, RootPhysical, result);
        else InterlockedIncrement(&Gfx->HotSubmitLinesSkipped);
        if (result != 0 || !NT_SUCCESS(Gfx->Sequence.Fault))
            return NT_SUCCESS(Gfx->Sequence.Fault) ? STATUS_DEVICE_HARDWARE_ERROR : Gfx->Sequence.Fault;
        // The table follows the register. A claim moves the previous tenant into the history first.
        if (decision.Claim)
        {
            KeAcquireSpinLock(&Gfx->VmidLock, &irql);
            Bc250VmidClaim(&Gfx->Vmid, &Gfx->VmidHistory, Vmid, RootPhysical);
            KeReleaseSpinLock(&Gfx->VmidLock, irql);
            if (decision.Pool) InterlockedIncrement(&Gfx->VmidClaims);
        }
        else if (decision.Pool) InterlockedIncrement(&Gfx->VmidReuses);
    }

    seq = (ULONG)InterlockedIncrement(&Gfx->FenceSeq);
    if (seq == 0) seq = (ULONG)InterlockedIncrement(&Gfx->FenceSeq);  // 0 means "nothing in flight" to GfxFenceArrived
    // Both set before the doorbell: the end-of-pipe interrupt can arrive inside bc250_gfx_submit_ib().
    previousSeq = Gfx->SubmitSeq;
    previousPending = InterlockedCompareExchange(&Gfx->SubmitInFlight, 0, 0);
    Gfx->SubmitSeq = seq;
    Gfx->SubmitAdev = Adev;
    InterlockedExchange(&Gfx->SubmitInFlight, (LONG)seq);

    // The ring test (VMID 0) stays one IB and one fence. A UMD job gets the gfx
    // job frame. No memory-sync packet: BC2S leaves ib_flags 0, and upstream
    // emits that packet only for AMDGPU_IB_FLAG_EMIT_MEM_SYNC.
    if (Vmid == 0)
        result = bc250_gfx_submit_ib(ring, GpuAddress, SizeBytes / 4, Vmid, address, seq, AMDGPU_FENCE_FLAG_INT);
    else
    {
        // D5: the two per-submit lines are behind HotSubmitLog. They say nothing that changes between
        // submissions except the identity, and at 1268 submissions a second they spend the whole log ring on
        // themselves. A fault hunt turns the gate on for its run and gets them back unchanged.
        if (Gfx->HotSubmitLog)
        {
            GuardLog("gfx: job frame C0004200 00000000  C0012800 81018003 00000000  C0009000 00000000  IB  C0009000 10000000  fence  C0008B00 00000000");
            // KMD193: the one line that names the submitter of the job frame above. 245 found the faulting job in
            // the ring with nothing anywhere to say whose context it was on. KMD214: and the VMID the job runs at.
            // The node left the line to make room within the 159 characters of a log entry: an identity reaches
            // this function only from node 0 (wddm.c WddmSubmitHardware), and the journal record keeps it.
            if (Identity != NULL)
                GuardLog("gfx: job seq %lu vmid %lu fence %lu ib 0x%llX x%lu ctx 0x%llX pid %lu ctxflags 0x%lX",
                         seq, Vmid, Identity->Fence, GpuAddress, SizeBytes / 4, Identity->Context,
                         Identity->ProcessId, Identity->ContextFlags);
        }
        // Two lines, not one, whenever there is an identity to name: the summary says "lines left out", and a
        // count of skip events would understate it by up to a factor of two, which is exactly the kind of quiet
        // number BD-070 taught us not to publish.
        else InterlockedExchangeAdd(&Gfx->HotSubmitLinesSkipped, (Identity != NULL) ? 2 : 1);
        // The VMID goes into the IB packet's control word (PACKET3_INDIRECT_BUFFER__VMID, bc250_gfx_emit_ib): the
        // CP fetches the IB, and the job makes every access, through this VMID's page tables.
        result = bc250_gfx_submit_job(ring, GpuAddress, SizeBytes / 4, Vmid, address, seq, AMDGPU_FENCE_FLAG_INT);
    }
    if (result != 0 || !NT_SUCCESS(Gfx->Sequence.Fault))
    {
        // Nothing was committed: both emitters refuse before writing and bc250_gfx_submit_ib undoes the allocation.
        // A claim above stands: the register holds the new root, the table says so, and the VMID has no live job.
        (void)InterlockedCompareExchange(&Gfx->SubmitInFlight, previousPending, (LONG)seq);
        Gfx->SubmitSeq = previousSeq;
        GuardLog("gfx: IB 0x%llX x%lu dwords at VMID %lu refused, result %d", GpuAddress, SizeBytes / 4, Vmid, result);
        return NT_SUCCESS(Gfx->Sequence.Fault) ? STATUS_INVALID_PARAMETER : Gfx->Sequence.Fault;
    }

    Gfx->SubmitVmid = Vmid;
    if (Vmid != 0)
    {
        KeAcquireSpinLock(&Gfx->VmidLock, &irql);
        Bc250VmidSubmitted(&Gfx->Vmid, Vmid, seq, Identity != NULL ? Identity->ProcessId : 0);
        KeReleaseSpinLock(&Gfx->VmidLock, irql);
    }
    if (VmidUsed != NULL) *VmidUsed = Vmid;
    // KMD193: committed, so the journal gets its BC250_PJ_GFX_SUBMIT record here - before the DPM call and
    // before this function can take any other exit. The record is what lets a dump put a faulting sequence
    // next to the unmap that took its memory away (bsod-245 item 4). KMD214: the record carries the VMID.
    if (Identity != NULL)
        PagingJournalGfxSubmit(seq, Identity->Fence, GpuAddress, RootPhysical, Identity->Context, Identity->Node,
                               Identity->ProcessId, Identity->ContextFlags, Vmid);
    DpmBusyBegin(&Device->Dpm);     // committed: the ring is busy from here (dpm.h)
    if (InterlockedIncrement(&Gfx->PipelineSamples) <= 16) {
        ULONG after = (ULONG)bc250_gfx_fence_read(Adev, BC250_SUBMIT_FENCE_SLOT);
        GuardLog("gfx: pipeline queued seq%lu prior%lu observed_after_doorbell%lu overlap%u vmid%lu",
                 seq, previousSeq, after,
                 previousSeq != 0 && !bc250_fence_reached(after, previousSeq), Vmid);
    }
    *Seq = seq;
    return STATUS_SUCCESS;
}

NTSTATUS GfxSubmitIb(_Inout_ BC250_DEVICE* Device, ULONG Vmid, ULONGLONG RootPhysical, ULONGLONG GpuAddress,
                     ULONG SizeBytes, _In_opt_ const BC250_GFX_SUBMIT_IDENTITY* Identity, _Out_ ULONG* Seq,
                     _Out_opt_ ULONG* VmidUsed)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE;
    NTSTATUS status;
    ULONG vram, gtt;

    *Seq = 0;
    if (VmidUsed != NULL) *VmidUsed = 0;
    ProgressEnter(ProgressSiteGfxSubmit);   // before GartLock: a wait for it counts as inside
    // PASSIVE_LEVEL only, because of this: DxgkDdiSubmitCommandVirtual is annotated PASSIVE_LEVEL
    // (d3dkmddi.h) and DxgkDdiSubmitCommand is not, which is exactly why the paging path may not come here.
    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else status = GartDevice(Device, &adev, &gartEnabled);
    if (NT_SUCCESS(status) && !gartEnabled) status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status))
    {
        previousBackend = adev->backend;
        adev->backend = &gfx->Sequence;
        SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
        GpuMemBeginSequence(Device, NULL, 0);
        status = SubmitIbLocked(Device, gfx, adev, Vmid, RootPhysical, GpuAddress, SizeBytes, Identity, Seq, VmidUsed);
        (void)GpuMemEndSequence(Device, &vram, &gtt);
        adev->backend = previousBackend;
    }
    ExReleaseFastMutex(&Device->GartLock);
    ProgressExit(ProgressSiteGfxSubmit, (LONG)*Seq);
    return status;
}

// Who ran at Vmid, for the reports that read a fault latch (ih.c) or a timeout (wddm.c): the tenant now, and the
// newest earlier tenant from the history. A latch names a VMID, not a job, and a latch read late can describe a
// VMID that was recycled since. Any IRQL up to DISPATCH_LEVEL; values are copied under VmidLock and logged after.
void GfxVmidReport(_In_ const BC250_DEVICE* Device, _In_z_ const char* Who, ULONG Vmid)
{
    BC250_GFX* gfx = GfxAccessAcquire(Device);
    BC250_VMID_TENANT live, before;
    int haveLive = 0, haveBefore = 0;
    KIRQL irql;

    RtlZeroMemory(&live, sizeof(live));
    RtlZeroMemory(&before, sizeof(before));
    if (gfx != NULL)
    {
        KeAcquireSpinLock(&gfx->VmidLock, &irql);
        Bc250VmidDescribe(&gfx->Vmid, &gfx->VmidHistory, Vmid, &live, &haveLive, &before, &haveBefore);
        KeReleaseSpinLock(&gfx->VmidLock, irql);
        GfxAccessRelease(Device);
    }
    if (haveLive)
        GuardLog("%s vmid %lu now: root 0x%llX pid %lu seq %lu-%lu", Who, Vmid, live.Root, live.Process,
                 live.FirstSeq, live.LastSeq);
    if (haveBefore)
        GuardLog("%s vmid %lu before: root 0x%llX pid %lu seq %lu-%lu", Who, Vmid, before.Root, before.Process,
                 before.FirstSeq, before.LastSeq);
    if (!haveLive && !haveBefore) GuardLog("%s vmid %lu: no tenant on record", Who, Vmid);
}

// For the wddm summary: the pool's gate, membership and counters. Zeros when the graphics state is gone.
void GfxVmidCounters(_In_ const BC250_DEVICE* Device, _Out_ BC250_GFX_VMID_COUNTERS* Counters)
{
    BC250_GFX* gfx = GfxAccessAcquire(Device);

    RtlZeroMemory(Counters, sizeof(*Counters));
    if (gfx == NULL) return;
    Counters->Gate = gfx->VmidPoolGate;
    Counters->Members = gfx->VmidMembers;
    Counters->Excluded = gfx->VmidExcluded;
    Counters->Claims = (ULONG)InterlockedCompareExchange(&gfx->VmidClaims, 0, 0);
    Counters->Reuses = (ULONG)InterlockedCompareExchange(&gfx->VmidReuses, 0, 0);
    Counters->Busy = (ULONG)InterlockedCompareExchange(&gfx->VmidBusy, 0, 0);
    Counters->RuleRefusals = (ULONG)InterlockedCompareExchange(&gfx->VmidRuleRefusals, 0, 0);
    Counters->Flushes = (ULONG)InterlockedCompareExchange(&gfx->VmidFlushes, 0, 0);
    Counters->FlushVmids = (ULONG)InterlockedCompareExchange(&gfx->VmidFlushVmids, 0, 0);
    GfxAccessRelease(Device);
}

// ---- fences (E12 part C) ------------------------------------------------------------------------------------------------

static struct amdgpu_ring* FenceRing(_In_ struct amdgpu_device* Adev, ULONG Ring)
{
    if (Ring == BC250_FENCE_RING_GFX) return &Adev->gfx.gfx_ring[0];
    if (Ring >= BC250_FENCE_RING_COMPUTE0 && Ring < BC250_FENCE_RING_COMPUTE0 + 8) return &Adev->gfx.compute_ring[Ring - BC250_FENCE_RING_COMPUTE0];
    if (Ring == BC250_FENCE_RING_KIQ) return &Adev->gfx.kiq[0].ring;
    if (Ring >= BC250_FENCE_RING_SDMA0 && Ring < BC250_FENCE_RING_SDMA0 + 2) return &Adev->sdma.instance[Ring - BC250_FENCE_RING_SDMA0].ring;
    return NULL;
}

// Count fences on one ring, one after the other: emit (RELEASE_MEM, or two WRITE_DATA on the KIQ), ring the doorbell, poll
// the slot in GTT memory for the value. Whether an interrupt came with each is ih.c's to say (`ih state`).
void GfxFenceEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_FENCE* Data)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    struct amdgpu_ring* ring = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER frequency, start;
    ULONG i, waited, vram, gtt, flags = Data->Interrupt == BC250_FENCE_MODE_INTERRUPT ? AMDGPU_FENCE_FLAG_INT : 0;
    BOOLEAN sdma = Data->Ring >= BC250_FENCE_RING_SDMA0;
    // ADR 0008 stage D: SDMA0 specifically, not "any SDMA ring" - node 1 only ever touches instance 0
    // (docs/design/paging-node.md section 4), so only this ring's escape traffic needs Sdma0RingLock.
    BOOLEAN sdma0 = Data->Ring == BC250_FENCE_RING_SDMA0;
    BOOLEAN dispatch = Data->Interrupt == BC250_FENCE_MODE_DISPATCH;
    BOOLEAN compute = Data->Ring >= BC250_FENCE_RING_COMPUTE0 && Data->Ring < BC250_FENCE_RING_COMPUTE0 + 8;
    BOOLEAN ib = Data->Interrupt == BC250_FENCE_MODE_IB;            // the driver's own ring-test IB, VMID 0
    BOOLEAN ibAt = Data->Interrupt == BC250_FENCE_MODE_IB_AT;       // the caller's IB, through GfxSubmitIb
    ULONG slot = sdma ? 2 + (Data->Ring - BC250_FENCE_RING_SDMA0) : Data->Ring;     // SDMA slots 0 and 1 are the ring tests'
    // Acquired and released under the same "if (sdma0)" each time (two call sites below), so the two are always
    // paired at run time; the initializer is only to satisfy /W4's flow analysis, which does not correlate two
    // separate "if" statements testing the same variable.
    KIRQL sdmaIrql = PASSIVE_LEVEL;
    long result = 0;

    Data->Version = BC250_KMD_VERSION;
    Data->Result = 0;
    Data->FaultOffset = 0;
    Data->Completed = 0;
    Data->DoorbellCount = 0;
    Data->LastSeq = 0;
    Data->LastValue = 0;
    Data->Microseconds = 0;
    Data->SlowestMicroseconds = 0;
    Data->DispatchCheck = 0;
    Data->DispatchBadOffset = 0;
    Data->IbFetched = 0;
    Data->Seq = 0;
    Data->Padding = 0;
    KeQueryPerformanceCounter(&frequency);

    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else if (Data->Count == 0 || Data->Count > BC250_FENCE_MAX_COUNT) status = STATUS_INVALID_PARAMETER;
    else if (Data->Interrupt > BC250_FENCE_MODE_IB_AT || (Data->Interrupt == BC250_FENCE_MODE_RING_TEST && !sdma)) status = STATUS_INVALID_PARAMETER;
    else if (dispatch && (!compute || Data->Count > BC250_DISPATCH_MAX_GROUPS)) status = STATUS_INVALID_PARAMETER;
    // Both IB modes are one submission on the gfx ring: the ring because that is the only one bc250_gfx_emit_ib()
    // emits for, one because the ring holds eight unanswered submissions and nothing here reads the read pointer.
    else if ((ib || ibAt) && (Data->Ring != BC250_FENCE_RING_GFX || Data->Count != 1)) status = STATUS_INVALID_PARAMETER;
    // An explicit VMID 0..15 only: BC250_VMID_AUTO is the WDDM path's, never the escape's (KMD214).
    else if (ibAt && (Data->Dwords == 0 || Data->Dwords > BC250_FENCE_IB_MAX_DWORDS || (Data->IbAddress & 3) != 0 ||
                      Data->Vmid >= BC250_VMID_COUNT ||
                      (Data->RootPhysical & (AMDGPU_GPU_PAGE_SIZE - 1)) != 0)) status = STATUS_INVALID_PARAMETER;
    else if (gfx->Failed || !gfx->SetUp || gfx->StagesDone < (ULONG)(sdma ? BC250_GFX_STAGE_SDMA : BC250_GFX_STAGE_CP)) status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status)) status = GartDevice(Device, &adev, &gartEnabled);
    if (NT_SUCCESS(status) && !gartEnabled) status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status) && (ring = FenceRing(adev, Data->Ring)) == NULL) status = STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status))
    {
        previousBackend = adev->backend;
        adev->backend = &gfx->Sequence;
        SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
        GpuMemBeginSequence(Device, NULL, 0);
        if (sdma && !gfx->SdmaFencePage)
        {
            result = bc250_sdma_fence_page_alloc(adev);
            if (result == 0) gfx->SdmaFencePage = TRUE; else status = STATUS_INSUFFICIENT_RESOURCES;
        }
        if (!sdma && !gfx->FencePage)
        {
            result = bc250_gfx_fence_page_alloc(adev);
            if (result == 0) gfx->FencePage = TRUE; else status = STATUS_INSUFFICIENT_RESOURCES;
        }
        // The page the driver builds its own IB in. Like the fence page it belongs to this sequence, so gpumem.c gives
        // it back with everything else at the FINI; unlike it, nothing but this mode ever asks for it.
        if (NT_SUCCESS(status) && ib && !gfx->IbPage)
        {
            result = bc250_gfx_ib_page_alloc(adev);
            if (result == 0) gfx->IbPage = TRUE; else status = STATUS_INSUFFICIENT_RESOURCES;
        }
        // Before the dispatch's reseed: a refused call must leave the destination as the pending dispatch may still fill it.
        if (NT_SUCCESS(status) && gfx->RingOwes[Data->Ring] != 0)
        {
            ULONG owedSlot = gfx->RingOwesSlot[Data->Ring];
            ULONG now = (ULONG)(sdma ? bc250_sdma_fence_read(adev, owedSlot) : bc250_gfx_fence_read(adev, owedSlot));

            if (now == gfx->RingOwes[Data->Ring]) gfx->RingOwes[Data->Ring] = 0;        // late, but it came
            else
            {
                Data->LastSeq = gfx->RingOwes[Data->Ring];
                Data->LastValue = now;
                result = -16;
                status = STATUS_DEVICE_BUSY;
            }
        }
        if (NT_SUCCESS(status) && dispatch)
        {
            // Its own two allocations (the shader, 256-byte aligned, and the destination), seeded again before every run.
            result = bc250_gfx_dispatch_setup(adev);
            if (result == 0) { gfx->Dispatch = TRUE; result = bc250_gfx_dispatch_reseed(adev); }
            if (result != 0) status = STATUS_INSUFFICIENT_RESOURCES;
        }
        start = KeQueryPerformanceCounter(NULL);
        if (NT_SUCCESS(status) && dispatch)
        {
            // The M6 exit criterion: ACQUIRE_MEM, libdrm's 18 packets, a partial flush and a fence, straight into the
            // compute ring; then the destination is read back, the part behind the last workgroup included.
            ULONG seq = (ULONG)InterlockedIncrement(&gfx->FenceSeq);
            u64 address = bc250_gfx_fence_addr(adev, slot);
            u32 bad = 0;

            if (address == 0) result = -62;
            else result = bc250_gfx_dispatch_memset(ring, BC250_DISPATCH_FILL, Data->Count, address, seq, AMDGPU_FENCE_FLAG_INT);
            if (result == 0 && NT_SUCCESS(gfx->Sequence.Fault))
            {
                Data->LastSeq = seq;
                for (waited = 0; waited < BC250_FENCE_TIMEOUT_US; waited += 10)
                {
                    Data->LastValue = (unsigned long)bc250_gfx_fence_read(adev, slot);
                    if (Data->LastValue == seq) break;
                    KeStallExecutionProcessor(10);
                }
                if (Data->LastValue != seq) { result = -62; gfx->RingOwes[Data->Ring] = seq; gfx->RingOwesSlot[Data->Ring] = slot; }
                // Read back even after a timeout: "nothing written" and "written, no fence" are different failures.
                Data->DispatchCheck = bc250_gfx_dispatch_check(adev, BC250_DISPATCH_FILL, Data->Count, &bad);
                Data->DispatchBadOffset = bad;
                if (result == 0 && Data->DispatchCheck == 0) Data->Completed = 1;
                else if (result == 0) result = Data->DispatchCheck;
            }
            GuardLog("gfx: dispatch %u groups, shader 0x%llX, destination 0x%llX, check %d at 0x%X", Data->Count,
                     bc250_gfx_dispatch_shader_addr(adev), bc250_gfx_dispatch_dst_addr(adev), Data->DispatchCheck, Data->DispatchBadOffset);
        }
        if (NT_SUCCESS(status) && ib)
        {
            // Stage C step C3: the ring test as an indirect buffer, at VMID 0, straight through the shim - the same
            // alloc, emit, commit and poll the modes above use, with one PACKET3_INDIRECT_BUFFER in front of the
            // fence. None of GfxSubmitIb's policy is involved, so this runs before EnableGpuSubmit is ever opened and
            // before stage 8: what it asks is only whether the CP fetches a buffer it was pointed at.
            ULONG seq = (ULONG)InterlockedIncrement(&gfx->FenceSeq);
            u64 address = bc250_gfx_fence_addr(adev, slot);
            u32 dwords = 0;

            result = bc250_gfx_ib_ring_test_build(adev, &dwords);
            // Every one of these is the driver's, not the caller's, so they are reported as the driver set them and
            // not as they arrived: this mode takes no input at all.
            Data->IbAddress = bc250_gfx_ib_addr(adev);
            Data->Dwords = dwords;
            Data->Vmid = 0;
            Data->RootPhysical = 0;
            if (result == 0 && (address == 0 || Data->IbAddress == 0)) result = -62;
            if (result == 0)
                result = bc250_gfx_submit_ib(&adev->gfx.gfx_ring[0], Data->IbAddress, dwords, 0, address, seq,
                                             AMDGPU_FENCE_FLAG_INT);
            if (result == 0 && NT_SUCCESS(gfx->Sequence.Fault))
            {
                Data->LastSeq = seq;
                Data->Seq = seq;
                for (waited = 0; waited < BC250_FENCE_TIMEOUT_US; waited += 10)
                {
                    Data->LastValue = (unsigned long)bc250_gfx_fence_read(adev, slot);
                    if (Data->LastValue == seq) break;
                    KeStallExecutionProcessor(10);
                }
                if (Data->LastValue != seq) { result = -62; gfx->RingOwes[Data->Ring] = seq; gfx->RingOwesSlot[Data->Ring] = slot; }
                // Read back after a timeout as well: "the fence never came" and "the fence came but the CP never read
                // the buffer" are different failures and want different next steps.
                Data->IbFetched = (bc250_gfx_ib_ring_test_result(adev) == 0) ? 1 : 0;
                if (result == 0 && Data->IbFetched) Data->Completed = 1;
                else if (result == 0) result = -5;      // the fence arrived, the scratch register did not take the IB
            }
            GuardLog("gfx: IB 0x%llX x%lu dwords at VMID 0: fetched %lu, fence 0x%lX/0x%lX, result %d", Data->IbAddress,
                     Data->Dwords, Data->IbFetched, Data->LastValue, Data->LastSeq, result);
        }
        if (NT_SUCCESS(status) && ibAt)
        {
            // Steps C4 and C6: the caller's buffer through the path wddm.c will use, with GfxSubmitIb's policy in
            // full - the gate, stage 8, the VMID root and exclusive diagnostic rule - and then a bounded wait, which is
            // the one thing a DDI must not do. The lock and the sequence are already ours, so the inner call is the
            // one that runs; GfxSubmitIb itself would deadlock on GartLock here.
            ULONG seq = 0;

            // No identity: the IB_AT escape has no WDDM context and no OS fence, so it writes no
            // BC250_PJ_GFX_SUBMIT record (KMD193).
            status = SubmitIbLocked(Device, gfx, adev, Data->Vmid, Data->RootPhysical, Data->IbAddress,
                                    Data->Dwords * 4u, NULL, &seq, NULL);
            if (NT_SUCCESS(status))
            {
                Data->LastSeq = seq;
                Data->Seq = seq;
                for (waited = 0; waited < BC250_SUBMIT_POLL_US; waited += 10)
                {
                    if (GfxFenceArrived(Device, seq)) break;
                    KeStallExecutionProcessor(10);
                }
                Data->LastValue = (unsigned long)bc250_gfx_fence_read(adev, BC250_SUBMIT_FENCE_SLOT);
                if (Data->LastValue == seq) Data->Completed = 1;
                else result = -62;      // the submission stands; the next one is refused until its value arrives
            }
            GuardLog("gfx: IB 0x%llX x%lu dwords at VMID %lu, root 0x%llX -> 0x%08X, fence 0x%lX/0x%lX", Data->IbAddress,
                     Data->Dwords, Data->Vmid, Data->RootPhysical, status, Data->LastValue, Data->LastSeq);
        }
        if (NT_SUCCESS(status) && Data->Interrupt == BC250_FENCE_MODE_RING_TEST)
        {
            // sdma_v5_0_ring_test_ring(): one WRITE_LINEAR of 0xDEADBEEF into the engine's scratch slot, polled by the shim.
            // Sdma0RingLock: unlike StageSdma's own ring-test calls (bring-up only, before node 1 can ever be
            // ready), this escape is reachable at any time after stage 7, so it can race a node-1 submission.
            // Review 23 MUST-FIX: only the push (bc250_sdma_ring_test_submit) goes under the lock, narrowly, the
            // same way the signal_fence/copy_test call sites below already do it - the original single call
            // held the lock across bc250_sdma_ring_test's own internal poll too, up to adev->usec_timeout
            // (100 ms) of KeStallExecutionProcessor at DISPATCH_LEVEL, far longer than a spinlock should ever
            // be held and inconsistent with this file's own pattern two call sites down.
            if (sdma0)
            {
                volatile u32* slotCpu = NULL;

                KeAcquireSpinLock(&gfx->Sdma0RingLock, &sdmaIrql);
                result = bc250_sdma_ring_test_submit(ring, &slotCpu);
                KeReleaseSpinLock(&gfx->Sdma0RingLock, sdmaIrql);
                if (result == 0) result = bc250_sdma_ring_test_wait(adev, slotCpu);
            }
            else result = bc250_sdma_ring_test(ring);
            Data->LastSeq = 0xDEADBEEF;
            Data->LastValue = (unsigned long)bc250_sdma_fence_read(adev, Data->Ring - BC250_FENCE_RING_SDMA0);
            if (result == 0) Data->Completed = 1;
            else { gfx->RingOwes[Data->Ring] = 0xDEADBEEF; gfx->RingOwesSlot[Data->Ring] = Data->Ring - BC250_FENCE_RING_SDMA0; }
        }
        for (i = 0; NT_SUCCESS(status) && !dispatch && !ib && !ibAt && Data->Interrupt != BC250_FENCE_MODE_RING_TEST && i < Data->Count; i++)
        {
            LARGE_INTEGER one = KeQueryPerformanceCounter(NULL);
            ULONG seq = (ULONG)InterlockedIncrement(&gfx->FenceSeq), took;
            u64 address = sdma ? bc250_sdma_fence_addr(adev, slot) : bc250_gfx_fence_addr(adev, slot);

            // The whole call holds GartLock and stalls: a run of slow fences ends here, not after Count timeouts.
            if (address == 0 || Microseconds(start, frequency) > BC250_FENCE_BUDGET_US) { result = -62; break; }
            if (sdma0) KeAcquireSpinLock(&gfx->Sdma0RingLock, &sdmaIrql);
            result = sdma ? bc250_sdma_signal_fence(ring, address, seq, flags) : bc250_gfx_signal_fence(ring, address, seq, flags);
            if (sdma0) KeReleaseSpinLock(&gfx->Sdma0RingLock, sdmaIrql);
            if (result != 0 || !NT_SUCCESS(gfx->Sequence.Fault)) break;
            Data->LastSeq = seq;
            for (waited = 0; waited < BC250_FENCE_TIMEOUT_US; waited += 10)
            {
                Data->LastValue = (unsigned long)(sdma ? bc250_sdma_fence_read(adev, slot) : bc250_gfx_fence_read(adev, slot));
                if (Data->LastValue == seq) break;
                KeStallExecutionProcessor(10);
            }
            if (Data->LastValue != seq) { result = -62; gfx->RingOwes[Data->Ring] = seq; gfx->RingOwesSlot[Data->Ring] = slot; break; }
            Data->Completed++;
            took = Microseconds(one, frequency);
            if (took > Data->SlowestMicroseconds) Data->SlowestMicroseconds = took;
        }
        Data->Microseconds = Microseconds(start, frequency);
        if (NT_SUCCESS(status)) status = gfx->Sequence.Fault;
        Data->FaultOffset = gfx->Sequence.FaultOffset;
        Data->DoorbellCount = GpuMemEndSequence(Device, &vram, &gtt);
        adev->backend = previousBackend;
    }
    GuardLog("gfx: fence ring %u x%u int %u -> 0x%08X, result %d, %u completed, last 0x%X/0x%X, %u us", Data->Ring, Data->Count,
             Data->Interrupt, status, result, Data->Completed, Data->LastValue, Data->LastSeq, Data->Microseconds);
    ExReleaseFastMutex(&Device->GartLock);

    Data->Result = result;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// ---- ADR 0013: the SDMA copy/fill positive control (BC250_ESCAPE_RUN_SDMACOPY) -----------------------------------------
//
// One linear copy and one constant fill on SDMA0, checked by the CPU, that never goes near the WDDM table - the
// positive control ADR 0013 asks for before BuildPagingBuffer is written. The source is seeded by the CPU with a
// counting pattern first (byte i = i & 0xFF, wrapping every 256 bytes), not left at whatever a previous call left
// behind or at zero: bc250_sdma_copy_test()'s own SDMA_OP_CONST_FILL packet then overwrites it with
// BC250_SDMACOPY_PATTERN before the copy runs, so a fill that silently did not execute leaves this byte-varying
// content for the copy to move instead of the flat pattern - which the comparison below catches and a check
// against "not zero" or "not the CPU seed" would not. The destination is poisoned to a third value first
// (0xEE, neither the seed nor the pattern), so "nothing ran at all" fails the comparison exactly as a real
// mismatch would. What the escape actually asks: does SDMA0 execute both packets, in that order, and does the
// copy really move what the fill wrote.

// gpumem.c's VRAM pool can only ever hand out addresses inside the top of the carve-out (BC250_GPUMEM_POOL_BELOW/
// _LENGTH, both well inside Device->VramMcBase/VramLength); checked again here rather than trusted by
// construction, the same doubled check vram.c's Access() makes against MmGetPhysicalMemoryRanges() for its own
// writes. Allocates once and keeps the two regions for later calls; TearDown() gives them back with everything
// else, not this function, so that a second call need not pay for the allocation again.
static NTSTATUS SdmaCopyAllocateRegions(_Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev, _In_ const BC250_DEVICE* Device)
{
    int r;

    if (Gfx->SdmaCopyRegions) return STATUS_SUCCESS;
    r = bc250_sdma_copy_regions_alloc(Adev, BC250_SDMACOPY_MAX_BYTES, &Gfx->SdmaCopySrc, &Gfx->SdmaCopyDst);
    if (r != 0) return STATUS_INSUFFICIENT_RESOURCES;
    if (Gfx->SdmaCopySrc.mc < Device->VramMcBase || Gfx->SdmaCopySrc.mc + BC250_SDMACOPY_MAX_BYTES > Device->VramMcBase + Device->VramLength ||
        Gfx->SdmaCopyDst.mc < Device->VramMcBase || Gfx->SdmaCopyDst.mc + BC250_SDMACOPY_MAX_BYTES > Device->VramMcBase + Device->VramLength)
    {
        bc250_sdma_copy_regions_free(Adev, &Gfx->SdmaCopySrc, &Gfx->SdmaCopyDst);
        return STATUS_ACCESS_DENIED;
    }
    Gfx->SdmaCopyRegions = TRUE;
    return STATUS_SUCCESS;
}

// The same AMD fill/copy emitters as the direct-ring control, in retained GTT.
// Call only after the prior owed fence has arrived, before overwriting its IB.
static NTSTATUS SdmaCopyBuildIb(BC250_GFX* Gfx, struct amdgpu_device* Adev, ULONG Bytes, ULONG* Dwords)
{
    u32* words;
    unsigned int fillDw=0, copyDw=0, length, capacity;
    int result;
    *Dwords=0;
    if (Gfx->SdmaCopyIb.size==0) {
        result=bc250_shim_mem_alloc(Adev,BC250_MEM_GTT,4*PAGE_SIZE,PAGE_SIZE,&Gfx->SdmaCopyIb);
        if (result!=0) return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!Gfx->SdmaCopyIb.cpu || Gfx->SdmaCopyIb.size<PAGE_SIZE || (Gfx->SdmaCopyIb.mc & 31u)!=0)
        return STATUS_INVALID_DEVICE_STATE;
    words=(u32*)Gfx->SdmaCopyIb.cpu;
    capacity=Gfx->SdmaCopyIb.size/sizeof(*words);
    result=bc250_sdma_paging_fill(Adev,words,capacity,Gfx->SdmaCopySrc.mc,
                                 BC250_SDMACOPY_PATTERN,Bytes,&fillDw);
    if (result!=BC250_SDMA_PAGING_OK) return STATUS_INVALID_BUFFER_SIZE;
    result=bc250_sdma_paging_copy(Adev,words+fillDw,capacity-fillDw,
                                 Gfx->SdmaCopySrc.mc,Gfx->SdmaCopyDst.mc,Bytes,&copyDw);
    if (result!=BC250_SDMA_PAGING_OK) return STATUS_INVALID_BUFFER_SIZE;
    length=fillDw+copyDw;
    // AMD sdma_v5_0_ring_pad_ib: pad payload to8DWORD, using ordinary NOPs.
    if (((length+7u)&~7u)>capacity) return STATUS_INVALID_BUFFER_SIZE;
    while (length & 7u) words[length++]=bc250_sdma_ring_funcs()->nop;
    KeMemoryBarrier();
    *Dwords=length;
    GuardLog("gfx: sdmaib buffer 0x%llX length %lu DWORD VMID0 CSA0",Gfx->SdmaCopyIb.mc,*Dwords);
    return STATUS_SUCCESS;
}

void SdmaCopyEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_SDMACOPY* Data)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    struct amdgpu_ring* ring = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE;
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER frequency, start = { 0 };
    ULONG bytes, i, waited, vram, gtt;
    u32 seq = 0;
    KIRQL sdmaIrql;
    long result = 0;
    ULONG ibDwords = 0;
    BOOLEAN indirect = Data->Command == BC250_ESCAPE_RUN_SDMAIB;

    Data->Version = BC250_KMD_VERSION;
    Data->Result = 0;
    Data->FaultOffset = 0;
    Data->BytesCompared = 0;
    Data->Matched = 0;
    Data->FirstMismatchOffset = 0;
    Data->FirstMismatchGot = 0;
    Data->FirstMismatchWant = 0;
    Data->LastSeq = 0;
    Data->LastValue = 0;
    Data->Microseconds = 0;
    Data->Padding = 0;
    Data->SrcMc = 0;
    Data->DstMc = 0;
    KeQueryPerformanceCounter(&frequency);

    bytes = (Data->Bytes == 0) ? BC250_SDMACOPY_DEFAULT_BYTES : Data->Bytes;

    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else if (bytes > BC250_SDMACOPY_MAX_BYTES) status = STATUS_INVALID_PARAMETER;
    // The CPU seed and read-back below are raw VRAM writes/reads of the kind vram.c's EnableVramWrite gate already
    // exists for, not the bring-up's own buffers that the gates below cover.
    else if (!Device->VramWriteEnabled) status = STATUS_ACCESS_DENIED;
    // The same stage the SDMA ring test needs (this file's BC250_FENCE_MODE_RING_TEST arm, above): stage 7 done,
    // nothing failed. Not stage 8 (EnableIh): the fence here carries no interrupt bit and is polled in memory
    // exactly as the ring test's own scratch dword is, so the IH ring is not part of what this needs.
    else if (gfx->Failed || !gfx->SetUp || gfx->StagesDone < (ULONG)BC250_GFX_STAGE_SDMA) status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status)) status = GartDevice(Device, &adev, &gartEnabled);
    if (NT_SUCCESS(status) && !gartEnabled) status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status) && (ring = FenceRing(adev, BC250_FENCE_RING_SDMA0)) == NULL) status = STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status))
    {
        previousBackend = adev->backend;
        adev->backend = &gfx->Sequence;
        SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
        GpuMemBeginSequence(Device, NULL, 0);

        if (!gfx->SdmaFencePage)
        {
            result = bc250_sdma_fence_page_alloc(adev);
            if (result == 0) gfx->SdmaFencePage = TRUE; else status = STATUS_INSUFFICIENT_RESOURCES;
        }
        // As GfxFenceEscape checks before every ring's next submission: a fence this ring still owes from an
        // earlier timed-out call must be seen before a new one is sent, or a late arrival could be read as this
        // call's own.
        if (NT_SUCCESS(status) && gfx->RingOwes[BC250_FENCE_RING_SDMA0] != 0)
        {
            ULONG owedSlot = gfx->RingOwesSlot[BC250_FENCE_RING_SDMA0];
            ULONG now = (ULONG)bc250_sdma_fence_read(adev, owedSlot);

            if (now == gfx->RingOwes[BC250_FENCE_RING_SDMA0]) gfx->RingOwes[BC250_FENCE_RING_SDMA0] = 0;
            else { result = -16; status = STATUS_DEVICE_BUSY; }
        }
        if (NT_SUCCESS(status)) status = SdmaCopyAllocateRegions(gfx, adev, Device);
        if (NT_SUCCESS(status) && indirect) status = SdmaCopyBuildIb(gfx, adev, bytes, &ibDwords);

        if (NT_SUCCESS(status))
        {
            u64 fenceAddr = bc250_sdma_fence_addr(adev, 2);       // slot 2: the first fence slot (0, 1 are the ring tests')

            Data->SrcMc = gfx->SdmaCopySrc.mc;
            Data->DstMc = gfx->SdmaCopyDst.mc;
            if (fenceAddr == 0) { result = -62; status = STATUS_INSUFFICIENT_RESOURCES; }
            else
            {
                UCHAR* src = (UCHAR*)gfx->SdmaCopySrc.cpu;
                UCHAR* dst = (UCHAR*)gfx->SdmaCopyDst.cpu;

                for (i = 0; i < bytes; i++) WRITE_REGISTER_UCHAR(&src[i], (UCHAR)(i & 0xFFu));
                for (i = 0; i < bytes; i++) WRITE_REGISTER_UCHAR(&dst[i], 0xEEu);

                start = KeQueryPerformanceCounter(NULL);
                seq = (ULONG)InterlockedIncrement(&gfx->FenceSeq);
                // Sdma0RingLock: this escape always names BC250_FENCE_RING_SDMA0 (FenceRing() call above), the
                // same physical ring node 1 submits to (design note section 4).
                KeAcquireSpinLock(&gfx->Sdma0RingLock, &sdmaIrql);
                if (indirect)
                    result = bc250_sdma_submit_ib(ring, gfx->SdmaCopyIb.mc, ibDwords, 0, 0, fenceAddr, seq, 0);
                else result = bc250_sdma_copy_test(ring, gfx->SdmaCopySrc.mc, gfx->SdmaCopyDst.mc, bytes,
                                              BC250_SDMACOPY_PATTERN, fenceAddr, seq, 0);
                KeReleaseSpinLock(&gfx->Sdma0RingLock, sdmaIrql);
                if (result == 0 && NT_SUCCESS(gfx->Sequence.Fault))
                {
                    Data->LastSeq = seq;
                    for (waited = 0; waited < BC250_FENCE_TIMEOUT_US; waited += 10)
                    {
                        Data->LastValue = (unsigned long)bc250_sdma_fence_read(adev, 2);
                        if (Data->LastValue == seq) break;
                        KeStallExecutionProcessor(10);
                    }
                    if (Data->LastValue != seq)
                    {
                        result = -62;
                        gfx->RingOwes[BC250_FENCE_RING_SDMA0] = seq;
                        gfx->RingOwesSlot[BC250_FENCE_RING_SDMA0] = 2;
                    }
                }
                Data->Microseconds = Microseconds(start, frequency);

                // Read back and compare whatever the fence says, timeout included: "the copy never ran" and "the
                // copy ran and moved the wrong bytes" are different failures and want different next steps, as the
                // dispatch and IB modes of GfxFenceEscape already read back after a timeout.
                Data->BytesCompared = bytes;
                Data->Matched = 1;
                for (i = 0; i < bytes; i++)
                {
                    UCHAR got = READ_REGISTER_UCHAR(&dst[i]);
                    if (got != (UCHAR)(BC250_SDMACOPY_PATTERN & 0xFFu))
                    {
                        Data->Matched = 0;
                        Data->FirstMismatchOffset = i;
                        Data->FirstMismatchGot = got;
                        Data->FirstMismatchWant = BC250_SDMACOPY_PATTERN & 0xFFu;
                        break;
                    }
                }
                if (result == 0 && !Data->Matched) result = -5;       // the fence arrived, the destination did not
            }
        }
        if (NT_SUCCESS(status)) status = gfx->Sequence.Fault;
        Data->FaultOffset = gfx->Sequence.FaultOffset;
        (void)GpuMemEndSequence(Device, &vram, &gtt);
        adev->backend = previousBackend;
    }
    GuardLog("gfx: %s %lu bytes -> 0x%08X, result %d, matched %lu, first mismatch at 0x%lX (got 0x%02lX want 0x%02lX), %lu us",
             indirect ? "sdmaib" : "sdmacopy", bytes, status, result, Data->Matched, Data->FirstMismatchOffset, Data->FirstMismatchGot, Data->FirstMismatchWant, Data->Microseconds);
    ExReleaseFastMutex(&Device->GartLock);

    Data->Result = result;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// Private pre-publication control. All backing is retained by BC250_GFX until
// engine retirement. VA deliberately differs from both GART MC and physical PA.
// Linux amdgpu_sdma_get_csa_mc_addr uses64bytes per engine. Map a zeroed full
// page for this SDMA0 CSA rather than assuming CSA0 is valid for nonzero VMID.

// Pre-publication positive control: change BOTH PTE mappings on the GPU after
// building the native copy, then execute the unchanged IB in VMID2. Backing
// belongs to this startup owner through the real fence, including timeout.
static NTSTATUS SdmaPteStartupControl(BC250_DEVICE* Device,BC250_GFX* gfx,
    struct amdgpu_device* adev,struct amdgpu_ring* ring,u64* tables,u64 rootPhys)
{
    const u64 baseVa=0x40000000ull,dmaVa=0x40004000ull;
    UCHAR* sourceA=(UCHAR*)gfx->SdmaCopySrc.cpu;
    UCHAR* sourceB=(UCHAR*)gfx->SdmaVaData.cpu;
    UCHAR* oldDestination=sourceB+PAGE_SIZE;
    UCHAR* destination=(UCHAR*)gfx->SdmaCopyDst.cpu;
    u64 sourcePhys=amdgpu_gmc_vram_mc2pa(adev,gfx->SdmaCopySrc.mc);
    u64 destinationPhys=amdgpu_gmc_vram_mc2pa(adev,gfx->SdmaCopyDst.mc);
    u64 systemPhys=(u64)MmGetPhysicalAddress(sourceB).QuadPart;
    u64 marker=bc250_sdma_fence_addr(adev,BC250_PAGING_MARKER_SLOT);
    u64 fence=bc250_sdma_fence_addr(adev,2);
    unsigned trial,i,words,length,seq,arrived,waited;
    u32 prelude[64];
    NTSTATUS status=STATUS_SUCCESS;
    KIRQL irql;
    int result;
    (void)Device;
    if(gfx->SdmaCopyIb.size<4*PAGE_SIZE || !marker || !fence)return STATUS_INVALID_DEVICE_STATE;
    for(i=0;i<4;i++) {
        u64 physical=(u64)MmGetPhysicalAddress((UCHAR*)gfx->SdmaCopyIb.cpu+i*PAGE_SIZE).QuadPart;
        WRITE_REGISTER_ULONG64(&tables[3*512+4+i],physical|bc250_pte_vm_flags(1,1,1,1));
    }
    for(trial=0;trial<3;trial++) {
        struct bc250_sdma_virtual_ptes built;
        u64 entries[2];
        unsigned sourceOffset=trial==2?8u:0u,destinationOffset=trial==1?8u:0u;
        for(i=0;i<PAGE_SIZE;i++) {
            WRITE_REGISTER_UCHAR(sourceA+i,(UCHAR)(i^0x5Au));
            sourceB[i]=(UCHAR)(i^0xC3u);
            WRITE_REGISTER_UCHAR(destination+i,(UCHAR)((i*13u)^0x39u));
        }
        RtlFillMemory(oldDestination,PAGE_SIZE,0xEE);
        // Old identities deliberately differ from the later GPU update.
        WRITE_REGISTER_ULONG64(&tables[3*512],sourcePhys|bc250_pte_vm_flags(1,1,0,0));
        WRITE_REGISTER_ULONG64(&tables[3*512+1],(systemPhys+PAGE_SIZE)|bc250_pte_vm_flags(1,1,1,1));
        result=bc250_sdma_build_virtual_ptes(adev,gfx->SdmaCopyIb.cpu,gfx->SdmaCopyIb.size,
            dmaVa,baseVa+sourceOffset,baseVa+PAGE_SIZE+destinationOffset,trial?511u:512u,&built);
        if(result)return STATUS_INVALID_BUFFER_SIZE;
        entries[0]=trial ? destinationPhys|bc250_pte_vm_flags(1,1,0,0) :
            systemPhys|bc250_pte_vm_flags(1,1,1,1);
        entries[1]=destinationPhys|bc250_pte_vm_flags(1,1,0,0);
        words=0;
        result=bc250_sdma_paging_update_ptes(adev,prelude,RTL_NUMBER_OF(prelude),
            gfx->SdmaVaTables.mc+3*PAGE_SIZE,entries,2,marker,32u+trial,&words);
        if(result)return STATUS_INVALID_BUFFER_SIZE;
        KeMemoryBarrier();
        seq=(ULONG)InterlockedIncrement(&gfx->FenceSeq);
        length=words+BC250_SDMA_VM_FLUSH_DWORDS+7u+6u+bc250_sdma_fence_size(ring,0);
        KeAcquireSpinLock(&gfx->Sdma0RingLock,&irql);
        result=amdgpu_ring_alloc(ring,length);
        if(!result) {
            amdgpu_ring_write_multiple(ring,prelude,(int)words);
            result=bc250_sdma_emit_vm_flush(ring,BC250_SDMA_PAGING_VMID,rootPhys);
            if(!result)result=bc250_sdma_emit_ib(ring,dmaVa+built.ib_offset,built.ib_dwords,
                BC250_SDMA_PAGING_VMID,dmaVa+built.csa_offset);
            if(!result)result=bc250_sdma_emit_fence(ring,fence,seq,0);
            if(!result)amdgpu_ring_commit(ring);
            else amdgpu_ring_undo(ring);
        }
        KeReleaseSpinLock(&gfx->Sdma0RingLock,irql);
        if(result || !NT_SUCCESS(gfx->Sequence.Fault))return STATUS_IO_DEVICE_ERROR;
        for(waited=0;waited<BC250_FENCE_TIMEOUT_US;waited+=10) {
            arrived=(ULONG)bc250_sdma_fence_read(adev,2);
            if(arrived==seq)break;
            KeStallExecutionProcessor(10);
        }
        if(arrived!=seq) {
            gfx->RingOwes[BC250_FENCE_RING_SDMA0]=seq;gfx->RingOwesSlot[BC250_FENCE_RING_SDMA0]=2;
            GuardLog("gfx: sdmapte trial%u fence%u/%u TIMEOUT",trial,seq,arrived);
            return STATUS_IO_TIMEOUT;
        }
        KeMemoryBarrier();
        for(i=0;i<PAGE_SIZE;i++) {
            unsigned index=trial==1 && i>=8?i-8:trial==2 && i<PAGE_SIZE-8?i+8:i;
            UCHAR want=trial ? (UCHAR)((index*13u)^0x39u) : (UCHAR)(i^0xC3u);
            if(READ_REGISTER_UCHAR(destination+i)!=want || oldDestination[i]!=0xEE ||
               READ_REGISTER_UCHAR(sourceA+i)!=(UCHAR)(i^0x5Au) || sourceB[i]!=(UCHAR)(i^0xC3u)) {
                status=STATUS_DATA_ERROR;break;
            }
        }
        GuardLog("gfx: sdmapte trial%u GPU-remap %s fence%u/%u compared%u status0x%08X",trial,
            trial==0?"system-to-VRAM":trial==1?"alias-forward":"alias-backward",seq,arrived,i,status);
        GuardLogKeep();
        if(!NT_SUCCESS(status))return status;
    }
    return status;
}

static NTSTATUS SdmaVaStartupControl(BC250_DEVICE* Device)
{
    const u64 baseVa=0x40000000ULL;
    BC250_GFX* gfx=(BC250_GFX*)Device->Gfx;
    struct amdgpu_device* adev=NULL;
    struct amdgpu_ring* ring;
    void* previousBackend;
    BOOLEAN enabled=FALSE;
    NTSTATUS status;
    u64 rootPhys,systemPhys,ibPhys,sourcePhys,fenceAddr;
    u64* tables;
    u32* ib;
    UCHAR *sourceA,*sourceB,*dest;
    ULONG i,trial,vram,gtt,seq=0,arrived=0,waited;
    unsigned int fillDw,copyDw,length;
    int result=0;
    BOOLEAN runPteControl=(GuardReadSetting(L"EnableSdmaPteControl",0)==1);
    KIRQL irql;
    if (!Device->FullWddm || Device->Wddm || Device->Started || !gfx || !gfx->SdmaCopyIb.cpu)
        return STATUS_INVALID_DEVICE_STATE;
    ExAcquireFastMutex(&Device->GartLock);
    status=GartDevice(Device,&adev,&enabled);
    if (!NT_SUCCESS(status) || !enabled) {
        ExReleaseFastMutex(&Device->GartLock);
        return NT_SUCCESS(status)?STATUS_INVALID_DEVICE_STATE:status;
    }
    previousBackend=adev->backend;adev->backend=&gfx->Sequence;
    SequenceBegin(&gfx->Sequence,Device,FALSE,NULL,0);GpuMemBeginSequence(Device,NULL,0);
    if (adev->vm_manager.num_level!=3 || adev->vm_manager.block_size!=9 ||
        gfx->RingOwes[BC250_FENCE_RING_SDMA0]!=0) {status=STATUS_INVALID_DEVICE_STATE;goto Done;}
    if (!gfx->SdmaVaTables.size)
        result=bc250_shim_mem_alloc(adev,BC250_MEM_VRAM,4*PAGE_SIZE,PAGE_SIZE,&gfx->SdmaVaTables);
    if (!result && !gfx->SdmaVaData.size)
        result=bc250_shim_mem_alloc(adev,BC250_MEM_GTT,3*PAGE_SIZE,PAGE_SIZE,&gfx->SdmaVaData);
    if (result || !gfx->SdmaVaTables.cpu || !gfx->SdmaVaData.cpu) {status=STATUS_INSUFFICIENT_RESOURCES;goto Done;}
    rootPhys=amdgpu_gmc_vram_mc2pa(adev,gfx->SdmaVaTables.mc);
    sourcePhys=amdgpu_gmc_vram_mc2pa(adev,gfx->SdmaCopySrc.mc);
    systemPhys=(u64)MmGetPhysicalAddress(gfx->SdmaVaData.cpu).QuadPart;
    ibPhys=(u64)MmGetPhysicalAddress(gfx->SdmaCopyIb.cpu).QuadPart;
    fenceAddr=bc250_sdma_fence_addr(adev,2);
    if (!fenceAddr) {status=STATUS_INVALID_DEVICE_STATE;goto Done;}
    tables=(u64*)gfx->SdmaVaTables.cpu;ib=(u32*)gfx->SdmaCopyIb.cpu;
    sourceA=(UCHAR*)gfx->SdmaCopySrc.cpu;sourceB=(UCHAR*)gfx->SdmaVaData.cpu;dest=sourceB+PAGE_SIZE;
    ring=&adev->sdma.instance[0].ring;
    for(i=0;i<4*PAGE_SIZE/sizeof(u64);i++) WRITE_REGISTER_ULONG64(&tables[i],0);
    // Established WDDM four-level/512-entry layout, physical PDEs in local VRAM.
    for(i=0;i<3;i++) {
        ULONG index=(ULONG)((baseVa>>(12+9*(3-i)))&511u);
        WRITE_REGISTER_ULONG64(&tables[i*512+index],(rootPhys+(i+1)*PAGE_SIZE)|AMDGPU_PTE_VALID);
    }
    WRITE_REGISTER_ULONG64(&tables[3*512+1],(systemPhys+PAGE_SIZE)|bc250_pte_vm_flags(1,1,1,1));
    WRITE_REGISTER_ULONG64(&tables[3*512+2],ibPhys|bc250_pte_vm_flags(1,1,1,1));
    WRITE_REGISTER_ULONG64(&tables[3*512+3],(systemPhys+2*PAGE_SIZE)|bc250_pte_vm_flags(1,1,1,1));
    RtlZeroMemory(sourceB+2*PAGE_SIZE,PAGE_SIZE);
    for(i=0;i<PAGE_SIZE;i++) {WRITE_REGISTER_UCHAR(&sourceA[i],(UCHAR)(i^0x5Au));sourceB[i]=(UCHAR)(i^0xC3u);}
    GuardLog("gfx: sdmava root PA0x%llX IB PA0x%llX system PA0x%llX source PA0x%llX VA0x%llX CSA0x%llX VMID%u",
             rootPhys,ibPhys,systemPhys,sourcePhys,baseVa,baseVa+3*PAGE_SIZE,BC250_SDMA_PAGING_VMID);
    for(trial=0;trial<2;trial++) {
        UCHAR fill=(UCHAR)(trial?0x66u:0x33u);
        // Remap the same source VA from local VRAM to a differently patterned
        // system page. The first real fence has arrived before changing the PTE.
        u64 sourcePte=trial ? systemPhys|bc250_pte_vm_flags(1,1,1,1) : sourcePhys|bc250_pte_vm_flags(1,1,0,0);
        WRITE_REGISTER_ULONG64(&tables[3*512],sourcePte);
        RtlFillMemory(dest,PAGE_SIZE,0xEE);
        fillDw=copyDw=0;
        result=bc250_sdma_paging_fill(adev,ib,PAGE_SIZE/4,baseVa+PAGE_SIZE,(u32)fill*0x01010101u,PAGE_SIZE,&fillDw);
        if (!result) result=bc250_sdma_paging_copy(adev,ib+fillDw,PAGE_SIZE/4-fillDw,baseVa,
                                                 baseVa+PAGE_SIZE,PAGE_SIZE/2,&copyDw);
        if (result) {status=STATUS_INVALID_BUFFER_SIZE;break;}
        length=fillDw+copyDw;while(length&7u) ib[length++]=ring->funcs->nop;
        KeMemoryBarrier();
        seq=(ULONG)InterlockedIncrement(&gfx->FenceSeq);
        KeAcquireSpinLock(&gfx->Sdma0RingLock,&irql);
        result=bc250_sdma_submit_vm_ib(ring,rootPhys,baseVa+2*PAGE_SIZE,length,BC250_SDMA_PAGING_VMID,
                                      baseVa+3*PAGE_SIZE,fenceAddr,seq,0);
        KeReleaseSpinLock(&gfx->Sdma0RingLock,irql);
        if (result || !NT_SUCCESS(gfx->Sequence.Fault)) {status=STATUS_IO_DEVICE_ERROR;break;}
        for(waited=0;waited<BC250_FENCE_TIMEOUT_US;waited+=10) {
            arrived=(ULONG)bc250_sdma_fence_read(adev,2);
            if (arrived==seq) break;
            KeStallExecutionProcessor(10);
        }
        if (arrived!=seq) {
            gfx->RingOwes[BC250_FENCE_RING_SDMA0]=seq;gfx->RingOwesSlot[BC250_FENCE_RING_SDMA0]=2;
            status=STATUS_IO_TIMEOUT;
            GuardLog("gfx: sdmava trial%lu fence%lu/%lu TIMEOUT",trial,seq,arrived);break;
        }
        KeMemoryBarrier();
        for(i=0;i<PAGE_SIZE;i++) {
            UCHAR want=i<PAGE_SIZE/2 ? (UCHAR)(i^(trial?0xC3u:0x5Au)) : fill;
            UCHAR got=READ_REGISTER_UCHAR(&dest[i]);
            if (got!=want) {
                GuardLog("gfx: sdmava trial%lu mismatch offset%lu got%u want%u",trial,i,(ULONG)got,(ULONG)want);
                status=STATUS_DATA_ERROR;break;
            }
        }
        GuardLog("gfx: sdmava trial%lu source%s fence%lu/%lu compared%lu status0x%08X",trial,
                 trial?"system-remap":"VRAM",seq,arrived,i,status);
        if (!NT_SUCCESS(status)) break;
    }
    if(NT_SUCCESS(status) && runPteControl)
        status=SdmaPteStartupControl(Device,gfx,adev,ring,tables,rootPhys);
Done:
    if (NT_SUCCESS(status)) status=gfx->Sequence.Fault;
    GuardLog("gfx: sdmava end status0x%08X result%d fault0x%lX",status,result,gfx->Sequence.FaultOffset);
    (void)GpuMemEndSequence(Device,&vram,&gtt);adev->backend=previousBackend;
    ExReleaseFastMutex(&Device->GartLock);
    return status;
}


// ---- ADR 0008 stage D: node 1, the paging node on SDMA0 (docs/design/paging-node.md) ---------------------------
//
// PASSIVE_LEVEL builder emits into OS-owned per-buffer nonpaged private records.
// DISPATCH_LEVEL submit validates those records, then copies selected words into
// the live SDMA ring. No command storage is shared across OS DMA buffers.

static BOOLEAN GfxPagingNodeGateAccess(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    return gfx != NULL && gfx->PagingGate;
}

BOOLEAN GfxPagingNodeGate(_In_ const BC250_DEVICE* Device)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxPagingNodeGateAccess(Device);
    GfxAccessRelease(Device);
    return result;
}

static BOOLEAN GfxPagingSubmitReadyAccess(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    // The node-1 twin of GfxSubmitReady: gated, set up, not failed, nothing outstanding, and PagingReady - the
    // RUN escape has reached stage 8 with the gate open and captured a live ring to write to.
    return gfx != NULL && gfx->PagingGate && gfx->SetUp && !gfx->PowerSuspended && !gfx->Failed && gfx->PagingSubmitFailed == 0 &&
           gfx->PagingSubmitInFlight == 0 && gfx->PagingReady;
}

BOOLEAN GfxPagingSubmitReady(_In_ const BC250_DEVICE* Device)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxPagingSubmitReadyAccess(Device);
    GfxAccessRelease(Device);
    return result;
}

// Startup owns the unpublished device lifecycle; no diagnostic escape or stop
// may run concurrently. Shared builder ownership protects the object while this
// checks the resources used by both local and system-memory paging.
BOOLEAN GfxStartupResources(BC250_DEVICE* Device, BOOLEAN Initialized)
{
    const BC250_GFX* gfx;
    BOOLEAN ready = FALSE;
    if (!Device || KeGetCurrentIrql()!=PASSIVE_LEVEL) return FALSE;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(const BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->SubmitGate || !gfx->PagingGate || gfx->Failed ||
        gfx->SubmitFailed || gfx->PagingSubmitFailed ||
        gfx->SubmitInFlight || gfx->PagingSubmitInFlight) goto Done;
    if (!Initialized) {
        ready=!gfx->SetUp && gfx->StagesDone==0 && gfx->PagingCpuBootstrap;
        goto Done;
    }
    ready=!Device->GfxTlbBootstrap && GfxSubmitReadyAccess(Device) && GfxPagingSubmitReadyAccess(Device) &&
        gfx->PagingWindowReady && gfx->PagingRing && gfx->PagingRing->funcs &&
        gfx->PagingRing->max_dw && gfx->PagingDevicePtr &&
        gfx->FencePage && gfx->SdmaFencePage &&
        gfx->PagingCopyStaging.size>=PAGE_SIZE && gfx->PagingCopyStaging.mc &&
        gfx->PagingDevicePtr->sdma.fence_mem.cpu &&
        bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_FENCE_SLOT)!=0 &&
        bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT)!=0;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return ready;
}

static void GfxPagingSubmitFailAccess(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    if (gfx == NULL) return;
    // Sticky, once, independent of node 0's own SubmitFailed (GfxSubmitFail): the two nodes fail on their own
    // hardware (design note section 5). The write pointer is left where it stands, for the same reason
    // GfxSubmitFail leaves node 0's: M59/M60 say a hardware pointer only counts up.
    if (InterlockedExchange(&gfx->PagingSubmitFailed, 1) == 0)
        GuardLog("gfx: paging submission path failed, no further ring writes this device start (seq %lu in flight, slot 0x%X)",
                 gfx->PagingSubmitSeq,
                 gfx->PagingDevicePtr != NULL ? (ULONG)bc250_sdma_fence_read(gfx->PagingDevicePtr, BC250_PAGING_FENCE_SLOT) : 0);
}

void GfxPagingSubmitFail(_Inout_ BC250_DEVICE* Device)
{
    StartHealthFault(Device);
    if (GfxAccessAcquire(Device) == NULL)
    {
        return;
    }
    GfxPagingSubmitFailAccess(Device);
    GfxAccessRelease(Device);
}

static BOOLEAN GfxPagingFenceArrivedAccess(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    // One read of a GTT page SDMA0 writes, under the outer CPU lifetime reference - the node-1 twin of
    // GfxFenceArrived. PagingDevicePtr is only ever non-NULL between GfxEscape's capture and the teardown that
    // frees the fence page, and pnp.c drains ih.c's DPCs before that teardown runs, exactly as SubmitAdev's own
    // comment states.
    if (gfx == NULL || gfx->PagingDevicePtr == NULL || Seq == 0) return FALSE;
    if ((ULONG)bc250_sdma_fence_read(gfx->PagingDevicePtr, BC250_PAGING_FENCE_SLOT) != Seq) return FALSE;
    if (gfx->PagingSubmitSeq == Seq) InterlockedExchange(&gfx->PagingSubmitInFlight, 0);
    return TRUE;
}

BOOLEAN GfxPagingFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BOOLEAN result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        return FALSE;
    }
    result = GfxPagingFenceArrivedAccess(Device, Seq);
    GfxAccessRelease(Device);
    return result;
}

// Translation/emission adapters: local pages use direct MC addresses; system pages
// use the reserved GART window with GPU-ordered mapping and cleanup. Hardware
// validation of this new path remains separate from host packet/stream tests.
// MDL PFNs describe OS system RAM for legacy physical paging. This adapter
// borrows the MDL only during construction; the OS owns its pinning/lifetime.
// ByteOffset is measured from PFN[FirstPage], not from TransferOffset and not
// implicitly adjusted by MDL ByteOffset. The described byte interval is checked.
BOOLEAN GfxPagingMdlAddress(_In_ PMDL Mdl, ULONG FirstPage, ULONGLONG ByteOffset,
                           ULONG Bytes, _Out_ ULONGLONG* Address)
{
    ULONGLONG begin,end,start,pages;
    if (Address==NULL) return FALSE;
    *Address=0;
    if (Mdl==NULL || sizeof(PFN_NUMBER)!=sizeof(PAGING_U64)) return FALSE;
    begin=MmGetMdlByteOffset(Mdl);
    if (begin>=PAGE_SIZE || MmGetMdlByteCount(Mdl)==0) return FALSE;
    end=begin+(ULONGLONG)MmGetMdlByteCount(Mdl);
    pages=(end+PAGE_SIZE-1)>>PAGE_SHIFT;
    start=(ULONGLONG)FirstPage<<PAGE_SHIFT;
    if (ByteOffset>MAXULONGLONG-start) return FALSE;
    start+=ByteOffset;
    if (!Bytes || start<begin || start>=end || Bytes>end-start) return FALSE;
    return PagingPageListAddress((const PAGING_U64*)MmGetMdlPfnArray(Mdl),
        (unsigned)pages,0,0,FirstPage,ByteOffset,Bytes,Address)!=0;
}

typedef struct _BC250_PAGING_STREAM {
    BC250_DEVICE* Device;
    BC250_GFX* Gfx;
    ULONGLONG Root;
    BOOLEAN Fill;
    ULONG Pattern;
    ULONGLONG StagingMc;
    ULONG CommandOffset;
    unsigned* Payload;
    BC250_WDDM_PAGING_UNSUPPORTED Unsupported;
} BC250_PAGING_STREAM;

static int PagingResolve(void* Context, PAGING_U64 Va, unsigned Bytes, PAGING_U64* Mc)
{
    BC250_PAGING_STREAM* stream = (BC250_PAGING_STREAM*)Context;
    ULONGLONG physical = 0;
    BOOLEAN system = FALSE;
    if (!VidMmTranslatePaging(stream->Root, Va, &physical, &system)) {
        stream->Unsupported = BC250PagingNoTranslation; return 0;
    }
    if (system) {
        ULONGLONG page=physical & ~(ULONGLONG)(PAGE_SIZE-1);
        if (!stream->Gfx->PagingWindowReady || (page & ~AMDGPU_PTE_ADDR_MASK) != 0) {
            stream->Unsupported = BC250PagingSystemMemory; return 0;
        }
        *Mc=physical | PAGING_SYSTEM_ADDRESS; return 1;
    }
    if (!PagingPhysicalToMc(physical, Bytes, (ULONGLONG)stream->Device->VramPhysical.QuadPart,
                            stream->Device->VramMcBase, stream->Device->VramLength, Mc)) {
        stream->Unsupported = BC250PagingNoTranslation; return 0;
    }
    return 1;
}

static int PagingEmit(void* Context, unsigned* Buffer, unsigned Capacity, PAGING_U64 Src,
                      PAGING_U64 Dst, unsigned Bytes, unsigned* Written)
{
    BC250_PAGING_STREAM* stream = (BC250_PAGING_STREAM*)Context;
    if ((Src | Dst) & PAGING_SYSTEM_ADDRESS) {
        struct amdgpu_device* adev=stream->Gfx->PagingDevicePtr;
        struct bc250_sdma_paging_mapping map;
        u64 ptes[2]={0,0};
        RtlZeroMemory(&map,sizeof(map));
        map.table_mc=stream->Gfx->PagingWindow.table;
        map.ptes=ptes; map.page_count=2;
        map.scratch_mc=bc250_sdma_fence_addr(adev,BC250_PAGING_MARKER_SLOT);
        if (map.scratch_mc==0) return BC250_SDMA_PAGING_EINVAL;
        // Command position is unique within the OS buffer, including earlier build calls.
        // Submit resets scratch only after the previous actual hardware fence.
        map.first_sequence=1u+3u*(stream->CommandOffset/4u+(unsigned)(Buffer-stream->Payload));
        map.src_mc=Src; map.dst_mc=Dst; map.bytes=Bytes;
        map.fill=stream->Fill; map.pattern=stream->Pattern;map.staging_mc=stream->StagingMc;
        if (Src & PAGING_SYSTEM_ADDRESS) {
            u64 physical=Src & ~PAGING_SYSTEM_ADDRESS;
            ptes[0]=bc250_gart_pte(physical & ~(u64)4095,bc250_gart_pte_flags(adev));
            map.src_mc=stream->Gfx->PagingWindow.mc+(physical & 4095);
        }
        if (Dst & PAGING_SYSTEM_ADDRESS) {
            u64 physical=Dst & ~PAGING_SYSTEM_ADDRESS;
            ptes[1]=bc250_gart_pte(physical & ~(u64)4095,bc250_gart_pte_flags(adev));
            map.dst_mc=stream->Gfx->PagingWindow.mc+PAGE_SIZE+(physical & 4095);
        }
        return bc250_sdma_paging_mapped_transfer(adev,Buffer,Capacity,&map,Written);
    }
    return stream->Fill ? bc250_sdma_paging_fill(stream->Gfx->PagingDevicePtr, Buffer, Capacity,
                                               Dst, stream->Pattern, Bytes, Written)
                        : bc250_sdma_paging_copy(stream->Gfx->PagingDevicePtr, Buffer, Capacity,
                                               Src, Dst, Bytes, Written);
}

typedef struct _BC250_PHYSICAL_STREAM {
    BC250_PAGING_STREAM Common; // First member: PagingEmit uses the common state.
    const BC250_PAGING_ENDPOINT* Source;
    const BC250_PAGING_ENDPOINT* Destination;
} BC250_PHYSICAL_STREAM;

static int PagingResolvePhysical(BC250_PHYSICAL_STREAM* Stream,
    const BC250_PAGING_ENDPOINT* Endpoint, PAGING_U64 Address, unsigned Bytes, PAGING_U64* Mc)
{
    ULONGLONG offset,physical;
    *Mc=0;
    if (Endpoint==NULL) return 0;
    if (Endpoint->Mdl!=NULL) offset=Address;
    else {
        if (Address<Endpoint->Address) return 0;
        offset=Address-Endpoint->Address;
    }
    if (!Bytes || offset>=Endpoint->Length || Bytes>Endpoint->Length-offset) return 0;
    if (Endpoint->Aperture) {
        if (Endpoint->Mdl || !Stream->Common.Gfx->PagingWindowReady ||
            !VidMmResolveAperture(Address,Bytes,&physical) ||
            ((physical & ~(ULONGLONG)(PAGE_SIZE-1)) & ~AMDGPU_PTE_ADDR_MASK)!=0) return 0;
        *Mc=physical | PAGING_SYSTEM_ADDRESS;
    } else if (Endpoint->Mdl!=NULL) {
        if (!Stream->Common.Gfx->PagingWindowReady ||
            !GfxPagingMdlAddress(Endpoint->Mdl,Endpoint->FirstPage,offset,Bytes,&physical) ||
            ((physical & ~(ULONGLONG)(PAGE_SIZE-1)) & ~AMDGPU_PTE_ADDR_MASK)!=0) return 0;
        *Mc=physical | PAGING_SYSTEM_ADDRESS;
    } else {
        ULONGLONG base=Stream->Common.Device->VramMcBase;
        ULONGLONG length=Stream->Common.Device->VramLength;
        if (Address<base || Address-base>=length || Bytes>length-(Address-base) ||
            (Address & PAGING_SYSTEM_ADDRESS)!=0) return 0;
        *Mc=Address;
    }
    return 1;
}

static int PagingResolveSource(void* Context, PAGING_U64 Address, unsigned Bytes, PAGING_U64* Mc)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    return PagingResolvePhysical(stream,stream->Source,Address,Bytes,Mc);
}

static int PagingResolveDestination(void* Context, PAGING_U64 Address, unsigned Bytes, PAGING_U64* Mc)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    return PagingResolvePhysical(stream,stream->Destination,Address,Bytes,Mc);
}

static int PagingSourceIdentity(void* Context,PAGING_U64 Address,unsigned Bytes,PAGING_U64* Physical)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    PAGING_U64 mc;
    if (!PagingResolveSource(Context,Address,Bytes,&mc)) return 0;
    if (mc&PAGING_SYSTEM_ADDRESS) *Physical=mc&~PAGING_SYSTEM_ADDRESS;
    else *Physical=mc-stream->Common.Device->VramMcBase+(ULONGLONG)stream->Common.Device->VramPhysical.QuadPart;
    return 1;
}
static int PagingDestinationIdentity(void* Context,PAGING_U64 Address,unsigned Bytes,PAGING_U64* Physical)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    PAGING_U64 mc;
    if (!PagingResolveDestination(Context,Address,Bytes,&mc)) return 0;
    if (mc&PAGING_SYSTEM_ADDRESS) *Physical=mc&~PAGING_SYSTEM_ADDRESS;
    else *Physical=mc-stream->Common.Device->VramMcBase+(ULONGLONG)stream->Common.Device->VramPhysical.QuadPart;
    return 1;
}

// Classification only. Workspace never reaches hardware and is released before
// packets are built. Sorting physical spans avoids quadratic all-pairs PFN checks.
NTSTATUS GfxPagingCheckDisjoint(BC250_DEVICE* Device,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,BOOLEAN* Disjoint)
{
    BC250_PHYSICAL_STREAM stream;BC250_GFX* gfx;PAGING_INTERVAL* work;
    ULONGLONG src=Source->Mdl?0:Source->Address,dst=Destination->Mdl?0:Destination->Address;
    unsigned count=PagingIntervalCapacity(src,Bytes);int separate=0;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Disjoint=FALSE;
    if (!count) return status;
    work=(PAGING_INTERVAL*)ExAllocatePool2(POOL_FLAG_PAGED,(SIZE_T)count*sizeof(*work),BC250_GFX_TAG);
    if (!work) return STATUS_INSUFFICIENT_RESOURCES;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (gfx && gfx->PagingReady && gfx->PagingWindowReady) {
        RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
        stream.Source=Source;stream.Destination=Destination;
        if (PagingIntervalsDisjoint(&stream,PagingSourceIdentity,PagingDestinationIdentity,
                src,dst,Bytes,work,count,&separate)) {status=STATUS_SUCCESS;*Disjoint=(BOOLEAN)separate;}
    }
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    ExFreePoolWithTag(work,BC250_GFX_TAG);
    return status;
}

// Complete cycles of system-page permutations fit in each submission. Scratch is
// driver-owned through engine retirement; no value must survive another paging
// job. Equal in-page offsets use independent byte bands for partial ranges.
static NTSTATUS PagingBuildPageGraphCore(BC250_DEVICE* Device,ULONGLONG Root,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,PVOID Buffer,
    ULONG Offset,ULONG Free,unsigned Resume,ULONG* Written,unsigned* NextResume)
{
    BC250_PHYSICAL_STREAM stream;
    BC250_GFX* gfx;
    PAGING_U64 *sources,*destinations,*physical;
    PAGING_PAGE_IDENTITY* identities;
    PAGING_PAGE_MOVE* moves;
    unsigned *sourceIndex,*destinationIndex,*readers,*writer,*queue,*forward;
    unsigned identityCount=0,bandCount=0,bandIndex=0,startOffset;
    PAGING_PAGE_BAND bands[3];
    ULONGLONG position=0;
    unsigned char* storage=NULL;
    unsigned pages,moveCapacity,moveCount=0,i,required=0,budget,maxBudget,used=0;
    unsigned first=0,last=0,cycleSize=0,nextResume=Resume;
    int batch;
    ULONGLONG src,dst,scratchPhysical;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int result;
    *Written=0;*NextResume=Resume;
    if(!Device || !Source || !Destination || !Buffer || !Bytes ||
       (Offset&3) || Offset>BC250_GFX_PAGING_BUFFER_BYTES || Bytes/PAGE_SIZE>0x0aaaaaa9u ||
       (!Root && (!(Source->Mdl || Source->Aperture) || !(Destination->Mdl || Destination->Aperture))))return status;
    src=Source->Mdl?0:Source->Address;dst=Destination->Mdl?0:Destination->Address;
    startOffset=(unsigned)(src&4095);
    if(startOffset!=(dst&4095) || !PagingPageBands(startOffset,Bytes,bands,&bandCount))return status;
    if(Root) {
        if(Source->Mdl || Destination->Mdl || Source->Aperture || Destination->Aperture ||
           Bytes>MAXULONGLONG-src || Bytes>MAXULONGLONG-dst)return status;
    } else if(!GfxPagingEndpointValid(Device,Source,Bytes) || !GfxPagingEndpointValid(Device,Destination,Bytes))return status;
    pages=(unsigned)((Bytes+startOffset+4095)/PAGE_SIZE);moveCapacity=pages*3;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingRing ||
       !gfx->PagingDevicePtr)goto Done;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Payload=(unsigned*)Buffer;stream.Common.CommandOffset=Offset;
    stream.Source=Source;stream.Destination=Destination;stream.Common.Root=Root;
    if(Root) {
        PAGING_U64 sourceMc,destinationMc;
        unsigned chunk=(unsigned)(PAGE_SIZE-startOffset);
        if(chunk>Bytes)chunk=(unsigned)Bytes;
        if(!PagingResolve(&stream.Common,src,chunk,&sourceMc) ||
           !PagingResolve(&stream.Common,dst,chunk,&destinationMc))goto Done;
        if(!(sourceMc&PAGING_SYSTEM_ADDRESS) || !(destinationMc&PAGING_SYSTEM_ADDRESS)) {
            status=STATUS_NOT_SUPPORTED;goto Done;
        }
    }
    if(!gfx->PagingWindowReady || gfx->PagingCopyStaging.size<PAGE_SIZE)goto Done;
    if(gfx->PagingCopyStaging.mc<Device->VramMcBase ||
       gfx->PagingCopyStaging.mc-Device->VramMcBase>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart)goto Done;
    scratchPhysical=gfx->PagingCopyStaging.mc-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;

    // 128 bytes/page: captures, identity union, graph workspace and moves. All typed arrays stay aligned.
    storage=ExAllocatePool2(POOL_FLAG_PAGED,(SIZE_T)pages*128,BC250_GFX_TAG);
    if(!storage){status=STATUS_INSUFFICIENT_RESOURCES;goto Done;}
    sources=(PAGING_U64*)storage;destinations=sources+pages;
    identities=(PAGING_PAGE_IDENTITY*)(destinations+pages);
    physical=(PAGING_U64*)(identities+pages*2);
    sourceIndex=(unsigned*)(physical+pages*2);destinationIndex=sourceIndex+pages;
    readers=destinationIndex+pages;writer=readers+pages*2;
    queue=writer+pages*2;forward=queue+pages*2;
    moves=(PAGING_PAGE_MOVE*)(forward+pages*2);
    for(i=0;i<pages;i++) {
        unsigned chunk=(unsigned)(PAGE_SIZE-((src+position)&4095));
        if(chunk>Bytes-position)chunk=(unsigned)(Bytes-position);
        if(Root) {
            if(!PagingResolve(&stream.Common,src+position,chunk,sources+i) ||
               !PagingResolve(&stream.Common,dst+position,chunk,destinations+i))goto Done;
            // Only system/system graphs use this publication path. Local/table
            // destinations also require ordered logical table-shadow commits.
            // Classification finishes before any packet or progress is published.
            if(!(sources[i]&PAGING_SYSTEM_ADDRESS) || !(destinations[i]&PAGING_SYSTEM_ADDRESS)) {
                status=STATUS_NOT_SUPPORTED;goto Done;
            }
            sources[i]&=~PAGING_SYSTEM_ADDRESS;destinations[i]&=~PAGING_SYSTEM_ADDRESS;
        } else if(!PagingSourceIdentity(&stream,src+position,chunk,sources+i) ||
                  !PagingDestinationIdentity(&stream,dst+position,chunk,destinations+i))goto Done;
        sources[i]&=~4095ull;destinations[i]&=~4095ull;
        if(sources[i]==scratchPhysical || destinations[i]==scratchPhysical)goto Done;
        position+=chunk;
    }
    if(Resume && Resume!=pages && !(Resume&PAGING_PERMUTATION_RESUME))goto Done;
    if(Resume==pages){*NextResume=pages;status=STATUS_SUCCESS;goto Done;}
    // Every action contains at least one system endpoint, using the same two-PTE
    // transaction. Ask the real packet builder for its aligned cost without writes.
    result=PagingEmit(&stream,(unsigned*)Buffer,0,sources[0]|PAGING_SYSTEM_ADDRESS,
        gfx->PagingCopyStaging.mc,PAGE_SIZE,&required);
    if(result!=BC250_SDMA_PAGING_INSUFFICIENT || !required)goto Done;
    maxBudget=PagingStreamCapacity(BC250_GFX_PAGING_BUFFER_BYTES,0,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if(maxBudget/required<3){status=Root?STATUS_INVALID_PARAMETER:STATUS_NOT_SUPPORTED;goto Done;}
    // Physical identities are independent of the byte band. Normalize the full
    // captured union once per callback; each band selects its logical edge span.
    // This preserves physical ordering while avoiding repeated O(N log N) sorts
    // during validation and emission. No endpoint or plan survives this callback.
    if(!PagingPageGraphNormalize(sources,destinations,pages,identities,physical,
         sourceIndex,destinationIndex,&identityCount))goto Done;
    // Validate all bands before publishing any prefix. Separate bands touch
    // disjoint byte offsets even when the same physical pages appear in each.
    for(bandIndex=0;bandIndex<bandCount;bandIndex++) {
        PAGING_PAGE_BAND* band=&bands[bandIndex];
        if(!PagingPageGraphPlan(sourceIndex+band->FirstPage,destinationIndex+band->FirstPage,
             (unsigned)band->PageCount,identityCount,
             readers,writer,queue,forward,moves,moveCapacity,maxBudget/required,&moveCount))goto Done;
    }
    bandIndex=Resume&PAGING_PERMUTATION_RESUME ? (Resume>>PAGING_GRAPH_BAND_SHIFT)&3u : 0;
    first=Resume&PAGING_PERMUTATION_RESUME ? Resume&PAGING_GRAPH_ACTION_MASK : 0;
    if(bandIndex>=bandCount)goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    for(;bandIndex<bandCount;bandIndex++,first=0) {
        PAGING_PAGE_BAND* band=&bands[bandIndex];
        if(!PagingPageGraphPlan(sourceIndex+band->FirstPage,destinationIndex+band->FirstPage,
             (unsigned)band->PageCount,identityCount,
             readers,writer,queue,forward,moves,moveCapacity,maxBudget/required,&moveCount))goto Done;
        if(first>moveCount || (first==moveCount && first))goto Done;
        if(!moveCount)continue;
        batch=PagingPermutationBatch(moves,moveCount,first,(budget-used)/required,&last,&cycleSize);
        if(batch==PagingPermutationInvalid)goto Done;
        for(i=first;i<last;i++) {
            PAGING_U64 from=moves[i].source==PAGING_PERMUTATION_SCRATCH ? gfx->PagingCopyStaging.mc :
                (physical[moves[i].source]+band->Offset)|PAGING_SYSTEM_ADDRESS;
            PAGING_U64 to=moves[i].destination==PAGING_PERMUTATION_SCRATCH ? gfx->PagingCopyStaging.mc :
                (physical[moves[i].destination]+band->Offset)|PAGING_SYSTEM_ADDRESS;
            unsigned written=0;
            // StagingMc remains zero; each SAVE/RESTORE group owns cycle scratch.
            result=PagingEmit(&stream,(unsigned*)Buffer+used,budget-used,from,to,band->Bytes,&written);
            if(result!=BC250_SDMA_PAGING_OK || !written || written>required)goto Done;
            used+=written;
        }
        if(last<moveCount) {
            nextResume=PAGING_PERMUTATION_RESUME|(bandIndex<<PAGING_GRAPH_BAND_SHIFT)|last;
            *Written=used;*NextResume=used?nextResume:Resume;
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;
        }
    }
    *Written=used;*NextResume=pages;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    if(storage)ExFreePoolWithTag(storage,BC250_GFX_TAG);return status;
}

NTSTATUS GfxPagingBuildPageGraph(BC250_DEVICE* Device,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,PVOID Buffer,
    ULONG Offset,ULONG Free,unsigned Resume,ULONG* Written,unsigned* NextResume)
{
    return PagingBuildPageGraphCore(Device,0,Source,Destination,Bytes,Buffer,Offset,Free,Resume,Written,NextResume);
}

NTSTATUS GfxPagingBuildVirtualPageGraph(BC250_DEVICE* Device,ULONGLONG Root,
    ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes,PVOID Buffer,
    ULONG Offset,ULONG Free,unsigned Resume,ULONG* Written,unsigned* NextResume)
{
    BC250_PAGING_ENDPOINT source={0},destination={0};
    *Written=0;*NextResume=Resume;
    if(!Root)return STATUS_INVALID_PARAMETER;
    source.Address=Source;destination.Address=Destination;source.Length=destination.Length=Bytes;
    return PagingBuildPageGraphCore(Device,Root,&source,&destination,Bytes,Buffer,Offset,Free,Resume,Written,NextResume);
}

// Exact caller-owned storage requirement, including unaligned endpoints. The
// system paging context can reserve its documented VA bound before admission.
SIZE_T GfxPagingCaptureStorageSize(ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes)
{
    SIZE_T work,capture;unsigned pages,sourceCount,destinationCount;
    if(!Bytes || Bytes>MAXULONGLONG-Source || Bytes>MAXULONGLONG-Destination ||
       Bytes/PAGE_SIZE>0x0aaaaaa9u)return 0;
    sourceCount=(unsigned)((Bytes+(Source&4095)+4095)/4096);
    destinationCount=(unsigned)((Bytes+(Destination&4095)+4095)/4096);
    pages=sourceCount>destinationCount?sourceCount:destinationCount;
    work=(SIZE_T)pages*(8*sizeof(unsigned)+3*sizeof(PAGING_PAGE_MOVE));
    capture=(SIZE_T)pages*(2*sizeof(PAGING_U64)+2*sizeof(PAGING_PAGE_IDENTITY));
    if(work<capture)work=capture;
    return sizeof(PAGING_GRAPH_CAPTURE)+work+(SIZE_T)pages*(2*sizeof(PAGING_U64)+2*sizeof(unsigned)+4);
}

// Capture once, before any table-changing copy can be accepted. Later batches
// use physical identities; neither the root nor an OS pointer is resolved again.
NTSTATUS GfxPagingCaptureVirtualGraphInPlace(BC250_DEVICE* Device,ULONGLONG Root,
    ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes,PVOID Storage,SIZE_T StorageBytes,
    PAGING_GRAPH_CAPTURE** Capture)
{
    PAGING_GRAPH_CAPTURE* c=NULL;BC250_PHYSICAL_STREAM stream;
    BC250_GFX* gfx;PAGING_PAGE_IDENTITY* identities;
    PAGING_U64 *sources,*destinations;unsigned char *sourceSystem,*destinationSystem;
    unsigned pages,i,j,offset=(unsigned)(Source&4095),query=0,required=0,capacity,moves=0;
    unsigned sourceCount,destinationCount;
    ULONGLONG position=0,scratchPhysical,probe=0;SIZE_T workBytes,captureBytes;int result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Capture=NULL;
    captureBytes=GfxPagingCaptureStorageSize(Source,Destination,Bytes);
    if(!Device || !Root || !captureBytes || !Storage || StorageBytes<captureBytes ||
       ((SIZE_T)Storage&(sizeof(ULONGLONG)-1)))return status;
    sourceCount=(unsigned)((Bytes+offset+4095)/4096);
    destinationCount=(unsigned)((Bytes+(Destination&4095)+4095)/4096);
    pages=sourceCount>destinationCount?sourceCount:destinationCount;
    // Capture temporaries die after normalization. Reuse that storage for the
    // planner arrays and cached moves; persistent identities remain disjoint.
    workBytes=(SIZE_T)pages*(8*sizeof(unsigned)+3*sizeof(PAGING_PAGE_MOVE));
    captureBytes=(SIZE_T)pages*(2*sizeof(PAGING_U64)+2*sizeof(PAGING_PAGE_IDENTITY));
    if(workBytes<captureBytes)workBytes=captureBytes;
    captureBytes=sizeof(*c)+workBytes+(SIZE_T)pages*(2*sizeof(PAGING_U64)+2*sizeof(unsigned)+4);
    // Caller owns the storage on both success and failure. Reused storage must
    // not carry old tokens, cursors, domain flags or planner state into a capture.
    c=(PAGING_GRAPH_CAPTURE*)Storage;
    RtlZeroMemory(c,captureBytes);
    c->Linear=offset!=(Destination&4095);c->SourcePageCount=sourceCount;c->DestinationPageCount=destinationCount;
    c->PageCount=pages;c->Owner.Root=Root;c->Owner.Source=Source;c->Owner.Destination=Destination;c->Owner.Bytes=Bytes;
    sources=(PAGING_U64*)(c+1);destinations=sources+pages;
    identities=(PAGING_PAGE_IDENTITY*)(destinations+pages);
    c->Readers=(unsigned*)(c+1);c->Writer=c->Readers+pages*2;
    c->Queue=c->Writer+pages*2;c->Forward=c->Queue+pages*2;c->Moves=(PAGING_PAGE_MOVE*)(c->Forward+pages*2);
    c->Pages=(PAGING_U64*)((PUCHAR)(c+1)+workBytes);
    c->SourceIndex=(unsigned*)(c->Pages+pages*2);c->DestinationIndex=c->SourceIndex+pages;
    sourceSystem=(unsigned char*)(c->DestinationIndex+pages);destinationSystem=sourceSystem+pages;c->SystemPages=destinationSystem+pages;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr || gfx->PagingCopyStaging.size<PAGE_SIZE ||
       gfx->PagingCopyStaging.mc<Device->VramMcBase ||
       gfx->PagingCopyStaging.mc-Device->VramMcBase>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart)goto Done;
    scratchPhysical=gfx->PagingCopyStaging.mc-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;
    c->ScratchMc=gfx->PagingCopyStaging.mc;c->VramMcBase=Device->VramMcBase;c->VramPhysical=(ULONGLONG)Device->VramPhysical.QuadPart;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;stream.Common.Root=Root;
    stream.Common.Payload=&query;
    for(j=0;j<2;j++) {
        ULONGLONG address=j?Destination:Source;
        PAGING_U64* addresses=j?destinations:sources;
        unsigned char* systems=j?destinationSystem:sourceSystem;
        unsigned count=j?destinationCount:sourceCount;
        position=0;
        for(i=0;i<count;i++) {
            unsigned chunk=(unsigned)(4096-((address+position)&4095));
            if(chunk>Bytes-position)chunk=(unsigned)(Bytes-position);
            if(!PagingResolve(&stream.Common,address+position,chunk,addresses+i))goto Done;
            systems[i]=(unsigned char)((addresses[i]&PAGING_SYSTEM_ADDRESS)!=0);
            if(systems[i])probe=addresses[i]&~4095ull;
            addresses[i]=systems[i]?addresses[i]&~PAGING_SYSTEM_ADDRESS:
                addresses[i]-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;
            addresses[i]&=~4095ull;
            if(addresses[i]==scratchPhysical)goto Done;
            position+=chunk;
        }
        // Normalize accepts equal-length lists; padding aliases the final real
        // page and is excluded from direction analysis and slice selection.
        for(;i<pages;i++){addresses[i]=addresses[count-1];systems[i]=systems[count-1];}
    }
    if(!PagingPageGraphNormalize(sources,destinations,pages,identities,c->Pages,c->SourceIndex,c->DestinationIndex,&c->Identities))goto Done;
    if(!c->Linear && !PagingPageBands(offset,Bytes,c->Bands,&c->BandCount))goto Done;
    for(i=0;i<c->Identities;i++)c->SystemPages[i]=2;
    for(i=0;i<pages;i++)for(j=0;j<2;j++) {
        unsigned at=j?c->DestinationIndex[i]:c->SourceIndex[i];
        unsigned char system=j?destinationSystem[i]:sourceSystem[i];
        // A single physical identity must have a consistent access/cache domain.
        // Supporting conflicting local/system views needs a proven alias policy.
        if(c->SystemPages[at]!=2 && c->SystemPages[at]!=system){status=STATUS_NOT_SUPPORTED;goto Done;}
        c->SystemPages[at]=system;
    }
    if(c->Linear) {
        int backward=0;
        // Readers and Writer are contiguous: together hold at least 2*Identities.
        if(!PagingPageAliasDirection(c->SourceIndex,sourceCount,c->DestinationIndex,destinationCount,
            c->Identities,c->Readers,offset,(unsigned)(Destination&4095),&backward)) {
            status=STATUS_NOT_SUPPORTED;goto Done;
        }
        c->Backward=(unsigned)backward;
        *Capture=c;c=NULL;status=STATUS_SUCCESS;goto Done;
    }
    if(!probe)probe=sources[0]-(ULONGLONG)Device->VramPhysical.QuadPart+Device->VramMcBase;
    result=PagingEmit(&stream,&query,0,probe,gfx->PagingCopyStaging.mc,PAGE_SIZE,&required);
    if(result!=BC250_SDMA_PAGING_INSUFFICIENT || !required)goto Done;
    capacity=PagingStreamCapacity(BC250_GFX_PAGING_BUFFER_BYTES,0,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    c->RequiredDwords=required;c->MaxAtomicMoves=capacity/required;
    if(c->MaxAtomicMoves<3){status=STATUS_NOT_SUPPORTED;goto Done;}
    for(i=0;i<c->BandCount;i++) {
        PAGING_PAGE_BAND* band=c->Bands+i;
        if(!PagingPageGraphPlan(c->SourceIndex+band->FirstPage,c->DestinationIndex+band->FirstPage,
            (unsigned)band->PageCount,c->Identities,c->Readers,c->Writer,c->Queue,c->Forward,
            c->Moves,pages*3,c->MaxAtomicMoves,&moves))goto Done;
        c->PlannedBand=i;c->MoveCount=moves;
    }
    *Capture=c;c=NULL;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    return status;
}

// Transitional allocating adapter. Context reservation will call the in-place
// core directly; the current DDI route still owns this allocation/failure policy.
NTSTATUS GfxPagingCaptureVirtualGraph(BC250_DEVICE* Device,ULONGLONG Root,
    ULONGLONG Source,ULONGLONG Destination,ULONGLONG Bytes,PAGING_GRAPH_CAPTURE** Capture)
{
    SIZE_T bytes=GfxPagingCaptureStorageSize(Source,Destination,Bytes);
    PVOID storage;NTSTATUS status;
    *Capture=NULL;
    if(!Device || !Root || !bytes)return STATUS_INVALID_PARAMETER;
    storage=ExAllocatePool2(POOL_FLAG_PAGED,bytes,BC250_GFX_TAG);
    if(!storage)return STATUS_INSUFFICIENT_RESOURCES;
    status=GfxPagingCaptureVirtualGraphInPlace(Device,Root,Source,Destination,Bytes,storage,bytes,Capture);
    if(NT_SUCCESS(status))(*Capture)->Owner.PoolTag=BC250_GFX_TAG;
    else ExFreePoolWithTag(storage,BC250_GFX_TAG);
    return status;
}

// Progress counts accepted bytes in traversal order, not a VA offset.
BOOLEAN GfxPagingCapturedLinearSlice(const PAGING_GRAPH_CAPTURE* Capture,ULONGLONG Progress,
    BC250_PAGING_COPY_SLICE* Slice,ULONGLONG* NextProgress)
{
    ULONGLONG position,source,destination,left;
    unsigned count,room,si,di;
    RtlZeroMemory(Slice,sizeof(*Slice));*NextProgress=Progress;
    if(!Capture || !Capture->Linear || Progress>=Capture->Owner.Bytes)return FALSE;
    left=Capture->Owner.Bytes-Progress;
    position=Capture->Backward?left:Progress;
    source=(Capture->Owner.Source&4095)+position;
    destination=(Capture->Owner.Destination&4095)+position;
    if(Capture->Backward) {
        count=(unsigned)((source-1)&4095)+1;room=(unsigned)((destination-1)&4095)+1;
    } else {count=4096-(unsigned)(source&4095);room=4096-(unsigned)(destination&4095);}
    if(count>room)count=room;if(count>left)count=(unsigned)left;
    if(Capture->Backward){source-=count;destination-=count;}
    if(source/4096>=Capture->SourcePageCount || destination/4096>=Capture->DestinationPageCount)return FALSE;
    si=Capture->SourceIndex[(unsigned)(source/4096)];di=Capture->DestinationIndex[(unsigned)(destination/4096)];
    Slice->SourcePhysical=Capture->Pages[si]+(source&4095);
    Slice->DestinationPhysical=Capture->Pages[di]+(destination&4095);
    Slice->SourceSystem=Capture->SystemPages[si];Slice->DestinationSystem=Capture->SystemPages[di];
    Slice->Bytes=count;*NextProgress=Progress+count;return TRUE;
}

NTSTATUS GfxPagingEmitCapturedLinear(BC250_DEVICE* Device,const PAGING_GRAPH_CAPTURE* Capture,
    PVOID Buffer,ULONG Offset,ULONG Free,ULONG* Written,BC250_PAGING_COPY_SLICE* Slice,ULONGLONG* NextProgress)
{
    BC250_PHYSICAL_STREAM stream;BC250_GFX* gfx;BC250_PAGING_COPY_SLICE candidate;
    ULONGLONG next,mc[2],pa[2];unsigned budget,written=0,i;int overlap,result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Written=0;RtlZeroMemory(Slice,sizeof(*Slice));*NextProgress=Capture?Capture->Progress:0;
    if(!Device || !Buffer || (Offset&3) || Offset>BC250_GFX_PAGING_BUFFER_BYTES ||
       !GfxPagingCapturedLinearSlice(Capture,Capture?Capture->Progress:0,&candidate,&next))return status;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr ||
       gfx->PagingCopyStaging.size<PAGE_SIZE || gfx->PagingCopyStaging.mc!=Capture->ScratchMc ||
       Device->VramMcBase!=Capture->VramMcBase || (ULONGLONG)Device->VramPhysical.QuadPart!=Capture->VramPhysical)goto Done;
    pa[0]=candidate.SourcePhysical;pa[1]=candidate.DestinationPhysical;
    for(i=0;i<2;i++) {
        if(i?candidate.DestinationSystem:candidate.SourceSystem)mc[i]=pa[i]|PAGING_SYSTEM_ADDRESS;
        else if(!PagingPhysicalToMc(pa[i],candidate.Bytes,Capture->VramPhysical,Capture->VramMcBase,Device->VramLength,mc+i))goto Done;
    }
    overlap=pa[0]<=pa[1]?pa[1]-pa[0]<candidate.Bytes:pa[0]-pa[1]<candidate.Bytes;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Payload=(unsigned*)Buffer;stream.Common.CommandOffset=Offset;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if(overlap && !candidate.SourceSystem && !candidate.DestinationSystem) {
        ULONGLONG marker=bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT);
        if(!marker)goto Done;
        result=bc250_sdma_paging_copy_bytes(gfx->PagingDevicePtr,(u32*)Buffer,budget,mc[0],mc[1],candidate.Bytes,
            gfx->PagingCopyStaging.mc,marker,1u+3u*(Offset/4u),&written);
    } else {
        if(overlap)stream.Common.StagingMc=gfx->PagingCopyStaging.mc;
        result=PagingEmit(&stream,(unsigned*)Buffer,budget,mc[0],mc[1],candidate.Bytes,&written);
    }
    if(result==BC250_SDMA_PAGING_INSUFFICIENT){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    if(result!=BC250_SDMA_PAGING_OK)goto Done;
    *Written=written;*Slice=candidate;*NextProgress=next;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();return status;
}

int GfxPagingPlanCapturedGraph(PAGING_GRAPH_CAPTURE* Capture,unsigned Band,unsigned Action,
    unsigned MaxMoves,PAGING_GRAPH_BATCH* Batch,unsigned* NextBand,unsigned* NextAction)
{
    unsigned count=0,next=0,required=0;int result;
    RtlZeroMemory(Batch,sizeof(*Batch));*NextBand=Band;*NextAction=Action;
    if(!Capture || Band>Capture->BandCount || (Band==Capture->BandCount && Action))return PagingPermutationInvalid;
    for(;Band<Capture->BandCount;Band++,Action=0) {
        PAGING_PAGE_BAND* band=Capture->Bands+Band;
        // A resumed batch uses the same immutable graph and atomic budget.
        // Rebuild only when moving to a different byte band, never per DMA buffer.
        if(Capture->PlannedBand!=Band) {
            if(!PagingPageGraphPlan(Capture->SourceIndex+band->FirstPage,Capture->DestinationIndex+band->FirstPage,
                (unsigned)band->PageCount,Capture->Identities,Capture->Readers,Capture->Writer,Capture->Queue,Capture->Forward,
                Capture->Moves,Capture->PageCount*3,Capture->MaxAtomicMoves,&count))return PagingPermutationInvalid;
            Capture->PlannedBand=Band;Capture->MoveCount=count;
        }
        count=Capture->MoveCount;
        if(Action>count)return PagingPermutationInvalid;
        if(!count)continue;
        result=PagingPermutationBatch(Capture->Moves,count,Action,MaxMoves,&next,&required);
        if(result==PagingPermutationInvalid || result==PagingPermutationNeedCycle)return result;
        Batch->Pages=Capture->Pages;Batch->SystemPages=Capture->SystemPages;Batch->Identities=Capture->Identities;
        Batch->Moves=Capture->Moves+Action;Batch->Count=next-Action;Batch->Offset=band->Offset;Batch->Bytes=band->Bytes;
        *NextBand=next==count?Band+1:Band;*NextAction=next==count?0:next;
        return *NextBand==Capture->BandCount?PagingPermutationDone:PagingPermutationMore;
    }
    *NextBand=Band;*NextAction=0;return PagingPermutationDone;
}

// Emit only from retained identities. Batch and proposed cursor become usable
// together on success; publication, not construction, advances the owner cursor.
NTSTATUS GfxPagingEmitCapturedGraph(BC250_DEVICE* Device,PAGING_GRAPH_CAPTURE* Capture,
    unsigned Band,unsigned Action,PVOID Buffer,ULONG Offset,ULONG Free,ULONG* Written,
    PAGING_GRAPH_BATCH* Batch,unsigned* NextBand,unsigned* NextAction)
{
    BC250_PHYSICAL_STREAM stream;BC250_GFX* gfx;
    PAGING_GRAPH_BATCH batch;unsigned nextBand=Band,nextAction=Action;
    unsigned budget,fresh,i,used=0;int result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Written=0;*NextBand=Band;*NextAction=Action;RtlZeroMemory(Batch,sizeof(*Batch));
    if(!Device || !Capture || !Buffer || (Offset&3) || Offset>BC250_GFX_PAGING_BUFFER_BYTES || !Capture->RequiredDwords)return status;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr ||
       gfx->PagingCopyStaging.size<PAGE_SIZE || gfx->PagingCopyStaging.mc!=Capture->ScratchMc ||
       Device->VramMcBase!=Capture->VramMcBase || (ULONGLONG)Device->VramPhysical.QuadPart!=Capture->VramPhysical)goto Done;
    fresh=PagingStreamCapacity(BC250_GFX_PAGING_BUFFER_BYTES,0,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if(Capture->MaxAtomicMoves>fresh/Capture->RequiredDwords)goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=GfxPagingPlanCapturedGraph(Capture,Band,Action,budget/Capture->RequiredDwords,&batch,&nextBand,&nextAction);
    if(result==PagingPermutationNeedCycle){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    if(result!=PagingPermutationDone && result!=PagingPermutationMore)goto Done;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Payload=(unsigned*)Buffer;stream.Common.CommandOffset=Offset;
    for(i=0;i<batch.Count;i++) {
        unsigned ids[2]={batch.Moves[i].source,batch.Moves[i].destination};
        PAGING_U64 mc[2];unsigned side,written=0;
        for(side=0;side<2;side++) {
            if(ids[side]==PAGING_PERMUTATION_SCRATCH)mc[side]=gfx->PagingCopyStaging.mc;
            else if(batch.SystemPages[ids[side]]) {
                if(!gfx->PagingWindowReady)goto Done;
                mc[side]=(batch.Pages[ids[side]]+batch.Offset)|PAGING_SYSTEM_ADDRESS;
            } else if(!PagingPhysicalToMc(batch.Pages[ids[side]]+batch.Offset,batch.Bytes,
                Capture->VramPhysical,Capture->VramMcBase,Device->VramLength,&mc[side]))goto Done;
        }
        result=PagingEmit(&stream,(unsigned*)Buffer+used,budget-used,mc[0],mc[1],batch.Bytes,&written);
        if(result!=BC250_SDMA_PAGING_OK || !written || written>Capture->RequiredDwords)goto Done;
        used+=written;
    }
    *Written=used;*Batch=batch;*NextBand=nextBand;*NextAction=nextAction;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    return status;
}

// Validate the complete requested interval before the first batch can escape.
// O(1): PFN contents are still checked page by page under the OS MDL lifetime.
BOOLEAN GfxPagingEndpointValid(const BC250_DEVICE* Device,
    const BC250_PAGING_ENDPOINT* Endpoint, ULONGLONG Bytes)
{
    ULONGLONG start,end,begin;
    if (!Endpoint || !Bytes || Bytes>Endpoint->Length) return FALSE;
    if (Endpoint->Aperture) {
        ULONGLONG base=Device->WddmAperture.mc,length=Device->WddmAperture.bytes;
        return !Endpoint->Mdl && Endpoint->Address>=base && Endpoint->Address-base<length &&
            Bytes<=length-(Endpoint->Address-base) && Bytes<=MAXULONGLONG-Endpoint->Address;
    }
    if (Endpoint->Mdl) {
        begin=MmGetMdlByteOffset(Endpoint->Mdl);
        if (sizeof(PFN_NUMBER)!=sizeof(PAGING_U64) ||
            begin>=PAGE_SIZE || !MmGetMdlByteCount(Endpoint->Mdl)) return FALSE;
        end=begin+(ULONGLONG)MmGetMdlByteCount(Endpoint->Mdl);
        start=(ULONGLONG)Endpoint->FirstPage<<PAGE_SHIFT;
        return start>=begin && start<end && Bytes<=end-start;
    }
    start=Endpoint->Address;
    if (start<Device->VramMcBase || start-Device->VramMcBase>=Device->VramLength ||
        Bytes>Device->VramLength-(start-Device->VramMcBase) ||
        Bytes>MAXULONGLONG-start) return FALSE;
    // The high bit is reserved for the internal system-page route marker.
    return ((start | (start+Bytes-1)) & PAGING_SYSTEM_ADDRESS)==0;
}

// PASSIVE_LEVEL. Endpoint selection/segment bounds and external resume encoding
// belong to the WDDM adapter. This builder never maps MDLs or takes ownership.
NTSTATUS GfxPagingBuildPhysical(_Inout_ BC250_DEVICE* Device,
    _In_opt_ const BC250_PAGING_ENDPOINT* Source, _In_ const BC250_PAGING_ENDPOINT* Destination,
    BOOLEAN Fill, ULONGLONG Bytes, ULONG Pattern, _Inout_ PVOID DmaBuffer,
    ULONG DmaBufferOffset, ULONG DmaBufferFree, ULONGLONG StartByte,
    _Out_ ULONG* DwordsWritten, _Out_ ULONGLONG* NextByte)
{
    BC250_PHYSICAL_STREAM stream;
    BC250_GFX* gfx;
    unsigned budget,written=0;
    PAGING_U64 next=StartByte,src=0,dst;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int result;
    *DwordsWritten=0; *NextByte=StartByte;
    if (!DmaBuffer || !Destination || (!Fill && !Source) || !Bytes || StartByte>Bytes ||
        !GfxPagingEndpointValid(Device,Destination,Bytes) ||
        (!Fill && !GfxPagingEndpointValid(Device,Source,Bytes)) ||
        (DmaBufferOffset & 3u)!=0 || DmaBufferOffset>BC250_GFX_PAGING_BUFFER_BYTES)
        return status;
    dst=Destination->Mdl ? 0 : Destination->Address;
    if (!Fill) src=Source->Mdl ? 0 : Source->Address;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingDevicePtr || !gfx->PagingRing) goto Done;
    RtlZeroMemory(&stream,sizeof(stream));
    stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Fill=Fill;stream.Common.Pattern=Pattern;
    stream.Common.Payload=(unsigned*)DmaBuffer;stream.Common.CommandOffset=DmaBufferOffset;
    stream.Source=Source;stream.Destination=Destination;
    budget=PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=PagingStreamBuildEndpoints64(&stream,PagingResolveSource,PagingResolveDestination,
        PagingEmit,Fill,src,dst,Bytes,StartByte,(unsigned*)DmaBuffer,budget,&written,&next);
    if (result!=PagingStreamDone && result!=PagingStreamMore) goto Done;
    *DwordsWritten=written;*NextByte=next;
    status=result==PagingStreamMore ? STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER : STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// Capture the same identities used by this one-page packet transaction. No
// shadow change occurs here; publication owns the later logical commit.
static NTSTATUS PagingBuildCopyPageCore(BC250_DEVICE* Device, ULONGLONG Root, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    BC250_PHYSICAL_STREAM stream;
    BC250_PAGING_COPY_SLICE candidate;
    BC250_GFX* gfx;
    PAGING_U64 src,dst,srcMc,dstMc;
    unsigned budget,written=0;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int overlap,result;
    *Written=0;RtlZeroMemory(Slice,sizeof(*Slice));
    if (!Buffer || !Source || !Destination || !Bytes || Progress>MAXULONGLONG-Bytes ||
        (Offset&3)!=0 || Offset>BC250_GFX_PAGING_BUFFER_BYTES) return status;
    if (Root) {
        if (Source->Mdl || Destination->Mdl ||
            Source->Address>MAXULONGLONG-(Progress+Bytes) ||
            Destination->Address>MAXULONGLONG-(Progress+Bytes)) return status;
    } else if (!GfxPagingEndpointValid(Device,Source,Progress+Bytes) ||
               !GfxPagingEndpointValid(Device,Destination,Progress+Bytes)) return status;
    src=(Source->Mdl?0:Source->Address)+Progress;
    dst=(Destination->Mdl?0:Destination->Address)+Progress;
    if (Bytes>PAGE_SIZE-(src&(PAGE_SIZE-1)) || Bytes>PAGE_SIZE-(dst&(PAGE_SIZE-1))) return status;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr) goto Done;
    RtlZeroMemory(&stream,sizeof(stream));RtlZeroMemory(&candidate,sizeof(candidate));
    stream.Common.Device=Device;stream.Common.Gfx=gfx;stream.Common.Payload=(unsigned*)Buffer;
    stream.Common.CommandOffset=Offset;stream.Source=Source;stream.Destination=Destination;
    stream.Common.Root=Root;
    if (Root) {
        if (!PagingResolve(&stream.Common,src,Bytes,&srcMc) ||
            !PagingResolve(&stream.Common,dst,Bytes,&dstMc)) goto Done;
    } else if (!PagingResolveSource(&stream,src,Bytes,&srcMc) ||
               !PagingResolveDestination(&stream,dst,Bytes,&dstMc)) goto Done;
    candidate.SourceSystem=(srcMc&PAGING_SYSTEM_ADDRESS)!=0;
    candidate.DestinationSystem=(dstMc&PAGING_SYSTEM_ADDRESS)!=0;
    candidate.SourcePhysical=candidate.SourceSystem ? srcMc&~PAGING_SYSTEM_ADDRESS : srcMc-Device->VramMcBase;
    candidate.DestinationPhysical=candidate.DestinationSystem ? dstMc&~PAGING_SYSTEM_ADDRESS : dstMc-Device->VramMcBase;
    if (!candidate.SourceSystem) {
        if (candidate.SourcePhysical>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) goto Done;
        candidate.SourcePhysical+=(ULONGLONG)Device->VramPhysical.QuadPart;
    }
    if (!candidate.DestinationSystem) {
        if (candidate.DestinationPhysical>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) goto Done;
        candidate.DestinationPhysical+=(ULONGLONG)Device->VramPhysical.QuadPart;
    }
    overlap=candidate.SourcePhysical<=candidate.DestinationPhysical ?
        candidate.DestinationPhysical-candidate.SourcePhysical<Bytes :
        candidate.SourcePhysical-candidate.DestinationPhysical<Bytes;
    // Preserve aliased system data while both GART mappings remain active.
    if ((overlap || ForceSnapshot) && (candidate.SourceSystem || candidate.DestinationSystem)) {
        if (gfx->PagingCopyStaging.size<PAGE_SIZE || !gfx->PagingCopyStaging.mc) goto Done;
        stream.Common.StagingMc=gfx->PagingCopyStaging.mc;
    }
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if ((overlap || ForceSnapshot) && !candidate.SourceSystem && !candidate.DestinationSystem) {
        ULONGLONG marker=bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT);
        if (!marker || gfx->PagingCopyStaging.size<PAGE_SIZE) goto Done;
        result=bc250_sdma_paging_copy_bytes(gfx->PagingDevicePtr,(u32*)Buffer,budget,srcMc,dstMc,Bytes,
            gfx->PagingCopyStaging.mc,marker,1u+3u*(Offset/4u),&written);
    } else result=PagingEmit(&stream,(unsigned*)Buffer,budget,srcMc,dstMc,Bytes,&written);
    if (result==BC250_SDMA_PAGING_INSUFFICIENT) {status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    if (result!=BC250_SDMA_PAGING_OK) goto Done;
    candidate.Bytes=Bytes;*Slice=candidate;*Written=written;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS GfxPagingBuildCopyPageEx(BC250_DEVICE* Device, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    return PagingBuildCopyPageCore(Device,0,Source,Destination,Progress,Bytes,ForceSnapshot,
        Buffer,Offset,Free,Written,Slice);
}

// Resolve both VAs under the same engine lifetime lock and return exactly the
// identities encoded in the packet. Whole-transfer ordering belongs to the caller.
NTSTATUS GfxPagingBuildVirtualCopyPage(BC250_DEVICE* Device, ULONGLONG Root,
    ULONGLONG SourceVa, ULONGLONG DestinationVa, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    BC250_PAGING_ENDPOINT source,destination;
    *Written=0;RtlZeroMemory(Slice,sizeof(*Slice));
    if (!Root) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&source,sizeof(source));RtlZeroMemory(&destination,sizeof(destination));
    source.Address=SourceVa;destination.Address=DestinationVa;
    return PagingBuildCopyPageCore(Device,Root,&source,&destination,0,Bytes,ForceSnapshot,
        Buffer,Offset,Free,Written,Slice);
}

NTSTATUS GfxPagingBuildCopyPage(BC250_DEVICE* Device, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    return GfxPagingBuildCopyPageEx(Device,Source,Destination,Progress,Bytes,FALSE,
        Buffer,Offset,Free,Written,Slice);
}

// Build in OS-owned aperture DMA memory. CSA is outside the executable IB.
// A192-byte minimum span pays for the worst34DWORD root/TLB/IB ring expansion,
// so the existing whole-DMA ring budget also bounds mixed native/direct work.
NTSTATUS GfxPagingBuildNative(BC250_DEVICE* Device, BOOLEAN Fill, ULONGLONG Source,
    ULONGLONG Destination, ULONGLONG Total, ULONG Pattern, PVOID Buffer,
    ULONGLONG DmaBase, ULONG Offset, ULONG Free, ULONG Token, PAGING_NATIVE_RESULT* Built)
{
    BC250_GFX* gfx;
    ULONGLONG progress,startVa,begin;
    unsigned budget,minimum,capacity,used=0,next=Token;
    u32* ib;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if(!Built)return status;
    RtlZeroMemory(Built,sizeof(*Built));Built->NextToken=Token;
    if(!Device || !Buffer || !DmaBase || ((DmaBase|Offset)&3u) ||
       DmaBase>MAXULONGLONG-Offset ||
       !PagingStreamTokenDecode(Fill,Source,Destination,Total,Token,&progress))return status;
    if(progress==Total)return STATUS_SUCCESS;
    startVa=DmaBase+Offset;begin=progress;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingDevicePtr || !gfx->PagingRing)goto Done;
    budget=4u*PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if(startVa>MAXULONGLONG-budget)goto Done;
    minimum=192u+((0u-(ULONG)(startVa+192u))&31u);
    if(budget<minimum){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    Built->CsaOffset=(0u-(ULONG)startVa)&63u;
    Built->IbOffset=Built->CsaOffset+PAGING_PRIVATE_CSA_BYTES;
    capacity=((budget-Built->IbOffset)/4u)&~7u;
    ib=(u32*)((UCHAR*)Buffer+Built->IbOffset);
    RtlZeroMemory(Buffer,Built->IbOffset);
    while(progress<Total) {
        unsigned count=(unsigned)((Total-progress)>0x400000u ? 0x400000u : Total-progress);
        unsigned written=0,token;
        int result;
        // A partial packet boundary must be representable by the stateless
        // page-slice token. The final packet may finish inside a page.
        if(count<Total-progress)count-=(unsigned)((Destination+progress+count)&4095u);
        if(!count || !PagingStreamTokenEncode(Fill,Source,Destination,Total,progress+count,&token))goto Done;
        result=Fill ? bc250_sdma_paging_fill(gfx->PagingDevicePtr,ib+used,capacity-used,
                     Destination+progress,Pattern,count,&written) :
                     bc250_sdma_paging_copy(gfx->PagingDevicePtr,ib+used,capacity-used,
                     Source+progress,Destination+progress,count,&written);
        if(result==BC250_SDMA_PAGING_INSUFFICIENT)break;
        if(result!=BC250_SDMA_PAGING_OK)goto Done;
        used+=written;progress+=count;next=token;
    }
    if(!used){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    while(used&7u)ib[used++]=gfx->PagingRing->funcs->nop;
    Built->IbDwords=used;Built->Bytes=Built->IbOffset+used*4u;
    if(Built->Bytes<minimum) {
        RtlZeroMemory((UCHAR*)Buffer+Built->Bytes,minimum-Built->Bytes);Built->Bytes=minimum;
    }
    Built->Moved=progress-begin;Built->NextToken=next;
    status=progress==Total ? STATUS_SUCCESS : STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    return status;
}

// Resolve exactly one page slice and return the identity used by its packets.
// The caller publishes its logical table effect before constructing another slice.
NTSTATUS GfxPagingBuildFillPage(BC250_DEVICE* Device, ULONGLONG Root, ULONGLONG Va,
    ULONG Bytes, ULONG Pattern, PVOID Buffer, ULONG Offset, ULONG Free,
    ULONG* Written, ULONGLONG* Physical, BOOLEAN* System)
{
    BC250_PAGING_STREAM stream;
    BC250_GFX* gfx;
    PAGING_U64 mc;
    unsigned budget,written=0;
    int result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Written=0;*Physical=0;*System=FALSE;
    if (!Root || !Buffer || !Bytes || Bytes>PAGE_SIZE-(Va&(PAGE_SIZE-1)) ||
        ((Va|Bytes|Offset)&3)!=0 || Va>MAXULONGLONG-Bytes || Offset>BC250_GFX_PAGING_BUFFER_BYTES)
        return status;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingDevicePtr || !gfx->PagingRing) goto Done;
    RtlZeroMemory(&stream,sizeof(stream));
    stream.Device=Device;stream.Gfx=gfx;stream.Root=Root;stream.Fill=TRUE;stream.Pattern=Pattern;
    stream.Payload=(unsigned*)Buffer;stream.CommandOffset=Offset;
    if (!PagingResolve(&stream,Va,Bytes,&mc)) goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=PagingEmit(&stream,(unsigned*)Buffer,budget,0,mc,Bytes,&written);
    if (result==BC250_SDMA_PAGING_INSUFFICIENT) { status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done; }
    if (result!=BC250_SDMA_PAGING_OK) goto Done;
    *System=(mc&PAGING_SYSTEM_ADDRESS)!=0;
    *Physical=*System ? mc&~PAGING_SYSTEM_ADDRESS :
        (ULONGLONG)Device->VramPhysical.QuadPart+(mc-Device->VramMcBase);
    *Written=written;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// Build one permanent aperture batch. OS MDLs describe page identities here,
// including partial boundary pages; byte-copy interval rules do not apply.
NTSTATUS GfxPagingBuildAperture(BC250_DEVICE* Device, const BC250_PAGING_APERTURE_OP* Operation,
    ULONG StartPage, PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, ULONG* NextPage)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev;
    PAGING_APERTURE live;
    ULONGLONG mc,table,pages=0,physical,flags,marker;
    u64 entries[256];
    unsigned budget,count,i,written=0;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int result;
    *Written=0;*NextPage=StartPage;
    if (!Device || !Operation || !Buffer || (Offset&3u) || Offset>BC250_GFX_PAGING_BUFFER_BYTES ||
        !PagingApertureRange(&Device->WddmAperture,Operation->FirstPage,Operation->PageCount,&mc,&table) ||
        StartPage>Operation->PageCount) return status;
    if (Operation->Unmap) {
        if ((Operation->DummyPhysical&~AMDGPU_PTE_ADDR_MASK)!=0) return status;
    } else {
        ULONGLONG begin,end;
        if (!Operation->Mdl || sizeof(PFN_NUMBER)!=sizeof(PAGING_U64)) return status;
        begin=MmGetMdlByteOffset(Operation->Mdl);
        if (begin>=PAGE_SIZE || !MmGetMdlByteCount(Operation->Mdl)) return status;
        end=begin+(ULONGLONG)MmGetMdlByteCount(Operation->Mdl);
        pages=(end+PAGE_SIZE-1)>>PAGE_SHIFT;
        if (Operation->MdlOffset>=pages || Operation->PageCount>pages-Operation->MdlOffset) return status;
    }
    if (StartPage==Operation->PageCount) return STATUS_SUCCESS;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr) {
        status=STATUS_DEVICE_NOT_READY;goto Done;
    }
    adev=gfx->PagingDevicePtr;
    // The live GART must still hold the geometry this start advertised, at this start's size.
    if (!adev->gart.bo || !PagingApertureInit(adev->gmc.gart_start,adev->gmc.gart_size,
            adev->gart.bo->gpu_addr,adev->gart.table_size,Device->WddmAperture.bytes,&live) ||
        live.mc!=Device->WddmAperture.mc || live.table!=Device->WddmAperture.table ||
        live.bytes!=Device->WddmAperture.bytes) goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    budget &= ~gfx->PagingRing->funcs->align_mask;
    if (budget<32u) {status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    count=(budget-29u)/2u;
    if (count>RTL_NUMBER_OF(entries)) count=RTL_NUMBER_OF(entries);
    if (count>Operation->PageCount-StartPage) count=(unsigned)(Operation->PageCount-StartPage);
    flags=bc250_gart_pte_flags(adev);
    // Match amdgpu cached TT flags. Noncoherent map requests need no host snoop;
    // dummy pages retain the cached-page default, independently of previous flags.
    if (!Operation->Unmap && !Operation->CacheCoherent) flags &= ~AMDGPU_PTE_SNOOPED;
    for (i=0;i<count;i++) {
        if (Operation->Unmap) physical=Operation->DummyPhysical;
        else if (!PagingPageListAddress((const PAGING_U64*)MmGetMdlPfnArray(Operation->Mdl),
                (unsigned)pages,0,0,Operation->MdlOffset,
                ((ULONGLONG)StartPage+i)*PAGE_SIZE,PAGE_SIZE,&physical) ||
                 (physical&~AMDGPU_PTE_ADDR_MASK)!=0) goto Done;
        entries[i]=bc250_gart_pte(physical,flags);
    }
    marker=bc250_sdma_fence_addr(adev,BC250_PAGING_MARKER_SLOT);
    result=bc250_sdma_paging_set_aperture(adev,(u32*)Buffer,budget,table+(ULONGLONG)StartPage*8,
        entries,count,marker,1u+3u*(Offset/4u),&written);
    if (result==BC250_SDMA_PAGING_OK) {
        *Written=written;*NextPage=StartPage+count;
        status=*NextPage==Operation->PageCount?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    } else if (result==BC250_SDMA_PAGING_INSUFFICIENT) status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    return status;
}

// PASSIVE_LEVEL. Resolve each page independently and emit into this call's
// dxgkrnl-owned private payload. The caller copies emitted bytes to pDmaBuffer.
// No global command shadow. The device lock protects pointer lookup and engine use.
NTSTATUS GfxPagingBuild(_Inout_ BC250_DEVICE* Device, ULONGLONG RootPhysical, BOOLEAN Fill, ULONGLONG SrcVa,
                        ULONGLONG DstVa, ULONGLONG Bytes, ULONG FillPattern, _Inout_ PVOID DmaBuffer,
                        ULONG DmaBufferOffset, ULONG DmaBufferFree, ULONG StartByte,
                        _Out_ ULONG* DwordsWritten, _Out_ ULONG* NextByte,
                        _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    BC250_PAGING_STREAM stream;
    unsigned int budget, written = 0, next = StartByte;
    u32* payload;
    int result;
    NTSTATUS status = STATUS_SUCCESS;
    *DwordsWritten = 0; *NextByte = StartByte; *Unsupported = BC250PagingSupported;
    if (DmaBuffer == NULL || Bytes == 0 || Bytes > 0xFFFFFFFFu || StartByte > Bytes)
        return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL) { status = STATUS_INVALID_PARAMETER; goto Done; }
    if (!gfx->PagingReady || gfx->PagingDevicePtr == NULL) { *Unsupported = BC250PagingNotReady; goto Done; }
    if (RootPhysical == 0) { *Unsupported = BC250PagingNoRoot; goto Done; }
    if ((DmaBufferOffset & 3u) != 0 || DmaBufferOffset > BC250_GFX_PAGING_BUFFER_BYTES) {
        status = STATUS_INVALID_PARAMETER; goto Done;
    }
    // The 64KiB OS command buffer is larger than the live SDMA reservation. Bound
    // accumulated packets too, reserving fence and amdgpu_ring_alloc alignment.
    budget = PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
                                  gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
                                  bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    payload = (u32*)DmaBuffer; // this call's dxgkrnl-owned private payload
    stream.Device=Device; stream.Gfx=gfx; stream.Root=RootPhysical;
    stream.CommandOffset=DmaBufferOffset; stream.Payload=payload;
    stream.Fill=Fill; stream.Pattern=FillPattern;stream.StagingMc=0; stream.Unsupported=BC250PagingSupported;
    result=PagingStreamBuild(&stream,PagingResolve,PagingEmit,Fill,SrcVa,DstVa,(unsigned)Bytes,
                            StartByte,payload,budget,&written,&next);
    if (result==PagingStreamAddress) { *Unsupported=stream.Unsupported; goto Done; }
    if (result==PagingStreamInvalid) { status=STATUS_INVALID_PARAMETER; goto Done; }
    // Publish only complete packets. On translation/emit failure no part of this
    // batch is exposed to dxgkrnl. Earlier submitted batches need fault handling.
    // Packet bytes already reside in this DMA buffer's private payload.
    *DwordsWritten=written; *NextByte=next;
    if (result==PagingStreamMore) status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// Shared lifetime lock excludes the first RUN and teardown. CPU initialization
// is allowed only before that first RUN, never merely because the ring is busy,
// failed or not ready again. Later updates are ordered GPU work with entry-based
// multipass progress (not command bytes). Scratch slot5 is reset at submission.
NTSTATUS GfxPagingBuildUpdate(_Inout_ BC250_DEVICE* Device,
                             _In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                             _Inout_ PVOID DmaBuffer, ULONG DmaBufferOffset, ULONG DmaBufferFree,
                             ULONG StartEntry, _Out_ ULONG* DwordsWritten, _Out_ ULONG* NextEntry,
                             _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    ULONGLONG entries[480], physical, mc; // keep fixed kernel frame below4096bytes
    unsigned budget, count, written = 0;
    u64 scratch;
    int result;
    NTSTATUS status = STATUS_SUCCESS;
    *DwordsWritten = 0; *NextEntry = StartEntry; *Unsupported = BC250PagingSupported;
    if (Update == NULL || Update->NumPageTableEntries == 0 || Update->NumPageTableEntries > 512 ||
        StartEntry >= Update->NumPageTableEntries || DmaBuffer == NULL ||
        (DmaBufferOffset & 3u) != 0 || DmaBufferOffset > BC250_GFX_PAGING_BUFFER_BYTES)
        return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL) { *Unsupported = BC250PagingNotReady; status = STATUS_DEVICE_NOT_READY; goto Done; }
    if (gfx->PagingCpuBootstrap) {
        if (StartEntry != 0 || !VidMmEncodePageTable(Update,0,1,&physical,entries)) {
            status = STATUS_INVALID_PARAMETER; goto Done;
        }
        status = VidMmUpdatePageTable(Update);
        if (NT_SUCCESS(status)) *NextEntry = Update->NumPageTableEntries;
        goto Done;
    }
    if (!gfx->PagingReady || gfx->PagingDevicePtr == NULL) {
        *Unsupported = BC250PagingNotReady; status = STATUS_DEVICE_NOT_READY; goto Done;
    }
    budget = PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
                                  gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
                                  bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    budget &= ~gfx->PagingRing->funcs->align_mask;
    if (budget < 16) { status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; goto Done; }
    count = (budget - 14u) / 2u;
    if (count > RTL_NUMBER_OF(entries)) count = RTL_NUMBER_OF(entries);
    if (count > Update->NumPageTableEntries - StartEntry) count = Update->NumPageTableEntries - StartEntry;
    if (!VidMmEncodePageTable(Update,StartEntry,count,&physical,entries) ||
        !PagingPhysicalToMc(physical,count*8u,(ULONGLONG)Device->VramPhysical.QuadPart,
                            Device->VramMcBase,Device->VramLength,&mc)) {
        status = STATUS_INVALID_PARAMETER; goto Done;
    }
    scratch = bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT);
    result = bc250_sdma_paging_update_ptes(gfx->PagingDevicePtr,(u32*)DmaBuffer,budget,mc,
                                          entries,count,scratch,1u+3u*(DmaBufferOffset/4u),&written);
    if (result == BC250_SDMA_PAGING_OK) {
        *DwordsWritten = written; *NextEntry = StartEntry + count;
        if (*NextEntry != Update->NumPageTableEntries) status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    } else if (result == BC250_SDMA_PAGING_INSUFFICIENT) status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else status = STATUS_INVALID_PARAMETER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// One complete CopyPageTableEntries range. The DDI owns list/multipass progress
// and logical publication. Both current page-table segments are local VRAM.
NTSTATUS GfxPagingBuildCopyRange(_Inout_ BC250_DEVICE* Device, ULONGLONG Root,
    _In_ const DXGK_BUILDPAGINGBUFFER_COPY_RANGE* Range, _Inout_ PVOID DmaBuffer,
    ULONG DmaBufferOffset, ULONG DmaBufferFree, _Out_ ULONG* DwordsWritten,
    _Out_ ULONGLONG* SourcePhysical, _Out_ ULONGLONG* DestinationPhysical,
    _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    ULONGLONG source,destination,srcMc,dstMc;
    BOOLEAN sourceSystem,destinationSystem;
    unsigned budget,written=0,bytes;
    int result;
    NTSTATUS status=STATUS_SUCCESS;
    *DwordsWritten=0;*SourcePhysical=0;*DestinationPhysical=0;*Unsupported=BC250PagingSupported;
    if (Range==NULL || DmaBuffer==NULL || Root==0 || !Range->NumPageTableEntries ||
        Range->SrcStartPteIndex>=512 || Range->DstStartPteIndex>=512 ||
        Range->NumPageTableEntries>512-Range->SrcStartPteIndex ||
        Range->NumPageTableEntries>512-Range->DstStartPteIndex ||
        ((Range->SrcPageTableAddress|Range->DstPageTableAddress)&65535ull)!=0 ||
        Range->SrcPageTableAddress>0xffffffffffffull-4095 ||
        Range->DstPageTableAddress>0xffffffffffffull-4095 ||
        (DmaBufferOffset&3u)!=0 || DmaBufferOffset>BC250_GFX_PAGING_BUFFER_BYTES)
        return STATUS_INVALID_PARAMETER;
    bytes=Range->NumPageTableEntries*8u;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (gfx==NULL || !gfx->PagingReady || gfx->PagingDevicePtr==NULL ||
        gfx->PagingRing==NULL || gfx->PagingCopyStaging.size<PAGE_SIZE) {
        *Unsupported=BC250PagingNotReady;status=STATUS_DEVICE_NOT_READY;goto Done;
    }
    if (!VidMmTranslatePaging(Root,Range->SrcPageTableAddress+(ULONGLONG)Range->SrcStartPteIndex*8u,&source,&sourceSystem) ||
        !VidMmTranslatePaging(Root,Range->DstPageTableAddress+(ULONGLONG)Range->DstStartPteIndex*8u,&destination,&destinationSystem)) {
        *Unsupported=BC250PagingNoTranslation;status=STATUS_INVALID_PARAMETER;goto Done;
    }
    if (sourceSystem || destinationSystem) {
        *Unsupported=BC250PagingSystemMemory;status=STATUS_INVALID_PARAMETER;goto Done;
    }
    if (!PagingPhysicalToMc(source,bytes,(ULONGLONG)Device->VramPhysical.QuadPart,Device->VramMcBase,Device->VramLength,&srcMc) ||
        !PagingPhysicalToMc(destination,bytes,(ULONGLONG)Device->VramPhysical.QuadPart,Device->VramMcBase,Device->VramLength,&dstMc)) {
        *Unsupported=BC250PagingNoTranslation;status=STATUS_INVALID_PARAMETER;goto Done;
    }
    budget=PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=bc250_sdma_paging_copy_ptes(gfx->PagingDevicePtr,(u32*)DmaBuffer,budget,srcMc,dstMc,
        Range->NumPageTableEntries,gfx->PagingCopyStaging.mc,
        bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT),
        1u+3u*(DmaBufferOffset/4u),&written);
    if (result==BC250_SDMA_PAGING_OK) {
        *DwordsWritten=written;*SourcePhysical=source;*DestinationPhysical=destination;
    } else if (result==BC250_SDMA_PAGING_INSUFFICIENT) status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else status=STATUS_INVALID_PARAMETER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// PASSIVE_LEVEL. DXGK_OPERATION_FLUSH_TLB: invalidate whole VMIDs instead of a range. Over-invalidation avoids
// capturing a mutable process binding while this OS paging buffer waits for submission. The OS paging fence
// follows the ACK poll, so dependent work cannot observe a reported-but-unexecuted flush.
//
// Root is the root page table the OS names, as VidMmRootPhysical resolves it (0 when it does not resolve). With
// EnableVmidPool 0 the buffer invalidates VMID 1, the single WDDM VMID, exactly as 0.7.213.1. With the pool it
// invalidates Bc250VmidFlushMask: every VMID that holds Root, every pool member and every other VMID holding a
// root at all, because the VMID that holds Root when this buffer is built need not be the one that holds it when
// SDMA executes it (vmid_pool.h). One invalidation per VMID, back to back in one record; all or nothing, so an
// INSUFFICIENT answer leaves no partial set behind and the next buffer starts the whole set again.
NTSTATUS GfxPagingBuildFlush(_Inout_ BC250_DEVICE* Device, ULONGLONG Root,
                            _Inout_ PVOID DmaBuffer, ULONG DmaBufferOffset, ULONG DmaBufferFree,
                            _Out_ ULONG* DwordsWritten,
                            _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    unsigned budget, written = 0, total = 0, mask, count = 0, v;
    int result = BC250_SDMA_PAGING_OK;
    BOOLEAN pool;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;
    *DwordsWritten = 0; *Unsupported = BC250PagingSupported;
    if (DmaBuffer == NULL || (DmaBufferOffset & 3u) != 0 ||
        DmaBufferOffset > BC250_GFX_PAGING_BUFFER_BYTES) return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL || !gfx->PagingReady || gfx->PagingDevicePtr == NULL) {
        *Unsupported = BC250PagingNotReady;
        status = STATUS_DEVICE_NOT_READY;
        goto Done;
    }
    budget = PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
                                  gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
                                  bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    pool = gfx->VmidPoolGate;
    if (pool) {
        // The submit path changes the table under GartLock and VmidLock; this path holds GfxPagingLock only.
        KeAcquireSpinLock(&gfx->VmidLock, &irql);
        mask = Bc250VmidFlushMask(&gfx->Vmid, gfx->VmidMembers, Root);
        KeReleaseSpinLock(&gfx->VmidLock, irql);
    } else mask = 1u << BC250_VMID_LEGACY;
    for (v = 0; v < BC250_VMID_COUNT && result == BC250_SDMA_PAGING_OK; v++) {
        if (!(mask & (1u << v))) continue;
        written = 0;
        result = bc250_sdma_paging_invalidate_vmid(gfx->PagingDevicePtr,(u32*)DmaBuffer + total,budget - total,v,
                                                   &written);
        if (result == BC250_SDMA_PAGING_OK) { total += written; count++; }
    }
    if (result == BC250_SDMA_PAGING_OK) {
        *DwordsWritten = total;
        if (pool) {
            LONG n = InterlockedIncrement(&gfx->VmidFlushes);
            InterlockedExchangeAdd(&gfx->VmidFlushVmids, (LONG)count);
            if (n <= 4)
                GuardLog("gfx: FLUSH_TLB root 0x%llX -> VMIDs 0x%04lX (%lu, %lu dwords)", Root, (ULONG)mask,
                         (ULONG)count, (ULONG)total);
        }
    }
    else if (result == BC250_SDMA_PAGING_INSUFFICIENT) status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else status = STATUS_INVALID_PARAMETER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// DISPATCH_LEVEL. Validate all private records before reserving/writing the ring.
// Direct records copy words; native records emit root/TLB/IB for retained OS DMA
// storage. One outer fence governs both. CPU private pointers are not retained.
static int PagingCountRing(void* Context, const PAGING_PRIVATE_SPAN* Span)
{
    ULONG* count=(ULONG*)Context;
    // Maximum INDIRECT padding is7DWORDs, followed by its6DWORD body.
    ULONG words=Span->Kind==PAGING_PRIVATE_DIRECT ? Span->Dwords : BC250_SDMA_VM_FLUSH_DWORDS+7u+6u;
    if(words>MAXULONG-*count)return 0;
    *count+=words;
    return 1;
}

static int PagingWriteRing(void* Context, const PAGING_PRIVATE_SPAN* Span)
{
    struct amdgpu_ring* ring=(struct amdgpu_ring*)Context;
    if(Span->Kind==PAGING_PRIVATE_DIRECT) {
        amdgpu_ring_write_multiple(ring,Span->Words,(int)Span->Dwords);
        return 1;
    }
    return bc250_sdma_emit_vm_flush(ring,BC250_SDMA_PAGING_VMID,Span->RootPhysical)==0 &&
           bc250_sdma_emit_ib(ring,Span->IbAddress,Span->Dwords,
                             BC250_SDMA_PAGING_VMID,Span->CsaAddress)==0;
}

// BD-097: one successful paging submit's line, rate limited (log_rate.h). The decision is made under
// PagingLogLock, which this is the only taker of, and the log call happens outside it. A summary line stands for
// the submits that wrote nothing, with the time they took and the totals of this device start, so a quiet log is
// never read as a quiet node - the same rule D5 gave the node-0 submit lines, with a cadence instead of a gate.
// Nothing here touches the ring or the fence: a failure to log is not a failure to submit.
static void GfxLogPagingSubmit(_Inout_ BC250_GFX* Gfx, ULONG Dwords, ULONGLONG Start, ULONG Seq)
{
    BC250_LOG_RATE_NOTE note;
    KIRQL irql;
    ULONG outcome;

    KeAcquireSpinLock(&Gfx->PagingLogLock, &irql);
    outcome = Bc250LogRateDecide(&Gfx->PagingLogRate, GuardLogMilliseconds(), &note);
    KeReleaseSpinLock(&Gfx->PagingLogLock, irql);

    if (outcome == BC250_LOG_RATE_LINE)
        GuardLog("gfx: paging submit, %lu ring dwords reserved for buffer at 0x%llX, seq %lu", Dwords, Start, Seq);
    else if (outcome == BC250_LOG_RATE_SUMMARY)
        GuardLog("gfx: paging submit, seq %lu at 0x%llX: %lu submits in %llu ms, %lu of %lu with no line",
                 Seq, Start, note.Pending, note.ElapsedMs, note.Skipped, note.Calls);
}

static NTSTATUS GfxSubmitPagingAccess(_Inout_ BC250_DEVICE* Device, const void* PrivateData, ULONG PrivateBytes,
                          ULONGLONG Start, ULONG ByteCount, BOOLEAN VirtualAddress, _Out_ ULONG* Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;
    struct amdgpu_ring* ring;
    KIRQL irql;
    ULONG dwords, count, seq;
    u64 fenceAddr, wptrBytes;
    int result;

    *Seq = 0;
    if (gfx == NULL) return STATUS_DEVICE_NOT_READY;
    if (gfx->PagingSubmitFailed) return STATUS_DEVICE_HARDWARE_ERROR;
    if (!GfxPagingSubmitReadyAccess(Device)) return STATUS_DEVICE_BUSY;
    dwords=0;
    if (!PagingPrivateVisitMixed(PrivateData,PrivateBytes,Start,ByteCount,VirtualAddress,PagingCountRing,&dwords))
        return STATUS_INVALID_PARAMETER;

    if (InterlockedCompareExchange(&gfx->PagingSubmitInFlight, 1, 0) != 0) return STATUS_DEVICE_BUSY;

    ring = gfx->PagingRing;
    fenceAddr = bc250_sdma_fence_addr(gfx->PagingDevicePtr, BC250_PAGING_FENCE_SLOT);
    if (fenceAddr == 0)
    {
        InterlockedExchange(&gfx->PagingSubmitInFlight, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // InterlockedIncrement, not "++": review 23 MUST-FIX. This runs at DISPATCH_LEVEL, outside GartLock, sharing
    // gfx->FenceSeq with GfxSubmitIb/GfxFenceEscape/SdmaCopyEscape, which only ever ran serialized by GartLock
    // before this path existed - a plain "++" here could race one of those on another CPU and tear the counter.
    seq = (ULONG)InterlockedIncrement(&gfx->FenceSeq);
    if (seq == 0) seq = (ULONG)InterlockedIncrement(&gfx->FenceSeq);

    KeAcquireSpinLock(&gfx->Sdma0RingLock, &irql);
    count = dwords + bc250_sdma_fence_size(ring, AMDGPU_FENCE_FLAG_INT);
    result = amdgpu_ring_alloc(ring, count);
    if (result == 0)
    {
        // Previous paging work actually completed before the in-flight claim above.
        // This slot is outside the temporary GART window and remains driver-owned.
        *(volatile u32*)((char*)gfx->PagingDevicePtr->sdma.fence_mem.cpu +
                         BC250_PAGING_MARKER_SLOT*8u)=0;
        KeMemoryBarrier();
        if (!PagingPrivateVisitMixed(PrivateData,PrivateBytes,Start,ByteCount,VirtualAddress,PagingWriteRing,ring))
            result = -1;
        else result = bc250_sdma_emit_fence(ring, fenceAddr, seq, AMDGPU_FENCE_FLAG_INT);
    }
    if (result != 0)
    {
        amdgpu_ring_undo(ring);
        KeReleaseSpinLock(&gfx->Sdma0RingLock, irql);
        InterlockedExchange(&gfx->PagingSubmitInFlight, 0);
        GuardLog("gfx: paging submit of %lu dwords at 0x%llX refused, result %d", dwords, Start, result);
        return STATUS_INVALID_PARAMETER;
    }
    // amdgpu_ring_commit(), minus the doorbell it would ring through adev->backend (bc250_ring.c:79-118): the
    // same padding-to-align_mask, the same write-pointer-in-bytes conversion for an SDMA ring, but published
    // through GpuMemDoorbellWrite, which needs no backend at all.
    count = ring->funcs->align_mask + 1u - (ULONG)(ring->wptr & ring->funcs->align_mask);
    count &= ring->funcs->align_mask;
    if (count != 0) amdgpu_ring_insert_nop(ring, count);
    wptrBytes = ring->wptr << 2;
    if (ring->wptr_cpu_addr != NULL) *(volatile u64*)ring->wptr_cpu_addr = wptrBytes;
    GpuMemDoorbellWrite(Device, ring->doorbell_index, wptrBytes);
    KeReleaseSpinLock(&gfx->Sdma0RingLock, irql);

    gfx->PagingSubmitSeq = seq;
    *Seq = seq;
    GfxLogPagingSubmit(gfx, dwords, Start, seq);
    return STATUS_SUCCESS;
}

NTSTATUS GfxSubmitPaging(_Inout_ BC250_DEVICE* Device, const void* PrivateData, ULONG PrivateBytes,
                          ULONGLONG Start, ULONG ByteCount, BOOLEAN VirtualAddress, _Out_ ULONG* Seq)
{
    NTSTATUS result;
    ProgressEnter(ProgressSitePagingSubmit);
    if (GfxAccessAcquire(Device) == NULL)
    {
        *Seq = 0;
        ProgressExit(ProgressSitePagingSubmit, 0);
        return STATUS_DEVICE_NOT_READY;
    }
    result = GfxSubmitPagingAccess(Device, PrivateData, PrivateBytes, Start, ByteCount, VirtualAddress, Seq);
    GfxAccessRelease(Device);
    ProgressExit(ProgressSitePagingSubmit, (LONG)*Seq);
    return result;
}

// With GartLock held. For gart.c and psp.c: a restore or an unload under a set-up GFX sequence is refused.
BOOLEAN GfxIsActive(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    return gfx != NULL && (gfx->SetUp || gfx->StagesDone != 0);
}

// A PLAN executes no write, so a read that follows one of its own writes would see the old value and the code would
// take a branch the run does not take (the GRBM CAM probe gives up, a read-modify-write starts from the wrong
// value). A PLAN therefore answers a read of a register it has already planned a write to with that value: the
// model of the host replay test (driver/shim/test), and like it a prediction, not a measurement. One alias is
// declared, the one the probe is about: on unit A a write to VGT_ESGS_RING_SIZE_UMD reads back through
// VGT_ESGS_RING_SIZE (E03 trace, 0.549735 s W 0x30900 DEADBEEF, 0.549737 s R 0x088C8 DEADBEEF). Whether that holds
// under Windows is what the run's stage 3 finds out. Registers the hardware changes by itself (status polls) are
// never written by the sequence before they are read, with one exception that ends a PLAN: a ring test.
static BOOLEAN GfxPlanAnswers(_In_ BC250_SEQUENCE* Sequence, ULONG DwordIndex, _Out_ ULONG* Value)
{
    ULONG offset = DwordIndex * 4, alias = offset, i;

    *Value = 0;
    if (Sequence->Writes == NULL) return FALSE;
    if (offset == BC250_REG_GC_VGT_ESGS_RING_SIZE) alias = BC250_REG_GC_VGT_ESGS_RING_SIZE_UMD;
    for (i = min(Sequence->WriteCount, Sequence->MaxWrites); i-- > 0; )
    {
        if (Sequence->Writes[i].Offset != offset && Sequence->Writes[i].Offset != alias) continue;
        *Value = Sequence->Writes[i].Value;
        return TRUE;
    }
    return FALSE;
}

// As in gart.c: the one write that must get through a stopped sequence is giving the invalidation semaphore back
// (gpumem.c flushes the TLB after every change of the GART table). A semaphore left held would wedge every later
// invalidation, the next Linux boot's included (facts M25).
static BOOLEAN GfxPassesFault(_In_ BC250_SEQUENCE* Sequence, ULONG DwordIndex, ULONG Value)
{
    UNREFERENCED_PARAMETER(Sequence);
    return Value == 0 && DwordIndex == BC250_REG_MMHUB_MMVM_INVALIDATE_ENG17_SEM / 4;
}

// Retained adapter power transaction. The coordinator has closed WDDM admission,
// drained OS work and joined IH before suspend. It owns the adapter lifecycle
// throughout both calls. Unlike PnP retirement, no owner mapping is invalidated
// and no allocation, OS handle or completion history is released here.
BOOLEAN GfxPowerIsSuspended(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx=(const BC250_GFX*)Device->Gfx;
    // Caller owns GartLock. This is a cached halt verdict, not a fresh MMIO read.
    return gfx && gfx->SetUp && gfx->PowerSuspended && !gfx->Failed;
}

static void GfxResetRetainedRing(struct amdgpu_ring* Ring)
{
    // All old packets completed before suspend; reset transport positions only.
    // AMD gfx_v10_0_kcq_init_queue(in_suspend) clears the ring/write pointer.
    // The shim rebuilds clean MQDs from the retained ring descriptors instead
    // of copying Linux's MQD backup, which our software context does not own.
    Ring->wptr=0;
    Ring->wptr_old=0;
    Ring->count_dw=0;
    amdgpu_ring_clear_ring(Ring);
}

static NTSTATUS GfxPowerRetainedLocked(BC250_DEVICE* Device, BC250_GFX* Gfx,
                                      struct amdgpu_device* Adev, BOOLEAN Resume)
{
    ULONG i,stage;
    int result=0;
    long undo=0;
    BOOLEAN quiet;
    if (!Gfx->SetUp || Gfx->Failed || Gfx->SubmitFailed || Gfx->PagingSubmitFailed ||
        !NT_SUCCESS(Gfx->Sequence.Fault) || Gfx->StagesDone!=BC250_GFX_STAGE_INTERRUPTS)
        return STATUS_INVALID_DEVICE_STATE;
    if (!Resume && Gfx->PowerSuspended) return STATUS_SUCCESS;
    if (Resume && !Gfx->PowerSuspended) return STATUS_INVALID_DEVICE_STATE;
    if (Gfx->SubmitInFlight || Gfx->PagingSubmitInFlight) return STATUS_DEVICE_BUSY;
    for (i=0;i<RTL_NUMBER_OF(Gfx->RingOwes);i++)
        if (Gfx->RingOwes[i]) return STATUS_DEVICE_BUSY;
    if (!Resume && (!Device->IhQuiet || Device->GfxTlbBootstrap)) return STATUS_DEVICE_NOT_READY;
    if (Resume && (!PspIsLoaded(Device) || !IhIsActive(Device) || !Device->GfxTlbBootstrap ||
                   !Gfx->FencePage || !Gfx->SdmaFencePage || !Gfx->PagingReady))
        return STATUS_DEVICE_NOT_READY;

    GfxAccessClose(Device);
    SequenceBegin(&Gfx->Sequence,Device,FALSE,NULL,0);
    GpuMemBeginSequence(Device,NULL,0);
    if (!Resume) {
        // Keep RLC until SDMA's existing reload reset completed. Crucially, do
        // not call HaltForMappingRetirement: these GTT identities survive D0.
        result=bc250_sdma_hw_fini(Adev);
        undo=bc250_gfx_hw_fini_keep_rlc(Adev);
        (void)bc250_nbio_enable_doorbell_selfring_aperture(Adev,false);
        GrbmSelectDefault(Device);
        quiet=result==0 && undo==0 && NT_SUCCESS(Gfx->Sequence.Fault) && EnginesHalted(Device);
        if (quiet) {
            result=bc250_sdma_reset_for_reload(Adev);
            quiet=result==0 && NT_SUCCESS(Gfx->Sequence.Fault);
        }
        bc250_gfx_rlc_stop(Adev);
        quiet=quiet && NT_SUCCESS(Gfx->Sequence.Fault);
        Gfx->PowerSuspended=quiet;
        if (!quiet) {
            Gfx->Failed=TRUE;
            Device->GpuStopUnconfirmed=TRUE;
            return STATUS_IO_DEVICE_ERROR;
        }
        return STATUS_SUCCESS;
    }

    // Only transport WB pages are reset. OS/diagnostic fence pages, FenceSeq,
    // SubmitSeq, PagingSubmitSeq and the WDDM completed fence history stay intact.
    RtlZeroMemory(Adev->gfx.wb_mem.cpu,Adev->gfx.wb_mem.size);
    RtlZeroMemory(Adev->sdma.wb_mem.cpu,Adev->sdma.wb_mem.size);
    GfxResetRetainedRing(&Adev->gfx.kiq[0].ring);
    for (i=0;i<Adev->gfx.num_compute_rings;i++) GfxResetRetainedRing(&Adev->gfx.compute_ring[i]);
    for (i=0;i<Adev->gfx.num_gfx_rings;i++) GfxResetRetainedRing(&Adev->gfx.gfx_ring[i]);
    for (i=0;i<(ULONG)Adev->sdma.num_instances;i++) GfxResetRetainedRing(&Adev->sdma.instance[i].ring);
    // Force VMID reprogramming on the next job. The tenancies go to the history; access is closed and
    // GfxPagingLock is exclusive here, so no reader of the table is left (the VmidLock comment in BC250_GFX).
    Bc250VmidResetAll(&Gfx->Vmid,&Gfx->VmidHistory);
    KeMemoryBarrier();
    // From the first hardware write onward the prior halt verdict is invalid.
    Gfx->PowerSuspended=FALSE;
    for (stage=1;stage<=BC250_GFX_STAGE_INTERRUPTS;stage++) {
        // RLC rewrites its private VRAM CSB; CP rebuilds VRAM MQDs at the same
        // addresses. Ring/EOP/fence/diagnostic storage is retained nonpaged GTT.
        // PagingCopyStaging is scratch and is filled before each copy operation.
        result=RunEngineStage(Device,Adev,stage,NULL);
        GuardLog("gfx: retained resume stage %lu result %d fault 0x%08X",stage,result,Gfx->Sequence.Fault);
        if (result || !NT_SUCCESS(Gfx->Sequence.Fault)) break;
    }
    if (result || !NT_SUCCESS(Gfx->Sequence.Fault) || Device->GfxTlbBootstrap) {
        Gfx->Failed=TRUE;
        Device->GpuStopUnconfirmed=TRUE;
        return STATUS_IO_DEVICE_ERROR;
    }
    GfxAccessOpen(Device);
    return STATUS_SUCCESS;
}

NTSTATUS GfxSetPowerRetained(_Inout_ BC250_DEVICE* Device, BOOLEAN Resume)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev=NULL;
    BOOLEAN enabled=FALSE;
    NTSTATUS status;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    gfx=(BC250_GFX*)Device->Gfx;
    status=GartDevice(Device,&adev,&enabled);
    if (NT_SUCCESS(status) && (!gfx || !enabled || !Device->MmioGfxEnabled || Device->GfxStopPrepared))
        status=STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status)) {
        void* previousBackend=adev->backend;
        adev->backend=&gfx->Sequence;
        status=GfxPowerRetainedLocked(Device,gfx,adev,Resume);
        adev->backend=previousBackend;
    }
    GuardLog("gfx: retained power resume %u status 0x%08X",(ULONG)Resume,status);
    ExReleaseFastMutex(&Device->GartLock);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// ---- start and stop -------------------------------------------------------------------------------------------------------

NTSTATUS GfxStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;

    Device->Gfx = NULL;
    Device->GfxStopQuiet = TRUE;
    Device->GfxStopPrepared = FALSE;
    if (!Device->MmioGfxEnabled || Device->Psp == NULL || Device->GpuMem == NULL) return STATUS_SUCCESS;
    gfx = (BC250_GFX*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*gfx), BC250_GFX_TAG);
    if (gfx == NULL) return STATUS_SUCCESS;         // never fails the start
    gfx->Sequence.Name = "gfx";
    gfx->Sequence.Read = MmioGfxRead;
    gfx->Sequence.Write = MmioGfxWrite;
    gfx->Sequence.PlanAnswers = GfxPlanAnswers;
    gfx->Sequence.PassesFault = GfxPassesFault;
    // ADR 0008 stage C. Read once here and never again, like every other gate: a submission path that could be opened
    // while the device runs would be a path nobody had decided to open.
    //   EnableGpuSubmit  REG_DWORD  1 = GfxSubmitIb may write the gfx ring. Needs EnableGfx and EnableIh (stage 8).
    gfx->SubmitGate = (GuardReadSetting(L"EnableGpuSubmit", 0) == 1);
    // ADR 0008 stage D (docs/design/paging-node.md). Read once, like every other gate: node 1's existence for
    // this device start is decided here and nowhere else.
    //   EnablePagingNode  REG_DWORD  1 = node 1 (SDMA0, the paging node) exists in the table and may submit.
    //                                 Needs EnableGpuSubmit's own preconditions (EnableGfx, EnableIh at stage 8).
    gfx->PagingCpuBootstrap = TRUE;
    gfx->PagingGate = (GuardReadSetting(L"EnablePagingNode", 0) == 1);
    //   HotSubmitLog  REG_DWORD  1 = the three per-submit lines of SubmitIbLocked reach the log ring. Default 0:
    //                            they wrote 3871 lines a second and 75 % of the ring was overwritten unread (D5).
    gfx->HotSubmitLog = (GuardReadSetting(L"HotSubmitLog", 0) == 1);
    gfx->HotSubmitLinesSkipped = 0;
    //   EnableVmidPool  REG_DWORD  0 = every WDDM job at VMID 1, as 0.7.213.1. Absent or any other value = a VMID per
    //                              page-table root from a pool (vmid_pool.h). Until the bring-up read at the first
    //                              pool submit, the pool is VMID 1 alone.
    gfx->VmidPoolGate = (GuardReadSetting(L"EnableVmidPool", 1) != 0);
    gfx->VmidMembers = (USHORT)(1u << BC250_VMID_LEGACY);
    KeInitializeSpinLock(&gfx->VmidLock);
    KeInitializeSpinLock(&gfx->Sdma0RingLock);
    // BD-097: the paging submit line's rate limit starts over at every device start, so the first submits of a
    // start are always in the log in full.
    KeInitializeSpinLock(&gfx->PagingLogLock);
    Bc250LogRateReset(&gfx->PagingLogRate);
    Device->Gfx = gfx;
    GfxAccessOpen(Device);
    GuardLog("gfx: ready, GPU submission %s, paging node %s, hot submit log %s", gfx->SubmitGate ? "allowed" : "off",
             gfx->PagingGate ? "allowed" : "off", gfx->HotSubmitLog ? "on" : "off");
    GuardLog("gfx: VMID pool %s", gfx->VmidPoolGate ? "on (EnableVmidPool)" : "off (EnableVmidPool 0): every job at VMID 1");
    return STATUS_SUCCESS;
}

// PnP/unwind hardware phase. Keep the owner, ring mappings and GpuMem alive
// through PSP and GART retirement. Manual diagnostic Fini remains combined.
void GfxPrepareStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled, quiet;
    long undo = 0;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    GfxAccessClose(Device);
    if (!Device->GfxStopPrepared) {
        Device->GfxStopPrepared = TRUE;
        quiet = Device->GfxStopQuiet;
        gfx = (BC250_GFX*)Device->Gfx;
        if (gfx != NULL && (gfx->StagesDone != 0 || gfx->SetUp)) {
            quiet = FALSE;
            if (NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled))) {
                void* previousBackend = adev->backend;
                adev->backend = &gfx->Sequence;
                SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
                GpuMemBeginSequence(Device, NULL, 0);
                quiet = HaltForMappingRetirement(Device, gfx, adev, &undo) && undo == 0 && NT_SUCCESS(gfx->Sequence.Fault);
                adev->backend = previousBackend;
            }
        }
        quiet = quiet && !Device->GfxTlbBootstrap;
        Device->GfxStopQuiet = quiet;
        if (!quiet || !Device->IhQuiet) Device->GpuStopUnconfirmed = TRUE;
    }
    ExReleaseFastMutex(&Device->GartLock);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
}

// Storage phase: all consumers and translation hardware have retired. GART's
// owner and table still exist here; final firmware restore/destruction follows.
void GfxStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled, quiet;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    GfxAccessClose(Device);
    quiet = Device->GfxStopPrepared && Device->GfxStopQuiet && Device->IhQuiet &&
            Device->PspStopQuiet && Device->GartStopPrepared && Device->GartStopQuiet;
    gfx = (BC250_GFX*)Device->Gfx;
    if (!quiet) {
        Device->GpuStopUnconfirmed = TRUE;
        GuardLog("gfx: retirement incomplete, retaining storage and owner");
        goto Done;
    }
    if (gfx != NULL && (gfx->StagesDone != 0 || gfx->SetUp)) {
        if (!NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled))) {
            Device->GfxStopQuiet = FALSE;
            Device->GpuStopUnconfirmed = TRUE;
            goto Done;
        }
        {
            void* previousBackend = adev->backend;
            adev->backend = &gfx->Sequence;
            SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
            GpuMemBeginSequence(Device, NULL, 0);
            ReleaseStoppedStorage(Device, gfx, adev, TRUE, 0);
            adev->backend = previousBackend;
        }
    }
    if (!Device->GfxStopQuiet) Device->GpuStopUnconfirmed = TRUE;
    GpuMemStop(Device, Device->GfxStopQuiet);
    Device->Gfx = NULL;
    if (gfx != NULL) ExFreePoolWithTag(gfx, BC250_GFX_TAG);
Done:
    ExReleaseFastMutex(&Device->GartLock);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
}

// Retained OS DMA contains commands, staging and synchronization storage.
// All addresses stay virtual; submission supplies the privileged root.
NTSTATUS GfxPagingBuildVirtualPtes(BC250_DEVICE* Device, ULONGLONG Source,
    ULONGLONG Destination, ULONG Entries, PVOID Buffer, ULONGLONG DmaBase,
    ULONG Offset, ULONG Free, PAGING_NATIVE_RESULT* Built)
{
    BC250_GFX* gfx;
    struct bc250_sdma_virtual_ptes layout;
    unsigned budget;
    int result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Built,sizeof(*Built));
    if(!Device || !Buffer || !DmaBase || DmaBase>MAXULONGLONG-Offset)return status;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr)goto Done;
    budget=4u*PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=bc250_sdma_build_virtual_ptes(gfx->PagingDevicePtr,Buffer,budget,DmaBase+Offset,
        Source,Destination,Entries,&layout);
    if(result==BC250_SDMA_PAGING_INSUFFICIENT)status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else if(result==BC250_SDMA_PAGING_OK) {
        Built->Bytes=layout.bytes;Built->IbOffset=layout.ib_offset;
        Built->IbDwords=layout.ib_dwords;Built->CsaOffset=layout.csa_offset;
        Built->Moved=(ULONGLONG)Entries*8u;status=STATUS_SUCCESS;
    }
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    return status;
}
