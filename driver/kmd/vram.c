// VRAM access for the bring-up (ADR 0007, experiment E08). On this APU "VRAM" is a carve-out of system DRAM
// that the firmware hides from the OS; amdgpu reaches it by system physical address (GCMC_VM_FB_OFFSET << 24)
// and so do we. BAR0 shows the first part of the same memory and is the independent second way in.
//
//   <service key>\Parameters
//     EnableVram       REG_DWORD  1 = identify the carve-out at start and allow VRAM_READ. Needs EnableMmio. Default 0.
//     EnableVramWrite  REG_DWORD  1 = also allow VRAM_WRITE, inside the test window only. Default 0.
//
// Nothing is kept mapped: every access maps one page and unmaps it again. The display path never calls in here.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"

#define PCI_BAR0_OFFSET 0x10
#define BC250_VRAM_MAX_LENGTH (16ull << 30)
// The only place VRAM_WRITE accepts: one page, 240 MB into VRAM. Inside BAR0's 256 MB, so both paths see it,
// and far above the firmware's framebuffer at the bottom of VRAM (checked again at run time).
#define BC250_VRAM_TEST_OFFSET 0x0F000000ull
#define BC250_VRAM_TEST_LENGTH 0x1000ull
// Reads are accepted where BAR0 can confirm them, and in the top 2 MB where amdgpu keeps its GART table on
// this unit (E03) and where M4 will put ours. The rest stays untouched until somebody needs it.
#define BC250_VRAM_TOP_WINDOW 0x200000ull

static BOOLEAN Overlaps(ULONGLONG StartA, ULONGLONG LengthA, ULONGLONG StartB, ULONGLONG LengthB)
{
    return StartA < StartB + LengthB && StartB < StartA + LengthA;
}

// BAR0 by PCI config space, confirmed by a memory resource at that address in PnP's translated list.
static NTSTATUS FindBar0(_In_ BC250_DEVICE* Device, _Out_ PHYSICAL_ADDRESS* Start, _Out_ ULONGLONG* Length)
{
    ULONG bar[2] = { 0, 0 }, read = 0;
    PCM_RESOURCE_LIST list = Device->DeviceInfo.TranslatedResourceList;
    ULONGLONG address;
    NTSTATUS status;
    ULONG i, j;

    Start->QuadPart = 0;
    *Length = 0;
    status = Device->Dxgk.DxgkCbReadDeviceSpace(Device->Dxgk.DeviceHandle, DXGK_WHICHSPACE_CONFIG, bar, PCI_BAR0_OFFSET,
                                                sizeof(bar), &read);
    if (!NT_SUCCESS(status) || read != sizeof(bar)) return NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status;
    if ((bar[0] & 1) != 0 || (bar[0] & 6) != 4) return STATUS_DEVICE_CONFIGURATION_ERROR;      // not a 64-bit memory BAR
    address = ((ULONGLONG)bar[1] << 32) | (bar[0] & ~0xFul);
    if (address == 0 || list == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (i = 0; i < list->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_LIST partial = &list->List[i].PartialResourceList;
        for (j = 0; j < partial->Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &partial->PartialDescriptors[j];
            if (d->Type == CmResourceTypeMemory && (ULONGLONG)d->u.Memory.Start.QuadPart == address &&
                d->u.Memory.Length >= PAGE_SIZE && (d->u.Memory.Length & (PAGE_SIZE - 1)) == 0)
            {
                *Start = d->u.Memory.Start;
                *Length = d->u.Memory.Length;
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

// A wrong carve-out address would make VRAM_WRITE a write into somebody's RAM. The OS knows what it owns.
static BOOLEAN IsOutsideOsMemory(ULONGLONG Start, ULONGLONG Length)
{
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    BOOLEAN outside = TRUE;
    ULONG i;

    if (ranges == NULL) return FALSE;
    for (i = 0; ranges[i].BaseAddress.QuadPart != 0 || ranges[i].NumberOfBytes.QuadPart != 0; i++)
    {
        if (Overlaps(Start, Length, (ULONGLONG)ranges[i].BaseAddress.QuadPart, (ULONGLONG)ranges[i].NumberOfBytes.QuadPart))
            outside = FALSE;
    }
    ExFreePool(ranges);
    return outside;
}

NTSTATUS VramStart(_Inout_ BC250_DEVICE* Device)
{
    ULONG offset = 0, base = 0, top = 0;
    ULONGLONG length;
    NTSTATUS status;

    Device->VramEnabled = FALSE;
    Device->VramWriteEnabled = FALSE;
    Device->VramPhysical.QuadPart = 0;
    Device->VramLength = 0;
    Device->VramMcBase = 0;
    Device->Bar0Physical.QuadPart = 0;
    Device->Bar0Length = 0;
    if (GuardReadSetting(L"EnableVram", 0) != 1 || Device->Mmio == NULL) return STATUS_SUCCESS;

    status = MmioRead(Device, BC250_REG_GC_GCMC_VM_FB_OFFSET, &offset);
    if (NT_SUCCESS(status)) status = MmioRead(Device, BC250_REG_GC_GCMC_VM_FB_LOCATION_BASE, &base);
    if (NT_SUCCESS(status)) status = MmioRead(Device, BC250_REG_GC_GCMC_VM_FB_LOCATION_TOP, &top);
    if (!NT_SUCCESS(status))
    {
        GuardLog("vram: location registers not readable (0x%08X)", status);
        return STATUS_SUCCESS;
    }
    // Same arithmetic as gfxhub_v2_0_get_fb_location / get_mc_fb_offset and gmc_v10_0_mc_init: units of 16 MB.
    base &= 0x00FFFFFF;
    top &= 0x00FFFFFF;
    length = top >= base ? ((ULONGLONG)(top - base) + 1) << 24 : 0;
    if (offset == 0 || length == 0 || length > BC250_VRAM_MAX_LENGTH)
    {
        GuardLog("vram: implausible location: offset 0x%X base 0x%X top 0x%X", offset, base, top);
        return STATUS_SUCCESS;
    }
    if (!IsOutsideOsMemory((ULONGLONG)offset << 24, length))
    {
        GuardLog("vram: 0x%llX + 0x%llX overlaps memory the OS owns, staying away", (ULONGLONG)offset << 24, length);
        return STATUS_SUCCESS;
    }
    status = FindBar0(Device, &Device->Bar0Physical, &Device->Bar0Length);
    if (!NT_SUCCESS(status)) GuardLog("vram: BAR0 not identified (0x%08X), physical path only", status);

    Device->VramPhysical.QuadPart = (LONGLONG)((ULONGLONG)offset << 24);
    Device->VramLength = length;
    Device->VramMcBase = (ULONGLONG)base << 24;
    Device->VramEnabled = TRUE;
    Device->VramWriteEnabled = (GuardReadSetting(L"EnableVramWrite", 0) == 1);
    GuardLog("vram: 0x%llX + 0x%llX, MC 0x%llX, BAR0 0x%llX + 0x%llX, writes %s", Device->VramPhysical.QuadPart, length,
             Device->VramMcBase, Device->Bar0Physical.QuadPart, Device->Bar0Length, Device->VramWriteEnabled ? "allowed" : "off");
    return STATUS_SUCCESS;
}

void VramStop(_Inout_ BC250_DEVICE* Device)
{
    Device->VramEnabled = FALSE;
    Device->VramWriteEnabled = FALSE;
}

// Where in VRAM the firmware's framebuffer is, if its address is inside one of the two views we know.
static BOOLEAN FramebufferOffset(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset)
{
    ULONGLONG fb = (ULONGLONG)Device->Post.PhysicAddress.QuadPart;
    ULONGLONG vram = (ULONGLONG)Device->VramPhysical.QuadPart, bar = (ULONGLONG)Device->Bar0Physical.QuadPart;

    *Offset = 0;
    if (fb >= vram && fb < vram + Device->VramLength) { *Offset = fb - vram; return TRUE; }
    if (Device->Bar0Length != 0 && fb >= bar && fb < bar + Device->Bar0Length) { *Offset = fb - bar; return TRUE; }
    return FALSE;
}

static NTSTATUS Access(_In_ const BC250_DEVICE* Device, ULONG Path, ULONGLONG Offset, BOOLEAN Write, _Inout_ ULONG* Value)
{
    PHYSICAL_ADDRESS page;
    ULONGLONG fbOffset;
    ULONG index;
    volatile ULONG* map;

    if (!Device->VramEnabled || (Write && !Device->VramWriteEnabled)) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= Device->VramLength) return STATUS_INVALID_PARAMETER;
    if (Path == BC250_VRAM_PATH_BAR0)
    {
        if (Offset >= Device->Bar0Length) return STATUS_INVALID_PARAMETER;
        page.QuadPart = Device->Bar0Physical.QuadPart + (LONGLONG)Offset;
    }
    else if (Path == BC250_VRAM_PATH_PHYSICAL)
    {
        page.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)Offset;
    }
    else return STATUS_INVALID_PARAMETER;

    if (Write)
    {
        if (Offset < BC250_VRAM_TEST_OFFSET || Offset >= BC250_VRAM_TEST_OFFSET + BC250_VRAM_TEST_LENGTH) return STATUS_ACCESS_DENIED;
        if (!FramebufferOffset(Device, &fbOffset) ||
            Overlaps(fbOffset, Device->FramebufferLength, BC250_VRAM_TEST_OFFSET, BC250_VRAM_TEST_LENGTH)) return STATUS_ACCESS_DENIED;
    }
    else if (Offset >= Device->Bar0Length && Offset < Device->VramLength - BC250_VRAM_TOP_WINDOW)
    {
        return STATUS_ACCESS_DENIED;
    }

    index = (ULONG)((ULONGLONG)page.QuadPart & (PAGE_SIZE - 1)) / 4;     // from the address, not from the VRAM offset
    page.QuadPart &= ~(LONGLONG)(PAGE_SIZE - 1);
    map = (volatile ULONG*)MmMapIoSpaceEx(page, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (map == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (Write) WRITE_REGISTER_ULONG((PULONG)&map[index], *Value);
    *Value = READ_REGISTER_ULONG((PULONG)&map[index]);
    MmUnmapIoSpace((PVOID)map, PAGE_SIZE);
    return STATUS_SUCCESS;
}

// The caller (display.c) has checked the size, the magic and that the caller is an administrator.
void VramEscape(_In_ const BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_MEMORY* Data)
{
    NTSTATUS status = STATUS_SUCCESS;

    Data->Version = BC250_KMD_VERSION;
    Data->Flags = (Device->Mmio != NULL ? BC250_ESCAPE_FLAG_MMIO_MAPPED : 0) | (Device->MmioWriteEnabled ? BC250_ESCAPE_FLAG_MMIO_WRITE : 0) |
                  (Device->VramEnabled ? BC250_ESCAPE_FLAG_VRAM : 0) | (Device->VramWriteEnabled ? BC250_ESCAPE_FLAG_VRAM_WRITE : 0);
    Data->FramebufferPhysical = (unsigned long long)Device->Post.PhysicAddress.QuadPart;
    Data->FramebufferLength = Device->FramebufferLength;
    Data->Bar0Physical = (unsigned long long)Device->Bar0Physical.QuadPart;
    Data->Bar0Length = Device->Bar0Length;
    Data->VramPhysical = (unsigned long long)Device->VramPhysical.QuadPart;
    Data->VramLength = Device->VramLength;
    Data->VramMcBase = Device->VramMcBase;
    Data->TestOffset = BC250_VRAM_TEST_OFFSET;
    Data->TestLength = BC250_VRAM_TEST_LENGTH;

    if (Data->Command != BC250_ESCAPE_GET_MEMORY)
    {
        ULONG value = Data->Value;
        BOOLEAN write = (Data->Command == BC250_ESCAPE_VRAM_WRITE);

        status = Access(Device, Data->Path, Data->Offset, write, &value);
        if (write) GuardLog("escape: vram write path %u offset 0x%llX = 0x%08X -> 0x%08X", Data->Path, Data->Offset, Data->Value, status);
        Data->Value = NT_SUCCESS(status) ? value : 0;
    }
    Data->NtStatus = (unsigned long)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
