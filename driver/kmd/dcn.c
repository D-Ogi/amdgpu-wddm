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
#include "display_timing_snapshot.h"
#include "dcn_2_0_1_sh_mask.h"       // field masks for the decoded summary only; every offset comes from regcalc
#include "dcn_translate.h"           // ADR 0011 point 3 step 3: the WDDM flip's host-testable address conversion
#include <ntstrsafe.h>

void DcnObserve(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DCN_OBSERVE* Data,
    _In_ BOOLEAN Admin, _In_ ULONG EscapeFlags)
{
    static const ULONG offsets[] = {
        BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,
        BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
        BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,
        BC250_REG_DMU_OTG0_OTG_STATUS_POSITION,
        BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,
        BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,
        BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL,
        BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT
    };
    ULONG* outputs[] = { &Data->PrimaryAddressLow, &Data->PrimaryAddressHigh,
        &Data->EarliestInUseLow, &Data->EarliestInUseHigh, &Data->FlipControl,
        &Data->SurfacePitch, &Data->OtgStatusPosition, &Data->OtgGlobalControl0,
        &Data->OtgBlankControl, &Data->OtgDoubleBufferControl, &Data->OtgFrameCount };
    ULONG requestedAbi=Data->AbiVersion, timing[TimingCount]={0}, i;
    NTSTATUS status=STATUS_SUCCESS, readStatus;
    C_ASSERT(sizeof(BC250_ESCAPE_DCN_OBSERVE)==128);
    C_ASSERT(RTL_NUMBER_OF(offsets)==RTL_NUMBER_OF(outputs));
    C_ASSERT(RTL_NUMBER_OF(offsets)+TimingCount==BC250_DCN_OBSERVE_REG_COUNT);
    RtlZeroMemory(Data,sizeof(*Data));
    Data->Magic=BC250_ESCAPE_MAGIC; Data->Command=BC250_ESCAPE_OBSERVE_DCN;
    Data->Version=BC250_KMD_VERSION; Data->AbiVersion=BC250_DCN_OBSERVE_ABI;
    Data->RegisterCount=BC250_DCN_OBSERVE_REG_COUNT;
    Data->Status=BC250_ESCAPE_STATUS_REFUSED;
    if (!Admin) {
        Data->Status=BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus=(ULONG)STATUS_ACCESS_DENIED; return;
    }
    // HardwareAccess requests Level Two exclusion of stop/MMIO unmap. No
    // NoAdapterSynchronization bypass: this reader has no separate lifetime join.
    if (EscapeFlags!=1u || requestedAbi!=BC250_DCN_OBSERVE_ABI) {
        Data->NtStatus=(ULONG)STATUS_INVALID_PARAMETER; return;
    }
    if (Device->Mmio==NULL) { Data->NtStatus=(ULONG)STATUS_DEVICE_NOT_READY; return; }
    Data->SequenceBefore=(ULONG)InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0);
    for (i=0;i<RTL_NUMBER_OF(offsets);i++) {
        ULONG value=0;
        readStatus=MmioDcnRead(Device,offsets[i],&value);
        if (NT_SUCCESS(readStatus)) { *outputs[i]=value; Data->ValidMask|=1u<<i; }
        else if (NT_SUCCESS(status)) status=readStatus;
    }
    readStatus=DisplayTimingSnapshot(Device,timing);
    if (NT_SUCCESS(readStatus)) {
        Data->TimingControl=timing[TimingControl]; Data->TimingHTotal=timing[TimingHTotal];
        Data->TimingVTotal=timing[TimingVTotal]; Data->TimingHBlank=timing[TimingHBlank];
        Data->TimingVBlank=timing[TimingVBlank]; Data->TimingPixelControl=timing[TimingPixelControl];
        Data->TimingPhase=timing[TimingPhase]; Data->TimingModulo=timing[TimingModulo];
        Data->TimingInterlace=timing[TimingInterlace]; Data->TimingVTotalControl=timing[TimingVTotalControl];
        Data->TimingReference=timing[TimingReference];
        Data->ValidMask|=BC250_DCN_OBSERVE_TIMING_MASK;
    } else if (NT_SUCCESS(status)) status=readStatus;
    Data->SequenceAfter=(ULONG)InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0);
    Data->NtStatus=(ULONG)status;
    Data->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
}

void DcnEscape(_In_ const BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DCN* Data)
{
    const BC250_DCN_REG_INFO* table = NULL;
    ULONG count = MmioDcnTable(&table);
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG hubp0Lo = 0, hubp0Hi = 0, pitch = 0, otgControl = 0, syncStatus = 0;
    ULONG masterLock = 0, flipControl = 0, vupdateKeepout = 0;

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
    Data->Otg0VupdateEventOccurred = 0;
    Data->Otg0VupdateIntStatus = 0;
    Data->Otg0MasterUpdateLocked = 0;
    Data->Hubp0FlipPending = 0;
    Data->Otg0VupdateKeepoutEn = 0;
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
    // Bit 14, VUPDATE_NO_LOCK_EVENT_OCCURRED: latched by hardware whether or not _INT_EN is set, so this is the
    // one field that can tell "event never happens" and "event happens but never reaches the IH ring/MSI" apart
    // (docs/design/vsync-interrupt-route.md). Same register as the enable bit above, no extra read.
    Data->Otg0VupdateEventOccurred = (syncStatus & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK) ? 1 : 0;
    // Bit 15, VUPDATE_NO_LOCK_INT_STATUS: not read by amdgpu's own source anywhere (irq_service_dcn20.c's generic
    // path only writes this register), so this is an empirical field - M92 vs M103 (docs/design/vsync-interrupt-
    // route.md section 12) is the only cross-check on record, and it tracks the enable bit, not EVENT_OCCURRED:
    // "this occurrence is qualified to raise the interrupt", distinct from "the event happened at all" above.
    // Same register, same read, no extra MmioDcnRead call.
    Data->Otg0VupdateIntStatus = (syncStatus & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_STATUS_MASK) ? 1 : 0;

    // Three more reads for the same open question: whether gart/psp/gfx/ih coming up (E22 run 004, M98, M99)
    // leaves the OTG update lock or the vupdate keepout window held, or a flip pending, in a way run 001/002's
    // display-only dumps (M92, M94) never exercised. All three offsets are already on DCN_REGISTERS above
    // (gen_regs.py) and named for BC250_REG_DMU_* besides; reading them again by name costs three MmioDcnRead
    // calls the loop above already proved safe.
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &masterLock);
    Data->Otg0MasterUpdateLocked = (masterLock & OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK) ? 1 : 0;
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &flipControl);
    Data->Hubp0FlipPending = (flipControl & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) ? 1 : 0;
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_VUPDATE_KEEPOUT, &vupdateKeepout);
    Data->Otg0VupdateKeepoutEn = (vupdateKeepout & OTG0_OTG_VUPDATE_KEEPOUT__OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN_MASK) ? 1 : 0;

    GuardLog("dcn: %u registers, status 0x%08X; hubp0 addr 0x%llX pitch %u cntl 0x%08X; otg0 control 0x%08X h_total %u v_total %u; "
             "vupdate int_en %u event_occurred %u int_status %u master_locked %u flip_pending %u keepout_en %u",
             Data->RegCount, status, Data->Hubp0Address, Data->Hubp0Pitch, Data->Hubp0Cntl, Data->Otg0Control,
             Data->Otg0HTotal, Data->Otg0VTotal, Data->Otg0VblankIntEnabled, Data->Otg0VupdateEventOccurred,
             Data->Otg0VupdateIntStatus, Data->Otg0MasterUpdateLocked, Data->Hubp0FlipPending, Data->Otg0VupdateKeepoutEn);
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
    height = DisplaySourceHeight(Device);   // the surface the plane reads: the committed source mode (modeset.c)
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
    map = (volatile ULONG*)VramMapCpuRange(Device,phys,(SIZE_T)length,PAGE_READONLY);
    if (map == NULL) { FbdumpRefuse(Data, STATUS_INSUFFICIENT_RESOURCES, "could not map the band"); return; }

    for (row = 0; row < Data->RowCount; row++)
    {
        ULONG* dst = (ULONG*)(Data->Pixels + (SIZE_T)row * pitch);
        ULONG x, words = pitch / 4;

        for (x = 0; x < words; x++) dst[x] = READ_REGISTER_ULONG((PULONG)&map[row * words + x]);
    }
    MmUnmapIoSpace((PVOID)map, (SIZE_T)length);

    Data->Width = (unsigned long)DisplaySourceWidth(Device);
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
// Dimensions come from POST; pitch follows the allocation and is restored with the firmware address.
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
// - vram.c's VramStart, the same numbers wddm.c's segment 1 is carved from). The alignment-and-range half of
// this is dcn_translate.c's DcnAddressFits (0.7.24, ADR 0011 point 3 step 3): one host-testable rule shared with
// DcnFlipSourceAddress below, rather than a second copy of the arithmetic. Width and Height are the surface's: the
// escape's own (Post, refused while a smaller source mode is committed) or the committed source mode's (modeset.c).
static BOOLEAN AddressAllowed(_In_ const BC250_DEVICE* Device, ULONGLONG Target, ULONG Pitch, ULONG Width, ULONG Height)
{
    ULONGLONG bytes;
    if (!DcnSurfaceBytes(Width,Height,Pitch,&bytes)) return FALSE;
    if (Device->DcnFirmwareKnown && Target==Device->DcnFirmwareAddress) return Pitch==Device->DcnFirmwarePitch;
    if (!Device->VramEnabled) return FALSE;
    return DcnAddressFits(Target,(ULONGLONG)Device->VramPhysical.QuadPart,Device->VramLength,bytes)!=0;
}
static NTSTATUS CaptureFirmwareSurface(_Inout_ BC250_DEVICE* Device)
{
    ULONG lo=0,hi=0,pitch=0;
    ULONGLONG bytes;
    NTSTATUS status;
    if (Device->DcnFirmwareKnown) return STATUS_SUCCESS;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,&lo);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,&hi);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&pitch);
    if (!NT_SUCCESS(status)) return status;
    pitch=((pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)+1)*4;
    if (!DcnSurfaceBytes(Device->Post.Width,Device->Post.Height,pitch,&bytes) ||
        pitch!=Device->Post.Pitch || bytes>Device->FramebufferLength) return STATUS_DEVICE_CONFIGURATION_ERROR;
    Device->DcnFirmwareAddress=((ULONGLONG)hi<<32)|lo;
    Device->DcnFirmwarePitch=pitch;
    Device->DcnFirmwareKnown=TRUE;
    return STATUS_SUCCESS;
}

// Fill's own pattern: solid FillColor, a 64-pixel white border, and a corner-to-corner diagonal (3 pixels wide)
// in the inverted colour - simple on purpose, it only has to look different from the desktop at a glance.
// Match POST aliases through the shared CPU mapping policy; otherwise retain NC.
// One mapping covers the whole surface. This does not prove GPU coherency.
static NTSTATUS FillSurface(_In_ const BC250_DEVICE* Device, ULONGLONG Physical, ULONG Pitch, ULONG FillColor)
{
    PHYSICAL_ADDRESS phys;
    volatile ULONG* map;
    ULONG inverted = (FillColor & 0xFF000000ul) | (~FillColor & 0x00FFFFFFul);
    ULONG x, y;
    ULONGLONG bytes;
    if (!DcnSurfaceBytes(Device->Post.Width,Device->Post.Height,Pitch,&bytes) ||
        !AddressAllowed(Device,Physical,Pitch,Device->Post.Width,Device->Post.Height)) return STATUS_INVALID_PARAMETER;

    phys.QuadPart = (LONGLONG)Physical;
    map = (volatile ULONG*)VramMapCpuRange(Device,phys,(SIZE_T)bytes,PAGE_READWRITE);
    if (map == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    for (y = 0; y < Device->Post.Height; y++)
    {
        // Where the corner-to-corner line crosses this row of the inherited mode.
        ULONG center = (ULONG)(((ULONGLONG)y * Device->Post.Width) / Device->Post.Height);
        ULONG left = center > 1 ? center - 1 : 0;
        ULONG right = center + 1 < Device->Post.Width ? center + 1 : Device->Post.Width - 1;

        for (x = 0; x < Device->Post.Width; x++)
        {
            BOOLEAN border = (x < BC250_DCNFLIP_BORDER || Device->Post.Width - x <= BC250_DCNFLIP_BORDER ||
                              y < BC250_DCNFLIP_BORDER || Device->Post.Height - y <= BC250_DCNFLIP_BORDER);
            BOOLEAN diagonal = (x >= left && x <= right);
            ULONG pixel = border ? 0xFFFFFFFFul : diagonal ? inverted : FillColor;

            WRITE_REGISTER_ULONG((PULONG)&map[(SIZE_T)y * (Pitch / 4) + x], pixel);
        }
    }
    MmUnmapIoSpace((PVOID)map, (SIZE_T)bytes);
    return STATUS_SUCCESS;
}

// M87's single-pipe flip, shared by the escape and the high-IRQL WDDM DDI.
// AMD v6.18 optc1_lock waits for UPDATE_LOCK_STATUS before changing the surface;
// hubp2_program_surface_flip_and_addr updates only SURFACE_FLIP_TYPE. Keep those
// semantics here. WDDM adaptation: no sleeping or allocation, at most ten 1 us
// stalls, and refuse an unacknowledged lock instead of programming an unprotected
// address. This limits explicit stalls, not the total MMIO execution time.
// OTG0's inherited MASTER_UPDATE_LOCK_SEL remains 0; this is not a modeset path.
// Retain the measured manual trigger: dcn201_tg_funcs uses
// optc2_program_manual_trigger, called after unlock by core/dc.c and dc_hw_sequencer.c.
// Quiet suppresses per-write logging for the DDI (up to PROFILE_LEVEL - 1).
static NTSTATUS DcnFlipWriteSequence(_Inout_ BC250_DEVICE* Device, ULONGLONG Target, ULONG Pitch, BOOLEAN Quiet)
{
    NTSTATUS status;
    ULONG value = 0, waited = 0;
    if (!Pitch || (Pitch&3ul) || ((Pitch/4-1)&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)) return STATUS_INVALID_PARAMETER;

    status = MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 1, Quiet);
    if (NT_SUCCESS(status))
    {
        for (;;)
        {
            status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &value);
            if (!NT_SUCCESS(status) ||
                (value & OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK) != 0) break;
            if (waited == 10)
            {
                InterlockedIncrement(&Device->DcnLockTimeouts);
                status = STATUS_IO_TIMEOUT;
                break;
            }
            KeStallExecutionProcessor(1);
            waited++;
        }
    }
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &value);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,
        value & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK, Quiet);
    // AMD hubp2 also updates only the two graphics TMZ fields here. Our
    // unprotected surfaces clear those fields without erasing other controls.
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL, &value);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL,
        value & ~(HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
                  HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_META_SURFACE_TMZ_MASK), Quiet);
    // AMD hubp2_program_size uses pixels minus one; preserve META_PITCH.
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&value);
    if (NT_SUCCESS(status)) status=MmioDcnWriteEx(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,
        (value&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)|(Pitch/4-1),Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (ULONG)(Target >> 32), Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, (ULONG)(Target & 0xFFFFFFFFull), Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0, Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG, 1, Quiet);
    if (!NT_SUCCESS(status))
    {
        // Release even after a missing acknowledgement. Never leave OTG0 locked
        // after a refused flip; no address or trigger write follows a timeout.
        (void)MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0, Quiet);
    }
    return status;
}

// M87's poll: HUBPREQ0_DCSURF_FLIP_CONTROL bit 0x100 (SURFACE_FLIP_PENDING), 100 us steps, up to 50 ms - the
// budget amdgpu's own poll never got close to at 60 Hz (facts M88: 2158 reads across 360 flips). PASSIVE_LEVEL
// only (the busy-wait): the escape's own poll, never DcnFlipSourceAddress's (see DcnFlipPending below instead).
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
    ULONG lo = 0, hi = 0, pitch;
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
    status=CaptureFirmwareSurface(Device);
    if (!NT_SUCCESS(status)) { Refuse(Out,status,"invalid firmware surface geometry");return; }
    Out->FirmwareAddress = Device->DcnFirmwareAddress;

    status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_INUSE, &Out->InUseBefore);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT, &Out->FrameCountBefore);
    if (!NT_SUCCESS(status)) { Refuse(Out, status, "could not read SURFACE_INUSE or OTG_STATUS_FRAME_COUNT"); return; }

    target = Restore ? Device->DcnFirmwareAddress : Physical;
    pitch=target==Device->DcnFirmwareAddress?Device->DcnFirmwarePitch:DcnPrimaryPitch(Device->Post.Width);
    if (!AddressAllowed(Device,target,pitch,Device->Post.Width,Device->Post.Height))
    {
        Refuse(Out, STATUS_ACCESS_DENIED, "target is not 4 KiB aligned, EnableVram is off, or the surface does not fit in VRAM");
        return;
    }

    if (Fill != 0 && !Restore)
    {
        if (target == Device->DcnFirmwareAddress) { Refuse(Out, STATUS_ACCESS_DENIED, "fill refused: that is the firmware's own address"); return; }
        if (!Device->VramWriteEnabled) { Refuse(Out, STATUS_ACCESS_DENIED, "fill needs EnableVramWrite as well"); return; }
        status = FillSurface(Device,target,pitch,FillColor);
        if (!NT_SUCCESS(status)) { Refuse(Out, status, "could not map the target surface to fill it"); return; }
    }

    // Step 2 (M87): the write sequence, factored out (DcnFlipWriteSequence above) so the DDI path shares it.
    // Quiet = FALSE: the escape's own occasional, human-driven flip keeps its per-write GuardLog, unchanged.
    InterlockedIncrement(&Device->DcnSurfaceSequence);
    status=DcnFlipWriteSequence(Device,target,pitch,FALSE);
    if (!NT_SUCCESS(status))
    {
        InterlockedIncrement(&Device->DcnSurfaceSequence);
        Refuse(Out, status, "the flip sequence failed or its lock was not acknowledged");
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

    Device->DcnCurrentAddress=target;
    Device->DcnCurrentPitch=pitch;
    Device->DcnDiverged=(target!=Device->DcnFirmwareAddress || pitch!=Device->DcnFirmwarePitch);
    InterlockedIncrement(&Device->DcnSurfaceSequence);

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

    // The WDDM DDI owns OTG0 while VidPn flips are enabled. Diagnostics must
    // not interleave another lock/address/unlock transaction with that owner.
    // DcnStop calls the core directly after WDDM has quiesced, so restoration
    // remains available to the normal stop path.
    if (Device->VidPnFlipEnabled)
    {
        Refuse(Data, STATUS_DEVICE_BUSY, "diagnostic flip refused while VidPn owns OTG0");
        return;
    }
    // The escape's surfaces have the firmware's size (Post); a smaller committed source mode has a smaller
    // viewport, and the fill pattern and the address checks of this path know nothing of it (modeset.c).
    if (Device->Modeset.PipeChanged)
    {
        Refuse(Data, STATUS_INVALID_DEVICE_STATE, "diagnostic flip refused while a scaled display mode is committed");
        return;
    }
    DcnFlipCore(Device, Data->Physical, Data->Restore != 0, Data->Fill, Data->FillColor, Data);
}

// Restore is shared by orderly stop and the bugcheck display. Only cached
// nonpaged state, checked MMIO, interlocked counters and bounded processor stalls
// are used. In particular, no pool, scheduler wait, log, software lock or GPU
// reset is reachable here. OS bugcheck/stop serialization owns the display.
static NTSTATUS DcnReadScanoutPhysical(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Physical)
{
    ULONG high, low, highAgain, attempt;
    ULONGLONG physical;
    NTSTATUS status;

    *Physical = 0;
    if (Device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    // A latch between the two halves must not manufacture an address. Bounded
    // retries only: the caller can run at DPC and never waits for the next frame.
    for (attempt = 0; attempt < 3; ++attempt)
    {
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, &high);
        if (!NT_SUCCESS(status)) return status;
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, &low);
        if (!NT_SUCCESS(status)) return status;
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, &highAgain);
        if (!NT_SUCCESS(status)) return status;
        high &= HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
        highAgain &= HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
        if (high != highAgain) continue;
        physical = ((ULONGLONG)high << 32) | low;
        *Physical = physical;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_READY;
}

static NTSTATUS DcnCheckPostSurface(_In_ const BC250_DEVICE* Device)
{
    ULONG pending, pitch;
    ULONGLONG scanned;
    NTSTATUS status;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,&pending);
    if (!NT_SUCCESS(status)) return status;
    if (pending&HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return STATUS_DEVICE_BUSY;
    status=DcnReadScanoutPhysical(Device,&scanned);
    if (!NT_SUCCESS(status)) return status;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&pitch);
    if (!NT_SUCCESS(status)) return status;
    pitch=((pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)+1)*4;
    return scanned==Device->DcnFirmwareAddress && pitch==Device->DcnFirmwarePitch ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
}

// PROVENANCE: Linux AMD display (MIT), dcn201_tg_funcs uses optc1_set_blank
// and optc1_set_blank_data_double_buffer. Keep the inherited blank color and
// timing running. This affects output pixels, never framebuffer contents.
// Quiet, allocation/lock-free and bounded so restore can also use it at bugcheck.
NTSTATUS DcnSetVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible)
{
    ULONG value, waited=0;
    NTSTATUS status;
    // A display-only profile has no hardware ownership to restore. Once MMIO
    // writes are owned, software DcnBlanked is not proof that inherited/live
    // hardware is visible (for example after adapter reinitialization).
    if (Device->Mmio==NULL || !Device->DcnWriteEnabled)
        return Visible && !Device->DcnBlanked ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&value);
    if (!NT_SUCCESS(status)) return status;
    if (Visible && !(value & (OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
                              OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DE_MODE_MASK |
                              OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK)))
    {
        Device->DcnBlanked=FALSE;
        return STATUS_SUCCESS; // hardware readback, never a cached visibility guess
    }
    value &= ~(OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
               OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DE_MODE_MASK);
    if (!Visible) value|=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK;
    status=MmioDcnWriteEx(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,value,TRUE);
    if (!NT_SUCCESS(status)) return status;
    if (!Visible)
    {
        Device->DcnBlanked=TRUE; // undo remains required even if confirmation fails
        status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL,&value);
        if (!NT_SUCCESS(status)) return status;
        status=MmioDcnWriteEx(Device,BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL,
            value&~OTG0_OTG_DOUBLE_BUFFER_CONTROL__OTG_BLANK_DATA_DOUBLE_BUFFER_EN_MASK,TRUE);
        if (!NT_SUCCESS(status)) return status;
    }
    for (;;)
    {
        status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&value);
        if (!NT_SUCCESS(status)) return status;
        if (((value&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK)!=0)==(!Visible)) break;
        if (waited>=BC250_DCNFLIP_POLL_MAX_US) return STATUS_IO_TIMEOUT;
        KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
        waited+=BC250_DCNFLIP_POLL_STEP_US;
    }
    Device->DcnBlanked=!Visible;
    return STATUS_SUCCESS;
}

static NTSTATUS DcnFirmwareSurfaceCore(_Inout_ BC250_DEVICE* Device, BOOLEAN Unblank)
{
    NTSTATUS status;
    ULONG waited=0;
    // Captured before any address write, so this also covers a bugcheck midway
    // through a flip before DcnDiverged/current-address publication completed.
    if (!Device->DcnFirmwareKnown)
        return Device->DcnDiverged ? STATUS_DEVICE_NOT_READY : (Unblank ? DcnSetVisibility(Device,TRUE) : STATUS_SUCCESS);
    if (!Device->Framebuffer || Device->DcnFirmwarePitch!=Device->Post.Pitch)
        return STATUS_DEVICE_NOT_READY;
    status=DcnCheckPostSurface(Device);
    if (!NT_SUCCESS(status))
    {
        status=DcnFlipWriteSequence(Device,Device->DcnFirmwareAddress,Device->DcnFirmwarePitch,TRUE);
        if (!NT_SUCCESS(status)) return status;
        for (;;)
        {
            status=DcnCheckPostSurface(Device);
            if (status!=STATUS_DEVICE_BUSY) break;
            if (waited>=BC250_DCNFLIP_POLL_MAX_US) return STATUS_IO_TIMEOUT;
            KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
            waited+=BC250_DCNFLIP_POLL_STEP_US;
        }
        if (!NT_SUCCESS(status)) return status;
    }
    if (Unblank) {
        status=DcnSetVisibility(Device,TRUE);
        if (!NT_SUCCESS(status)) return status;
    }
    Device->DcnCurrentAddress=Device->DcnFirmwareAddress;
    Device->DcnCurrentPitch=Device->DcnFirmwarePitch;
    Device->DcnDiverged=FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device)
{
    return DcnFirmwareSurfaceCore(Device,TRUE);
}

// modeset.c, before the viewport grows: the plane back on the firmware surface, which holds the native size, so that
// a larger viewport never reads past the end of the smaller surface it scanned. The source's visibility is the
// OS's business (SetVidPnSourceVisibility) and stays as it is. PASSIVE_LEVEL in practice; same rules as above.
NTSTATUS DcnFlipToFirmwareSurface(_Inout_ BC250_DEVICE* Device)
{
    return DcnFirmwareSurfaceCore(Device,FALSE);
}

NTSTATUS DcnStop(_Inout_ BC250_DEVICE* Device)
{
    NTSTATUS status;
    DcnUnmapScanout(Device);
    status=DcnRestorePostDisplay(Device);
    GuardLog("dcnflip: stop: verified firmware scanout status 0x%08X",status);
    // After the firmware surface is back: the native viewport and scaler shape for the next owner (modeset.c). It
    // runs when the restore above failed as well; the screen of the next owner needs the shape either way.
    ModesetStop(Device);
    return status;
}

// ---- ADR 0011 point 3 step 3 (0.7.24): SetVidPnSourceAddress's own path, VUPDATE_NO_LOCK ----------------------
//
// Everything below is reachable only with Device->VidPnFlipEnabled (mmio.c's MmioStart: EnableMmio &&
// EnableDcnWrite && EnableVidPnFlip together), which in turn is reachable only under EnableFullWddm - the
// display-only DDI table never calls DxgkDdiSetVidPnSourceAddress or DxgkDdiControlInterrupt at all. With the
// gate closed none of this runs and the escape path above (DcnFlipCore/DcnFlipEscape) is untouched.
#define BC250_DCN_LOG_CALLS 8        // how many of DcnFlipSourceAddress's outcomes reach the guard log, each way

// SetVidPnSourceAddress's own flip: the same M87 write sequence DcnFlipCore uses (DcnFlipWriteSequence), never
// PollFlipPending's 50 ms busy-wait - d3dkmddi.h allows this DDI up to PROFILE_LEVEL - 1, where a busy-wait of
// that length would stall the DPC that is supposed to report the flip's completion (WddmDcnVsync, wddm.c;
// DcnFlipPending below is that DPC's bounded, non-blocking observation). CardAddress is what dxgkrnl's
// DXGKARG_SETVIDPNSOURCEADDRESS.PrimaryAddress carries for this adapter's one segment - BaseAddress-relative
// (Device->VramMcBase + an offset, wddm.c's WddmQuerySegment4), not the physical address DCN wants - and
// DcnTranslateCardAddress (dcn_translate.c) is the inverse of that same arithmetic (facts M85), run from what
// VramStart measured, never a literal. PhysicalOut, if not NULL, gets the translated address for the caller's
// own log line.
NTSTATUS DcnFlipSourceAddress(_Inout_ BC250_DEVICE* Device, ULONGLONG CardAddress, ULONG Pitch, ULONGLONG AllocationBytes, _Out_opt_ ULONGLONG* PhysicalOut)
{
    ULONGLONG physical,bytes;
    NTSTATUS status;

    if (Device->Mmio == NULL || !Device->VidPnFlipEnabled) return STATUS_DEVICE_NOT_READY;
    if (!DcnSurfaceBytes(DisplaySourceWidth(Device),DisplaySourceHeight(Device),Pitch,&bytes) || bytes>AllocationBytes)
        return STATUS_INVALID_PARAMETER;
    status=CaptureFirmwareSurface(Device);
    if (!NT_SUCCESS(status)) return status;
    if (!Device->VramEnabled ||
        !DcnTranslateCardAddress(CardAddress, Device->VramMcBase, (ULONGLONG)Device->VramPhysical.QuadPart,
                                 Device->VramLength, &physical) ||
        !AddressAllowed(Device,physical,Pitch,DisplaySourceWidth(Device),DisplaySourceHeight(Device)))
    {
        if (InterlockedIncrement(&Device->DcnFlipRefused) <= BC250_DCN_LOG_CALLS)
            GuardLog("dcnflip: SetVidPnSourceAddress refused: card address 0x%llX does not translate inside the carve-out", CardAddress);
        return STATUS_ACCESS_DENIED;
    }
    if (PhysicalOut != NULL) *PhysicalOut = physical;


    // Quiet = TRUE (review 16 section 24): a real present, potentially once per frame for as long as the
    // desktop is up - MmioDcnWrite's per-write GuardLog was sized for the escape's occasional calls, not this.
    // The outcome is still logged, once, below, capped like every other counter here (BC250_DCN_LOG_CALLS).
    InterlockedIncrement(&Device->DcnSurfaceSequence);
    status=DcnFlipWriteSequence(Device,physical,Pitch,TRUE);
    if (NT_SUCCESS(status))
    {
        // Publish address and pitch together under DcnSurfaceSequence. The
        // PASSIVE_LEVEL CPU mapper rejects a snapshot crossing this update.
        Device->DcnCurrentAddress=physical;
        Device->DcnCurrentPitch=Pitch;
        Device->DcnDiverged=(physical!=Device->DcnFirmwareAddress || Pitch!=Device->DcnFirmwarePitch);
        if (InterlockedIncrement(&Device->DcnFlipsHardware) <= BC250_DCN_LOG_CALLS)
            GuardLog("dcnflip: SetVidPnSourceAddress flip: card 0x%llX -> physical 0x%llX", CardAddress, physical);
    }
    else if (InterlockedIncrement(&Device->DcnFlipRefused) <= BC250_DCN_LOG_CALLS)
        GuardLog("dcnflip: SetVidPnSourceAddress: the flip sequence failed or its lock was not acknowledged (0x%08X)", status);
    InterlockedIncrement(&Device->DcnSurfaceSequence);
    return status;
}

// PROVENANCE: Linux AMD display dc/hubp/dcn10/dcn10_hubp.c (MIT),
// hubp1_is_flip_pending checks both the pending bit and EARLIEST_INUSE against
// the requested address. DCN addresses are CPU-physical here; dxgkrnl uses our
// MC-base-relative card addresses. Keep that conversion paired with the flip.

NTSTATUS DcnReadScanoutAddress(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* CardAddress)
{
    ULONGLONG physical, offset;
    NTSTATUS status;
    *CardAddress = 0;
    if (!Device->VramEnabled) return STATUS_DEVICE_NOT_READY;
    status = DcnReadScanoutPhysical(Device, &physical);
    if (!NT_SUCCESS(status)) return status;
    if (physical < (ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_DEVICE_NOT_READY;
    offset = physical - (ULONGLONG)Device->VramPhysical.QuadPart;
    if (offset >= Device->VramLength) return STATUS_DEVICE_NOT_READY;
    *CardAddress = Device->VramMcBase + offset;
    return STATUS_SUCCESS;
}

BOOLEAN DcnFlipPending(_In_ const BC250_DEVICE* Device, ULONGLONG RequestedAddress)
{
    ULONG value;
    ULONGLONG scanned;

    if (Device->Mmio == NULL) return TRUE;
    if (!NT_SUCCESS(MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &value))) return TRUE;
    if (value & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return TRUE;
    if (!NT_SUCCESS(DcnReadScanoutAddress(Device, &scanned))) return TRUE;
    return scanned != RequestedAddress;
}

// PROVENANCE: Linux AMD display dc/optc/dcn10/dcn10_optc.c (MIT),
// optc1_get_position / optc1_get_crtc_scanoutpos. Sample the vertical counter,
// with the static mode's blank bounds, rather than a software timer's phase.
// Deriving blank from that same counter avoids a status/position boundary race.
NTSTATUS DcnReadScanLine(_In_ const BC250_DEVICE* Device, _Out_ BOOLEAN* InBlank, _Out_ UINT* ScanLine)
{
    ULONG bounds, total, position, start, end, line;
    NTSTATUS status;

    if (Device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_V_BLANK_START_END, &bounds);
    if (!NT_SUCCESS(status)) return status;
    status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_V_TOTAL, &total);
    if (!NT_SUCCESS(status)) return status;
    status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_POSITION, &position);
    if (!NT_SUCCESS(status)) return status;
    start = (bounds & OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START_MASK) >> OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START__SHIFT;
    end = (bounds & OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END_MASK) >> OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT;
    total = ((total & OTG0_OTG_V_TOTAL__OTG_V_TOTAL_MASK) >> OTG0_OTG_V_TOTAL__OTG_V_TOTAL__SHIFT) + 1;
    line = (position & OTG0_OTG_STATUS_POSITION__OTG_VERT_COUNT_MASK) >> OTG0_OTG_STATUS_POSITION__OTG_VERT_COUNT__SHIFT;
    if (start >= total || end >= total || line >= total || start == end) return STATUS_DEVICE_NOT_READY;
    *InBlank = start < end ? (line >= start && line < end) : (line >= start || line < end);
    // D3D raster status starts line zero at the first active line (blank end).
    *ScanLine = *InBlank ? 0 : (line + total - end) % total;
    return STATUS_SUCCESS;
}

// GLOBAL_SYNC_STATUS mixes enable fields and W1C commands. A readback must
// never acknowledge another event as a side effect of the no-lock-vupdate ACK.
static ULONG DcnVsyncAckValue(ULONG Value)
{
    Value &= ~(OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK |
               OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_EVENT_CLEAR_MASK |
               OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK |
               OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_EVENT_CLEAR_MASK);
    return Value | OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK;
}

typedef struct _BC250_DCN_VSYNC_CHANGE {
    BC250_DEVICE* Device;
    BOOLEAN On;
    NTSTATUS Status;
} BC250_DCN_VSYNC_CHANGE;

// Runs under the same interrupt lock as DcnVsyncInterrupt. Never acquire the
// WDDM lock here: callers may already hold it while preserving arm/stop order.
static BOOLEAN DcnVsyncEnableSynchronized(_In_ PVOID Context)
{
    BC250_DCN_VSYNC_CHANGE* change=(BC250_DCN_VSYNC_CHANGE*)Context;
    ULONG value;
    change->Status=MmioDcnRead(change->Device,BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS,&value);
    if (!NT_SUCCESS(change->Status)) return FALSE;
    if (change->On) value|=OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK;
    else value&=~OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK;
    change->Status=MmioDcnWriteEx(change->Device,BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS,
                                DcnVsyncAckValue(value),TRUE);
    if (NT_SUCCESS(change->Status))
        InterlockedExchange(&change->Device->DcnVsyncArmed,change->On?1:0);
    return NT_SUCCESS(change->Status);
}

NTSTATUS DcnVsyncEnable(_Inout_ BC250_DEVICE* Device, BOOLEAN On)
{
    BC250_DCN_VSYNC_CHANGE change;
    BOOLEAN returned=FALSE;
    NTSTATUS status;
    if (Device->Mmio==NULL || Device->Dxgk.DxgkCbSynchronizeExecution==NULL) return STATUS_DEVICE_NOT_READY;
    change.Device=Device;change.On=On;change.Status=STATUS_DEVICE_NOT_READY;
    // The miniport has one interrupt message (M38). The graphics callback is
    // available through DISPATCH_LEVEL and owns the actual KINTERRUPT object.
    status=Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle,DcnVsyncEnableSynchronized,&change,0,&returned);
    if (!NT_SUCCESS(status)) return status;
    return returned?change.Status:(NT_SUCCESS(change.Status)?STATUS_UNSUCCESSFUL:change.Status);
}

// Called only by the HardwareAccess, Level Two summary escape. Never from
// CollectDbgInfo or stop: a mapped BAR alone does not grant hardware access.
// Samples are sequential and can straddle a scan line/frame; no wait or write.
void DcnLogVsyncSnapshot(_In_ const BC250_DEVICE* Device)
{
    static const ULONG offsets[] = {
        BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT,
        BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS,
        BC250_REG_DMU_OTG0_OTG_CONTROL,
        BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,
        BC250_REG_DMU_OTG0_OTG_STATUS_POSITION
    };
    ULONG values[RTL_NUMBER_OF(offsets)] = {0};
    ULONG valid = 0, i;
    ULONGLONG begin, end;

    if (!Device->VidPnFlipEnabled || Device->Mmio == NULL) return;
    begin = KeQueryInterruptTime();
    for (i = 0; i < RTL_NUMBER_OF(offsets); ++i)
    {
        NTSTATUS status = MmioDcnRead(Device, offsets[i], &values[i]);
        if (NT_SUCCESS(status)) valid |= 1u << i;
        else GuardLog("vsync snapshot: slot %lu read failed %08lX", i, (ULONG)status);
    }
    end = KeQueryInterruptTime();
    GuardLog("vsync snapshot: 100ns begin %llu end %llu valid %02lX", begin, end, valid);
    GuardLog("vsync snapshot: frame %08lX sync %08lX control %08lX lock %08lX position %08lX",
             values[0], values[1], values[2], values[3], values[4]);
}

// pnp.c's Bc250InterruptRoutine calls this on every interrupt this driver's ISR takes, the same shape as ih.c's
// own IhInterrupt: a no-op (FALSE, nothing read) unless Device->VidPnFlipEnabled and Device->DcnVsyncArmed are
// both true, which is what makes this inert with the gate closed or nobody listening. DIRQL: MmioDcnRead and
// MmioDcnWrite take no lock (mmio.c, a mapped BAR5 and a binary search over a static table), which is what
// makes both legal here. The enable callback changes DcnVsyncArmed and the RMW
// under this interrupt's synchronization, so it cannot lose a disable to this ACK.
BOOLEAN DcnVsyncInterrupt(_Inout_ BC250_DEVICE* Device)
{
    ULONG status;

    InterlockedExchange64(&Device->DcnVsyncEntryTime, (LONG64)KeQueryInterruptTime());
    if (Device->Mmio == NULL) { InterlockedIncrement(&Device->DcnVsyncNoMmio); return FALSE; }
    if (!Device->VidPnFlipEnabled) { InterlockedIncrement(&Device->DcnVsyncFlipDisabled); return FALSE; }
    if (Device->DcnVsyncArmed == 0) { InterlockedIncrement(&Device->DcnVsyncUnarmed); return FALSE; }
    if (!NT_SUCCESS(MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS, &status)))
    {
        InterlockedIncrement(&Device->DcnVsyncReadFailed);
        InterlockedIncrement(&Device->DcnVsyncRefused);
        return FALSE;
    }
    InterlockedExchange(&Device->DcnVsyncLastStatus, (LONG)status);
    if ((status & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK) == 0)
    {
        InterlockedIncrement(&Device->DcnVsyncNoEvent);
        return FALSE;
    }

    // Acknowledge only our event, preserving unrelated enable fields. The
    // synchronized enable callback cannot interleave this read-modify-write.
    // Quiet = TRUE (review 16 section 24): this runs at DIRQL, once every vblank, for as long as the hardware
    // vsync stays armed - MmioDcnWrite's own per-write GuardLog would call DbgPrintEx from here on every frame,
    // forever, which is exactly the kind of unbounded ISR-path cost this driver's hardware-safety rules ask to
    // avoid (worse under a kernel debugger, where DbgPrint can block on the transport). DcnVsyncTicks below is
    // the counter that says this ran; nothing here needs a log line to be trusted.
    if (!NT_SUCCESS(MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS,
                                   DcnVsyncAckValue(status), TRUE)))
    {
        InterlockedIncrement(&Device->DcnVsyncAckFailed);
        InterlockedIncrement(&Device->DcnVsyncRefused);
        return FALSE;
    }
    InterlockedExchange64(&Device->DcnVsyncAckTime, (LONG64)KeQueryInterruptTime());
    InterlockedIncrement(&Device->DcnVsyncTicks);
    InterlockedIncrement(&Device->DcnVsyncAcked);
    Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
    return TRUE;
}

// A coalesced IH vector can be consumed without another ISR entry. Poll/ACK
// under the same interrupt lock as ISR and enable/disable read-modify-writes.
typedef struct _BC250_DCN_VECTOR_POLL {
    BC250_DEVICE* Device;
    BOOLEAN Invoked;
} BC250_DCN_VECTOR_POLL;

static BOOLEAN DcnVsyncVectorSynchronized(_In_ PVOID Context)
{
    BC250_DCN_VECTOR_POLL* poll=(BC250_DCN_VECTOR_POLL*)Context;
    poll->Invoked=TRUE;
    InterlockedIncrement(&poll->Device->DcnVsyncDpcPolls);
    if (DcnVsyncInterrupt(poll->Device))
        InterlockedIncrement(&poll->Device->DcnVsyncDpcAcked);
    return TRUE; // invocation succeeded even if ISR already cleared the event
}

void DcnVsyncFromVector(_Inout_ BC250_DEVICE* Device)
{
    BC250_DCN_VECTOR_POLL poll;
    BOOLEAN returned=FALSE;
    NTSTATUS status;
    if (Device->Dxgk.DxgkCbSynchronizeExecution==NULL)
    {
        InterlockedIncrement(&Device->DcnVsyncDpcSyncFailures);
        return;
    }
    poll.Device=Device;poll.Invoked=FALSE;
    status=Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle,
        DcnVsyncVectorSynchronized,&poll,0,&returned);
    if (!NT_SUCCESS(status) || !returned || !poll.Invoked)
        InterlockedIncrement(&Device->DcnVsyncDpcSyncFailures);
}

// ---- the present path's own destination once the flip is live (2026-09-22, ADR 0011 consequences) -------------
//
// M97/M100: once DxgkDdiSetVidPnSourceAddress has flipped HUBP0 away from the firmware's framebuffer, nothing
// scans that framebuffer out any more, and wddm.c's WddmPresentBlit has to land its CPU copy at
// Device->DcnCurrentAddress instead. Mapping the whole active surface on every present is not
// acceptable at a present rate that can reach 60 Hz, so this mapping is made once, kept across presents, and
// only remade when the flip target actually changed - which M97 measured happening far less often than
// presents arrive (one flip served 60 of them).
//
// IRQL is why this lives here and not in DcnFlipSourceAddress, right where DcnCurrentAddress itself changes:
// that DDI may run above DISPATCH_LEVEL (design note section 3, d3dkmddi.h's own
// _IRQL_requires_max_(PROFILE_LEVEL - 1) on DXGKDDI_SETVIDPNSOURCEADDRESS), where MmMapIoSpaceEx and
// MmUnmapIoSpace - PASSIVE_LEVEL only - would be illegal to call. WddmPresentBlit already requires
// PASSIVE_LEVEL for its own source mapping (wddm.c: "else if (KeGetCurrentIrql() != PASSIVE_LEVEL) why =
// \"IRQL\""), so the remap happens lazily there instead, on the next present after a flip - a comparison
// against Device->DcnScanoutMapAddress on every other present, and the actual MmMapIoSpaceEx only when it
// changed.
BOOLEAN DcnScanoutMapping(_Inout_ BC250_DEVICE* Device, _Out_ PVOID* Mapping, _Out_ SIZE_T* Length, _Out_ ULONG* Pitch)
{
    LONG generation=InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0);
    ULONGLONG target,bytes;
    ULONG pitch;
    BOOLEAN diverged;
    PHYSICAL_ADDRESS phys;
    SIZE_T length;
    *Mapping=NULL;*Length=0;*Pitch=0;
    if (generation&1) return FALSE;
    target=(ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&Device->DcnCurrentAddress,0,0);
    pitch=Device->DcnCurrentPitch;diverged=Device->DcnDiverged;
    if (InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0)!=generation || !diverged ||
        !DcnSurfaceBytes(DisplaySourceWidth(Device),DisplaySourceHeight(Device),pitch,&bytes)) return FALSE;
    if (Device->DcnScanoutMap && Device->DcnScanoutMapAddress==target && Device->DcnScanoutMapLength==bytes)
    {
        *Mapping=Device->DcnScanoutMap;*Length=Device->DcnScanoutMapLength;*Pitch=pitch;
        return TRUE;
    }

    DcnUnmapScanout(Device);            // the old target, if any, is not what HUBP0 reads any more

    // Re-validated here, not trusted from the flip that set DcnCurrentAddress (review 16's own standard: a
    // privileged CPU mapping earns its own bounds check at the point it is made). AddressAllowed is the exact
    // rule DcnFlipSourceAddress already refused this address against, over dcn_translate.c's DcnAddressFits -
    // never a second, hand-typed range.
    if (!AddressAllowed(Device,target,pitch,DisplaySourceWidth(Device),DisplaySourceHeight(Device)))
    {
        if (InterlockedIncrement(&Device->DcnScanoutMapFailed) <= BC250_DCN_LOG_CALLS)
            GuardLog("dcnflip: scanout mapping refused: 0x%llX is no longer inside the carve-out", target);
        return FALSE;
    }

    length = (SIZE_T)bytes;
    phys.QuadPart = (LONGLONG)target;
    Device->DcnScanoutMap = VramMapCpuRange(Device,phys,length,PAGE_READWRITE);
    if (Device->DcnScanoutMap == NULL)
    {
        if (InterlockedIncrement(&Device->DcnScanoutMapFailed) <= BC250_DCN_LOG_CALLS)
            GuardLog("dcnflip: scanout mapping failed: 0x%llX, %lu bytes", target, (ULONG)length);
        return FALSE;
    }
    Device->DcnScanoutMapAddress = target;
    Device->DcnScanoutMapLength = length;
    if (InterlockedIncrement(&Device->DcnScanoutRemaps) <= BC250_DCN_LOG_CALLS)
        GuardLog("dcnflip: scanout mapping: 0x%llX, %lu bytes", target, (ULONG)length);
    *Mapping = Device->DcnScanoutMap;
    *Length = Device->DcnScanoutMapLength;
    *Pitch = pitch;
    return TRUE;
}

// Unmap, if mapped, and zero the three fields either way - called from WddmStop and, idempotently, from
// DcnStop above. Not a DDI: PASSIVE_LEVEL only (MmUnmapIoSpace), same as its only real caller, WddmPresentBlit,
// by way of DcnScanoutMapping above.
void DcnUnmapScanout(_Inout_ BC250_DEVICE* Device)
{
    if (Device->DcnScanoutMap != NULL) MmUnmapIoSpace(Device->DcnScanoutMap, Device->DcnScanoutMapLength);
    Device->DcnScanoutMap = NULL;
    Device->DcnScanoutMapAddress = 0;
    Device->DcnScanoutMapLength = 0;
    Device->DcnScanoutSeedAddress = 0;       // the next target is not the one the firmware picture was copied into
}
