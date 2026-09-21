// RLC, CP, KIQ, the queues, the ring tests and SDMA (milestone M5 second part, ADR 0002 and 0007). The sequence is
// driver/shim/bc250_gfx.c, bc250_sdma.c and bc250_nbio.c: amdgpu's gfx_v10_0_hw_init() and sdma_v5_0_hw_init()
// transcribed against AMD's imported tables and structures, which a host test replays against amdgpu's recorded
// register traffic of unit A. This file is the command around it; the memory is gpumem.c.
//
//   <service key>\Parameters
//     EnableGfx   REG_DWORD  1 = allow the GFX command. Needs EnableMmio, EnableVram, EnableGart, EnablePsp. Default 0.
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
// that writes a value we can read and raises an end-of-pipe interrupt (experiment E12 part C).
// The driver does FINI by itself when the device stops, before the PSP unload and the GART restore.
//
// Registers only through g_MmioGfxAllow: what amdgpu itself read or wrote on unit A in these steps (E03 trace).
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "bc250_gmc.h"
#include "bc250_gfx.h"
#include "bc250_sdma.h"
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

// Stage numbers are part of the escape's interface (bc250kmd_cli prints the names).
static const struct { const char* Name; BC250_GFX_STAGE_FUNCTION Run; } g_Stages[] = {
    { NULL, NULL },
    { "doorbell aperture", StageDoorbellAperture },             // 1  nv_common_hw_init(): nbio_v2_3_enable_doorbell_aperture
    { "golden registers", bc250_gfx_init_golden_registers },    // 2  gfx_v10_0_init_golden_registers
    { "GRBM CAM probe", StageCamProbe },                        // 3  gfx_v10_0_check_grbm_cam_remapping
    { "constants", bc250_gfx_constants_init },                  // 4  gfx_v10_0_constants_init
    { "RLC", bc250_gfx_rlc_resume },                            // 5  gfx_v10_0_rlc_resume
    { "CP", bc250_gfx_cp_resume },                              // 6  gfx_v10_0_cp_resume: KIQ, MEC, queues, ring tests
    { "SDMA", bc250_sdma_hw_init },                             // 7  sdma_v5_0_hw_init
    { "interrupt sources", StageInterrupts },                   // 8  amdgpu_fence_driver_hw_init, gfx_v10_0_late_init, amdkfd
};
#define BC250_GFX_STAGE_COUNT (RTL_NUMBER_OF(g_Stages) - 1)
#define BC250_GFX_STAGE_CP 6
#define BC250_GFX_STAGE_SDMA 7
#define BC250_GFX_STAGE_INTERRUPTS 8
#define BC250_FENCE_BUDGET_US 1000000     // all fences of one call
#define BC250_FENCE_TIMEOUT_US 100000ul

typedef struct _BC250_GFX {
    BC250_SEQUENCE Sequence;
    BOOLEAN SetUp;                  // bc250_gfx_setup and bc250_sdma_setup have allocated
    BOOLEAN Failed;                 // a stage failed on the hardware: only FINI from here
    BOOLEAN FencePage;              // bc250_gfx_fence_page_alloc has allocated
    BOOLEAN SdmaFencePage;          // bc250_sdma_fence_page_alloc has
    ULONG FenceSeq;                 // last fence value emitted
    ULONG StagesDone;               // last stage that ran on the hardware in this driver instance
} BC250_GFX;

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
    if (Gfx->FencePage) { bc250_gfx_fence_page_free(Adev); Gfx->FencePage = FALSE; }
    if (Gfx->SdmaFencePage) { bc250_sdma_fence_page_free(Adev); Gfx->SdmaFencePage = FALSE; }
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

// hw_fini in amdgpu's order (SDMA before GFX), then the memory. Returns whether the engines read halted.
static BOOLEAN Fini(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev)
{
    BOOLEAN quiet;

    if (Gfx->StagesDone >= BC250_GFX_STAGE_CP)
    {
        bc250_sdma_hw_fini(Adev);
        // Disables the three fault sources of late_init. The end-of-pipe enables of stage 8 stay set: harmless with the
        // queues unmapped and both CPs halted, but a second run to stage 8 meets them enabled.
        bc250_gfx_hw_fini(Adev);
        // nv_common_hw_fini(): the self-ring aperture goes last.
        if (Gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS) (void)bc250_nbio_enable_doorbell_selfring_aperture(Adev, false);
    }
    // Before the CP stage no engine was released by us and no queue was mapped: nothing of ours is in use. (The PSP
    // releases SDMA by itself, facts M35, but an SDMA engine without a ring has no address of ours.)
    quiet = (Gfx->StagesDone < BC250_GFX_STAGE_CP) || EnginesHalted(Device);
    TearDown(Gfx, Adev);
    GpuMemRelease(Device, &Gfx->Sequence, quiet);
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
            break;

        case BC250_GFX_OP_FINI:
            if (gfx->StagesDone == 0 && !gfx->SetUp) { status = STATUS_INVALID_DEVICE_STATE; break; }
            if (!Fini(Device, gfx, adev)) status = STATUS_IO_DEVICE_ERROR;
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
    ULONG slot = sdma ? 2 + (Data->Ring - BC250_FENCE_RING_SDMA0) : Data->Ring;     // SDMA slots 0 and 1 are the ring tests'
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
    KeQueryPerformanceCounter(&frequency);

    ExAcquireFastMutex(&Device->GartLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL || Device->GpuMem == NULL) status = STATUS_DEVICE_NOT_READY;
    else if (Data->Count == 0 || Data->Count > BC250_FENCE_MAX_COUNT) status = STATUS_INVALID_PARAMETER;
    else if (Data->Interrupt > BC250_FENCE_MODE_RING_TEST || (Data->Interrupt == BC250_FENCE_MODE_RING_TEST && !sdma)) status = STATUS_INVALID_PARAMETER;
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
        start = KeQueryPerformanceCounter(NULL);
        if (NT_SUCCESS(status) && Data->Interrupt == BC250_FENCE_MODE_RING_TEST)
        {
            // sdma_v5_0_ring_test_ring(): one WRITE_LINEAR of 0xDEADBEEF into the engine's scratch slot, polled by the shim.
            result = bc250_sdma_ring_test(ring);
            Data->LastSeq = 0xDEADBEEF;
            Data->LastValue = (unsigned long)bc250_sdma_fence_read(adev, Data->Ring - BC250_FENCE_RING_SDMA0);
            if (result == 0) Data->Completed = 1;
        }
        for (i = 0; NT_SUCCESS(status) && Data->Interrupt != BC250_FENCE_MODE_RING_TEST && i < Data->Count; i++)
        {
            LARGE_INTEGER one = KeQueryPerformanceCounter(NULL);
            ULONG seq = ++gfx->FenceSeq, took;
            u64 address = sdma ? bc250_sdma_fence_addr(adev, slot) : bc250_gfx_fence_addr(adev, slot);

            // The whole call holds GartLock and stalls: a run of slow fences ends here, not after Count timeouts.
            if (address == 0 || Microseconds(start, frequency) > BC250_FENCE_BUDGET_US) { result = -62; break; }
            result = sdma ? bc250_sdma_signal_fence(ring, address, seq, flags) : bc250_gfx_signal_fence(ring, address, seq, flags);
            if (result != 0 || !NT_SUCCESS(gfx->Sequence.Fault)) break;
            Data->LastSeq = seq;
            for (waited = 0; waited < BC250_FENCE_TIMEOUT_US; waited += 10)
            {
                Data->LastValue = (unsigned long)(sdma ? bc250_sdma_fence_read(adev, slot) : bc250_gfx_fence_read(adev, slot));
                if (Data->LastValue == seq) break;
                KeStallExecutionProcessor(10);
            }
            if (Data->LastValue != seq) { result = -62; break; }
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
    Device->Gfx = gfx;
    GuardLog("gfx: ready");
    return STATUS_SUCCESS;
}

// Called first of the sequences' stops (pnp.c): engines halted while the PSP still has its ring and the GART is still
// enabled, as in amdgpu's teardown order. Hands gpumem.c the verdict on whether GTT pages may go back to Windows.
void GfxStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled, quiet = TRUE;

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
            quiet = Fini(Device, gfx, adev);
            adev->backend = previousBackend;
        }
    }
    GpuMemStop(Device, quiet && Device->IhQuiet);       // ih.c stopped before us and said whether its ring is off
    ExReleaseFastMutex(&Device->GartLock);
    if (gfx != NULL) ExFreePoolWithTag(gfx, BC250_GFX_TAG);
}
