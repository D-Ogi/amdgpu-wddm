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
#include <ntstrsafe.h>
#include <stdarg.h>
#include "bc250_gmc.h"

#define BC250_GART_TAG 'gC2B'
// Where amdgpu put its GART table on this unit (E03: MC 0xF5FFE00000, the top 2 MB of VRAM), and E08 read it.
// First MB: the table (512 MB of GART, 8 bytes per 4 KB page). Second MB, first page: the scratch page.
#define BC250_GART_WINDOW 0x200000ull
#define BC250_GART_SCRATCH_OFFSET 0x100000ull
#define BC250_GART_INVALIDATE_ENGINES 18

typedef struct _BC250_GART {
    BC250_DEVICE* Device;
    BOOLEAN Plan;
    NTSTATUS Fault;                 // first refused register access of the running sequence
    ULONG FaultOffset;
    ULONG WriteCount;
    BC250_ESCAPE_GART* Report;      // where the writes are listed, may be NULL

    // Kept from ENABLE until RESTORE.
    BOOLEAN Enabled;
    BOOLEAN SnapshotValid;
    ULONG Snapshot[BC250_GART_MAX_WRITES];      // by index into the generated table
    PVOID DummyPage;
    PHYSICAL_ADDRESS DummyPhysical;

    struct amdgpu_device Adev;
    struct amdgpu_bo TableBo;
} BC250_GART;

// ---- the shim's backend (driver/shim/include/bc250_shim.h) ------------------------------------------------------

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

static void RecordFault(_Inout_ BC250_GART* Gart, NTSTATUS Status, ULONG Offset)
{
    if (NT_SUCCESS(Gart->Fault))
    {
        Gart->Fault = Status;
        Gart->FaultOffset = Offset;
        GuardLog("gart: register 0x%05X refused (0x%08X), sequence stopped", Offset, Status);
    }
}

unsigned int bc250_shim_rreg(struct amdgpu_device* adev, unsigned int dword_index)
{
    BC250_GART* gart = (BC250_GART*)adev->backend;
    ULONG value = 0;
    NTSTATUS status;

    // After a fault nothing is read any more, and all ones ends every bit poll of the sequence at once. No value
    // read here can reach the hardware: writes are stopped as well.
    if (!NT_SUCCESS(gart->Fault)) return 0xFFFFFFFFu;
    if (gart->Plan && IsProtocolRegister(adev, dword_index)) return 0xFFFFFFFFu;    // semaphore taken, every VMID acknowledged
    status = MmioGartRead(gart->Device, dword_index * 4, &value);
    if (!NT_SUCCESS(status)) RecordFault(gart, status, dword_index * 4);
    return value;
}

void bc250_shim_wreg(struct amdgpu_device* adev, unsigned int dword_index, unsigned int value)
{
    BC250_GART* gart = (BC250_GART*)adev->backend;
    NTSTATUS status;

    if (!NT_SUCCESS(gart->Fault))
    {
        // The one write that must get through a stopped sequence: giving the invalidation semaphore back. A
        // semaphore left held would wedge every later invalidation, the next Linux boot's included (facts M25).
        if (!gart->Plan && value == 0 && IsSemaphoreRegister(adev, dword_index)) MmioGartWrite(gart->Device, dword_index * 4, 0);
        return;
    }
    if (!gart->Plan)
    {
        status = MmioGartWrite(gart->Device, dword_index * 4, value);
        if (!NT_SUCCESS(status)) { RecordFault(gart, status, dword_index * 4); return; }
    }
    if (gart->Report != NULL && gart->WriteCount < BC250_GART_MAX_WRITES)
    {
        gart->Report->Writes[gart->WriteCount].Offset = dword_index * 4;
        gart->Report->Writes[gart->WriteCount].Value = value;
    }
    gart->WriteCount++;
}

void bc250_shim_udelay(unsigned int usec)
{
    KeStallExecutionProcessor(usec);
}

void bc250_shim_log(int level, void* dev, const char* fmt, ...)
{
    va_list arguments;
    char line[160];

    UNREFERENCED_PARAMETER(dev);
    va_start(arguments, fmt);
    if (NT_SUCCESS(RtlStringCchVPrintfA(line, sizeof(line), fmt, arguments))) GuardLog("amdgpu[%d]: %s", level, line);
    va_end(arguments);
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
    Gart->Adev.backend = Gart;
    inputs.gart_table_mc = device->VramMcBase + window;
    inputs.mem_scratch_mc = device->VramMcBase + window + BC250_GART_SCRATCH_OFFSET;
    inputs.dummy_page_dma = (u64)Gart->DummyPhysical.QuadPart;
    inputs.noretry = true;          // what amdgpu used on unit A: CONTEXTn_CNTL in the E03 trace, driver/shim/README.md
    return bc250_gmc_setup(&Gart->Adev, &inputs, &Gart->TableBo);
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
    if (Gart->Adev.vmhub[AMDGPU_GFXHUB(0)].eng_distance == 0 || Gart->Adev.backend != Gart) return STATUS_INVALID_DEVICE_STATE;
    for (pass = 0; pass < 2; pass++)
    {
        for (i = 0; i < count; i++)
        {
            BOOLEAN control = IsContextControl(&Gart->Adev, table[i] / 4);
            if (IsProtocolRegister(&Gart->Adev, table[i] / 4) || control != (pass == 0)) continue;
            bc250_shim_wreg(&Gart->Adev, table[i] / 4, Gart->Snapshot[i]);
        }
    }
    return Gart->Fault;
}

void GartEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_GART* Data)
{
    BC250_GART* gart;
    ULONGLONG window = Device->VramLength - BC250_GART_WINDOW;
    NTSTATUS status;
    int result = 0;

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
    gart->Plan = (Data->Op == BC250_GART_OP_PLAN);
    gart->Fault = STATUS_SUCCESS;
    gart->FaultOffset = 0;
    gart->WriteCount = 0;
    gart->Report = Data;

    if (NT_SUCCESS(status) && RunSetup(gart) != 0) status = STATUS_DEVICE_DATA_ERROR;
    if (NT_SUCCESS(status)) status = gart->Fault;
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
                result = bc250_gmc_gart_enable(&gart->Adev);
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
    if (NT_SUCCESS(status)) status = gart->Fault;
    GuardLog("gart: op %u -> 0x%08X, result %d, %u writes, fault offset 0x%05X", Data->Op, status, result, gart->WriteCount, gart->FaultOffset);

    gart->Report = NULL;
    Data->Result = result;
    Data->FaultOffset = gart->FaultOffset;
    Data->WriteCount = gart->WriteCount;
    Data->State = (gart->Enabled ? BC250_GART_STATE_ENABLED : 0) | (gart->SnapshotValid ? BC250_GART_STATE_SNAPSHOT : 0);
    Data->TablePhysical = (unsigned long long)Device->VramPhysical.QuadPart + window;
    Data->TableMc = Device->VramMcBase + window;
    Data->ScratchMc = Device->VramMcBase + window + BC250_GART_SCRATCH_OFFSET;
    Data->DummyPhysical = (unsigned long long)gart->DummyPhysical.QuadPart;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
    ExReleaseFastMutex(&Device->GartLock);
}

// ---- start and stop ---------------------------------------------------------------------------------------------------

NTSTATUS GartStart(_Inout_ BC250_DEVICE* Device)
{
    PHYSICAL_ADDRESS low, high, boundary;
    BC250_GART* gart;

    Device->Gart = NULL;
    if (!Device->MmioGartEnabled || !Device->VramEnabled) return STATUS_SUCCESS;

    gart = (BC250_GART*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*gart), BC250_GART_TAG);
    if (gart == NULL) return STATUS_SUCCESS;        // never fails the start
    gart->Device = Device;
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

// Called while MMIO is still mapped. If the GPU was told about our pages, the firmware's state goes back first;
// if that fails, the pages are deliberately never freed: a leak is better than a GPU writing into freed memory.
void GartStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GART* gart;

    ExAcquireFastMutex(&Device->GartLock);
    gart = (BC250_GART*)Device->Gart;
    Device->Gart = NULL;
    ExReleaseFastMutex(&Device->GartLock);      // nobody can find the context any more; the rest needs no lock
    if (gart == NULL) return;
    if (gart->Enabled)
    {
        gart->Plan = FALSE;
        gart->Fault = STATUS_SUCCESS;
        gart->WriteCount = 0;
        gart->Report = NULL;
        if (!NT_SUCCESS(WriteSnapshotBack(gart)))
        {
            GuardLog("gart: restore at stop failed, keeping the dummy page allocated");
            return;
        }
        GuardLog("gart: firmware state restored at stop (%u writes)", gart->WriteCount);
    }
    MmFreeContiguousMemory(gart->DummyPage);
    ExFreePoolWithTag(gart, BC250_GART_TAG);
}
