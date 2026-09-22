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
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "bc250_gmc.h"
#include "bc250_gfx.h"
#include "bc250_sdma.h"
#include "bc250_sdma_paging.h"
#include "paging_mc.h"
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
#define BC250_PAGING_POLL_US 500000ul       // node 1's own watchdog budget, same shape as BC250_SUBMIT_POLL_US
// E24 run 001 (docs/design/paging-node.md, "What run 001 hung on"): the bounded wait TearDown gives a
// GfxPagingBuild() call that is still touching PagingShadowMem when a FINI or a stop tears it down. Same shape
// as WddmStop's own wait for a hardware submission (BC250_WDDM_SUBMIT_TIMEOUT_MS): there is no GPU reset on
// this part (facts M53), so a wait here is bounded and logged loudly on timeout, never infinite.
#define BC250_GFX_PAGING_DRAIN_TIMEOUT_MS 200ul
// The shadow buffer BuildPagingBuffer fills and SubmitCommand reads back at DISPATCH_LEVEL (design note section
// 4). Matches wddm.c's own BC250_WDDM_PAGING_BUFFER_BYTES (0x10000, wddm.c:57): the driver never advertises a
// paging buffer larger than what its own shadow can hold, so dxgkrnl's MultipassOffset is the answer to "too
// big for this call", never a silent truncation on this driver's part.
#define BC250_GFX_PAGING_SHADOW_BYTES 0x10000ul

typedef struct _BC250_GFX {
    BC250_SEQUENCE Sequence;
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
    ULONG StagesDone;               // last stage that ran on the hardware in this driver instance
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
    // GfxEscape's RUN arm has captured PagingRing/PagingDevicePtr/PagingShadow. Read with no lock by
    // GfxPagingBuild (PASSIVE_LEVEL, BuildPagingBuffer - see PagingBuildersActive below, this field's own
    // publish/clear stays under GartLock) and by the DISPATCH_LEVEL trio (GfxSubmitPaging/GfxPagingFenceArrived/
    // GfxPagingSubmitReady), the ih.c DpcAdev shape.
    BOOLEAN PagingReady;
    struct amdgpu_ring* PagingRing;       // &adev->sdma.instance[0].ring: a pointer into the live, persistent adev,
                                           // never a copy (section 4: forking .wptr into two counters is the bug this avoids)
    struct amdgpu_device* PagingDevicePtr; // the same adev; GfxPagingFenceArrived reads bc250_sdma_fence_read through it,
                                           // with no lock, the same shape as SubmitAdev above
    KSPIN_LOCK Sdma0RingLock;             // every direct writer of the live SDMA0 ring takes this narrowly (section 4)
    BOOLEAN PagingShadowAlloc;            // bc250_shim_mem_alloc(BC250_MEM_GTT) has allocated PagingShadowMem
    struct bc250_mem PagingShadowMem;     // BC250_GFX_PAGING_SHADOW_BYTES: BuildPagingBuffer's own copy of the packets
    // E24 run 001 (docs/design/paging-node.md, "What run 001 hung on"): GfxPagingBuild (BuildPagingBuffer,
    // PASSIVE_LEVEL) cannot serialize against TearDown through Device->GartLock the way every other reader of
    // gfx.c's PASSIVE_LEVEL state does - GartLock is a FAST_MUTEX, ExAcquireFastMutex raises IRQL to APC_LEVEL,
    // and VidMmTranslate refuses anything but exactly PASSIVE_LEVEL (vidmm.c:238), so holding GartLock across a
    // translation makes every translation fail rather than serialize it. This counter is what stands in for the
    // lock: incremented for the duration of every GfxPagingBuild call that got past the PagingReady check,
    // decremented on every exit; TearDown waits for it to reach 0 before freeing PagingShadowMem, bounded by
    // BC250_GFX_PAGING_DRAIN_TIMEOUT_MS.
    volatile LONG PagingBuildersActive;
    volatile LONG PagingSubmitFailed;     // sticky, like SubmitFailed, but independent: node 1 fails on its own hardware
    volatile LONG PagingSubmitInFlight;   // one in flight, like SubmitInFlight
    ULONG PagingSubmitSeq;
} BC250_GFX;

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
    // For the self-ring aperture of stage 8: the doorbell BAR's bus address, which is its CPU physical address here
    // (facts M37). 0 if unknown, and then the shim refuses the stage.
    Adev->doorbell.base = GpuMemDoorbellBase(Gfx->Sequence.Device);
    Gfx->SetUp = TRUE;
    return 0;
}

static void TearDown(_Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev)
{
    if (!Gfx->SetUp) return;
    Gfx->SubmitAdev = NULL;             // first: GfxFenceArrived reads the fence page through it, without a lock
    // Same reasoning, first among the paging fields: GfxPagingFenceArrived and GfxSubmitReady's node-1 twin read
    // PagingReady/PagingDevicePtr/PagingRing with no lock (design note section 4). A stage-7-and-back-to-8 re-run
    // must never let a DISPATCH_LEVEL caller find a stale ring or adev pointer here. GfxPagingBuild (PASSIVE_LEVEL)
    // is the third no-lock reader of PagingReady, and its own PagingShadowMem use is what the wait below, on
    // PagingBuildersActive, is for - setting PagingReady FALSE here is what stops it from starting a new one.
    Gfx->PagingReady = FALSE;
    Gfx->PagingRing = NULL;
    Gfx->PagingDevicePtr = NULL;
    if (Gfx->FencePage) { bc250_gfx_fence_page_free(Adev); Gfx->FencePage = FALSE; }
    if (Gfx->SdmaFencePage) { bc250_sdma_fence_page_free(Adev); Gfx->SdmaFencePage = FALSE; }
    if (Gfx->IbPage) { bc250_gfx_ib_page_free(Adev); Gfx->IbPage = FALSE; }
    if (Gfx->SdmaCopyRegions)
    {
        bc250_sdma_copy_regions_free(Adev, &Gfx->SdmaCopySrc, &Gfx->SdmaCopyDst);
        Gfx->SdmaCopyRegions = FALSE;
    }
    if (Gfx->PagingShadowAlloc)
    {
        // PagingReady is already FALSE (above), so no new GfxPagingBuild call can start using PagingShadowMem;
        // this is the bounded wait for one that had already passed that check on another CPU - BuildPagingBuffer
        // cannot serialize through GartLock here, see PagingBuildersActive's own comment - to finish touching it
        // before it is freed out from under that call (E24 run 001, docs/design/paging-node.md). PASSIVE_LEVEL:
        // both of TearDown's callers (Fini, from the FINI escape or GfxStop) are. Timing out and freeing anyway,
        // loudly, is the same choice WddmStop already makes for a hardware submission that does not finish in
        // time - there is no GPU reset on this part (facts M53), so waiting forever is not on offer either.
        LARGE_INTEGER tick;
        ULONG waited;

        tick.QuadPart = -10000ll * 10;
        for (waited = 0; waited < BC250_GFX_PAGING_DRAIN_TIMEOUT_MS && Gfx->PagingBuildersActive != 0; waited += 10)
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
        if (waited != 0)
            GuardLog("gfx: paging teardown waited %lu ms for %ld builder(s) touching the shadow buffer%s", waited,
                     Gfx->PagingBuildersActive, Gfx->PagingBuildersActive != 0 ? " - STILL ACTIVE, freeing anyway" : "");
        bc250_shim_mem_free(Adev, &Gfx->PagingShadowMem);
        Gfx->PagingShadowAlloc = FALSE;
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

// hw_fini in amdgpu's order (SDMA before GFX), then the memory. Returns whether the engines read halted; Undo gets what
// the shim's undo returned (BC250_ETIME: the MEC did not let go of the KIQ's queue, facts M44; BC250_EBUSY: an SDMA
// engine halted with its read pointer still behind its write pointer, so it stopped holding packets that name pages of
// ours). GFX first if both failed, because the KIQ is the one whose recovery the next bring-up has a branch for.
static BOOLEAN Fini(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev, _Out_ long* Undo)
{
    BOOLEAN quiet;
    long sdma = 0;

    *Undo = 0;

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
    }
    // Before the CP stage no engine was released by us and no queue was mapped: nothing of ours is in use. (The PSP
    // releases SDMA by itself, facts M35, but an SDMA engine without a ring has no address of ours.)
    quiet = (Gfx->StagesDone < BC250_GFX_STAGE_CP) || EnginesHalted(Device);
    TearDown(Gfx, Adev);
    // An undo that failed leaves an engine that may still hold an address of ours: its pages stay (they go back with a
    // later undo that succeeds, or never), but the state is reset all the same, because the way out of this is the next
    // bring-up's recovery branch (bc250_kiq_init_register), not a second undo on a halted MEC.
    GpuMemRelease(Device, &Gfx->Sequence, quiet && *Undo == 0);
    if (quiet) { Gfx->StagesDone = 0; Gfx->Failed = FALSE; }
    return quiet;
}

void GfxEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GFX* Data)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE, plan = (Data->Op == BC250_GFX_OP_PLAN);
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER frequency, start;
    ULONG stage, first;
    long result = 0;

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

    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (Data->Op > BC250_GFX_OP_STATE) status = STATUS_INVALID_PARAMETER;
    else if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else if (Data->Op <= BC250_GFX_OP_RUN && (Data->LastStage == 0 || Data->LastStage > BC250_GFX_STAGE_COUNT)) status = STATUS_INVALID_PARAMETER;
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
            if (plan ? (gfx->StagesDone != 0 || gfx->SetUp) : (!gartEnabled || !PspIsLoaded(Device) || gfx->Failed || Data->LastStage <= gfx->StagesDone ||
                                                              (Data->LastStage >= BC250_GFX_STAGE_INTERRUPTS && !IhIsActive(Device))))
            {
                status = STATUS_INVALID_DEVICE_STATE;
                break;
            }
            result = SetUp(gfx, adev);
            if (result != 0) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            first = plan ? 1 : gfx->StagesDone + 1;
            for (stage = first; stage <= Data->LastStage; stage++)
            {
                BC250_ESCAPE_GFX_STAGE* got = &Data->Stages[Data->StageCount++];

                got->Stage = stage;
                got->FirstWrite = gfx->Sequence.WriteCount;
                start = KeQueryPerformanceCounter(NULL);
                if (!plan) gfx->StagesDone = stage;         // from its first write on, the stage has touched the hardware
                got->Result = g_Stages[stage].Run(adev);
                got->Microseconds = Microseconds(start, frequency);
                GuardLog("gfx: stage %u (%s)%s: rc %d, %u writes so far, %u us", stage, g_Stages[stage].Name, plan ? " planned" : "",
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
            // shadow buffer - the same place and the same reasoning as ih.c's DpcAdev (ih.c:319-324). Once, not
            // on every call: PagingReady stays set until the next TearDown, which clears it first of everything
            // (matching SubmitAdev's own comment above).
            if (gfx->PagingGate && !gfx->PagingReady && gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS &&
                     !gfx->Failed && NT_SUCCESS(gfx->Sequence.Fault))
            {
                if (!gfx->PagingShadowAlloc)
                {
                    result = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_GFX_PAGING_SHADOW_BYTES, AMDGPU_GPU_PAGE_SIZE,
                                                  &gfx->PagingShadowMem);
                    if (result == 0 && gfx->PagingShadowMem.cpu != NULL) gfx->PagingShadowAlloc = TRUE;
                    else GuardLog("gfx: paging shadow allocation failed, result %d - node 1 stays inert this device start", result);
                }
                if (gfx->PagingShadowAlloc)
                {
                    gfx->PagingRing = &adev->sdma.instance[0].ring;
                    gfx->PagingDevicePtr = adev;
                    gfx->PagingReady = TRUE;
                    GuardLog("gfx: paging node ready, SDMA0 ring at doorbell 0x%X, shadow %lu bytes",
                             gfx->PagingRing->doorbell_index, BC250_GFX_PAGING_SHADOW_BYTES);
                }
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
    if (gfx != NULL) Data->StagesDone = gfx->StagesDone;
    GuardLog("gfx: op %u to stage %u -> 0x%08X, result %d, %u writes, %u doorbells", Data->Op, Data->LastStage, status, result,
             Data->WriteCount, Data->DoorbellCount);
    ExReleaseFastMutex(&Device->GartLock);

    Data->Result = result;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
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

BOOLEAN GfxSubmitReady(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    // Stage 8 and not 6: the fence carries AMDGPU_FENCE_FLAG_INT, and without the interrupt sources of stage 8 the
    // completion would never be reported, only polled. Without the IH ring stage 8 cannot have run at all (GfxEscape).
    return gfx != NULL && gfx->SubmitGate && gfx->SetUp && !gfx->Failed && gfx->SubmitFailed == 0 &&
           gfx->SubmitInFlight == 0 && gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS;
}

void GfxSubmitFail(_Inout_ BC250_DEVICE* Device)
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

BOOLEAN GfxFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    // One read of a GTT page the CP writes. No register, no lock, no allocation: everything a DISPATCH_LEVEL caller
    // may not do is somewhere else. SubmitAdev is only ever non-NULL between a submission and the teardown that frees
    // the page, and pnp.c drains ih.c's DPCs before that teardown runs (see the field's comment).
    if (gfx == NULL || gfx->SubmitAdev == NULL || Seq == 0) return FALSE;
    if ((ULONG)bc250_gfx_fence_read(gfx->SubmitAdev, BC250_SUBMIT_FENCE_SLOT) != Seq) return FALSE;
    // The sequence numbers only count up, so a slot holding Seq means that submission and every earlier one is done.
    if (gfx->SubmitSeq == Seq) InterlockedExchange(&gfx->SubmitInFlight, 0);
    return TRUE;
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
    // refuses to touch; a caller submitting at VMID 0 is submitting out of the driver's own GTT pages. For 1..15 the
    // root is programmed only when it moved, because the invalidation behind it polls for up to 100 ms.
    if (Vmid != 0 && RootPhysical != Gfx->VmidRoot[Vmid])
    {
        result = bc250_gmc_set_vmid_pd(Adev, Vmid, RootPhysical, 0);
        GuardLog("gfx: VMID %lu root 0x%llX -> %d", Vmid, RootPhysical, result);
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

    result = bc250_gfx_submit_ib(ring, GpuAddress, SizeBytes / 4, Vmid, address, seq, AMDGPU_FENCE_FLAG_INT);
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
// BuildPagingBuffer (PASSIVE_LEVEL, under GartLock, like GfxSubmitIb) turns one paging operation into SDMA
// packets through the same emitters SdmaCopyEscape already proved (M95): GfxPagingBuild. SubmitCommand
// (DISPATCH_LEVEL, no GartLock, no adev->backend) pushes the bytes GfxPagingBuild left in the shadow onto the
// live SDMA0 ring: GfxSubmitPaging. The two meet only through Gfx->PagingShadowMem and Gfx->PagingRing/
// PagingDevicePtr, both set up once (GfxEscape's RUN arm, below) and read without a lock at DISPATCH_LEVEL,
// exactly as ih.c's DpcAdev is (design note section 4).

BOOLEAN GfxPagingNodeGate(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    return gfx != NULL && gfx->PagingGate;
}

ULONG GfxPagingShadowBytes(void)
{
    return BC250_GFX_PAGING_SHADOW_BYTES;
}

BOOLEAN GfxPagingSubmitReady(_In_ const BC250_DEVICE* Device)
{
    const BC250_GFX* gfx = (const BC250_GFX*)Device->Gfx;

    // The node-1 twin of GfxSubmitReady: gated, set up, not failed, nothing outstanding, and PagingReady - the
    // RUN escape has reached stage 8 with the gate open and captured a live ring to write to.
    return gfx != NULL && gfx->PagingGate && gfx->SetUp && !gfx->Failed && gfx->PagingSubmitFailed == 0 &&
           gfx->PagingSubmitInFlight == 0 && gfx->PagingReady;
}

void GfxPagingSubmitFail(_Inout_ BC250_DEVICE* Device)
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

BOOLEAN GfxPagingFenceArrived(_Inout_ BC250_DEVICE* Device, ULONG Seq)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;

    // One read of a GTT page SDMA0 writes, no register, no lock, no allocation - the node-1 twin of
    // GfxFenceArrived. PagingDevicePtr is only ever non-NULL between GfxEscape's capture and the teardown that
    // frees the fence page, and pnp.c drains ih.c's DPCs before that teardown runs, exactly as SubmitAdev's own
    // comment states.
    if (gfx == NULL || gfx->PagingDevicePtr == NULL || Seq == 0) return FALSE;
    if ((ULONG)bc250_sdma_fence_read(gfx->PagingDevicePtr, BC250_PAGING_FENCE_SLOT) != Seq) return FALSE;
    if (gfx->PagingSubmitSeq == Seq) InterlockedExchange(&gfx->PagingSubmitInFlight, 0);
    return TRUE;
}

// PASSIVE_LEVEL, exactly - not under GartLock (E24 run 001, docs/design/paging-node.md, "What run 001 hung
// on"): a FAST_MUTEX raises IRQL to APC_LEVEL, and VidMmTranslate's own IRQL check refuses anything but exactly
// PASSIVE_LEVEL (vidmm.c:238), APC_LEVEL included, so GartLock cannot be held across it - see
// GfxPagingBuild/PagingBuildersActive for what serializes this caller instead. Verifies that [Va, Va+Bytes)
// resolves to one physically contiguous run before GfxPagingBuild trusts Physical0 (the translation of Va
// itself) for the whole range: VidMmTranslate resolves one 4 KB page at a time (bc250_pte_fields, PAGE_SHIFT),
// and a GPU VA range backed by VidMm's own page tables has no reason to stay physically contiguous past the
// first page - review 23 MUST-FIX: GfxPagingBuild used to translate only Va and hand the whole Bytes span to
// bc250_sdma_paging_copy/fill as if it were, which corrupts memory the moment a multi-page TRANSFER_VIRTUAL/
// FILL_VIRTUAL lands on a fragmented VRAM allocation. Answers FALSE (refuse to build, not build wrong) rather
// than attempt to split one operation into several packets - out of scope for this cut, same as the
// system-memory limit section 2 already states plainly instead of hiding.
static BOOLEAN PagingRangeContiguous(ULONGLONG RootPhysical, ULONGLONG Va, ULONGLONG Physical0, ULONGLONG Bytes)
{
    ULONGLONG boundary;

    for (boundary = (Va & ~((ULONGLONG)PAGE_SIZE - 1)) + PAGE_SIZE; boundary < Va + Bytes; boundary += PAGE_SIZE)
    {
        ULONGLONG physical = 0;
        BOOLEAN system = FALSE;

        if (!VidMmTranslate(RootPhysical, boundary, &physical, &system) || system) return FALSE;
        if (physical != Physical0 + (boundary - Va)) return FALSE;
    }
    return TRUE;
}

// PASSIVE_LEVEL, exactly - NOT under GartLock, and deliberately so (E24 run 001, docs/design/paging-node.md,
// "What run 001 hung on"). wddm.c calls VidMmUpdatePageTable, right next to this arm in
// Bc250WddmBuildPagingBuffer, the same way and with the same absence of GartLock - that much was already true
// before stage D and is not new. What stage D added is a second thing this function reads with no lock:
// PagingReady/PagingShadowMem, published by GfxEscape's RUN arm and cleared/freed by TearDown, both under
// GartLock. Taking GartLock here to close that gap the obvious way does not work: ExAcquireFastMutex raises
// IRQL to APC_LEVEL, and VidMmTranslate refuses anything but exactly PASSIVE_LEVEL (vidmm.c:238) - a
// GartLock-held call into it would fail every single translation, silently, for as long as the lock was held.
// PagingBuildersActive is the narrower thing that is actually possible: an interlocked count of calls that are
// still touching PagingShadowMem, which TearDown waits to drain (bounded) before freeing it. It does not
// serialize this function against a concurrent GfxEscape RUN the way GartLock would - two calls can genuinely
// run at once - only against the one thing that is actually unsafe, PagingShadowMem disappearing mid-use.
//
// RootPhysical names the paging process's root (wddm.c resolves hSystemContext to it, design note section 2);
// Fill selects DXGK_OPERATION_VIRTUAL_FILL (SrcVa/FillPattern) over DXGK_OPERATION_VIRTUAL_TRANSFER (SrcVa,
// DstVa). Both virtual addresses are resolved to physical with VidMmTranslate before any packet is emitted: no
// VMID is ever pointed at anything for this path (design note section 2). DmaBuffer/DmaBufferOffset/
// DmaBufferFree describe dxgkrnl's own pDmaBuffer at the caller's DmaBufferWriteOffset, exactly as wddm.c will
// pass them; this function writes the same bytes there and into Gfx->PagingShadowMem at the same offset, so
// that GfxSubmitPaging can find them later without ever dereferencing pDmaBuffer's physical address.
//
// The packet itself comes from driver/shim/bc250_sdma_paging.c (ADR 0013), not from this function: that file
// is the host-testable half (driver/shim/test/paging_packets.c checks it dword for dword against
// bc250_sdma_emit_copy_linear()/emit_fill(), M95, plus the room check below), and this one supplies only what
// that file cannot have - the MC addresses (paging_mc.c, from VidMmTranslate's system physical) and the
// shadow buffer to write them into.
//
// DmaBuffer is pBuildPagingBuffer->pDmaBuffer as the DDI hands it over: "a virtual address to the first
// available byte in the paging buffer", not the buffer's start. DmaBufferOffset is that same byte's distance
// from the start (pBuildPagingBuffer->DmaBufferWriteOffset), which is what the shadow is indexed by and what
// SubmitCommand will later name in DmaBufferSubmissionStartOffset. The caller - and only the caller - advances
// pDmaBuffer past what this function wrote; leaving it where it was is how the DDI says "nothing was written",
// and saying that by accident is what left four built fills unsubmitted in E24 run 005 (facts M108).
NTSTATUS GfxPagingBuild(_Inout_ BC250_DEVICE* Device, ULONGLONG RootPhysical, BOOLEAN Fill, ULONGLONG SrcVa,
                        ULONGLONG DstVa, ULONGLONG Bytes, ULONG FillPattern, _Inout_ PVOID DmaBuffer,
                        ULONG DmaBufferOffset, ULONG DmaBufferFree, _Out_ ULONG* DwordsWritten,
                        _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx = (BC250_GFX*)Device->Gfx;
    ULONGLONG srcPhysical = 0, dstPhysical = 0, srcMc = 0, dstMc = 0;
    BOOLEAN srcSystem = FALSE, dstSystem = FALSE;
    unsigned int budget, written = 0;
    u32* shadow;
    int result;
    NTSTATUS status = STATUS_SUCCESS;

    *DwordsWritten = 0;
    *Unsupported = BC250PagingSupported;
    if (gfx == NULL || Bytes == 0 || Bytes > 0xFFFFFFFFu) return STATUS_INVALID_PARAMETER;

    // PagingBuildersActive from here to Done: see this function's own header comment and the field's. Counted
    // even for a call that turns out "not ready" below, so that TearDown's wait and this function's own read of
    // PagingReady can never straddle a free of PagingShadowMem no matter how early that read happens to lose.
    InterlockedIncrement(&gfx->PagingBuildersActive);

    if (!gfx->PagingReady || gfx->PagingDevicePtr == NULL) { *Unsupported = BC250PagingNotReady; goto Done; }

    // hSystemContext's root: RootPhysical is 0 when wddm.c found no BC250_WDDM_CONTEXT to resolve it against.
    if (RootPhysical == 0) { *Unsupported = BC250PagingNoRoot; goto Done; }

    if (!VidMmTranslate(RootPhysical, DstVa, &dstPhysical, &dstSystem)) { *Unsupported = BC250PagingNoTranslation; goto Done; }
    if (!Fill)
    {
        if (!VidMmTranslate(RootPhysical, SrcVa, &srcPhysical, &srcSystem)) { *Unsupported = BC250PagingNoTranslation; goto Done; }
    }
    // Section 2's stated limit: no MC mapping exists for a page VidMm resolved to system memory unless this
    // driver itself allocated it through bc250_shim_mem_alloc(BC250_MEM_GTT, ...). Answered inertly, not refused:
    // dxgkrnl still gets STATUS_SUCCESS for an operation this cut does not build.
    if (dstSystem || (!Fill && srcSystem)) { *Unsupported = BC250PagingSystemMemory; goto Done; }

    // Review 23 MUST-FIX: one COPY_LINEAR/CONST_FILL packet covers [dstPhysical, dstPhysical+Bytes) (and, for a
    // transfer, [srcPhysical, srcPhysical+Bytes)) as one physically linear run. dstPhysical/srcPhysical are only
    // proven for the first page Va names; every further page this operation's Bytes reaches must be confirmed to
    // continue that run before a packet is built, or a non-contiguous allocation silently corrupts memory instead
    // of just answering slowly.
    if (!PagingRangeContiguous(RootPhysical, DstVa, dstPhysical, Bytes) ||
        (!Fill && !PagingRangeContiguous(RootPhysical, SrcVa, srcPhysical, Bytes)))
    {
        *Unsupported = BC250PagingNotContiguous;
        goto Done;
    }

    // VidMmTranslate's number is a system physical address (vidmm.c, vram_base). The packet wants the MC
    // address M95's sdmacopy used. 0.7.33 put the physical one in and SDMA0 page-faulted (facts M113).
    if (!PagingPhysicalToMc(dstPhysical, Bytes, (ULONGLONG)Device->VramPhysical.QuadPart, Device->VramMcBase,
                            Device->VramLength, &dstMc) ||
        (!Fill && !PagingPhysicalToMc(srcPhysical, Bytes, (ULONGLONG)Device->VramPhysical.QuadPart,
                                      Device->VramMcBase, Device->VramLength, &srcMc)))
    {
        *Unsupported = BC250PagingNoTranslation;
        goto Done;
    }

    if ((DmaBufferOffset & 3u) != 0 || DmaBufferOffset > BC250_GFX_PAGING_SHADOW_BYTES) { status = STATUS_INVALID_PARAMETER; goto Done; }

    // budget: the smaller of dxgkrnl's own remaining DmaBufferFree and the shadow's - bc250_sdma_paging_copy/
    // fill's own room check (paging_packets.c's "insufficient buffer" cases) is what turns either one running
    // out into BC250_SDMA_PAGING_INSUFFICIENT, exactly as the task asks (design note section 4a).
    budget = (unsigned int)min((BC250_GFX_PAGING_SHADOW_BYTES - DmaBufferOffset) / 4u, DmaBufferFree / 4u);
    shadow = (u32*)((PUCHAR)gfx->PagingShadowMem.cpu + DmaBufferOffset);
    // PagingDevicePtr, the live adev, and not a local one: facts M104. The throwaway ring inside these two needs a
    // non-NULL ring->adev, and struct amdgpu_device is 0x5B00 bytes - the version that put one on the stack died in
    // nt!_chkstk (bugcheck 0x50) the first time VidMm actually called this path, on a kernel stack that dxgmms2's
    // own eight frames had already eaten 0x12D0 of.
    result = Fill ? bc250_sdma_paging_fill(gfx->PagingDevicePtr, shadow, budget, dstMc, FillPattern, (unsigned int)Bytes, &written)
                  : bc250_sdma_paging_copy(gfx->PagingDevicePtr, shadow, budget, srcMc, dstMc, (unsigned int)Bytes, &written);
    if (result == BC250_SDMA_PAGING_INSUFFICIENT) { status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; goto Done; }
    if (result != BC250_SDMA_PAGING_OK) { status = STATUS_INVALID_PARAMETER; goto Done; }

    // dxgkrnl's own buffer gets the same bytes. DmaBuffer is pBuildPagingBuffer->pDmaBuffer, which the DDI
    // documents as "a virtual address to the first available byte in the paging buffer" - the byte
    // DmaBufferOffset already counts from the start - so the copy goes there directly. Adding the offset a
    // second time, which this did until M108 was diagnosed, wrote the packet one whole operation further into
    // dxgkrnl's buffer than dxgkrnl believed; harmless only while every operation of a run started at offset 0.
    // The shadow keeps the offset, because that is the coordinate SubmitCommand's
    // DmaBufferSubmissionStartOffset speaks in (bytes from the start of the buffer).
    RtlCopyMemory(DmaBuffer, shadow, (SIZE_T)written * 4u);
    *DwordsWritten = (ULONG)written;
    GuardLog("gfx: paging %s %llu bytes physical 0x%llX -> 0x%llX, mc 0x%llX -> 0x%llX, %u dwords at shadow offset 0x%lX",
             Fill ? "fill" : "transfer", Bytes, srcPhysical, dstPhysical, srcMc, dstMc, written, DmaBufferOffset);

Done:
    InterlockedDecrement(&gfx->PagingBuildersActive);
    return status;
}

// DISPATCH_LEVEL, no GartLock, no sequence, no adev->backend (design note section 4b). Pushes
// [ShadowOffset, ShadowOffset + ByteCount) of Gfx->PagingShadowMem onto the live SDMA0 ring with an
// interrupting fence, then rings the doorbell through GpuMemDoorbellWrite - never through
// amdgpu_ring_commit()/bc250_shim_wdoorbell64(), which need a backend this level does not have. One
// submission in flight, exactly as GfxSubmitIb's SubmitIbLocked.
NTSTATUS GfxSubmitPaging(_Inout_ BC250_DEVICE* Device, ULONG ShadowOffset, ULONG ByteCount, _Out_ ULONG* Seq)
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
    if (!GfxPagingSubmitReady(Device)) return STATUS_DEVICE_BUSY;
    if (ByteCount == 0 || (ByteCount & 3) != 0 || (ShadowOffset & 3) != 0 ||
        (ULONGLONG)ShadowOffset + ByteCount > BC250_GFX_PAGING_SHADOW_BYTES)
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
        amdgpu_ring_write_multiple(ring, (const u32*)((PUCHAR)gfx->PagingShadowMem.cpu + ShadowOffset), (int)dwords);
        result = bc250_sdma_emit_fence(ring, fenceAddr, seq, AMDGPU_FENCE_FLAG_INT);
    }
    if (result != 0)
    {
        amdgpu_ring_undo(ring);
        KeReleaseSpinLock(&gfx->Sdma0RingLock, irql);
        InterlockedExchange(&gfx->PagingSubmitInFlight, 0);
        GuardLog("gfx: paging submit of %lu dwords at shadow offset 0x%lX refused, result %d", dwords, ShadowOffset, result);
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
    GuardLog("gfx: paging submit, %lu dwords at shadow offset 0x%lX, seq %lu", dwords, ShadowOffset, seq);
    return STATUS_SUCCESS;
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
    gfx->PagingGate = (GuardReadSetting(L"EnablePagingNode", 0) == 1);
    KeInitializeSpinLock(&gfx->Sdma0RingLock);
    Device->Gfx = gfx;
    GuardLog("gfx: ready, GPU submission %s, paging node %s", gfx->SubmitGate ? "allowed" : "off",
             gfx->PagingGate ? "allowed" : "off");
    return STATUS_SUCCESS;
}

// Called first of the sequences' stops (pnp.c): engines halted while the PSP still has its ring and the GART is still
// enabled, as in amdgpu's teardown order. Hands gpumem.c the verdict on whether GTT pages may go back to Windows.
void GfxStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled, quiet = TRUE;
    long undo = 0;

    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    Device->Gfx = NULL;
    if (gfx != NULL && (gfx->StagesDone != 0 || gfx->SetUp))
    {
        quiet = FALSE;
        if (NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled)))
        {
            void* previousBackend = adev->backend;

            adev->backend = &gfx->Sequence;
            SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
            GpuMemBeginSequence(Device, NULL, 0);
            quiet = Fini(Device, gfx, adev, &undo) && undo == 0;
            adev->backend = previousBackend;
        }
    }
    GpuMemStop(Device, quiet && Device->IhQuiet);       // ih.c stopped before us and said whether its ring is off
    ExReleaseFastMutex(&Device->GartLock);
    if (gfx != NULL) ExFreePoolWithTag(gfx, BC250_GFX_TAG);
}
