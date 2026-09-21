// Register access (ADR 0007): BAR5 of the GPU, mapped only when the registry gate is open, every access
// checked against a generated table. Nothing in the display path calls in here: the miniport still runs its
// display without MMIO, and a closed gate gives back exactly the M3 driver.
//
//   <service key>\Parameters
//     EnableMmio       REG_DWORD  1 = map BAR5 at start and allow READ_REG escapes. Default 0.
//     EnableMmioWrite  REG_DWORD  1 = also allow WRITE_REG escapes to g_MmioWriteAllow. Default 0.
//     EnableGart       REG_DWORD  1 = also allow the GART command of gart.c its own table, g_MmioGartAllow,
//                                 which no escape can reach register by register. Default 0.
//     EnablePsp        REG_DWORD  1 = also allow the PSP command of psp.c its own table, g_MmioPspAllow. Needs
//                                 EnableGart. Default 0.
//     EnableGfx        REG_DWORD  1 = also allow the GFX command of gfx.c its own table, g_MmioGfxAllow. Needs
//                                 EnablePsp. Default 0.
//
// Both are read once per start. A read of a wrong BAR5 address can hang this SoC (facts M16) and some reads
// change state (facts M25), hence a table for reads as well.
#include "bc250kmd.h"
#define BC250_REGS_WITH_TABLES
#include "regs.generated.h"

#define BC250_BAR5_INDEX 5
#define BC250_BAR5_LENGTH 0x80000ul
#define PCI_BAR0_OFFSET 0x10

static BOOLEAN InTable(_In_reads_(Count) const unsigned long* Table, ULONG Count, ULONG Offset)
{
    ULONG low = 0, high = Count;

    while (low < high)
    {
        ULONG middle = low + (high - low) / 2;
        if (Table[middle] == Offset) return TRUE;
        if (Table[middle] < Offset) low = middle + 1; else high = middle;
    }
    return FALSE;
}

// The register BAR is identified twice, independently: by what PCI config space says BAR5 holds, and by a
// memory resource of the expected length at that very address in the list PnP translated for us. If the two
// do not agree, nothing is mapped.
static NTSTATUS FindRegisterBar(_In_ BC250_DEVICE* Device, _Out_ PHYSICAL_ADDRESS* Start)
{
    ULONG bar = 0, read = 0;
    PCM_RESOURCE_LIST list = Device->DeviceInfo.TranslatedResourceList;
    NTSTATUS status;
    ULONG i, j;

    Start->QuadPart = 0;
    status = Device->Dxgk.DxgkCbReadDeviceSpace(Device->Dxgk.DeviceHandle, DXGK_WHICHSPACE_CONFIG, &bar,
                                                PCI_BAR0_OFFSET + 4 * BC250_BAR5_INDEX, sizeof(bar), &read);
    if (!NT_SUCCESS(status) || read != sizeof(bar)) return NT_SUCCESS(status) ? STATUS_DEVICE_DATA_ERROR : status;
    if ((bar & 1) != 0 || (bar & ~0xFul) == 0) return STATUS_DEVICE_CONFIGURATION_ERROR;   // I/O space or empty
    if (list == NULL) return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (i = 0; i < list->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_LIST partial = &list->List[i].PartialResourceList;     // one full descriptor on PCI
        for (j = 0; j < partial->Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &partial->PartialDescriptors[j];
            if (d->Type == CmResourceTypeMemory && d->u.Memory.Length == BC250_BAR5_LENGTH &&
                d->u.Memory.Start.HighPart == 0 && d->u.Memory.Start.LowPart == (bar & ~0xFul))
            {
                *Start = d->u.Memory.Start;
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS MmioStart(_Inout_ BC250_DEVICE* Device)
{
    PHYSICAL_ADDRESS start;
    NTSTATUS status;

    Device->Mmio = NULL;
    Device->MmioWriteEnabled = FALSE;
    Device->MmioGartEnabled = FALSE;
    Device->MmioPspEnabled = FALSE;
    Device->MmioGfxEnabled = FALSE;
    if (GuardReadSetting(L"EnableMmio", 0) != 1) return STATUS_SUCCESS;        // the gate is closed: M3 behaviour

    status = FindRegisterBar(Device, &start);
    if (!NT_SUCCESS(status))
    {
        GuardLog("mmio: register BAR not identified (0x%08X), staying without MMIO", status);
        return STATUS_SUCCESS;          // the display does not depend on it
    }
    Device->Mmio = (volatile ULONG*)MmMapIoSpaceEx(start, BC250_BAR5_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    if (Device->Mmio == NULL)
    {
        GuardLog("mmio: mapping failed");
        return STATUS_SUCCESS;
    }
    Device->MmioPhysical = start;
    Device->MmioWriteEnabled = (GuardReadSetting(L"EnableMmioWrite", 0) == 1);
    Device->MmioGartEnabled = (GuardReadSetting(L"EnableGart", 0) == 1);
    Device->MmioPspEnabled = Device->MmioGartEnabled && (GuardReadSetting(L"EnablePsp", 0) == 1);
    Device->MmioGfxEnabled = Device->MmioPspEnabled && (GuardReadSetting(L"EnableGfx", 0) == 1);
    GuardLog("mmio: BAR5 at 0x%08X mapped, writes %s", start.LowPart, Device->MmioWriteEnabled ? "allowed" : "off");
    return STATUS_SUCCESS;
}

void MmioStop(_Inout_ BC250_DEVICE* Device)
{
    if (Device->Mmio != NULL) MmUnmapIoSpace((PVOID)Device->Mmio, BC250_BAR5_LENGTH);
    Device->Mmio = NULL;
    Device->MmioWriteEnabled = FALSE;
    Device->MmioGartEnabled = FALSE;
    Device->MmioPspEnabled = FALSE;
    Device->MmioGfxEnabled = FALSE;
}

NTSTATUS MmioRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    *Value = 0;
    if (Device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioReadAllow, BC250_MMIO_READ_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;        // the length check does not rely on what the generator put into the table
    *Value = READ_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

NTSTATUS MmioWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value)
{
    if (Device->Mmio == NULL || !Device->MmioWriteEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioWriteAllow, BC250_MMIO_WRITE_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4], Value);
    return STATUS_SUCCESS;
}

// The GART command's registers (gart.c). Reads: its own table, which holds two registers that are on no other
// list because reading them is part of a protocol (the MMHUB invalidation semaphore, facts M25), or the general
// read table. Writes: its own table only.
NTSTATUS MmioGartRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    *Value = 0;
    if (Device->Mmio == NULL || !Device->MmioGartEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH ||
        (!InTable(g_MmioGartAllow, BC250_MMIO_GART_ALLOW_COUNT, Offset) && !InTable(g_MmioReadAllow, BC250_MMIO_READ_ALLOW_COUNT, Offset)))
        return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

NTSTATUS MmioGartWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value)
{
    if (Device->Mmio == NULL || !Device->MmioGartEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioGartAllow, BC250_MMIO_GART_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4], Value);
    return STATUS_SUCCESS;
}

// The PSP command's registers (psp.c): the five mailbox registers amdgpu used for the ring. None has a read
// side effect (they are on the general read list as well); writes: this table only.
NTSTATUS MmioPspRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    *Value = 0;
    if (Device->Mmio == NULL || !Device->MmioPspEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioPspAllow, BC250_MMIO_PSP_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

NTSTATUS MmioPspWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value)
{
    if (Device->Mmio == NULL || !Device->MmioPspEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioPspAllow, BC250_MMIO_PSP_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4], Value);
    return STATUS_SUCCESS;
}

// The GFX command's registers (gfx.c): what amdgpu read or wrote on unit A in gfx_v10_0_hw_init(), sdma_v5_0_hw_init()
// and the doorbell aperture enable. Reads and writes: this table only. It holds registers that are on no other list
// because reading them has an effect (GRBM_GFX_CNTL latches GRBM_READ_ERROR, facts M25): here amdgpu's own sequence
// reads them, at the point where amdgpu read them.
NTSTATUS MmioGfxRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    *Value = 0;
    if (Device->Mmio == NULL || !Device->MmioGfxEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioGfxAllow, BC250_MMIO_GFX_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

NTSTATUS MmioGfxWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value)
{
    if (Device->Mmio == NULL || !Device->MmioGfxEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioGfxAllow, BC250_MMIO_GFX_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4], Value);
    return STATUS_SUCCESS;
}

// For gart.c's snapshot: the table itself.
ULONG MmioGartTable(_Outptr_ const unsigned long** Table)
{
    *Table = g_MmioGartAllow;
    return BC250_MMIO_GART_ALLOW_COUNT;
}
