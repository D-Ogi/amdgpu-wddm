// DCN 2.0.1 ("DMU") register dump (ADR 0011 point 3): a READ-ONLY escape that proves the offsets and the BAR5
// mapping are right under Windows before ADR 0011's first write. HUBPREQ0..3, HUBP0..3, OTG0..1 and
// DCHUBBUB_CTRL_STATUS - the registers a flip will read (and, later, write) before it ever moves the scanout.
//
// No gate of its own, no Start/Stop, no sequence, no Device state: MmioDcnRead answers as soon as BAR5 is mapped
// (EnableMmio), the same condition every other read-only escape here already needs, and refuses gracefully
// (STATUS_DEVICE_NOT_READY) when it is not. The registers themselves are on their own generated table
// (driver/kmd/gen_regs.py's DCN_REGISTERS, tools/regcalc, ip DMU) - never amdgpu's trace, because there is no
// Windows trace of this IP yet: proving these offsets from Windows is what this command is for.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"       // field masks for the decoded summary only; every offset comes from regcalc
#include <ntstrsafe.h>

void DcnEscape(_In_ const BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DCN* Data)
{
    const BC250_DCN_REG_INFO* table = NULL;
    ULONG count = MmioDcnTable(&table);
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG hubp0Lo = 0, hubp0Hi = 0, pitch = 0, otgControl = 0, syncStatus = 0;

    // Ties gen_regs.py's count (regs.generated.h) to the escape struct's fixed array (bc250kmd_escape.h): the two
    // are edited in different places and can only be kept equal by a check like every other one in this driver.
    C_ASSERT(BC250_DCN_REG_COUNT == BC250_DCN_REG_INFO_COUNT);

    Data->Version = BC250_KMD_VERSION;
    Data->RegCount = 0;
    Data->FaultOffset = 0;
    Data->Hubp0Address = 0;
    Data->Hubp0Pitch = 0;
    Data->Hubp0Cntl = 0;
    Data->Otg0Control = 0;
    Data->Otg0MasterEnable = 0;
    Data->Otg0HTotal = 0;
    Data->Otg0VTotal = 0;
    Data->Otg0VblankIntEnabled = 0;
    RtlZeroMemory(Data->Regs, sizeof(Data->Regs));

    if (Device->Mmio == NULL)
    {
        GuardLog("dcn: BAR5 not mapped (EnableMmio is closed)");
        Data->NtStatus = (unsigned long)STATUS_DEVICE_NOT_READY;
        Data->Status = BC250_ESCAPE_STATUS_REFUSED;
        return;
    }

    for (i = 0; i < count && i < BC250_DCN_REG_COUNT; i++)
    {
        ULONG value = 0;
        NTSTATUS regStatus = MmioDcnRead(Device, table[i].Offset, &value);

        if (!NT_SUCCESS(regStatus))
        {
            // Every offset in table[] came out of the same generated list MmioDcnRead checks against, so this is
            // not expected; if it ever happens it means the two disagreed, which is worth knowing rather than a
            // silently short dump.
            if (NT_SUCCESS(status)) { status = regStatus; Data->FaultOffset = table[i].Offset; }
            value = 0;
        }
        RtlStringCbCopyA(Data->Regs[i].Name, sizeof(Data->Regs[i].Name), table[i].Name);
        Data->Regs[i].Offset = table[i].Offset;
        Data->Regs[i].Value = value;
        Data->RegCount++;
    }

    // The decoded summary (evidence/linux/2026-09-22-E21-linux-reference-4/dmupre.txt is the Linux reference to
    // compare against): HUBP0 and OTG0 only, the pipe and timing generator the firmware is already scanning out
    // on. Read again by name rather than searched out of Regs[] above - eight more reads of registers the loop
    // just proved safe.
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, &hubp0Lo);
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, &hubp0Hi);
    Data->Hubp0Address = ((ULONGLONG)hubp0Hi << 32) | hubp0Lo;
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH, &pitch);
    Data->Hubp0Pitch = pitch & HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK;
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &Data->Hubp0Cntl);
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_CONTROL, &otgControl);
    Data->Otg0Control = otgControl;
    Data->Otg0MasterEnable = (otgControl & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) ? 1 : 0;
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_H_TOTAL, &Data->Otg0HTotal);
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_V_TOTAL, &Data->Otg0VTotal);
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS, &syncStatus);
    // Bit 12: AMD's own name for it in dcn_2_0_1_sh_mask.h is VUPDATE_NO_LOCK_INT_EN, not "vblank" - the field
    // name here is this escape's own, to compare against the Linux reference's timing, not a claim about AMD's.
    Data->Otg0VblankIntEnabled = (syncStatus & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK) ? 1 : 0;

    GuardLog("dcn: %u registers, status 0x%08X; hubp0 addr 0x%llX pitch %u cntl 0x%08X; otg0 control 0x%08X h_total %u v_total %u",
             Data->RegCount, status, Data->Hubp0Address, Data->Hubp0Pitch, Data->Hubp0Cntl, Data->Otg0Control,
             Data->Otg0HTotal, Data->Otg0VTotal);
    Data->NtStatus = (unsigned long)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// ---- FbdumpEscape (BC250_ESCAPE_RUN_FBDUMP, 2026-09-22): a read-only band of the scanned-out surface -----------
//
// The owner's practical-debugging request: a screenshot that works under the full WDDM table too, where
// mon.py's GDI capture reads the CDD's surfaces and shows black (facts M84). PASSIVE_LEVEL only (MmMapIoSpaceEx).
// No register write; needs EnableMmio only, the same condition DcnEscape above already answers to.

// The band's physical range has to land inside the VRAM carve-out (EnableVram, Device->VramPhysical/VramLength -
// the same range dcn.c's own AddressAllowed and wddm.c's Blt validate against) or inside the firmware's own
// framebuffer, which display.c maps at every device start regardless of any gate (Device->Post.PhysicAddress /
// FramebufferLength) - so a fresh boot that has never taken EnableVram can still be dumped before anything else
// is enabled, the same way DcnFlip's firmware address is always allowed.
static BOOLEAN FbdumpRangeAllowed(_In_ const BC250_DEVICE* Device, ULONGLONG Start, ULONGLONG Length)
{
    ULONGLONG fwBase = (ULONGLONG)Device->Post.PhysicAddress.QuadPart;
    ULONGLONG fwTop = fwBase + Device->FramebufferLength;
    ULONGLONG vramBase, vramTop;

    if (Start >= fwBase && Start < fwTop && Length <= fwTop - Start) return TRUE;
    if (!Device->VramEnabled) return FALSE;
    vramBase = (ULONGLONG)Device->VramPhysical.QuadPart;
    vramTop = vramBase + Device->VramLength;
    if (Start < vramBase || Start >= vramTop) return FALSE;
    return Length <= vramTop - Start;
}

static void FbdumpRefuse(_Inout_ BC250_ESCAPE_FBDUMP* Out, NTSTATUS Status, _In_z_ const char* Reason)
{
    Out->NtStatus = (unsigned long)Status;
    Out->Status = BC250_ESCAPE_STATUS_REFUSED;
    RtlStringCbCopyA(Out->Reason, sizeof(Out->Reason), Reason);
    GuardLog("fbdump: refused: %s (0x%08X)", Reason, (ULONG)Status);
}

void FbdumpEscape(_In_ const BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_FBDUMP* Data)
{
    ULONG lo = 0, hi = 0, pitchRaw = 0, pitch, height, row;
    ULONGLONG address, start, length;
    PHYSICAL_ADDRESS phys;
    volatile ULONG* map;
    NTSTATUS status;

    Data->Version = BC250_KMD_VERSION;
    Data->NtStatus = 0;
    Data->Width = 0; Data->Height = 0; Data->Pitch = 0; Data->ColorFormat = 0;
    Data->Address = 0;
    Data->Reason[0] = '\0';
    RtlZeroMemory(Data->Pixels, sizeof(Data->Pixels));

    if (Device->Mmio == NULL)
    {
        FbdumpRefuse(Data, STATUS_DEVICE_NOT_READY, "BAR5 not mapped (EnableMmio is closed)");
        return;
    }
    if (Data->Hubp != 0)
    {
        FbdumpRefuse(Data, STATUS_NOT_SUPPORTED, "only HUBP 0 is read today");
        return;
    }
    if (Data->RowCount == 0 || Data->RowCount > BC250_FBDUMP_MAX_ROWS)
    {
        FbdumpRefuse(Data, STATUS_INVALID_PARAMETER, "RowCount must be 1..BC250_FBDUMP_MAX_ROWS");
        return;
    }
    height = (ULONG)Device->Post.Height;
    if (Data->FirstRow >= height || Data->RowCount > height - Data->FirstRow)
    {
        FbdumpRefuse(Data, STATUS_INVALID_PARAMETER, "FirstRow/RowCount runs past the surface height");
        return;
    }

    // Read fresh every call, like DcnEscape's decoded summary: a present between two bands of the same dump is a
    // torn frame in the tool's own BMP, not a driver bug - locking OTG0 for the whole dump would make this the
    // write escape it deliberately is not.
    status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, &lo);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, &hi);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH, &pitchRaw);
    if (!NT_SUCCESS(status)) { FbdumpRefuse(Data, status, "could not read the scanout address or pitch"); return; }

    address = ((ULONGLONG)hi << 32) | lo;
    pitch = ((pitchRaw & HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) + 1) * 4;     // pixels -> A8R8G8B8 bytes
    if (pitch == 0 || pitch > BC250_FBDUMP_ROW_BYTES)
    {
        FbdumpRefuse(Data, STATUS_DEVICE_DATA_ERROR, "the pitch register reads outside what this escape can hold");
        return;
    }

    start = address + (ULONGLONG)Data->FirstRow * pitch;
    length = (ULONGLONG)Data->RowCount * pitch;
    if (!FbdumpRangeAllowed(Device, start, length))
    {
        FbdumpRefuse(Data, STATUS_ACCESS_DENIED, "band not inside VRAM or the firmware framebuffer");
        return;
    }

    phys.QuadPart = (LONGLONG)start;
    map = (volatile ULONG*)MmMapIoSpaceEx(phys, (SIZE_T)length, PAGE_READONLY | PAGE_NOCACHE);
    if (map == NULL) { FbdumpRefuse(Data, STATUS_INSUFFICIENT_RESOURCES, "could not map the band"); return; }

    for (row = 0; row < Data->RowCount; row++)
    {
        ULONG* dst = (ULONG*)(Data->Pixels + (SIZE_T)row * pitch);
        ULONG x, words = pitch / 4;

        for (x = 0; x < words; x++) dst[x] = READ_REGISTER_ULONG((PULONG)&map[row * words + x]);
    }
    MmUnmapIoSpace((PVOID)map, (SIZE_T)length);

    Data->Width = (unsigned long)Device->Post.Width;
    Data->Height = height;
    Data->Pitch = pitch;
    Data->ColorFormat = (unsigned long)Device->Post.ColorFormat;
    Data->Address = start;
    Data->NtStatus = (unsigned long)STATUS_SUCCESS;
    Data->Status = BC250_ESCAPE_STATUS_DONE;
    GuardLog("fbdump: hubp %u rows %u..%u of %u, pitch %u, address 0x%llX", Data->Hubp, Data->FirstRow,
             Data->FirstRow + Data->RowCount - 1, height, pitch, start);
}

// ---- DcnFlip (0.7.20, ADR 0011 point 3 step 2): one gated flip on HUBP0/OTG0 -----------------------------------
//
// PASSIVE_LEVEL only (the poll below stalls the processor, and the optional fill maps memory). Gated by
// Device->DcnWriteEnabled (EnableMmio && EnableDcnWrite, mmio.c); the fill needs EnableVramWrite as well. The
// six registers this writes are g_MmioDcnWriteAllow (gen_regs.py's DCN_WRITE_REGISTERS), checked by MmioDcnWrite;
// everything else here is MmioDcnRead, the same read-only path DcnEscape above uses.
//
// The surface this operates on is fixed at 1920x1200 A8R8G8B8, pitch 7680 (M84's present surface, the mode the
// firmware left, facts M14): the flip does not yet take a pitch or a format from the caller because nothing else
// does either at this stage (no VidPN source, no allocation) - that comes with SetVidPnSourceAddress.
#define BC250_DCNFLIP_WIDTH 1920ul
#define BC250_DCNFLIP_HEIGHT 1200ul
#define BC250_DCNFLIP_PITCH (BC250_DCNFLIP_WIDTH * 4ul)                                // 7680
#define BC250_DCNFLIP_SURFACE_BYTES ((ULONGLONG)BC250_DCNFLIP_PITCH * BC250_DCNFLIP_HEIGHT)
#define BC250_DCNFLIP_BORDER 64ul
#define BC250_DCNFLIP_POLL_STEP_US 100ul
#define BC250_DCNFLIP_POLL_MAX_US 50000ul                                              // 50 ms

static void Refuse(_Inout_ BC250_ESCAPE_DCNFLIP* Out, NTSTATUS Status, _In_z_ const char* Reason)
{
    Out->NtStatus = (unsigned long)Status;
    Out->Status = BC250_ESCAPE_STATUS_REFUSED;
    RtlStringCbCopyA(Out->Reason, sizeof(Out->Reason), Reason);
    GuardLog("dcnflip: refused: %s (0x%08X)", Reason, (ULONG)Status);
}

// Target == the firmware's own address is always allowed (the no-op flip, and what Restore asks for); anything
// else needs the VRAM carve-out identified (EnableVram) and the whole surface, 4 KiB aligned, inside it (facts
// M31: base 0x270000000, length from GCMC_VM_FB_LOCATION_BASE/TOP, both already in Device->VramPhysical/VramLength
// - vram.c's VramStart, the same numbers wddm.c's segment 1 is carved from).
static BOOLEAN AddressAllowed(_In_ const BC250_DEVICE* Device, ULONGLONG Target)
{
    ULONGLONG vramBase, vramTop;

    if (Device->DcnFirmwareKnown && Target == Device->DcnFirmwareAddress) return TRUE;
    if ((Target & 0xFFFull) != 0) return FALSE;
    if (!Device->VramEnabled) return FALSE;
    vramBase = (ULONGLONG)Device->VramPhysical.QuadPart;
    vramTop = vramBase + Device->VramLength;
    if (Target < vramBase || Target >= vramTop) return FALSE;
    return BC250_DCNFLIP_SURFACE_BYTES <= vramTop - Target;
}

// Fill's own pattern: solid FillColor, a 64-pixel white border, and a corner-to-corner diagonal (3 pixels wide)
// in the inverted colour - simple on purpose, it only has to look different from the desktop at a glance.
// PAGE_NOCACHE like every other CPU access to this memory (facts M32: BAR0 is not a coherent view of VRAM, so
// nothing here is cached either). One mapping for the whole surface, like display.c's DisplayMapFramebuffer.
static NTSTATUS FillSurface(ULONGLONG Physical, ULONG FillColor)
{
    PHYSICAL_ADDRESS phys;
    volatile ULONG* map;
    ULONG inverted = (FillColor & 0xFF000000ul) | (~FillColor & 0x00FFFFFFul);
    ULONG x, y;

    phys.QuadPart = (LONGLONG)Physical;
    map = (volatile ULONG*)MmMapIoSpaceEx(phys, (SIZE_T)BC250_DCNFLIP_SURFACE_BYTES, PAGE_READWRITE | PAGE_NOCACHE);
    if (map == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    for (y = 0; y < BC250_DCNFLIP_HEIGHT; y++)
    {
        // Where the corner-to-corner line crosses this row, 1920x1200 scaled to this y.
        ULONG center = (y * BC250_DCNFLIP_WIDTH) / BC250_DCNFLIP_HEIGHT;
        ULONG left = center > 1 ? center - 1 : 0;
        ULONG right = center + 1 < BC250_DCNFLIP_WIDTH ? center + 1 : BC250_DCNFLIP_WIDTH - 1;

        for (x = 0; x < BC250_DCNFLIP_WIDTH; x++)
        {
            BOOLEAN border = (x < BC250_DCNFLIP_BORDER || x >= BC250_DCNFLIP_WIDTH - BC250_DCNFLIP_BORDER ||
                              y < BC250_DCNFLIP_BORDER || y >= BC250_DCNFLIP_HEIGHT - BC250_DCNFLIP_BORDER);
            BOOLEAN diagonal = (x >= left && x <= right);
            ULONG pixel = border ? 0xFFFFFFFFul : diagonal ? inverted : FillColor;

            WRITE_REGISTER_ULONG((PULONG)&map[y * (BC250_DCNFLIP_PITCH / 4) + x], pixel);
        }
    }
    MmUnmapIoSpace((PVOID)map, (SIZE_T)BC250_DCNFLIP_SURFACE_BYTES);
    return STATUS_SUCCESS;
}

// M87's poll: HUBPREQ0_DCSURF_FLIP_CONTROL bit 0x100 (SURFACE_FLIP_PENDING), 100 us steps, up to 50 ms - the
// budget amdgpu's own poll never got close to at 60 Hz (facts M88: 2158 reads across 360 flips).
static void PollFlipPending(_In_ const BC250_DEVICE* Device, _Out_ ULONG* WaitUs, _Out_ BOOLEAN* Cleared)
{
    ULONG waited = 0, value = 0;

    *Cleared = FALSE;
    for (;;)
    {
        if (!NT_SUCCESS(MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &value))) break;
        if ((value & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) == 0) { *Cleared = TRUE; break; }
        if (waited >= BC250_DCNFLIP_POLL_MAX_US) break;
        KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
        waited += BC250_DCNFLIP_POLL_STEP_US;
    }
    *WaitUs = waited;
}

// The whole flip: gate, read and remember the firmware address (once per device start, whichever call gets here
// first), validate the target, the optional fill, the M87 write sequence in order, the poll, and the readback.
// Device is mutated (the firmware/current address and Diverged) only after every write of the sequence went
// through; a refused flip leaves it untouched.
static void DcnFlipCore(_Inout_ BC250_DEVICE* Device, ULONGLONG Physical, BOOLEAN Restore, ULONG Fill, ULONG FillColor,
                        _Inout_ BC250_ESCAPE_DCNFLIP* Out)
{
    ULONG lo = 0, hi = 0;
    ULONGLONG before, target;
    NTSTATUS status;
    BOOLEAN cleared;

    Out->Reason[0] = '\0';
    if (Device->Mmio == NULL || !Device->DcnWriteEnabled)
    {
        Refuse(Out, STATUS_DEVICE_NOT_READY, "gate closed: EnableMmio and EnableDcnWrite must both be 1");
        return;
    }

    // Step 1 (M87): read and remember the current address. Always done, flip or restore, fill or not - it is
    // also how the firmware's own address is discovered, the first time anything here runs in a device start.
    status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, &lo);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, &hi);
    if (!NT_SUCCESS(status)) { Refuse(Out, status, "could not read the current HUBP0 address"); return; }
    before = ((ULONGLONG)hi << 32) | lo;
    Out->AddressBefore = before;
    if (!Device->DcnFirmwareKnown) { Device->DcnFirmwareKnown = TRUE; Device->DcnFirmwareAddress = before; }
    Out->FirmwareAddress = Device->DcnFirmwareAddress;

    status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_INUSE, &Out->InUseBefore);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT, &Out->FrameCountBefore);
    if (!NT_SUCCESS(status)) { Refuse(Out, status, "could not read SURFACE_INUSE or OTG_STATUS_FRAME_COUNT"); return; }

    target = Restore ? Device->DcnFirmwareAddress : Physical;
    if (!AddressAllowed(Device, target))
    {
        Refuse(Out, STATUS_ACCESS_DENIED, "target is not 4 KiB aligned, EnableVram is off, or the surface does not fit in VRAM");
        return;
    }

    if (Fill != 0 && !Restore)
    {
        if (target == Device->DcnFirmwareAddress) { Refuse(Out, STATUS_ACCESS_DENIED, "fill refused: that is the firmware's own address"); return; }
        if (!Device->VramWriteEnabled) { Refuse(Out, STATUS_ACCESS_DENIED, "fill needs EnableVramWrite as well"); return; }
        status = FillSurface(target, FillColor);
        if (!NT_SUCCESS(status)) { Refuse(Out, status, "could not map the target surface to fill it"); return; }
    }

    // Step 2 (M87), in order: lock, arm the flip (synchronous, unlocked), the new address, unlock, trigger.
    status = MmioDcnWrite(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 1);
    if (NT_SUCCESS(status)) status = MmioDcnWrite(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, 0);
    if (NT_SUCCESS(status)) status = MmioDcnWrite(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL, 0);
    if (NT_SUCCESS(status)) status = MmioDcnWrite(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (ULONG)(target >> 32));
    if (NT_SUCCESS(status)) status = MmioDcnWrite(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, (ULONG)(target & 0xFFFFFFFFull));
    if (NT_SUCCESS(status)) status = MmioDcnWrite(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0);
    if (NT_SUCCESS(status)) status = MmioDcnWrite(Device, BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG, 1);
    if (!NT_SUCCESS(status))
    {
        // The lock write (step 1) is the only one of these six that must never be left standing: if it went
        // through and a later write in the sequence did not, unlock now, best effort, rather than leave OTG0
        // locked. Harmless when the lock write itself was what failed, or when the sequence already unlocked
        // before the failing write - LOCK=0 onto an already-unlocked OTG0 is a no-op.
        (void)MmioDcnWrite(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0);
        Refuse(Out, status, "a register write of the flip sequence was refused");
        return;
    }

    PollFlipPending(Device, &Out->WaitUs, &cleared);
    Out->FlipPendingCleared = cleared ? 1 : 0;

    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, &lo);
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, &hi);
    Out->AddressAfter = ((ULONGLONG)hi << 32) | lo;
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_INUSE, &Out->InUseAfter);
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT, &Out->FrameCountAfter);
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &Out->DchubpCntl);
    Out->Underflow = Out->DchubpCntl & HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS_MASK;

    Device->DcnCurrentAddress = target;
    Device->DcnDiverged = (target != Device->DcnFirmwareAddress);

    Out->NtStatus = (unsigned long)STATUS_SUCCESS;
    Out->Status = BC250_ESCAPE_STATUS_DONE;
    GuardLog("dcnflip: 0x%llX -> 0x%llX%s%s, pending %s (%u us), inuse 0x%08X -> 0x%08X, frame %u -> %u, underflow 0x%X",
             before, target, Restore ? " (restore)" : "", Fill ? " (filled)" : "", cleared ? "cleared" : "STILL SET",
             Out->WaitUs, Out->InUseBefore, Out->InUseAfter, Out->FrameCountBefore, Out->FrameCountAfter, Out->Underflow);
}

void DcnFlipEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DCNFLIP* Data)
{
    Data->Version = BC250_KMD_VERSION;
    Data->NtStatus = 0;
    Data->FirmwareAddress = 0;
    Data->AddressBefore = 0;
    Data->AddressAfter = 0;
    Data->InUseBefore = 0;
    Data->InUseAfter = 0;
    Data->FrameCountBefore = 0;
    Data->FrameCountAfter = 0;
    Data->FlipPendingCleared = 0;
    Data->WaitUs = 0;
    Data->DchubpCntl = 0;
    Data->Underflow = 0;
    RtlZeroMemory(Data->Reason, sizeof(Data->Reason));

    DcnFlipCore(Device, Data->Physical, Data->Restore != 0, Data->Fill, Data->FillColor, Data);
}

// The stop path's undo (pnp.c, ADR 0011 consequences): called unconditionally, a no-op (logged as one) unless
// the most recent flip left HUBP0 somewhere other than the firmware's address. Runs with a scratch output
// struct - the caller (pnp.c) has nowhere to put one and nothing to do with it beyond the log line here.
void DcnStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_ESCAPE_DCNFLIP scratch;

    if (!Device->DcnDiverged) return;
    if (Device->Mmio == NULL || !Device->DcnWriteEnabled)
    {
        // Diverged is only ever set after a successful write, which needs both - so this should not happen. If
        // it does (the registry changed under a running device, say), leaving HUBP0 where it is beats a write
        // through a gate the stop path itself just found closed.
        GuardLog("dcnflip: stop: cannot restore, gate closed");
        return;
    }
    RtlZeroMemory(&scratch, sizeof(scratch));
    DcnFlipCore(Device, 0, TRUE, 0, 0, &scratch);
    GuardLog("dcnflip: stop: restore to the firmware address %s (NTSTATUS 0x%08X)",
             scratch.Status == BC250_ESCAPE_STATUS_DONE ? "done" : "REFUSED", scratch.NtStatus);
}
