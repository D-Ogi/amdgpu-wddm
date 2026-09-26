// RLC, CP, KIQ, the queues, the ring tests and SDMA (milestone M5 second part, ADR 0002 and 0007). The sequence is
// driver/shim/bc250_gfx.c, bc250_sdma.c and bc250_nbio.c: amdgpu's gfx_v10_0_hw_init() and sdma_v5_0_hw_init()
// transcribed against AMD's imported tables and structures, which a host test replays against amdgpu's recorded
// register traffic of unit A. This file is the command around it; the memory is gpumem.c.
//
//   <service key>\Parameters
//     EnableGfx        REG_DWORD  1 = allow the GFX command. Needs EnableMmio, EnableVram, EnableGart, EnablePsp. Default 0.
//     EnableGpuSubmit  REG_DWORD  1 = GfxSubmitIb() may write the gfx ring (ADR 0008 stage C). Needs EnableGfx and a
//                                 bring-up that reached stage 8, i.e. EnableIh as well. Default 0.
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
#include "paging_intervals.h"
#include "paging_permutation.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "bc250_gmc.h"
#include "bc250_gfx.h"
#include "bc250_sdma.h"
#include "bc250_sdma_paging.h"
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
    BOOLEAN IbPage;                 // bc250_gfx_ib_page_alloc has allocated
    volatile LONG SubmitFailed;     // sticky: nothing goes to the ring through GfxSubmitIb again this device start
    volatile LONG SubmitInFlight;   // a submission whose fence has not been seen; the one-in-flight rule below
    ULONG SubmitSeq;                // its sequence number, 0 when none was ever emitted
    // The DPC reads the fence slot without the lock, so it needs a device pointer it can use there. It is this
    // sequence's own adev, set under GartLock at the submission and never freed before GfxStop: pnp.c stops ih.c
    // first, and IhStop() clears Active and drains the DPCs, so by the time TearDown() releases the fence page no
    // consumer of this field can still be running.
    struct amdgpu_device* SubmitAdev;
    // The page directory root each VMID was last given, so that a submission whose context has not moved does not pay
    // for an invalidation (bc250_gmc_flush_gpu_tlb polls for up to adev->usec_timeout). Index 0 is unused: VMID 0 is
    // the GART aperture and has no root of ours.
    ULONGLONG VmidRoot[16];
    // ADR 0013: the two VRAM scratch regions BC250_ESCAPE_RUN_SDMACOPY copies between. Allocated once from the
    // same VRAM pool the rest of this file's memory comes from (gpumem.c), on the first call, and freed with
    // everything else in TearDown - not by the escape itself, so that two calls in a row need not pay for the
    // allocation twice.
    BOOLEAN SdmaCopyRegions;
    struct bc250_mem SdmaCopySrc, SdmaCopyDst;

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
    volatile LONG PagingSubmitInFlight;   // one in flight, like SubmitInFlight
    ULONG PagingSubmitSeq;
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
    inputs.max_shader_engines = 2;
    inputs.max_sh_per_se = 2;
    inputs.max_cu_per_sh = 10;
    inputs.max_backends_per_se = 2;
    inputs.async_gfx_ring = true;       // the trace: no CP_RB0 programming, a KIQ MAP_QUEUES for the gfx queue
    inputs.pp_gfxoff = true;            // the trace: one RLC_PG_CNTL write, not two
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
    RtlZeroMemory(Gfx->VmidRoot, sizeof(Gfx->VmidRoot));
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
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    if (persistKiq) {
        GfxStartupCheckpoint("acquire-gart-lock");
        // ExAcquireFastMutexUnsafe is permitted inside KeEnterCriticalRegion.
        // It preserves PASSIVE_LEVEL and special APC delivery for Zw file I/O.
        // The same mutex and lock order remain held throughout the sequence.
        ExAcquireFastMutexUnsafe(&Device->GartLock);
        GfxStartupCheckpoint("locks-acquired");
    } else ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (Data->Op > BC250_GFX_OP_STATE) status = STATUS_INVALID_PARAMETER;
    else if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else if (Data->Op <= BC250_GFX_OP_RUN && (Data->LastStage == 0 || Data->LastStage > BC250_GFX_STAGE_COUNT)) status = STATUS_INVALID_PARAMETER;
    // CpStep is internal to unpublished StartDevice, never supplied by an escape.
    if (NT_SUCCESS(status) && !CpStep && Data->Op == BC250_GFX_OP_RUN &&
        gfx->CpStepDone != 0 && gfx->CpStepDone < BC250_CP_COMPUTE_TEST)
        status = STATUS_INVALID_DEVICE_STATE;
    if (NT_SUCCESS(status) && CpStep &&
        (Data->Op != BC250_GFX_OP_RUN || Data->LastStage != BC250_GFX_STAGE_CP ||
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
                got->Result = CpStep ? bc250_gfx_cp_resume_step_traced(adev, CpStep,
                    persistKiq ? GfxStartupCheckpoint : NULL) : g_Stages[stage].Run(adev);
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
    if (persistKiq) ExReleaseFastMutexUnsafe(&Device->GartLock);
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
        // setup and completed stages. Snapshot only outside GfxExecute's locks,
        // at PASSIVE_LEVEL. In trace mode Report describes the last attempted
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
// One submission in flight, and it is a correctness requirement rather than a simplification. The shim's
// amdgpu_ring_alloc() does not look at the read pointer (bc250_ring.c:26, the deviation is stated at the RingOwes
// comment above), the gfx ring's align_mask rounds every allocation up to 256 dwords, and the ring holds 2048: eight
// unanswered submissions would wrap it onto packets the CP has not read yet. So a submission is taken only when the
// previous sequence number has arrived, and the fence slot in GTT memory is the whole of how that is decided - the
// same rule the fence escape's owed slots follow, on one slot instead of ten.
//
// Nothing here waits for the GPU. GfxSubmitIb() returns as soon as the doorbell is rung; the completion is the
// end-of-pipe interrupt, which ih.c's DPC turns into a GfxFenceArrived() call.

static BOOLEAN GfxSubmitArmed(_In_ const BC250_GFX* gfx)
{
    // Stage 8 and not 6: the fence carries AMDGPU_FENCE_FLAG_INT, and without the interrupt sources of stage 8 the
    // completion would never be reported, only polled. Without the IH ring stage 8 cannot have run at all (GfxEscape).
    return gfx != NULL && gfx->SubmitGate && gfx->SetUp && !gfx->Failed && gfx->SubmitFailed == 0 &&
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

    // Ready in every way except the one IB the ring is already holding. WddmSubmitUmd waits this
    // out: retiring that submission's scheduler fence without running the IB would make dxgkrnl
    // signal the UMD fence for work the GPU never saw.
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
}

void GfxSubmitFail(_Inout_ BC250_DEVICE* Device)
{
    if (GfxAccessAcquire(Device) == NULL)
    {
        return;
    }
    GfxSubmitFailAccess(Device);
    GfxAccessRelease(Device);
}

static BOOLEAN GfxFenceArrivedAccess(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    // One read of a GTT page the CP writes. The outer lifetime reference protects
    // the device and fence page against concurrent FINI or Stop. SubmitAdev is only ever non-NULL between a submission and the teardown that frees
    // the page, and pnp.c drains ih.c's DPCs before that teardown runs (see the field's comment).
    if (gfx == NULL || gfx->SubmitAdev == NULL || Seq == 0) return FALSE;
    if ((ULONG)bc250_gfx_fence_read(gfx->SubmitAdev, BC250_SUBMIT_FENCE_SLOT) != Seq) return FALSE;
    // The sequence numbers only count up, so a slot holding Seq means that submission and every earlier one is done.
    if (gfx->SubmitSeq == Seq) InterlockedExchange(&gfx->SubmitInFlight, 0);
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

// With GartLock held, the gfx sequence installed as adev->backend and a GpuMem sequence open. GfxSubmitIb is this plus
// all three; GfxFenceEscape's IB_AT mode calls it directly, because it already holds them.
static NTSTATUS SubmitIbLocked(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev,
                               ULONG Vmid, ULONGLONG RootPhysical, ULONGLONG GpuAddress, ULONG SizeBytes, _Out_ ULONG* Seq)
{
    struct amdgpu_ring* ring = &Adev->gfx.gfx_ring[0];      // BC250_FENCE_RING_GFX; the only ring that takes an IB here
    ULONG seq;
    u64 address;
    long result;

    *Seq = 0;
    if (Gfx->SubmitFailed) return STATUS_DEVICE_HARDWARE_ERROR;
    if (!Gfx->SubmitGate) return STATUS_ACCESS_DENIED;
    if (Gfx->Failed || !Gfx->SetUp || Gfx->StagesDone < BC250_GFX_STAGE_INTERRUPTS) return STATUS_INVALID_DEVICE_STATE;
    // The shim refuses the same three things, but a size in bytes is this file's unit, so the division is checked here:
    // an odd length would otherwise become a shorter IB rather than an error.
    if (SizeBytes == 0 || (SizeBytes & 3) != 0 || Vmid >= RTL_NUMBER_OF(Gfx->VmidRoot)) return STATUS_INVALID_PARAMETER;

    // One in flight. A late fence is taken here rather than held against the caller: GfxFenceArrived also clears the
    // mark, so a submission whose interrupt was missed still unblocks the next one as soon as its value lands.
    if (Gfx->SubmitInFlight != 0 && !GfxFenceArrived(Device, Gfx->SubmitSeq)) return STATUS_DEVICE_BUSY;

    if (!Gfx->FencePage)
    {
        result = bc250_gfx_fence_page_alloc(Adev);
        if (result != 0) return STATUS_INSUFFICIENT_RESOURCES;
        Gfx->FencePage = TRUE;
    }
    address = bc250_gfx_fence_addr(Adev, BC250_SUBMIT_FENCE_SLOT);
    if (address == 0) return STATUS_INSUFFICIENT_RESOURCES;

    // VMID 0 is the GART aperture, whose root bc250_gmc_gart_enable() programmed and which bc250_gmc_set_vmid_pd()
    // refuses to touch; a caller submitting at VMID 0 is submitting out of the driver's own GTT pages. A job
    // flushes VMID 1 on every submit. Remembering the root misses a leaf change under the same root, and the
    // invalidation is what a real job's VM flush is for. It polls for up to 100 ms.
    if (Vmid != 0)
    {
        result = bc250_gmc_set_vmid_pd(Adev, Vmid, RootPhysical, 0);
        GuardLog("gfx: VMID %lu root 0x%llX flush -> %d", Vmid, RootPhysical, result);
        if (result != 0 || !NT_SUCCESS(Gfx->Sequence.Fault))
            return NT_SUCCESS(Gfx->Sequence.Fault) ? STATUS_DEVICE_HARDWARE_ERROR : Gfx->Sequence.Fault;
        Gfx->VmidRoot[Vmid] = RootPhysical;
    }

    seq = (ULONG)InterlockedIncrement(&Gfx->FenceSeq);
    if (seq == 0) seq = (ULONG)InterlockedIncrement(&Gfx->FenceSeq);  // 0 means "nothing in flight" to GfxFenceArrived
    // Both set before the doorbell: the end-of-pipe interrupt can arrive inside bc250_gfx_submit_ib().
    Gfx->SubmitSeq = seq;
    Gfx->SubmitAdev = Adev;
    InterlockedExchange(&Gfx->SubmitInFlight, 1);

    // The ring test (VMID 0) stays one IB and one fence. A UMD job gets the gfx
    // job frame. No memory-sync packet: BC2S leaves ib_flags 0, and upstream
    // emits that packet only for AMDGPU_IB_FLAG_EMIT_MEM_SYNC.
    if (Vmid == 0)
        result = bc250_gfx_submit_ib(ring, GpuAddress, SizeBytes / 4, Vmid, address, seq, AMDGPU_FENCE_FLAG_INT);
    else
    {
        GuardLog("gfx: job frame C0004200 00000000  C0012800 81018003 00000000  C0009000 00000000  IB  C0009000 10000000  fence  C0008B00 00000000");
        result = bc250_gfx_submit_job(ring, GpuAddress, SizeBytes / 4, Vmid, address, seq, AMDGPU_FENCE_FLAG_INT);
    }
    if (result != 0 || !NT_SUCCESS(Gfx->Sequence.Fault))
    {
        // Nothing was committed: both emitters refuse before writing and bc250_gfx_submit_ib undoes the allocation.
        InterlockedExchange(&Gfx->SubmitInFlight, 0);
        Gfx->SubmitSeq = 0;
        GuardLog("gfx: IB 0x%llX x%lu dwords at VMID %lu refused, result %d", GpuAddress, SizeBytes / 4, Vmid, result);
        return NT_SUCCESS(Gfx->Sequence.Fault) ? STATUS_INVALID_PARAMETER : Gfx->Sequence.Fault;
    }

    *Seq = seq;
    return STATUS_SUCCESS;
}

NTSTATUS GfxSubmitIb(_Inout_ BC250_DEVICE* Device, ULONG Vmid, ULONGLONG RootPhysical, ULONGLONG GpuAddress,
                     ULONG SizeBytes, _Out_ ULONG* Seq)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE;
    NTSTATUS status;
    ULONG vram, gtt;

    *Seq = 0;
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
        status = SubmitIbLocked(Device, gfx, adev, Vmid, RootPhysical, GpuAddress, SizeBytes, Seq);
        (void)GpuMemEndSequence(Device, &vram, &gtt);
        adev->backend = previousBackend;
    }
    ExReleaseFastMutex(&Device->GartLock);
    return status;
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
    else if (ibAt && (Data->Dwords == 0 || Data->Dwords > BC250_FENCE_IB_MAX_DWORDS || (Data->IbAddress & 3) != 0 ||
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
            // full - the gate, stage 8, the VMID root and the one-in-flight rule - and then a bounded wait, which is
            // the one thing a DDI must not do. The lock and the sequence are already ours, so the inner call is the
            // one that runs; GfxSubmitIb itself would deadlock on GartLock here.
            ULONG seq = 0;

            status = SubmitIbLocked(Device, gfx, adev, Data->Vmid, Data->RootPhysical, Data->IbAddress,
                                    Data->Dwords * 4u, &seq);
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
                result = bc250_sdma_copy_test(ring, gfx->SdmaCopySrc.mc, gfx->SdmaCopyDst.mc, bytes,
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
    GuardLog("gfx: sdmacopy %lu bytes -> 0x%08X, result %d, matched %lu, first mismatch at 0x%lX (got 0x%02lX want 0x%02lX), %lu us",
             bytes, status, result, Data->Matched, Data->FirstMismatchOffset, Data->FirstMismatchGot, Data->FirstMismatchWant, Data->Microseconds);
    ExReleaseFastMutex(&Device->GartLock);

    Data->Result = result;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
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
    return gfx != NULL && gfx->PagingGate && gfx->SetUp && !gfx->Failed && gfx->PagingSubmitFailed == 0 &&
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
    ready=GfxSubmitReadyAccess(Device) && GfxPagingSubmitReadyAccess(Device) &&
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
    if (!adev->gart.bo || !PagingApertureInit(adev->gmc.gart_start,adev->gmc.gart_size,
            adev->gart.bo->gpu_addr,adev->gart.table_size,&live) ||
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

// PASSIVE_LEVEL. Invalidate the entire application VMID instead of a range.
// WDDM currently binds every process to VMID1; every later root assignment also
// invalidates it. Over-invalidation avoids capturing a mutable process binding
// while this OS paging buffer waits for submission. The OS paging fence follows
// the ACK poll, so dependent work cannot observe a reported-but-unexecuted flush.
NTSTATUS GfxPagingBuildFlush(_Inout_ BC250_DEVICE* Device, ULONG Vmid,
                            _Inout_ PVOID DmaBuffer, ULONG DmaBufferOffset, ULONG DmaBufferFree,
                            _Out_ ULONG* DwordsWritten,
                            _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    unsigned budget, written = 0;
    int result;
    NTSTATUS status = STATUS_SUCCESS;
    *DwordsWritten = 0; *Unsupported = BC250PagingSupported;
    if (DmaBuffer == NULL || Vmid == 0 || Vmid >= 16 || (DmaBufferOffset & 3u) != 0 ||
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
    result = bc250_sdma_paging_invalidate_vmid(gfx->PagingDevicePtr,(u32*)DmaBuffer,budget,Vmid,&written);
    if (result == BC250_SDMA_PAGING_OK) *DwordsWritten = written;
    else if (result == BC250_SDMA_PAGING_INSUFFICIENT) status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else status = STATUS_INVALID_PARAMETER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// DISPATCH_LEVEL. Validate all private records before reserving/writing the ring.
// Copy selected spans into SDMA0 with one fence; no CPU buffer is retained afterward.
static void PagingWriteRing(void* Context, const unsigned* Words, unsigned Count)
{
    amdgpu_ring_write_multiple((struct amdgpu_ring*)Context,Words,(int)Count);
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
    if (!PagingPrivateVisit(PrivateData,PrivateBytes,Start,ByteCount,VirtualAddress,NULL,NULL))
        return STATUS_INVALID_PARAMETER;

    if (InterlockedCompareExchange(&gfx->PagingSubmitInFlight, 1, 0) != 0) return STATUS_DEVICE_BUSY;

    ring = gfx->PagingRing;
    dwords = ByteCount / 4u;
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
        if (!PagingPrivateVisit(PrivateData,PrivateBytes,Start,ByteCount,VirtualAddress,PagingWriteRing,ring))
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
    GuardLog("gfx: paging submit, %lu dwords from private buffer at 0x%llX, seq %lu", dwords, Start, seq);
    return STATUS_SUCCESS;
}

NTSTATUS GfxSubmitPaging(_Inout_ BC250_DEVICE* Device, const void* PrivateData, ULONG PrivateBytes,
                          ULONGLONG Start, ULONG ByteCount, BOOLEAN VirtualAddress, _Out_ ULONG* Seq)
{
    NTSTATUS result;
    if (GfxAccessAcquire(Device) == NULL)
    {
        *Seq = 0;
        return STATUS_DEVICE_NOT_READY;
    }
    result = GfxSubmitPagingAccess(Device, PrivateData, PrivateBytes, Start, ByteCount, VirtualAddress, Seq);
    GfxAccessRelease(Device);
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
    KeInitializeSpinLock(&gfx->Sdma0RingLock);
    Device->Gfx = gfx;
    GfxAccessOpen(Device);
    GuardLog("gfx: ready, GPU submission %s, paging node %s", gfx->SubmitGate ? "allowed" : "off",
             gfx->PagingGate ? "allowed" : "off");
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
                quiet = HaltEngines(Device, gfx, adev, &undo) && undo == 0 && NT_SUCCESS(gfx->Sequence.Fault);
                adev->backend = previousBackend;
            }
        }
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
