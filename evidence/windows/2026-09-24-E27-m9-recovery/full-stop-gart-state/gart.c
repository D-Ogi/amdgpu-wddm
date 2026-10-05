// GART and VM context 0 (milestone M4, ADR 0002 and 0007). The register work is AMD's own code, unmodified
// (driver/amdgpu-import: gfxhub_v2_0.c, mmhub_v2_0.c), driven by driver/shim/bc250_gmc.c in the order of
// gmc_v10_0_gart_enable(); a host test proves that this code produces amdgpu's recorded writes for unit A
// (driver/shim/test). This file is the kernel backend of that shim plus the command around it.
//
//   <service key>\Parameters
//     EnableGart   REG_DWORD  1 = allow the GART command. Needs EnableMmio and EnableVram. Default 0.
//
// Three operations, one escape (BC250_ESCAPE_RUN_GART):
//   PLAN     run the whole sequence against the real registers but execute no write and no protocol read; return
//            the writes it would issue. What the hardware would be told, before it is told.
//   ENABLE   snapshot every register of the sequence, zero the page table and the scratch page at the top of
//            VRAM, run the sequence for real.
//   RESTORE  write the snapshot back: the firmware's state, which is where Windows found the GPU.
//
// Register access goes through MmioGartRead/MmioGartWrite only, i.e. through the table generated from amdgpu's
// own trace of this step. The first refused access stops all further writes of the sequence.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "bc250_gmc.h"

#define BC250_GART_TAG 'gC2B'
// Where amdgpu put its GART table on this unit (E03: MC 0xF5FFE00000, the top 2 MB of VRAM), and E08 read it.
// First MB: the table (512 MB of GART, 8 bytes per 4 KB page). Second MB, first page: the scratch page.
#define BC250_GART_WINDOW BC250_VRAM_GART_BELOW     // the number itself is in the reservation table, bc250kmd.h
#define BC250_GART_SCRATCH_OFFSET 0x100000ull
#define BC250_GART_INVALIDATE_ENGINES 18

typedef struct _BC250_GART {
    BC250_DEVICE* Device;
    BC250_SEQUENCE Sequence;        // the shim's backend state for this command (sequence.c)
    BOOLEAN SetUp;                  // Adev holds the register bases and the VRAM window

    // Kept from ENABLE until RESTORE.
    BOOLEAN Enabled;
    BOOLEAN SnapshotValid;
    ULONG Snapshot[BC250_GART_MAX_WRITES];      // by index into the generated table
    PVOID DummyPage;
    PHYSICAL_ADDRESS DummyPhysical;

    struct amdgpu_device Adev;
    struct amdgpu_bo TableBo;
} BC250_GART;

// ---- what the shim's backend (sequence.c) needs to know about this sequence ---------------------------------------

// The invalidation protocol registers: reading the semaphore acquires it (facts M25), writing the request
// starts an invalidation. PLAN answers them instead of touching them, and the snapshot leaves them out. Their
// places come from the hub descriptors the imported init() filled in, not from us.
static BOOLEAN IsProtocolRegisterOfKind(_In_ const struct amdgpu_device* Adev, ULONG DwordIndex, ULONG FirstKind, ULONG LastKind)
{
    ULONG h;

    for (h = 0; h < AMDGPU_MAX_VMHUBS; h++)
    {
        const struct amdgpu_vmhub* hub = &Adev->vmhub[h];
        const ULONG firsts[3] = { hub->vm_inv_eng0_sem, hub->vm_inv_eng0_req, hub->vm_inv_eng0_ack };
        ULONG k;

        if (hub->eng_distance == 0) continue;
        for (k = FirstKind; k <= LastKind; k++)
        {
            if (DwordIndex >= firsts[k] && DwordIndex < firsts[k] + hub->eng_distance * BC250_GART_INVALIDATE_ENGINES &&
                (DwordIndex - firsts[k]) % hub->eng_distance == 0) return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN IsProtocolRegister(_In_ const struct amdgpu_device* Adev, ULONG DwordIndex)
{
    return IsProtocolRegisterOfKind(Adev, DwordIndex, 0, 2);
}

static BOOLEAN IsSemaphoreRegister(_In_ const struct amdgpu_device* Adev, ULONG DwordIndex)
{
    return IsProtocolRegisterOfKind(Adev, DwordIndex, 0, 0);
}

// PLAN answers the protocol registers instead of touching them: semaphore taken, every VMID acknowledged.
static BOOLEAN GartPlanAnswers(_In_ BC250_SEQUENCE* Sequence, ULONG DwordIndex, _Out_ ULONG* Value)
{
    const BC250_GART* gart = (const BC250_GART*)Sequence->Owner;

    *Value = 0xFFFFFFFFu;
    return IsProtocolRegister(&gart->Adev, DwordIndex);
}

// The one write that must get through a stopped sequence: giving the invalidation semaphore back. A semaphore
// left held would wedge every later invalidation, the next Linux boot's included (facts M25).
static BOOLEAN GartPassesFault(_In_ BC250_SEQUENCE* Sequence, ULONG DwordIndex, ULONG Value)
{
    const BC250_GART* gart = (const BC250_GART*)Sequence->Owner;

    return Value == 0 && IsSemaphoreRegister(&gart->Adev, DwordIndex);
}

// ---- memory ---------------------------------------------------------------------------------------------------------

static NTSTATUS ZeroVram(_In_ const BC250_DEVICE* Device, ULONGLONG Offset, SIZE_T Length)
{
    PHYSICAL_ADDRESS address;
    volatile ULONG64* map;
    SIZE_T i;

    address.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)Offset;
    map = (volatile ULONG64*)MmMapIoSpaceEx(address, Length, PAGE_READWRITE | PAGE_NOCACHE);
    if (map == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    for (i = 0; i < Length / sizeof(ULONG64); i++) map[i] = 0;
    for (i = 0; i < Length / sizeof(ULONG64); i += 511) if (map[i] != 0) break;       // spot check, prime stride
    MmUnmapIoSpace((PVOID)map, Length);
    return i < Length / sizeof(ULONG64) ? STATUS_DEVICE_DATA_ERROR : STATUS_SUCCESS;
}

// The window must be inside VRAM, above the firmware framebuffer, and the carve-out identified (vram.c).
static NTSTATUS CheckWindow(_In_ const BC250_DEVICE* Device)
{
    ULONGLONG fb = (ULONGLONG)Device->Post.PhysicAddress.QuadPart, fbOffset;

    if (!Device->VramEnabled || Device->VramLength < 2 * BC250_GART_WINDOW) return STATUS_DEVICE_NOT_READY;
    if (fb >= (ULONGLONG)Device->VramPhysical.QuadPart && fb < (ULONGLONG)Device->VramPhysical.QuadPart + Device->VramLength)
        fbOffset = fb - (ULONGLONG)Device->VramPhysical.QuadPart;
    else if (Device->Bar0Length != 0 && fb >= (ULONGLONG)Device->Bar0Physical.QuadPart &&
             fb < (ULONGLONG)Device->Bar0Physical.QuadPart + Device->Bar0Length)
        fbOffset = fb - (ULONGLONG)Device->Bar0Physical.QuadPart;
    else return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (fbOffset + Device->FramebufferLength > Device->VramLength - BC250_GART_WINDOW) return STATUS_CONFLICTING_ADDRESSES;
    return STATUS_SUCCESS;
}

// ---- the command ----------------------------------------------------------------------------------------------------

static int RunSetup(_Inout_ BC250_GART* Gart)
{
    const BC250_DEVICE* device = Gart->Device;
    struct bc250_gmc_inputs inputs;
    ULONGLONG window = device->VramLength - BC250_GART_WINDOW;

    RtlZeroMemory(&Gart->Adev, sizeof(Gart->Adev));
    RtlZeroMemory(&Gart->TableBo, sizeof(Gart->TableBo));
    Gart->Adev.dev = (void*)device;
    Gart->Adev.backend = &Gart->Sequence;
    inputs.gart_table_mc = device->VramMcBase + window;
    inputs.mem_scratch_mc = device->VramMcBase + window + BC250_GART_SCRATCH_OFFSET;
    inputs.dummy_page_dma = (u64)Gart->DummyPhysical.QuadPart;
    inputs.noretry = true;          // what amdgpu used on unit A: CONTEXTn_CNTL in the E03 trace, driver/shim/README.md
    Gart->SetUp = (bc250_gmc_setup(&Gart->Adev, &inputs, &Gart->TableBo) == 0);
    return Gart->SetUp ? 0 : -1;
}

static NTSTATUS TakeSnapshot(_Inout_ BC250_GART* Gart)
{
    const unsigned long* table;
    ULONG count = MmioGartTable(&table), i;
    NTSTATUS status;

    if (Gart->SnapshotValid) return STATUS_SUCCESS;         // the firmware's state is taken once, before our first write
    if (count > BC250_GART_MAX_WRITES) return STATUS_BUFFER_TOO_SMALL;
    for (i = 0; i < count; i++)
    {
        Gart->Snapshot[i] = 0;
        if (IsProtocolRegister(&Gart->Adev, table[i] / 4)) continue;
        status = MmioGartRead(Gart->Device, table[i], &Gart->Snapshot[i]);
        if (!NT_SUCCESS(status)) return status;
    }
    Gart->SnapshotValid = TRUE;
    return STATUS_SUCCESS;
}

// Context control registers first, so that no context is left enabled while its page table address goes away.
// Which ones they are comes from the hub descriptors again.
static BOOLEAN IsContextControl(_In_ const struct amdgpu_device* Adev, ULONG DwordIndex)
{
    ULONG h;

    for (h = 0; h < AMDGPU_MAX_VMHUBS; h++)
    {
        const struct amdgpu_vmhub* hub = &Adev->vmhub[h];
        if (hub->ctx_distance != 0 && DwordIndex >= hub->vm_context0_cntl &&
            DwordIndex < hub->vm_context0_cntl + hub->ctx_distance * 16 && (DwordIndex - hub->vm_context0_cntl) % hub->ctx_distance == 0)
            return TRUE;
    }
    return FALSE;
}

static NTSTATUS WriteSnapshotBack(_Inout_ BC250_GART* Gart)
{
    const unsigned long* table;
    ULONG count = MmioGartTable(&table), i, pass;

    if (!Gart->SnapshotValid || count > BC250_GART_MAX_WRITES) return STATUS_INVALID_DEVICE_STATE;
    if (Gart->Adev.vmhub[AMDGPU_GFXHUB(0)].eng_distance == 0 || Gart->Adev.backend != &Gart->Sequence) return STATUS_INVALID_DEVICE_STATE;
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < count; i++)
        {
            BOOLEAN control = IsContextControl(&Gart->Adev, table[i] / 4);
            if (IsProtocolRegister(&Gart->Adev, table[i] / 4) || control != (pass == 0)) continue;
            bc250_shim_wreg(&Gart->Adev, table[i] / 4, Gart->Snapshot[i]);
        }
    }
    return Gart->Sequence.Fault;
}

static void ObserveStartupGart(struct amdgpu_device* adev, const char* phase, u32 value)
{
    BC250_SEQUENCE* sequence = (BC250_SEQUENCE*)adev->backend;
    GuardLog("gart: startup %s sample 0x%08X fault 0x%08X",phase,value,sequence->Fault);
    GfxTraceRlcState(sequence->Device,phase);
}

static void GartExecute(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GART* Data)
{
    BC250_GART* gart;
    ULONGLONG window = Device->VramLength - BC250_GART_WINDOW;
    NTSTATUS status;
    int result = 0;
    const char* activeReason = "-";      // which precondition below refused the call; named so a REFUSED exit code
                                         // (bc250kmd_cli's 3) has an answer in GuardLog without reading this source

    Data->Version = BC250_KMD_VERSION;
    Data->Result = 0;
    Data->FaultOffset = 0;
    Data->WriteCount = 0;
    Data->State = 0;
    // dxgkrnl does not serialize escapes, and everything below works on one shared context: one at a time, and
    // never while the device stops.
    ExAcquireFastMutex(&Device->GartLock);
    gart = (BC250_GART*)Device->Gart;
    if (gart == NULL || !Device->MmioGartEnabled)
    {
        ExReleaseFastMutex(&Device->GartLock);
        Data->NtStatus = (unsigned long)STATUS_DEVICE_NOT_READY;
        Data->Status = BC250_ESCAPE_STATUS_REFUSED;
        return;
    }
    status = CheckWindow(Device);
    // Every GART command sets the shim's device up afresh, and gfx.c keeps its state in that device; a restore would
    // also take the GART away from under mapped queues. amdgpu's order: the engines go first (gfx.c's FINI).
    if (NT_SUCCESS(status) && GfxIsActive(Device)) { status = STATUS_INVALID_DEVICE_STATE; activeReason = "gfx active (gfx fini needed first)"; }
    else if (NT_SUCCESS(status) && IhIsActive(Device)) { status = STATUS_INVALID_DEVICE_STATE; activeReason = "ih active (ih fini needed first)"; }
    SequenceBegin(&gart->Sequence, Device, Data->Op == BC250_GART_OP_PLAN, Data->Writes, BC250_GART_MAX_WRITES);

    if (NT_SUCCESS(status) && RunSetup(gart) != 0) status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(status)) status = gart->Sequence.Fault;
    if (NT_SUCCESS(status))
    {
        switch (Data->Op)
        {
        case BC250_GART_OP_PLAN:
            result = bc250_gmc_gart_enable(&gart->Adev);
            break;
        case BC250_GART_OP_ENABLE:
            status = TakeSnapshot(gart);
            if (NT_SUCCESS(status) && (gart->Adev.gart.table_size == 0 || gart->Adev.gart.table_size > BC250_GART_SCRATCH_OFFSET))
                status = STATUS_BUFFER_OVERFLOW;        // the table has the first MB of the window and not a byte more
            if (NT_SUCCESS(status)) status = ZeroVram(Device, window, (SIZE_T)gart->Adev.gart.table_size);
            if (NT_SUCCESS(status)) status = ZeroVram(Device, window + BC250_GART_SCRATCH_OFFSET, PAGE_SIZE);
            if (NT_SUCCESS(status))
            {
                gart->Enabled = TRUE;               // from the first write on, a RESTORE is owed
                if (Device->GfxTlbBootstrap) {
                    // M362/M364 deviation: defer GFX invalidation until RLC runs.
                    // Full startup owns the unpublished phase. MM consumers must
                    // see mappings now; GFX visibility is committed after RLC.
                    result=bc250_gmc_gart_configure_observed(&gart->Adev,ObserveStartupGart);
                    if (result==0 && NT_SUCCESS(gart->Sequence.Fault))
                        result=bc250_gmc_flush_gpu_tlb(&gart->Adev,0,AMDGPU_MMHUB0(0),0);
                    ObserveStartupGart(&gart->Adev,"bootstrap-after-mmhub-flush",(u32)result);
                } else result = bc250_gmc_gart_enable_observed(&gart->Adev,Device->FullWddm ? ObserveStartupGart : NULL);
            }
            break;
        case BC250_GART_OP_RESTORE:
            status = WriteSnapshotBack(gart);
            if (NT_SUCCESS(status)) gart->Enabled = FALSE;
            break;
        default:
            status = STATUS_INVALID_PARAMETER;
            break;
        }
    }
    if (NT_SUCCESS(status)) status = gart->Sequence.Fault;
    GuardLog("gart: op %u -> 0x%08X (%s), result %d, %u writes, fault offset 0x%05X", Data->Op, status, activeReason,
             result, gart->Sequence.WriteCount, gart->Sequence.FaultOffset);

    gart->Sequence.Writes = NULL;           // the caller's buffer goes away with this call
    gart->Sequence.MaxWrites = 0;
    Data->Result = result;
    Data->FaultOffset = gart->Sequence.FaultOffset;
    Data->WriteCount = gart->Sequence.WriteCount;
    Data->State = (gart->Enabled ? BC250_GART_STATE_ENABLED : 0) | (gart->SnapshotValid ? BC250_GART_STATE_SNAPSHOT : 0);
    Data->TablePhysical = (unsigned long long)Device->VramPhysical.QuadPart + window;
    Data->TableMc = Device->VramMcBase + window;
    Data->ScratchMc = Device->VramMcBase + window + BC250_GART_SCRATCH_OFFSET;
    Data->DummyPhysical = (unsigned long long)gart->DummyPhysical.QuadPart;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
    ExReleaseFastMutex(&Device->GartLock);
}

// Diagnostic commands and device startup share the same implementation.
void GartEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GART* Data)
{
    GartExecute(Device,Data);
}

// PASSIVE_LEVEL. Caller owns nonpaged report storage through this synchronous
// call. Keep detailed partial-progress output for the eventual startup unwind.
NTSTATUS GartInitializeHardware(BC250_DEVICE* Device, BC250_ESCAPE_GART* Report)
{
    NTSTATUS status;
    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    Report->Magic=BC250_ESCAPE_MAGIC;Report->Command=BC250_ESCAPE_RUN_GART;
    Report->Op=BC250_GART_OP_ENABLE;
    if (!Device) status=STATUS_INVALID_PARAMETER;
    else if (KeGetCurrentIrql()!=PASSIVE_LEVEL) status=STATUS_INVALID_DEVICE_STATE;
    else if (Device->GpuStopUnconfirmed) status=STATUS_DEVICE_HARDWARE_ERROR;
    else {
        GartExecute(Device,Report);
        status=(NTSTATUS)Report->NtStatus;
        if (NT_SUCCESS(status) && (Report->Status!=BC250_ESCAPE_STATUS_DONE || Report->Result!=0 ||
            (Report->State&BC250_GART_STATE_ENABLED)==0)) status=STATUS_IO_DEVICE_ERROR;
    }
    Report->NtStatus=(unsigned long)status;
    Report->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
    return status;
}


// For the sequences that come after this one (psp.c): the shim's device. Caller holds GartLock. If no GART command
// has run yet in this driver instance, the device is set up here, which only reads registers.
NTSTATUS GartDevice(_In_ BC250_DEVICE* Device, _Outptr_ struct amdgpu_device** Adev, _Out_ BOOLEAN* Enabled)
{
    BC250_GART* gart = (BC250_GART*)Device->Gart;
    NTSTATUS status;

    *Adev = NULL;
    *Enabled = FALSE;
    if (gart == NULL || !Device->MmioGartEnabled) return STATUS_DEVICE_NOT_READY;
    if (!gart->SetUp)
    {
        status = CheckWindow(Device);
        if (!NT_SUCCESS(status)) return status;
        SequenceBegin(&gart->Sequence, Device, TRUE, NULL, 0);
        if (RunSetup(gart) != 0 || !NT_SUCCESS(gart->Sequence.Fault)) return STATUS_DEVICE_DATA_ERROR;
    }
    *Adev = &gart->Adev;
    *Enabled = gart->Enabled;
    return STATUS_SUCCESS;
}

// Copy only geometry while the GART owner is locked. Lazy setup uses the
// existing AMD placement code in planning mode; this does not enable hardware.
NTSTATUS GartCaptureAperture(BC250_DEVICE* Device, PAGING_APERTURE* Aperture)
{
    struct amdgpu_device* adev=NULL;
    BOOLEAN enabled=FALSE;
    NTSTATUS status;
    if (!Aperture) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Aperture,sizeof(*Aperture));
    if (!Device) return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&Device->GartLock);
    status=GartDevice(Device,&adev,&enabled);
    if (NT_SUCCESS(status)) {
        if (!adev || !adev->gart.bo ||
            !PagingApertureInit(adev->gmc.gart_start,adev->gmc.gart_size,
                adev->gart.bo->gpu_addr,adev->gart.table_size,Aperture))
            status=STATUS_DEVICE_NOT_READY;
    }
    ExReleaseFastMutex(&Device->GartLock);
    return status;
}

// ---- start and stop ---------------------------------------------------------------------------------------------------

NTSTATUS GartStart(_Inout_ BC250_DEVICE* Device)
{
    PHYSICAL_ADDRESS low, high, boundary;
    BC250_GART* gart;

    Device->Gart = NULL;
    Device->GartStopPrepared = FALSE;
    Device->GartStopQuiet = TRUE;
    if (!Device->MmioGartEnabled || !Device->VramEnabled) return STATUS_SUCCESS;

    gart = (BC250_GART*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*gart), BC250_GART_TAG);
    if (gart == NULL) return STATUS_SUCCESS;        // never fails the start
    gart->Device = Device;
    gart->Sequence.Name = "gart";
    gart->Sequence.Read = MmioGartRead;
    gart->Sequence.Write = MmioGartWrite;
    gart->Sequence.PlanAnswers = GartPlanAnswers;
    gart->Sequence.PassesFault = GartPassesFault;
    gart->Sequence.Owner = gart;
    // The page faulting GPU accesses are redirected to. It stays allocated for as long as the GPU may know it.
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFFFFll;                // 44 bits, amdgpu's DMA mask for this generation
    boundary.QuadPart = 0;
    gart->DummyPage = MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, low, high, boundary, MmCached);
    if (gart->DummyPage == NULL)
    {
        ExFreePoolWithTag(gart, BC250_GART_TAG);
        return STATUS_SUCCESS;
    }
    RtlZeroMemory(gart->DummyPage, PAGE_SIZE);
    gart->DummyPhysical = MmGetPhysicalAddress(gart->DummyPage);
    Device->Gart = gart;
    GuardLog("gart: ready, dummy page at 0x%llX", gart->DummyPhysical.QuadPart);
    return STATUS_SUCCESS;
}

// Hardware retirement only. Retain the adev, table and dummy page until GFX
// has destroyed its software resources and final firmware restoration succeeds.
void GartPrepareStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GART* gart;
    ExAcquireFastMutex(&Device->GartLock);
    if (!Device->GartStopPrepared) {
        Device->GartStopPrepared = TRUE;
        Device->GartStopQuiet = FALSE;
        gart = (BC250_GART*)Device->Gart;
        if (!Device->GfxTlbBootstrap && Device->GfxStopPrepared && Device->GfxStopQuiet && Device->IhQuiet && Device->PspStopQuiet) {
            if (gart == NULL || !gart->Enabled) Device->GartStopQuiet = TRUE;
            else {
                void* previousBackend = gart->Adev.backend;
                gart->Adev.backend = &gart->Sequence;
                SequenceBegin(&gart->Sequence, Device, FALSE, NULL, 0);
                bc250_gmc_gart_disable(&gart->Adev);
                Device->GartStopQuiet = NT_SUCCESS(gart->Sequence.Fault);
                gart->Adev.backend = previousBackend;
                GuardLog("gart: hardware disable status 0x%08X",gart->Sequence.Fault);
                GfxTraceRlcState(Device,"after-gart-hardware-disable");
            }
        }
        if (!Device->GartStopQuiet) Device->GpuStopUnconfirmed = TRUE;
    }
    ExReleaseFastMutex(&Device->GartLock);
}

// Owner destruction after GFX storage cleanup. Full WDDM keeps the contexts
// and caches disabled by GartPrepareStop, as gmc_v10_0_hw_fini does in Linux
// v6.18. Restoring pre-driver register values is diagnostic-mode policy only:
// that snapshot is not a new set of owned page tables for a stopped adapter.
// An uncertain hardware phase retains the complete GART owner and dummy page.
void GartStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GART* gart;
    ExAcquireFastMutex(&Device->GartLock);
    gart = (BC250_GART*)Device->Gart;
    if (gart == NULL) { ExReleaseFastMutex(&Device->GartLock); return; }
    if (!Device->GartStopPrepared || !Device->GartStopQuiet || !Device->GfxStopQuiet ||
        !Device->IhQuiet || !Device->PspStopQuiet) {
        Device->GpuStopUnconfirmed = TRUE;
        GuardLog("gart: consumer/translation stop unconfirmed, retaining owner and dummy page");
        ExReleaseFastMutex(&Device->GartLock);
        return;
    }
    if (gart->Enabled && !Device->FullWddm) {
        SequenceBegin(&gart->Sequence, Device, FALSE, NULL, 0);
        if (!NT_SUCCESS(WriteSnapshotBack(gart))) {
            Device->GpuStopUnconfirmed = TRUE;
            Device->GartStopQuiet = FALSE; // latch failed restoration; repeated stop cannot retry it
            GuardLog("gart: restore at stop failed, retaining owner and dummy page");
            ExReleaseFastMutex(&Device->GartLock);
            return;
        }
        GuardLog("gart: firmware state restored at stop (%u writes)",gart->Sequence.WriteCount);
    }
    if (gart->Enabled && Device->FullWddm) GuardLog("gart: full stop retains disabled translation state");
    Device->Gart = NULL;
    ExReleaseFastMutex(&Device->GartLock);
    MmFreeContiguousMemory(gart->DummyPage);
    ExFreePoolWithTag(gart, BC250_GART_TAG);
}
