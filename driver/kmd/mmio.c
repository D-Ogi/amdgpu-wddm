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
//     EnableIh         REG_DWORD  1 = also allow the IH command of ih.c its own table, g_MmioIhAllow. Needs
//                                 EnableGfx (the ring is GTT memory of gpumem.c). Default 0.
//     EnableVidPnFlip  REG_DWORD  1 = SetVidPnSourceAddress performs the DCN flip and ControlInterrupt arms the
//                                 hardware vsync (dcn.c, ADR 0011 point 3 step 3), instead of 0.7.23's software
//                                 path. Needs EnableDcnWrite (and so, transitively, EnableMmio) - the same chain
//                                 shape as EnableIh needing EnableGfx above. Meaningful only under
//                                 EnableFullWddm: the display-only DDI table never reaches either DDI. Default 0.
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
    Device->MmioIhEnabled = FALSE;
    Device->DcnWriteEnabled = FALSE;
    Device->VidPnFlipEnabled = FALSE;
    // dcn.c's flip state (0.7.20): reset here, at the top of every start, like every other gate above - not in
    // DcnStop, because there is none; the DCN dump has never had a Start/Stop of its own (ADR 0011 point 3) and
    // the write side does not get one either, MmioStart already being the one place every device start passes.
    Device->DcnSurfaceSequence = 0;
    Device->DcnFirmwarePitch = Device->DcnCurrentPitch = 0;
    Device->DcnFirmwareKnown = FALSE;
    Device->DcnFirmwareAddress = 0;
    Device->DcnCurrentAddress = 0;
    Device->DcnDiverged = FALSE;
    // 0.7.24 (ADR 0011 point 3 step 3): the hardware vsync's own state, same reset rule.
    Device->DcnVsyncArmed = 0;
    Device->DcnVsyncAcked = 0;
    Device->DcnVsyncTicks = 0;
    Device->DcnVsyncRefused = 0;
    Device->DcnVsyncDeferred = 0;
    Device->DcnFlipsHardware = 0;
    Device->DcnFlipRefused = 0;
    // 2026-09-22 (ADR 0011 consequences): the present path's own destination mapping, same reset rule and the
    // same trust as Device->Mmio just above - the stop path (WddmStop/DcnStop, dcn.c's DcnUnmapScanout) already
    // unmapped it; this only documents that a fresh start never inherits a stale pointer from the last one.
    Device->DcnScanoutMap = NULL;
    Device->DcnScanoutMapAddress = 0;
    Device->DcnScanoutMapLength = 0;
    Device->DcnScanoutRemaps = 0;
    Device->DcnScanoutMapFailed = 0;
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
    Device->MmioIhEnabled = Device->MmioGfxEnabled && (GuardReadSetting(L"EnableIh", 0) == 1);
    // Independent of the Gart/Psp/Gfx/Ih chain above (DMU is not behind any of them): EnableMmio and
    // EnableDcnWrite together, exactly as ADR 0011 point 3 step 2 asks. Read only once mapping succeeded, so
    // this is already "EnableMmio == 1 && EnableDcnWrite == 1" without saying so twice.
    Device->DcnWriteEnabled = (GuardReadSetting(L"EnableDcnWrite", 0) == 1);
    // 0.7.24 (ADR 0011 point 3 step 3): a fourth link in the same chain - EnableVidPnFlip on top of
    // Device->DcnWriteEnabled, which already means EnableMmio && EnableDcnWrite. wddm.c and dcn.c read this one
    // field; neither reads the registry itself.
    Device->VidPnFlipEnabled = Device->DcnWriteEnabled && (GuardReadSetting(L"EnableVidPnFlip", 0) == 1);
    GuardLog("mmio: BAR5 at 0x%08X mapped, writes %s, dcn writes %s, vidpn flip %s", start.LowPart, Device->MmioWriteEnabled ? "allowed" : "off",
             Device->DcnWriteEnabled ? "allowed" : "off", Device->VidPnFlipEnabled ? "allowed" : "off");
    return STATUS_SUCCESS;
}

void MmioStop(_Inout_ BC250_DEVICE* Device)
{
    SmuOwnerStop(&Device->Smu); // close/join telemetry before the mapping disappears
    if (Device->Mmio != NULL) MmUnmapIoSpace((PVOID)Device->Mmio, BC250_BAR5_LENGTH);
    Device->Mmio = NULL;
    Device->MmioWriteEnabled = FALSE;
    Device->MmioGartEnabled = FALSE;
    Device->MmioPspEnabled = FALSE;
    Device->MmioGfxEnabled = FALSE;
    Device->MmioIhEnabled = FALSE;
    Device->DcnWriteEnabled = FALSE;
    Device->VidPnFlipEnabled = FALSE;
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

// The IH command's registers (ih.c): what amdgpu read or wrote on unit A in navi10_ih_irq_init(). The DPC's three
// registers are in this table too; it reaches them through these functions, which take no lock and touch nothing
// but the mapping.
NTSTATUS MmioIhRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    *Value = 0;
    if (Device->Mmio == NULL || !Device->MmioIhEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioIhAllow, BC250_MMIO_IH_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

NTSTATUS MmioIhWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value)
{
    if (Device->Mmio == NULL || !Device->MmioIhEnabled) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioIhAllow, BC250_MMIO_IH_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4], Value);
    return STATUS_SUCCESS;
}

// The DCN dump's registers (dcn.c, ADR 0011 point 3): read-only, and on its own table (g_MmioDcnAllow) rather
// than g_MmioReadAllow, because these are DMU registers and the general read list is GC/MMHUB/OSSSYS/HDP only
// (third_party/linux-amdgpu/PROVENANCE.md). No gate of its own: whatever EnableMmio already decided about BAR5
// is all this needs, the same condition READ_REG answers to.
NTSTATUS MmioDcnRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    *Value = 0;
    if (Device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioDcnAllow, BC250_MMIO_DCN_ALLOW_COUNT, Offset))
        return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

// For dcn.c's dump: the same 75 registers, named, in gen_regs.py's DCN_REGISTERS order. Kept private to this
// translation unit like g_MmioGartAllow (MmioGartTable's table), handed out through a function instead of a
// second copy of BC250_REGS_WITH_TABLES's tables in dcn.c's own translation unit.
ULONG MmioDcnTable(_Outptr_ const BC250_DCN_REG_INFO** Table)
{
    *Table = g_DcnRegisters;
    return BC250_DCN_REG_INFO_COUNT;
}

// 0.7.20 (ADR 0011 point 3 step 2): DcnFlip's write, on its own six-register table (g_MmioDcnWriteAllow), gated
// by Device->DcnWriteEnabled - EnableMmio and EnableDcnWrite together (mmio.c's MmioStart), never EnableMmioWrite:
// the general WRITE_REG allow list is GC/MMHUB/OSSSYS/HDP only (third_party/linux-amdgpu/PROVENANCE.md) and does
// not carry DMU. Every write is logged here, the one place all of them pass through, the same way VramEscape logs
// every VRAM write at its own single call site - Quiet is the one exception (review 16's DIRQL item): the
// VUPDATE_NO_LOCK ack (dcn.c's DcnVsyncInterrupt) reaches here once every vblank, continuously, for as long as
// the hardware vsync source stays armed, which the escape's own occasional writes never did. GuardLog itself
// checks IRQL before it takes its ring's spin lock (guard.c) so calling it from here was never a lock-at-DIRQL
// bug, but DbgPrintEx runs unconditionally before that check, and a DbgPrint on every vblank forever is exactly
// the kind of unbounded ISR-path cost this driver's own hardware-safety rules (bc250-win/CLAUDE.md) ask to avoid
// - worse yet under a kernel debugger, where DbgPrint can block on the transport. Quiet skips the log line and
// nothing else; the validation and the write themselves are identical either way.
NTSTATUS MmioDcnWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value)
{
    return MmioDcnWriteEx(Device, Offset, Value, FALSE);
}

// SetVidPnSourceAddress's own flip sequence (dcn.c's DcnFlipWriteSequence, called from DcnFlipSourceAddress) is
// the other hot path added by 0.7.24 (ADR 0011 point 3 step 3): up to six writes per real present, which
// DcnFlipSourceAddress's own GuardLog already summarizes and caps (BC250_DCN_LOG_CALLS). Both hot paths call
// this instead of MmioDcnWrite; the escape (DcnFlipCore, occasional, human-driven) still calls MmioDcnWrite and
// keeps its per-write log, unchanged from 0.7.20.
NTSTATUS MmioDcnWriteEx(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value, BOOLEAN Quiet)
{
    NTSTATUS status;

    if (Device->Mmio == NULL || !Device->DcnWriteEnabled) status = STATUS_DEVICE_NOT_READY;
    else if ((Offset & 3) != 0 || Offset >= BC250_BAR5_LENGTH || !InTable(g_MmioDcnWriteAllow, BC250_MMIO_DCN_WRITE_ALLOW_COUNT, Offset))
        status = STATUS_ACCESS_DENIED;
    else
    {
        WRITE_REGISTER_ULONG((PULONG)&Device->Mmio[Offset / 4], Value);
        status = STATUS_SUCCESS;
    }
    if (!Quiet) GuardLog("dcnflip: write 0x%05X = 0x%08X -> 0x%08X", Offset, Value, status);
    return status;
}
