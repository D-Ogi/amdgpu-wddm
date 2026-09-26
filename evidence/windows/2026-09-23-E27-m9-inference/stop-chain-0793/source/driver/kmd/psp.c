// Firmware loading through the PSP (milestone M5 first part, ADR 0002 and 0007). The register work is AMD's own
// psp_v11_0_8.c, unmodified; the command path is driver/shim/bc250_psp.c, which follows amdgpu_psp.c and which a
// host test runs against amdgpu's recorded PSP traffic of unit A (driver/shim/test/replay_psp.c). This file is the
// memory, the firmware files and the command around it.
//
//   <service key>\Parameters
//     EnablePsp   REG_DWORD  1 = allow the PSP command. Needs EnableMmio, EnableVram and EnableGart. Default 0.
//
// Three operations, one escape (BC250_ESCAPE_RUN_PSP):
//   PLAN     read the firmware files, lay the images out, run the ring create against the real registers without
//            executing a write. Returns the register writes and the eleven commands that LOAD would issue.
//            Touches no VRAM and writes no register.
//   LOAD     needs the GART sequence enabled (amdgpu's order: GART, then PSP). Stage the images in VRAM, create the
//            PSP's kernel-mode ring, SETUP_TMR, ten LOAD_IP_FW. Stops at the first command the PSP does not accept.
//   UNLOAD   DESTROY_TMR and ring stop, what amdgpu does in psp_hw_fini(). Also done by the driver itself when the
//            device stops.
//
// Memory, all in the top 8 MB of VRAM, which Windows does not know exists (the carve-out is reserved in the
// firmware's memory map, facts M31), reached by physical address (facts M32), below the last 64 KB:
//   end - 8 MB    TMR, 4 MB      MC 0xF5FF800000 on unit A: where amdgpu had it
//   end - 4 MB    staging, 2 MB  the ten images, each on a page boundary (amdgpu: a GTT buffer, or VRAM with
//                                amdgpu.debug's use_vram_fw_buf; GART-mapped staging comes with the first ring)
//   end - 2 MB    GART table and scratch page (gart.c)
//   end - 0x19000 PSP ring, 4 KB MC 0xF5FFFE7000 on unit A: where amdgpu had it, so C2PMSG_69/70 equal the trace
//   end - 0x18000 command buffer, end - 0x17000 fence page
//
// Registers only through g_MmioPspAllow, generated from amdgpu's own trace of this step: the five PSP mailbox
// registers it used.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "bc250_gmc.h"
#include "bc250_psp.h"

#define BC250_PSP_TAG 'pS2B'
#define BC250_PSP_TOP_WINDOW     BC250_VRAM_PSP_BELOW       // the numbers themselves are in the reservation
#define BC250_PSP_TMR_BELOW      BC250_VRAM_PSP_BELOW       // table, bc250kmd.h
#define BC250_PSP_STAGING_BELOW  0x400000ull
#define BC250_PSP_STAGING_LENGTH 0x200000ul
#define BC250_PSP_PAGES_BELOW    0x19000ull         // ring, command buffer, fence: three pages
#define BC250_PSP_PAGES_LENGTH   0x3000ul
#define BC250_PSP_MAX_FILE       0x100000ul         // the largest file is 268592 bytes
// The global DOS device directory, not the caller's own (\??\ resolves per logon session, and a drive letter can be
// redefined there): these bytes go to the PSP.
#define BC250_PSP_FIRMWARE_DIR   L"\\GLOBAL??\\C:\\BC250\\firmware\\"

typedef struct _BC250_PSP {
    BC250_SEQUENCE Sequence;
    struct bc250_psp Context;
    PUCHAR Pages;                   // mapping of ring, command buffer and fence, kept from LOAD to UNLOAD
    BOOLEAN RingUp;                 // the PSP has been told about our ring
    BOOLEAN TmrUp;                  // the PSP has been told about our TMR
    BOOLEAN Loaded;                 // all eleven commands of a LOAD were accepted, and no unload since
} BC250_PSP;

typedef struct _BC250_PSP_FILES {
    PUCHAR Data[BC250_FILE_COUNT];
    ULONG Size[BC250_FILE_COUNT];
} BC250_PSP_FILES;

// ---- firmware files, PASSIVE_LEVEL, before any lock is taken ------------------------------------------------------

static void FreeFiles(_Inout_ BC250_PSP_FILES* Files)
{
    ULONG i;

    for (i = 0; i < BC250_FILE_COUNT; i++)
    {
        if (Files->Data[i] != NULL) ExFreePoolWithTag(Files->Data[i], BC250_PSP_TAG);
        Files->Data[i] = NULL;
    }
}

static NTSTATUS ReadOneFile(_In_z_ const char* Name, _Outptr_ PUCHAR* Data, _Out_ ULONG* Size)
{
    WCHAR path[128];
    UNICODE_STRING unicode;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK io;
    FILE_STANDARD_INFORMATION info;
    HANDLE file = NULL;
    PUCHAR buffer = NULL;
    NTSTATUS status;
    ULONG i, prefix = (ULONG)(sizeof(BC250_PSP_FIRMWARE_DIR) / sizeof(WCHAR)) - 1;

    *Data = NULL;
    *Size = 0;
    RtlCopyMemory(path, BC250_PSP_FIRMWARE_DIR, sizeof(BC250_PSP_FIRMWARE_DIR));
    for (i = 0; Name[i] != 0 && prefix + i + 1 < RTL_NUMBER_OF(path); i++) path[prefix + i] = (WCHAR)Name[i];   // ASCII names
    path[prefix + i] = 0;
    RtlInitUnicodeString(&unicode, path);
    InitializeObjectAttributes(&attributes, &unicode, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwCreateFile(&file, FILE_READ_DATA | SYNCHRONIZE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(status)) return status;
    status = ZwQueryInformationFile(file, &io, &info, sizeof(info), FileStandardInformation);
    if (NT_SUCCESS(status) && (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > BC250_PSP_MAX_FILE)) status = STATUS_FILE_TOO_LARGE;
    if (NT_SUCCESS(status))
    {
        buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_PAGED, (SIZE_T)info.EndOfFile.QuadPart, BC250_PSP_TAG);
        if (buffer == NULL) status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (NT_SUCCESS(status))
    {
        status = ZwReadFile(file, NULL, NULL, NULL, &io, buffer, (ULONG)info.EndOfFile.QuadPart, NULL, NULL);
        if (NT_SUCCESS(status) && io.Information != (ULONG_PTR)info.EndOfFile.QuadPart) status = STATUS_END_OF_FILE;
    }
    ZwClose(file);
    if (!NT_SUCCESS(status))
    {
        if (buffer != NULL) ExFreePoolWithTag(buffer, BC250_PSP_TAG);
        return status;
    }
    *Data = buffer;
    *Size = (ULONG)info.EndOfFile.QuadPart;
    return STATUS_SUCCESS;
}

static NTSTATUS ReadFiles(_Out_ BC250_PSP_FILES* Files, _Out_ ULONG* FailedFile)
{
    ULONG i;
    NTSTATUS status;

    RtlZeroMemory(Files, sizeof(*Files));
    *FailedFile = 0;
    for (i = 0; i < BC250_FILE_COUNT; i++)
    {
        status = ReadOneFile(bc250_fw_file_name((enum bc250_fw_file)i), &Files->Data[i], &Files->Size[i]);
        if (!NT_SUCCESS(status))
        {
            GuardLog("psp: firmware file %s: 0x%08X", bc250_fw_file_name((enum bc250_fw_file)i), status);
            *FailedFile = i;
            FreeFiles(Files);
            return status;
        }
    }
    return STATUS_SUCCESS;
}

// ---- memory -----------------------------------------------------------------------------------------------------------

static PUCHAR MapVram(_In_ const BC250_DEVICE* Device, ULONGLONG BelowEnd, SIZE_T Length)
{
    PHYSICAL_ADDRESS address;

    address.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)(Device->VramLength - BelowEnd);
    return (PUCHAR)MmMapIoSpaceEx(address, Length, PAGE_READWRITE | PAGE_NOCACHE);
}

// The window must be inside VRAM and above the firmware framebuffer, wherever the firmware says that is.
static NTSTATUS CheckWindow(_In_ const BC250_DEVICE* Device)
{
    ULONGLONG fb = (ULONGLONG)Device->Post.PhysicAddress.QuadPart, fbOffset;

    if (!Device->VramEnabled || Device->VramLength < 2 * BC250_PSP_TOP_WINDOW) return STATUS_DEVICE_NOT_READY;
    if (fb >= (ULONGLONG)Device->VramPhysical.QuadPart && fb < (ULONGLONG)Device->VramPhysical.QuadPart + Device->VramLength)
        fbOffset = fb - (ULONGLONG)Device->VramPhysical.QuadPart;
    else if (Device->Bar0Length != 0 && fb >= (ULONGLONG)Device->Bar0Physical.QuadPart &&
             fb < (ULONGLONG)Device->Bar0Physical.QuadPart + Device->Bar0Length)
        fbOffset = fb - (ULONGLONG)Device->Bar0Physical.QuadPart;
    else return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (fbOffset + Device->FramebufferLength > Device->VramLength - BC250_PSP_TOP_WINDOW) return STATUS_CONFLICTING_ADDRESSES;
    return STATUS_SUCCESS;
}

// ---- the command --------------------------------------------------------------------------------------------------------

static void FillAddresses(_In_ const BC250_DEVICE* Device, _Out_ BC250_ESCAPE_PSP* Data)
{
    ULONGLONG end = Device->VramMcBase + Device->VramLength;

    Data->TmrMc = end - BC250_PSP_TMR_BELOW;
    Data->TmrPhysical = (unsigned long long)Device->VramPhysical.QuadPart + Device->VramLength - BC250_PSP_TMR_BELOW;
    Data->StagingMc = end - BC250_PSP_STAGING_BELOW;
    Data->RingMc = end - BC250_PSP_PAGES_BELOW;
    Data->CommandMc = Data->RingMc + PAGE_SIZE;
    Data->FenceMc = Data->RingMc + 2 * PAGE_SIZE;
}

static int Setup(_Inout_ BC250_PSP* Psp, _In_ struct amdgpu_device* Adev, _In_ const BC250_ESCAPE_PSP* Data, _In_ PUCHAR Pages)
{
    struct bc250_psp_inputs inputs;

    inputs.ring_mem = Pages;                    inputs.ring_mc = Data->RingMc;
    inputs.cmd_buf = Pages + PAGE_SIZE;         inputs.cmd_buf_mc = Data->CommandMc;
    inputs.fence_buf = Pages + 2 * PAGE_SIZE;   inputs.fence_buf_mc = Data->FenceMc;
    inputs.tmr_mc = Data->TmrMc;
    return bc250_psp_setup(Adev, &Psp->Context, &inputs);
}

// Lay the images out the way amdgpu_ucode_init_bo() does. With Staging == NULL nothing is copied (PLAN).
static NTSTATUS LayOut(_In_ const BC250_PSP_FILES* Files, _Inout_ BC250_ESCAPE_PSP* Data, _Out_writes_bytes_opt_(BC250_PSP_STAGING_LENGTH) PUCHAR Staging)
{
    ULONG i, used = 0;

    for (i = 0; i < BC250_FW_COUNT; i++)
    {
        enum bc250_fw_file file = bc250_fw_file_of((enum bc250_fw_id)i);
        enum psp_gfx_fw_type type;
        u32 offset = 0, size = 0;

        if ((ULONG)file >= BC250_FILE_COUNT) return STATUS_INVALID_PARAMETER;
        if (bc250_fw_locate((enum bc250_fw_id)i, Files->Data[file], Files->Size[file], &offset, &size, &type) != 0)
        {
            GuardLog("psp: %s: header of %s not as expected", bc250_fw_name((enum bc250_fw_id)i), bc250_fw_file_name(file));
            return STATUS_INVALID_IMAGE_FORMAT;
        }
        if (size > BC250_PSP_STAGING_LENGTH - used) return STATUS_BUFFER_OVERFLOW;
        if (Staging != NULL)
        {
            RtlCopyMemory(Staging + used, Files->Data[file] + offset, size);
            if (RtlCompareMemory(Staging + used, Files->Data[file] + offset, size) != size) return STATUS_DEVICE_DATA_ERROR;
        }
        Data->Commands[i + 1].CommandId = GFX_CMD_ID_LOAD_IP_FW;
        Data->Commands[i + 1].FirmwareType = (unsigned long)type;
        Data->Commands[i + 1].Size = size;
        Data->Commands[i + 1].McAddress = Data->StagingMc + used;
        used += (size + PAGE_SIZE - 1) & ~(ULONG)(PAGE_SIZE - 1);
    }
    Data->Commands[0].CommandId = GFX_CMD_ID_SETUP_TMR;
    Data->Commands[0].Size = PSP_TMR_SIZE(0);
    Data->Commands[0].McAddress = Data->TmrMc;
    Data->CommandCount = BC250_FW_COUNT + 1;
    Data->StagingUsed = used;
    return STATUS_SUCCESS;
}

static ULONG Microseconds(LARGE_INTEGER From, LARGE_INTEGER Frequency)
{
    LARGE_INTEGER now = KeQueryPerformanceCounter(NULL);

    return (ULONG)(((now.QuadPart - From.QuadPart) * 1000000ll) / Frequency.QuadPart);
}

// DESTROY_TMR and ring stop, amdgpu's psp_hw_fini() order. Both are tried even if the first fails: a ring the PSP
// still knows points at memory that the next owner of this GPU will want to use.
static NTSTATUS Unload(_Inout_ BC250_PSP* Psp, _Out_ long* Result)
{
    int tmr = 0, ring = 0;

    Psp->Loaded = FALSE;
    // DESTROY_TMR travels over the ring: without a ring the PSP knows, there is no way to send it (what is left is a reboot).
    if (Psp->TmrUp && !Psp->RingUp) tmr = -EINVAL;
    else if (Psp->TmrUp) { tmr = bc250_psp_tmr_unload(&Psp->Context); if (tmr == 0) Psp->TmrUp = FALSE; }
    if (Psp->RingUp) { ring = bc250_psp_ring_stop(&Psp->Context); if (ring == 0) Psp->RingUp = FALSE; }
    *Result = (tmr != 0) ? tmr : ring;
    GuardLog("psp: unload: destroy TMR %d, ring stop %d", tmr, ring);
    if (!Psp->TmrUp && !Psp->RingUp && Psp->Pages != NULL)
    {
        MmUnmapIoSpace(Psp->Pages, BC250_PSP_PAGES_LENGTH);
        Psp->Pages = NULL;
        Psp->Context.psp.km_ring.ring_mem = NULL;       // nothing may find the old mapping in the context
        Psp->Context.psp.cmd_buf_mem = NULL;
        Psp->Context.psp.fence_buf = NULL;
    }
    return NT_SUCCESS(Psp->Sequence.Fault) ? ((tmr == 0 && ring == 0) ? STATUS_SUCCESS : STATUS_IO_DEVICE_ERROR) : Psp->Sequence.Fault;
}

void PspEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_PSP* Data)
{
    BC250_PSP_FILES files;
    BC250_PSP* psp;
    struct amdgpu_device* adev = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE;
    PUCHAR staging = NULL;
    PUCHAR planPages = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    LARGE_INTEGER frequency, start;
    ULONG failedFile = 0, i;
    long result = 0;

    Data->Version = BC250_KMD_VERSION;
    Data->Result = 0;
    Data->FaultOffset = 0;
    Data->WriteCount = 0;
    Data->CommandCount = 0;
    Data->CommandsDone = 0;
    Data->StagingUsed = 0;
    Data->State = 0;
    RtlZeroMemory(Data->Commands, sizeof(Data->Commands));
    RtlZeroMemory(Data->Writes, sizeof(Data->Writes));
    RtlZeroMemory(&files, sizeof(files));
    KeQueryPerformanceCounter(&frequency);

    if (Data->Op > BC250_PSP_OP_UNLOAD) status = STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status) && !Device->MmioPspEnabled) status = STATUS_DEVICE_NOT_READY;
    // Files first: file I/O needs PASSIVE_LEVEL, and the lock below raises to APC_LEVEL.
    if (NT_SUCCESS(status) && Data->Op != BC250_PSP_OP_UNLOAD) status = ReadFiles(&files, &failedFile);
    if (!NT_SUCCESS(status))
    {
        Data->Result = (long)failedFile;
        Data->NtStatus = (unsigned long)status;
        Data->Status = BC250_ESCAPE_STATUS_REFUSED;
        return;
    }

    ExAcquireFastMutex(&Device->GartLock);
    psp = (BC250_PSP*)Device->Psp;
    if (psp == NULL) status = STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status)) status = CheckWindow(Device);
    if (NT_SUCCESS(status)) status = GartDevice(Device, &adev, &gartEnabled);       // register bases and the VRAM window
    if (NT_SUCCESS(status))
    {
        FillAddresses(Device, Data);
        previousBackend = adev->backend;
        adev->backend = &psp->Sequence;
        SequenceBegin(&psp->Sequence, Device, Data->Op == BC250_PSP_OP_PLAN, Data->Writes, BC250_PSP_MAX_WRITES);

        switch (Data->Op)
        {
        case BC250_PSP_OP_PLAN:
            // A plan is about a PSP we have not talked to. It also points the context at pool pages that go away with
            // this call, which a later UNLOAD must never find there.
            if (psp->RingUp || psp->TmrUp) { status = STATUS_INVALID_DEVICE_STATE; break; }
            status = LayOut(&files, Data, NULL);
            if (!NT_SUCCESS(status)) break;
            // The ring create reads C2PMSG_64 for real and would write four registers. It does not touch the ring's
            // memory, but the setup wants pointers and clears the fence page: pool pages stand in for the VRAM ones.
            planPages = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, BC250_PSP_PAGES_LENGTH, BC250_PSP_TAG);
            if (planPages == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            if (Setup(psp, adev, Data, planPages) != 0) { status = STATUS_DEVICE_DATA_ERROR; break; }
            result = bc250_psp_ring_create(&psp->Context);
            psp->Context.ring_created = 0;
            break;

        case BC250_PSP_OP_LOAD:
            if (!gartEnabled || psp->RingUp || psp->TmrUp) { status = STATUS_INVALID_DEVICE_STATE; break; }
            staging = MapVram(Device, BC250_PSP_STAGING_BELOW, BC250_PSP_STAGING_LENGTH);
            if (psp->Pages == NULL) psp->Pages = MapVram(Device, BC250_PSP_PAGES_BELOW, BC250_PSP_PAGES_LENGTH);
            if (staging == NULL || psp->Pages == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; break; }
            RtlZeroMemory(psp->Pages, BC250_PSP_PAGES_LENGTH);
            RtlZeroMemory(staging, BC250_PSP_STAGING_LENGTH);
            status = LayOut(&files, Data, staging);
            if (!NT_SUCCESS(status)) break;
            if (Setup(psp, adev, Data, psp->Pages) != 0) { status = STATUS_DEVICE_DATA_ERROR; break; }

            psp->RingUp = TRUE;                 // from the first write on, a ring stop is owed
            result = bc250_psp_ring_create(&psp->Context);
            if (result != 0 || !NT_SUCCESS(psp->Sequence.Fault)) break;
            for (i = 0; i < Data->CommandCount; i++)
            {
                BC250_ESCAPE_PSP_COMMAND* got = &Data->Commands[i];
                struct psp_gfx_resp response;

                RtlZeroMemory(&response, sizeof(response));
                start = KeQueryPerformanceCounter(NULL);
                if (i == 0)
                {
                    psp->TmrUp = TRUE;          // the PSP may know the TMR from here on, even if the command fails
                    got->Result = bc250_psp_tmr_load(&psp->Context);
                }
                else
                {
                    got->Result = bc250_psp_load_ip_fw(&psp->Context, (enum psp_gfx_fw_type)got->FirmwareType, got->McAddress,
                                                       got->Size, &response);
                }
                got->Microseconds = Microseconds(start, frequency);
                got->PspStatus = bc250_psp_last_status(&psp->Context);
                got->TmrAddress = ((unsigned long long)response.fw_addr_hi << 32) | response.fw_addr_lo;
                Data->CommandsDone = i + 1;
                GuardLog("psp: command %u (id %u, type %u, %u bytes): rc %d, status 0x%X, %u us, in TMR at 0x%llX", i + 1,
                         got->CommandId, got->FirmwareType, got->Size, got->Result, got->PspStatus, got->Microseconds, got->TmrAddress);
                if (got->Result != 0) { result = got->Result; break; }
            }
            psp->Loaded = (result == 0 && Data->CommandsDone == Data->CommandCount && NT_SUCCESS(psp->Sequence.Fault));
            break;

        case BC250_PSP_OP_UNLOAD:
            // amdgpu's order: the engines go before the PSP does (gfx.c's FINI first).
            if ((!psp->RingUp && !psp->TmrUp) || GfxIsActive(Device)) { status = STATUS_INVALID_DEVICE_STATE; break; }
            status = Unload(psp, &result);
            break;
        }
        if (NT_SUCCESS(status)) status = psp->Sequence.Fault;
        Data->FaultOffset = psp->Sequence.FaultOffset;
        Data->WriteCount = psp->Sequence.WriteCount;
        Data->State = (psp->RingUp ? BC250_PSP_STATE_RING : 0) | (psp->TmrUp ? BC250_PSP_STATE_TMR : 0) |
                      (gartEnabled ? BC250_PSP_STATE_GART : 0);
        psp->Sequence.Writes = NULL;            // the caller's buffer goes away with this call
        psp->Sequence.MaxWrites = 0;
        adev->backend = previousBackend;
    }
    GuardLog("psp: op %u -> 0x%08X, result %d, %u writes, %u of %u commands", Data->Op, status, result, Data->WriteCount,
             Data->CommandsDone, Data->CommandCount);
    ExReleaseFastMutex(&Device->GartLock);

    if (staging != NULL) MmUnmapIoSpace(staging, BC250_PSP_STAGING_LENGTH);
    if (planPages != NULL) ExFreePoolWithTag(planPages, BC250_PSP_TAG);
    FreeFiles(&files);
    Data->Result = result;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// With GartLock held. For gfx.c: has this driver instance loaded the firmware?
BOOLEAN PspIsLoaded(_In_ const BC250_DEVICE* Device)
{
    const BC250_PSP* psp = (const BC250_PSP*)Device->Psp;

    return psp != NULL && psp->Loaded;
}

// ---- start and stop -------------------------------------------------------------------------------------------------------

NTSTATUS PspStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_PSP* psp;

    Device->Psp = NULL;
    Device->PspStopQuiet = TRUE;
    if (!Device->MmioPspEnabled || !Device->MmioGartEnabled || !Device->VramEnabled || Device->Gart == NULL) return STATUS_SUCCESS;
    psp = (BC250_PSP*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*psp), BC250_PSP_TAG);
    if (psp == NULL) return STATUS_SUCCESS;         // never fails the start
    psp->Sequence.Name = "psp";
    psp->Sequence.Read = MmioPspRead;
    psp->Sequence.Write = MmioPspWrite;
    Device->Psp = psp;
    GuardLog("psp: ready");
    return STATUS_SUCCESS;
}

// Called before GartStop, while the registers are mapped and the GART sequence's device is alive. If the PSP was
// told about our ring or our TMR, it is told to forget them, as amdgpu does when it goes.
void PspStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_PSP* psp;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled;
    long result = 0;

    ExAcquireFastMutex(&Device->GartLock);
    psp = (BC250_PSP*)Device->Psp;
    Device->Psp = NULL;
    if (Device->GfxStopQuiet && psp != NULL && (psp->RingUp || psp->TmrUp) && NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled)))
    {
        void* previousBackend = adev->backend;

        adev->backend = &psp->Sequence;
        SequenceBegin(&psp->Sequence, Device, FALSE, NULL, 0);
        Unload(psp, &result);
        adev->backend = previousBackend;
    }
    ExReleaseFastMutex(&Device->GartLock);
    if (psp == NULL) return;
    Device->PspStopQuiet=!(psp->RingUp || psp->TmrUp);
    if (!Device->PspStopQuiet)
    {
        // Keep firmware state and its CPU views when a consumer could still run,
        // or when PSP teardown itself failed. Do not dismantle GART afterwards.
        Device->GpuStopUnconfirmed=TRUE;
        GuardLog("psp: stop unconfirmed (gfx quiet %u, ring %u, TMR %u), retaining state",
                 Device->GfxStopQuiet,psp->RingUp,psp->TmrUp);
        return;
    }
    if (psp->Pages != NULL) MmUnmapIoSpace(psp->Pages, BC250_PSP_PAGES_LENGTH);
    ExFreePoolWithTag(psp, BC250_PSP_TAG);
}
