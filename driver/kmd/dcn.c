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
#include "connector_surface.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "lab_gate.h"          // BD-167: DcnEscape/FbdumpEscape/DcnFlipEscape are lab-only (RUN_DCN/FBDUMP/DCNFLIP)
#include "display_timing_snapshot.h"
#include "dcn_2_0_1_sh_mask.h"       // field masks for the decoded summary only; every offset comes from regcalc
#include "dcn_translate.h"           // ADR 0011 point 3 step 3: the WDDM flip's host-testable address conversion
#include "plane_format.h"            // M15.14: the plane's pixel format per flip
#include "flip_state.h"              // BD-170 r6: the DRAINED fact (Bc250FlipDrained)
#include "../shim/bc250_dcn201_pipe.h"
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

// BD-167: RUN_DCN (a DCN register dump) and RUN_FBDUMP (a read of the scanned-out framebuffer, i.e. presented
// pixels) are diagnostic reads with no release consumer and are compiled only into the lab flavour. DcnObserve
// above (the named read-only OBSERVE_DCN) and the display/flip paths below stay in every build.
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
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
#endif /* AMDGPU_WDDM_LAB_FLAVOUR (DcnEscape + FbdumpEscape) */

// ---- DcnFlip (0.7.20, ADR 0011 point 3 step 2): one gated flip on HUBP0/OTG0 -----------------------------------
//
// PASSIVE_LEVEL only (the poll below stalls the processor, and the optional fill maps memory). Gated by
// Device->DcnWriteEnabled (EnableMmio && EnableDcnWrite, mmio.c); the fill needs EnableVramWrite as well. The
// registers this writes are g_MmioDcnWriteAllow (gen_regs.py's DCN_WRITE_REGISTERS), checked by MmioDcnWrite;
// everything else here is MmioDcnRead, the same read-only path DcnEscape above uses.
//
// Dimensions come from POST; pitch follows the allocation and is restored with the firmware address.
#define BC250_DCNFLIP_BORDER 64ul
#define BC250_DCNFLIP_POLL_STEP_US 100ul
#define BC250_DCNFLIP_POLL_MAX_US 50000ul                                              // 50 ms
#define BC250_DCN_VISIBILITY_POLL_MAX_US 100000ul // two frame edges at >=23Hz, including blank acknowledgement

// BD-167: Refuse, FillSurface and the PollFlipPending/DcnFlipCore/DcnFlipEscape block below belong only to the
// lab RUN_DCNFLIP escape; the shared flip helpers (AddressAllowed, CaptureFirmwareSurface, DcnFlipWriteSequence,
// DcnPlaneFormatRegisters) stay in every build because the WDDM flip and restore paths use them.
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
static void Refuse(_Inout_ BC250_ESCAPE_DCNFLIP* Out, NTSTATUS Status, _In_z_ const char* Reason)
{
    Out->NtStatus = (unsigned long)Status;
    Out->Status = BC250_ESCAPE_STATUS_REFUSED;
    RtlStringCbCopyA(Out->Reason, sizeof(Out->Reason), Reason);
    GuardLog("dcnflip: refused: %s (0x%08X)", Reason, (ULONG)Status);
}
#endif /* AMDGPU_WDDM_LAB_FLAVOUR (DcnFlip Refuse) */

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
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
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
#endif /* AMDGPU_WDDM_LAB_FLAVOUR (DcnFlip FillSurface) */

// M15.14 (0.7.216.20): the three register values of PlaneFormat (plane_format.h), composed from the firmware's own
// values with only the format fields replaced: SURFACE_PIXEL_FORMAT, CROSSBAR_SRC_CB_B, CROSSBAR_SRC_CR_R and
// CNVC_SURFACE_PIXEL_FORMAT, the fields hubp1_program_pixel_format and dpp201_cnv_setup set for a format. So the
// value for ARGB8888 is the firmware's value itself, and restoring is a flip to ARGB8888. FALSE when PlaneFormat
// has no encoding or the firmware's format was not captured.
// BD-197: the other fields are the pipe owner's. Of all the fields of the three registers, the connector's commit
// writes one: DET_BUF_PLANE1_BASE_ADDRESS (Bc250Dcn201PipeApply, from its DML plan). Nothing in this driver writes
// ROTATION_ANGLE, H_MIRROR_EN, PACK_3TO2_ELEMENT_DISABLE, the alpha and Y/G crossbars or the spare bits except this
// composition, so the firmware's capture is their owner's value on every pipe. After a commit (DcnPipeConnector)
// the DET field is the commit's, so neither a format change nor a restore writes the firmware's DET base back over
// the plan's. For the commit's 32-bit 444 surface the plan gives 0 (extract_rq_regs, display_rq_dlg_calc_20.c:231),
// as the firmware does on unit A (0x00E40000).
static BOOLEAN DcnPlaneFormatRegisters(_In_ const BC250_DEVICE* Device, ULONG PlaneFormat,
    _Out_ BC250_PLANE_REGISTERS* Out)
{
    const BC250_PLANE_ENCODING* e = Bc250PlaneEncoding(PlaneFormat);
    ULONG hubpret = Device->DcnFirmwareHubpretControl;
    Out->SurfaceConfig = Out->HubpretControl = Out->CnvcFormat = 0;
    if (!e || !Device->DcnPlaneFormats) return FALSE;
    if (Device->DcnPipeConnector)
        hubpret = (hubpret & ~HUBPRET0_HUBPRET_CONTROL__DET_BUF_PLANE1_BASE_ADDRESS_MASK) |
            ((Device->DcnPipeDetPlane1 << HUBPRET0_HUBPRET_CONTROL__DET_BUF_PLANE1_BASE_ADDRESS__SHIFT) &
             HUBPRET0_HUBPRET_CONTROL__DET_BUF_PLANE1_BASE_ADDRESS_MASK);
    Out->SurfaceConfig = (Device->DcnFirmwareSurfaceConfig & ~HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) |
        ((e->HubpPixelFormat << HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT__SHIFT) &
         HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK);
    Out->HubpretControl = (hubpret &
        ~(HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK | HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK)) |
        ((e->CrossbarCbB << HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B__SHIFT) &
         HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK) |
        ((e->CrossbarCrR << HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R__SHIFT) &
         HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK);
    Out->CnvcFormat = (Device->DcnFirmwareCnvcFormat &
        ~CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT_MASK) |
        ((e->CnvcPixelFormat << CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT__SHIFT) &
         CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT_MASK);
    return TRUE;
}

// The plane format the three register values name, by their format fields only.
static ULONG DcnPlaneFormatDecodeRegisters(ULONG SurfaceConfig, ULONG HubpretControl, ULONG CnvcFormat)
{
    return Bc250PlaneFormatDecode(
        (SurfaceConfig & HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) >>
            HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT__SHIFT,
        (CnvcFormat & CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT_MASK) >>
            CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT__SHIFT,
        (HubpretControl & HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK) >>
            HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B__SHIFT,
        (HubpretControl & HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK) >>
            HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R__SHIFT);
}

// Read once per start (WddmStart), before the first flip of this start can change the plane. MmioStart cleared
// the state. Only a plane that decodes to ARGB8888 is "the firmware's BGRA8": restoring writes these values back,
// and the POST framebuffer and the CPU blit (DcnScanoutMapping) hold B8G8R8A8 pixels. Any other answer keeps
// this start at the firmware's format, as before 0.7.216.20. Reads only; the blend mode, FORMAT_CONTROL (ALPHA_EN,
// CNVC_UPDATE_PENDING) and the 2-bit alpha table are logged because they decide whether a surface's alpha reaches
// the output (dcn20_update_mpcc, dpp201_cnv_setup), and nothing in this driver writes them.
void DcnCaptureFirmwareFormat(_Inout_ BC250_DEVICE* Device)
{
    ULONG config = 0, crossbar = 0, cnvc = 0, control = 0, mpcc = 0, lut = 0, decoded;
    NTSTATUS status;
    if (Device->Mmio == NULL || Device->DcnPlaneFormats) return;
    status = MmioDcnRead(Device, BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG, &config);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL, &crossbar);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT, &cnvc);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_CNVC_CFG0_FORMAT_CONTROL, &control);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_MPCC0_MPCC_CONTROL, &mpcc);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_CNVC_CFG0_ALPHA_2BIT_LUT, &lut);
    if (!NT_SUCCESS(status))
    {
        GuardLog("dcnflip: plane format: firmware registers unreadable (0x%08X), BGRA8 only", (ULONG)status);
        return;
    }
    decoded = DcnPlaneFormatDecodeRegisters(config, crossbar, cnvc);
    GuardLog("dcnflip: plane format: firmware %s config 0x%08X crossbar 0x%08X cnvc 0x%08X",
             Bc250PlaneFormatText(decoded), config, crossbar, cnvc);
    GuardLog("dcnflip: plane format: cnvc control 0x%08X alpha lut 0x%08X mpcc control 0x%08X (blend mode %lu)",
             control, lut, mpcc, (mpcc & MPCC0_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE_MASK) >>
             MPCC0_MPCC_CONTROL__MPCC_ALPHA_BLND_MODE__SHIFT);
    if (decoded != BC250_PLANE_FORMAT_ARGB8888) return;
    Device->DcnFirmwareSurfaceConfig = config;
    Device->DcnFirmwareHubpretControl = crossbar;
    Device->DcnFirmwareCnvcFormat = cnvc;
    Device->DcnPlaneFormat = BC250_PLANE_FORMAT_ARGB8888;
    Device->DcnPlaneFormats = TRUE;
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
// BytesPerPixel is the pitch field's unit (hubp2_program_size: pitch in pixels minus one). Format, when not NULL,
// is the plane's new pixel format (DcnPlaneFormatRegisters), written inside the same acknowledged lock as the
// address, so the new format and the new address latch at the same update and no frame shows one without the
// other. dcn20_program_pipe likewise programs dpp_setup and hubp_program_surface_config under the pipe's lock
// (dc/hwss/dcn20/dcn20_hwseq.c:1747 and :1843). NULL writes exactly the register set of 0.7.216.18.
// Immediate (BD-170): the flip type of this flip. dcn201_hubp_funcs uses hubp1_program_surface_flip_and_addr
// (dc/hubp/dcn201/dcn201_hubp.c:115, dc/hubp/dcn10/dcn10_hubp.c:349), which writes SURFACE_FLIP_TYPE =
// flip_immediate before the address, inside the TG master update lock that dcn201_pipe_control_lock takes
// (dc/core/dc.c:4194 lock, :4398 update_plane_addr, :4406 unlock). 0 latches the address at the next VUPDATE,
// 1 is AMD's immediate flip, which dcn201_update_plane_addr records as the current address at once
// (dc/hwss/dcn201/dcn201_hwseq.c:156-157). FALSE writes the value 0.7.216.64 wrote. The two stereo-sync fields
// hubp1 clears for a mono plane stay as the firmware left them, as since M87, so a vsync flip is unchanged.
static NTSTATUS DcnFlipWriteSequence(_Inout_ BC250_DEVICE* Device, ULONGLONG Target, ULONG Pitch,
    ULONG BytesPerPixel, _In_opt_ const BC250_PLANE_REGISTERS* Format, BOOLEAN Immediate, BOOLEAN Quiet)
{
    NTSTATUS status;
    ULONG value = 0, waited = 0;
    if (!Pitch || !BytesPerPixel || (Pitch%BytesPerPixel) ||
        ((Pitch/BytesPerPixel-1)&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)) return STATUS_INVALID_PARAMETER;

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
        (value & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK) |
        (Immediate ? HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK : 0), Quiet);
    // AMD hubp2 also updates only the two graphics TMZ fields here. Our
    // unprotected surfaces clear those fields without erasing other controls.
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL, &value);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL,
        value & ~(HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
                  HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_META_SURFACE_TMZ_MASK), Quiet);
    // M15.14: the format, in AMD's order (dpp_setup before hubp_program_surface_config; hubp1_program_pixel_format
    // writes the crossbar before SURFACE_PIXEL_FORMAT). The lock makes the order invisible to the scan-out.
    if (NT_SUCCESS(status) && Format != NULL) {
        status = MmioDcnWriteEx(Device, BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT, Format->CnvcFormat, Quiet);
        if (NT_SUCCESS(status))
            status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL, Format->HubpretControl, Quiet);
        if (NT_SUCCESS(status))
            status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG, Format->SurfaceConfig, Quiet);
    }
    // AMD hubp2_program_size uses pixels minus one; preserve META_PITCH.
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&value);
    if (NT_SUCCESS(status)) status=MmioDcnWriteEx(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,
        (value&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)|(Pitch/BytesPerPixel-1),Quiet);
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

// BD-170: when an immediate flip has taken effect, and so when the surface it replaced may be used again.
//
// The contract. DXGK_FLIPCAPS.FlipImmediateMmIo is a flip that "takes effect immediately following a call to
// DxgkDdiSetVidPnSourceAddress without waiting for the next vertical sync" (local ref/ddi-display/d3dkmddi.md:
// 21235-21239). For the return value the DDI page documents only "STATUS_SUCCESS if it succeeds; otherwise, it
// returns one of the error codes defined in Ntstatus.h" (d3dkmddi.md:12791). The rest comes from the binary,
// dxgmms2 10.0.22621.6199 (the lab's 22631) and 10.0.26100.9549, read by function (docs/design/vidpn-flip.md section
// 7.1, facts M904). VidSchiExecuteMmIoFlip completes an immediate flip's entry and releases the previous primary at a
// success status: nothing waits for a CRTC_VSYNC report (wddm.c's WddmDcnVsync). Any other status leaves the entry at
// the head of its flip queue with the previous primary still referenced, and the VSync DPC executes it again once a
// VSync, with no count and no time limit. A busy answer therefore holds the source until a restart (the a03 trial),
// and r6 has none (docs/design/vidpn-flip.md section 7.2, flip_state.h).
//
// The witness. P' holds at one sample when SURFACE_FLIP_PENDING is clear ("pending first observed clear", not a
// latch) and DCSURF_PRIMARY_SURFACE_ADDRESS, read high word first, reads back as the request in the field widths of
// that register, with the primary generation unchanged. EARLIEST_INUSE is not part of it: in a03 it named the old
// surface until the next vblank while the pending bit cleared about 2 us after the programming. hubp1_is_flip_pending
// (dc/hubp/dcn10/dcn10_hubp.c:751-777) is the stricter vblank test, and DcnFlipPending below keeps it for the reports.
//
// The bound. The samples run inside the flip transaction, under the interrupt lock L (wddm.c, WddmFlipLocked), right
// after the programming, with 1 us stalls between samples. BC250_DCN_IMMEDIATE_OBSERVE_US is counted from the end of
// the programming sequence and held after the witness reads of each sample, so no sample starts past it; a step count
// ends the loop as well. A sample that started inside the bound can end past it: EndUs is taken after the loop, and an
// observation that ended past the bound is counted in DcnImmediateOverruns. 50 us is 4.2 lines of the lab's 1920x1080
// 75 Hz mode (11.9 us a line), and Microsoft asks for KeStallExecutionProcessor waits below 50 us. DONE is P' at a
// sample that started inside the bound; DONE_LATE is P' at a first sample that started after it, so that its time is
// an upper bound only. Every other outcome is edge U of section 7.2, which wddm.c answers with E5 (facts M903). The
// record keeps the request's flags as they came. The generation is
// read at both ends of the observation; under L nothing can move it, so SUPERSEDED counts the case facts M904 says
// cannot happen.
static LONG DcnQpcUs(LONGLONG Ticks, LONGLONG Frequency)
{
    LONGLONG us;
    if (Ticks <= 0 || Frequency <= 0) return 0;
    us = Ticks * 1000000 / Frequency;
    return us < 0x7FFFFFFF ? (LONG)us : 0x7FFFFFFF;
}

static void DcnImmediateMax(_Inout_ volatile LONG* Max, LONG Value)
{
    LONG max;
    for (max = *Max; Value > max; max = *Max)
        if (InterlockedCompareExchange(Max, Value, max) == max) break;
}

// The request as DCSURF_PRIMARY_SURFACE_ADDRESS and its _HIGH word hold it: each word in its field's width.
static ULONGLONG DcnPrimaryNormalized(ULONGLONG Physical)
{
    return ((((Physical >> 32) & HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK)) << 32) |
           (Physical & HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS__PRIMARY_SURFACE_ADDRESS_MASK);
}

// DCSURF_PRIMARY_SURFACE_ADDRESS read back, high word first, each word masked to its field.
static NTSTATUS DcnReadPrimaryPhysical(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Physical)
{
    ULONG high = 0, low = 0;
    NTSTATUS status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, &high);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, &low);
    *Physical = ((ULONGLONG)(high & HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK) << 32) |
                (low & HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS__PRIMARY_SURFACE_ADDRESS_MASK);
    return status;
}

ULONG DcnImmediateObserve(_Inout_ BC250_DEVICE* Device, ULONGLONG CardAddress, ULONGLONG Physical,
    LONGLONG ProgramStartQpc, LONGLONG ProgramEndQpc, _In_ volatile LONG* Generation, ULONG ExpectedGeneration,
    ULONG Flags)
{
    BC250_DCN_IMMEDIATE_RECORD record;
    BC250_DCN_IMMEDIATE_RECORD* slot;
    const LONGLONG frequency = Device->DcnQpcFrequency;
    const ULONGLONG request = DcnPrimaryNormalized(Physical);
    LARGE_INTEGER now;
    LONG us;
    ULONGLONG primary = 0;
    ULONG value, steps;
    LONG sequence, previousFrame;
    BOOLEAN pendingSet = TRUE;
    NTSTATUS status;

    RtlZeroMemory(&record, sizeof(record));
    record.CardAddress = CardAddress;
    record.Physical = request;
    record.Flags = Flags;
    record.Generation = ExpectedGeneration;
    record.ReadStatus = STATUS_DEVICE_NOT_READY;
    record.FirstUs = record.ClearUs = record.DoneUs = record.EndUs = -1;
    record.ProgramUs = DcnQpcUs(ProgramEndQpc - ProgramStartQpc, frequency);
    if (Device->Mmio == NULL) record.Outcome = BC250_DCN_IMMEDIATE_READ_FAILED;
    else if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration)
        record.Outcome = BC250_DCN_IMMEDIATE_SUPERSEDED;
    else
    {
        (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT, &record.Frame);
        (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_POSITION, &record.PositionBefore);
        for (steps = 0;; steps++)
        {
            now = KeQueryPerformanceCounter(NULL);
            us = DcnQpcUs(now.QuadPart - ProgramStartQpc, frequency);
            // Always admit the first observation, even if entry was delayed. Every additional sample must earn
            // its budget again after the stall: an interruption there must not start another poll past the bound.
            if (steps != 0 && (us - record.ProgramUs >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US ||
                              steps >= BC250_DCN_IMMEDIATE_OBSERVE_US))
                break;
            if (steps == 0) record.FirstUs = us;
            record.Samples++;
            status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &value);
            if (NT_SUCCESS(status))
            {
                if (steps == 0) record.Control = value;
                pendingSet = (value & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) != 0;
                if (!pendingSet && record.ClearUs < 0) record.ClearUs = us;
                status = DcnReadPrimaryPhysical(Device, &primary);
            }
            record.ReadStatus = status;
            record.Observed = NT_SUCCESS(status) ? primary : 0;
            if (NT_SUCCESS(status) && !pendingSet && primary == request)
            {
                record.DoneUs = us;
                break;
            }
            // Do not stall after an observation already consumed the budget. The admission check above repeats
            // after the stall; the step cap also bounds a counter that does not move.
            now = KeQueryPerformanceCounter(NULL);
            if (DcnQpcUs(now.QuadPart - ProgramStartQpc, frequency) - record.ProgramUs >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US ||
                steps >= BC250_DCN_IMMEDIATE_OBSERVE_US)
                break;
            KeStallExecutionProcessor(1);
        }
        // EndUs is after the last sample's reads. P' at a sample that started inside the bound is P' (the witness is
        // what that sample read); its reads ending past the bound, or a first sample already past it, are counted.
        now = KeQueryPerformanceCounter(NULL);
        record.EndUs = DcnQpcUs(now.QuadPart - ProgramStartQpc, frequency);
        if (record.EndUs - record.ProgramUs > (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US)
            InterlockedIncrement(&Device->DcnImmediateOverruns);
        (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_POSITION, &record.PositionAfter);
        if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration)
            record.Outcome = BC250_DCN_IMMEDIATE_SUPERSEDED;
        else if (record.DoneUs >= 0)
            record.Outcome = record.DoneUs - record.ProgramUs >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US ?
                BC250_DCN_IMMEDIATE_DONE_LATE : BC250_DCN_IMMEDIATE_DONE;
        else if (!NT_SUCCESS(record.ReadStatus)) record.Outcome = BC250_DCN_IMMEDIATE_READ_FAILED;
        else if (pendingSet) record.Outcome = BC250_DCN_IMMEDIATE_PENDING;
        else record.Outcome = BC250_DCN_IMMEDIATE_PRIMARY_OTHER;
    }

    InterlockedIncrement(&Device->DcnImmediateOutcomes[record.Outcome]);
    if (record.Outcome == BC250_DCN_IMMEDIATE_DONE)
    {
        InterlockedExchangeAdd64(&Device->DcnImmediateDoneSumUs, record.DoneUs);
        DcnImmediateMax(&Device->DcnImmediateDoneMaxUs, record.DoneUs);
    }
    if (record.EndUs >= 0) DcnImmediateMax(&Device->DcnImmediateObserveMaxUs, record.EndUs);
    sequence = InterlockedIncrement(&Device->DcnImmediateRecordNext);
    if (record.Samples)
    {
        previousFrame = InterlockedExchange(&Device->DcnImmediateLastFrame, (LONG)record.Frame);
        if (sequence > 1 && previousFrame == (LONG)record.Frame) InterlockedIncrement(&Device->DcnImmediateSameFrame);
    }
    // The slot's sequence lock (BC250_DCN_IMMEDIATE_RECORD): odd while the fields change, even again after them.
    record.Sequence = sequence;
    slot = &Device->DcnImmediateRecords[(ULONG)(sequence - 1) % BC250_DCN_IMMEDIATE_RECORDS];
    InterlockedIncrement(&slot->Version);
    RtlCopyMemory((PUCHAR)slot + FIELD_OFFSET(BC250_DCN_IMMEDIATE_RECORD, Sequence),
                  (const UCHAR*)&record + FIELD_OFFSET(BC250_DCN_IMMEDIATE_RECORD, Sequence),
                  sizeof(record) - FIELD_OFFSET(BC250_DCN_IMMEDIATE_RECORD, Sequence));
    InterlockedIncrement(&slot->Version);
    return record.Outcome;
}

// M87's poll: HUBPREQ0_DCSURF_FLIP_CONTROL bit 0x100 (SURFACE_FLIP_PENDING), 100 us steps, up to 50 ms - the
// budget amdgpu's own poll never got close to at 60 Hz (facts M88: 2158 reads across 360 flips). PASSIVE_LEVEL
// only (the busy-wait): the escape's own poll, never DcnFlipSourceAddress's (see DcnFlipPending below instead).
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
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
    BC250_PLANE_REGISTERS registers;
    const BC250_PLANE_REGISTERS* format;

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
    // Its surfaces are the firmware's format (4 bytes, B8G8R8A8); a plane a VidPn flip left in another format
    // is set back to it in the same update (M15.14).
    format=NULL;
    if (Device->DcnPlaneFormats && Device->DcnPlaneFormat!=BC250_PLANE_FORMAT_ARGB8888 &&
        DcnPlaneFormatRegisters(Device,BC250_PLANE_FORMAT_ARGB8888,&registers)) format=&registers;
    InterlockedIncrement(&Device->DcnSurfaceSequence);
    status=DcnFlipWriteSequence(Device,target,pitch,4,format,FALSE,FALSE);
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
    if (format!=NULL) Device->DcnPlaneFormat=BC250_PLANE_FORMAT_ARGB8888;
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
#endif /* AMDGPU_WDDM_LAB_FLAVOUR (PollFlipPending + DcnFlipCore + DcnFlipEscape) */

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
    // In the firmware's 4-byte unit: the comparison is with the firmware's pitch, and the format check below makes
    // it exact, because a plane of another format fails that check whatever its pitch field says.
    pitch=((pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)+1)*4;
    if (scanned!=Device->DcnFirmwareAddress || pitch!=Device->DcnFirmwarePitch) return STATUS_DEVICE_BUSY;
    // M15.14: the firmware's surface is also its pixel format. Only when the start captured that format; without
    // it no flip of this start changed the format, and the check is the one of 0.7.216.18.
    if (Device->DcnPlaneFormats)
    {
        ULONG config=0, crossbar=0, cnvc=0;
        status=MmioDcnRead(Device,BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG,&config);
        if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL,&crossbar);
        if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT,&cnvc);
        if (!NT_SUCCESS(status)) return status;
        if (DcnPlaneFormatDecodeRegisters(config,crossbar,cnvc)!=BC250_PLANE_FORMAT_ARGB8888) return STATUS_DEVICE_BUSY;
    }
    return STATUS_SUCCESS;
}

// PROVENANCE: Linux AMD display (MIT), v6.18 7d0a66e4: dc_hw_sequencer.c
// output_csc_matrix/black_color_format, mpc2_set_output_csc, opp1_program_fmt.
// CSC_MODE selects a bank at a frame boundary. The DCN201 offset header has no
// indexed debug-data port, so witness a frame with the same selected mode before
// inspecting that bank. Two counter transitions enclose a complete frame even
// when the first sample falls immediately before a latch boundary. Refuse an
// inherited held update lock. Never borrow DCN2.0 debug-port addresses.
static NTSTATUS DcnReadBlackColor(BC250_DEVICE* Device, ULONG* Color, ULONG* Waited)
{
    static const ULONG banks[2][6]={
        {BC250_REG_DMU_MPC_OUT0_CSC_C11_C12_A,BC250_REG_DMU_MPC_OUT0_CSC_C13_C14_A,
         BC250_REG_DMU_MPC_OUT0_CSC_C21_C22_A,BC250_REG_DMU_MPC_OUT0_CSC_C23_C24_A,
         BC250_REG_DMU_MPC_OUT0_CSC_C31_C32_A,BC250_REG_DMU_MPC_OUT0_CSC_C33_C34_A},
        {BC250_REG_DMU_MPC_OUT0_CSC_C11_C12_B,BC250_REG_DMU_MPC_OUT0_CSC_C13_C14_B,
         BC250_REG_DMU_MPC_OUT0_CSC_C21_C22_B,BC250_REG_DMU_MPC_OUT0_CSC_C23_C24_B,
         BC250_REG_DMU_MPC_OUT0_CSC_C31_C32_B,BC250_REG_DMU_MPC_OUT0_CSC_C33_C34_B}};
    static const unsigned short matrices[8][12]={
        {0x2000,0,0,0,0,0x2000,0,0,0,0,0x2000,0},
        {0x1b67,0,0,0x201,0,0x1b67,0,0x201,0,0,0x1b67,0x201},
        {0xe04,0xf444,0xfdb9,0x1004,0x831,0x1016,0x320,0x201,0xfb45,0xf6b7,0xe04,0x1004},
        {0xe04,0xf345,0xfeb7,0x1004,0x5d3,0x1399,0x1fa,0x201,0xfcca,0xf533,0xe04,0x1004},
        {0xe00,0xf447,0xfdb9,0x1000,0x991,0x12c9,0x3a6,0x200,0xfb47,0xf6b9,0xe00,0x1000},
        {0xe00,0xf349,0xfeb7,0x1000,0x6ce,0x16e3,0x24f,0x200,0xfccb,0xf535,0xe00,0x1000},
        {0x1000,0xf149,0xfeb7,0x1004,0x868,0x15b2,0x1e6,0x201,0xfb88,0xf478,0x1000,0x1004},
        {0,0,0,0x1000,0,0,0,0x200,0,0,0,0x1000}};
    ULONG fmt,mode,frame,value,after,i,j,kind=0,words[6],b,g,r;
    NTSTATUS status;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,&value);
    if(!NT_SUCCESS(status))return status;
    if(value&(OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
              OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK))return STATUS_DEVICE_BUSY;
    status=MmioDcnRead(Device,BC250_REG_DMU_FMT0_FMT_CONTROL,&fmt);
    if(NT_SUCCESS(status))status=MmioDcnRead(Device,BC250_REG_DMU_MPC_OUT0_CSC_MODE,&mode);
    if(!NT_SUCCESS(status))return status;
    fmt=(fmt&FMT0_FMT_CONTROL__FMT_PIXEL_ENCODING_MASK)>>FMT0_FMT_CONTROL__FMT_PIXEL_ENCODING__SHIFT;
    mode&=MPC_OUT0_CSC_MODE__MPC_OCSC_MODE_MASK;
    if(fmt>2 || mode>2)return STATUS_NOT_SUPPORTED;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT,&frame);
    if(!NT_SUCCESS(status))return status;
    frame&=OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK;
    for(;;) {
        status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT,&value);
        if(!NT_SUCCESS(status))return status;
        if(((value-frame)&OTG0_OTG_STATUS_FRAME_COUNT__OTG_FRAME_COUNT_MASK)>=2)break;
        if(*Waited>=BC250_DCN_VISIBILITY_POLL_MAX_US)return STATUS_IO_TIMEOUT;
        KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
        *Waited+=BC250_DCNFLIP_POLL_STEP_US;
    }
    status=MmioDcnRead(Device,BC250_REG_DMU_MPC_OUT0_CSC_MODE,&after);
    if(!NT_SUCCESS(status))return status;
    if((after&MPC_OUT0_CSC_MODE__MPC_OCSC_MODE_MASK)!=mode)return STATUS_DEVICE_BUSY;
    if(mode) {
        status=MmioDcnRead(Device,BC250_REG_DMU_MPC_OUT_CSC_COEF_FORMAT,&value);
        if(!NT_SUCCESS(status))return status;
        if(value&MPC_OUT_CSC_COEF_FORMAT__MPC_OCSC0_COEF_FORMAT_MASK)return STATUS_NOT_SUPPORTED;
        for(i=0;i<6;i++) {
            status=MmioDcnRead(Device,banks[mode-1][i],&words[i]);
            if(!NT_SUCCESS(status))return status;
        }
        for(kind=0;kind<8;kind++) {
            for(j=0;j<6;j++) {
                value=(ULONG)matrices[kind][2*j] | ((ULONG)matrices[kind][2*j+1]<<16);
                if(words[j]!=value)break;
            }
            if(j==6)break;
        }
        if(kind==8)return STATUS_NOT_SUPPORTED;
    }
    status=MmioDcnRead(Device,BC250_REG_DMU_MPC_OUT0_CSC_MODE,&after);
    if(NT_SUCCESS(status))status=MmioDcnRead(Device,BC250_REG_DMU_FMT0_FMT_CONTROL,&value);
    if(!NT_SUCCESS(status))return status;
    if((after&MPC_OUT0_CSC_MODE__MPC_OCSC_MODE_MASK)!=mode ||
       ((value&FMT0_FMT_CONTROL__FMT_PIXEL_ENCODING_MASK)>>FMT0_FMT_CONTROL__FMT_PIXEL_ENCODING__SHIFT)!=fmt)
        return STATUS_DEVICE_BUSY;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,&value);
    if(!NT_SUCCESS(status))return status;
    if(value&(OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
              OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK))return STATUS_DEVICE_BUSY;
    // 0=RGB/444, 1=422, 2=420 (opp1_program_fmt). RGB matrices require 444.
    if(kind<2 && fmt)return STATUS_NOT_SUPPORTED;
    b=g=r=kind==1?0x40:0;
    if(kind>=2) {b=r=0x1f4;g=0x40;if(fmt==2)r=g;}
    *Color=(b<<OTG0_OTG_BLACK_COLOR__OTG_BLACK_COLOR_B_CB__SHIFT) |
        (g<<OTG0_OTG_BLACK_COLOR__OTG_BLACK_COLOR_G_Y__SHIFT) |
        (r<<OTG0_OTG_BLACK_COLOR__OTG_BLACK_COLOR_R_CR__SHIFT);
    return STATUS_SUCCESS;
}

// PROVENANCE: Linux AMD display (MIT), optc1_program_blank_color,
// optc1_set_blank and optc1_set_blank_data_double_buffer. Hardware black keeps
// sync/timing and source pixels intact. Known black stays programmed while
// visible, where the color is unused; the next blank revalidates its encoding.
// Quiet, allocation/lock-free and bounded so restore can also use it at bugcheck.
static NTSTATUS DcnSetVisibilityCore(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible, BOOLEAN OsRequest, BOOLEAN* Touched)
{
    ULONG value, waited=0, color, wanted;
    NTSTATUS status;
    // A mode commit can restore a surface before the OS grants visibility.
    // Keep the startup hold through those internal restore calls. The OS DDI
    // bypasses it without publishing an early release to another caller.
    if (!OsRequest && InterlockedCompareExchange(&Device->StartVisibilityPending,0,0)) Visible=FALSE;
    // A display-only profile has no hardware ownership to restore. Once MMIO
    // writes are owned, software DcnBlanked is not proof that inherited/live
    // hardware is visible (for example after adapter reinitialization).
    if (Device->Mmio==NULL || !Device->DcnWriteEnabled)
        return Visible && !Device->DcnBlanked &&
            !InterlockedCompareExchange(&Device->StartVisibilityPending,0,0) ?
            STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&value);
    if (!NT_SUCCESS(status)) return status;
    if (!Visible) {
        const ULONG mask=OTG0_OTG_BLACK_COLOR__OTG_BLACK_COLOR_B_CB_MASK |
            OTG0_OTG_BLACK_COLOR__OTG_BLACK_COLOR_G_Y_MASK | OTG0_OTG_BLACK_COLOR__OTG_BLACK_COLOR_R_CR_MASK;
        status=DcnReadBlackColor(Device,&color,&waited);
        if(NT_SUCCESS(status))status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLACK_COLOR,&wanted);
        if(!NT_SUCCESS(status))return status;
        wanted=(wanted&~mask)|color;
        status=MmioDcnWriteEx(Device,BC250_REG_DMU_OTG0_OTG_BLACK_COLOR,wanted,TRUE);
        if(!NT_SUCCESS(status))return status;
        if(Touched)*Touched=TRUE;
        status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLACK_COLOR,&color);
        if(!NT_SUCCESS(status))return status;
        if((color&mask)!=(wanted&mask))return STATUS_DEVICE_DATA_ERROR;
    }
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
    if(Touched)*Touched=TRUE;
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
        if (waited>=(Visible?BC250_DCNFLIP_POLL_MAX_US:BC250_DCN_VISIBILITY_POLL_MAX_US)) return STATUS_IO_TIMEOUT;
        KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
        waited+=BC250_DCNFLIP_POLL_STEP_US;
    }
    Device->DcnBlanked=!Visible;
    return STATUS_SUCCESS;
}

NTSTATUS DcnSetVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible)
{
    return DcnSetVisibilityCore(Device,Visible,FALSE,NULL);
}

NTSTATUS DcnBeginStartVisibility(_Inout_ BC250_DEVICE* Device, _Out_ BOOLEAN* Touched)
{
    ULONG active=0,peer=0;
    NTSTATUS status;
    *Touched=FALSE;
    if(!Device->Mmio || !Device->DcnWriteEnabled)return STATUS_DEVICE_NOT_READY;
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_CONTROL,&active);
    if(NT_SUCCESS(status))status=MmioDcnRead(Device,BC250_REG_DMU_OTG1_OTG_CONTROL,&peer);
    if(!NT_SUCCESS(status))return status;
    // This driver owns only OTG0. Do not mutate an inactive or additional pipe.
    if(!(active&OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) || (peer&OTG1_OTG_CONTROL__OTG_MASTER_EN_MASK))
        return STATUS_NOT_SUPPORTED;
    return DcnSetVisibilityCore(Device,FALSE,FALSE,Touched);
}

// The PASSIVE_LEVEL visibility DDI owns StartHealth and Connector.Operation.
// Only a confirmed OS TRUE ends startup blanking; a flip is not that request.
NTSTATUS DcnSetSourceVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible)
{
    NTSTATUS status=DcnSetVisibilityCore(Device,Visible,TRUE,NULL);
    if (NT_SUCCESS(status) && Visible) InterlockedExchange(&Device->StartVisibilityPending,0);
    return status;
}

static NTSTATUS DcnRuntimeSurfaceAuthorized(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible, BOOLEAN Serialized);

static NTSTATUS DcnResumeOwnedFetch(_Inout_ BC250_DEVICE* Device)
{
    ULONG value = 0;
    NTSTATUS status = MmioDcnRead(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &value);
    if (NT_SUCCESS(status) && (value & (HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK | HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK)))
        status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL,
            value & ~(HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK | HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK), TRUE);
    if (NT_SUCCESS(status)) InterlockedExchange(&Device->RuntimePlaneBlank, 0);
    return status;
}

static NTSTATUS DcnFirmwareSurfaceCore(_Inout_ BC250_DEVICE* Device, BOOLEAN Unblank, BOOLEAN Serialized)
{
    NTSTATUS status;
    ULONG waited=0;
    if (!Serialized) {
        status = WddmWriterResume(Device);
        if (!NT_SUCCESS(status)) return status;
    }
    // Captured before any address write, so this also covers a bugcheck midway
    // through a flip before DcnDiverged/current-address publication completed.
    if (!Device->DcnFirmwareKnown)
        return Device->DcnDiverged ? STATUS_DEVICE_NOT_READY : (Unblank ? DcnSetVisibility(Device,TRUE) : STATUS_SUCCESS);
    if (!Device->Framebuffer || Device->DcnFirmwarePitch!=Device->Post.Pitch)
        return STATUS_DEVICE_NOT_READY;
    status=DcnCheckPostSurface(Device);
    if (!NT_SUCCESS(status))
    {
        // M15.14: the firmware's format with its address, in the same update. NULL (no format write) when the
        // start did not capture the format, which also means no flip of this start changed it.
        BC250_PLANE_REGISTERS registers;
        const BC250_PLANE_REGISTERS* format=
            DcnPlaneFormatRegisters(Device,BC250_PLANE_FORMAT_ARGB8888,&registers) ? &registers : NULL;
        status=DcnFlipWriteSequence(Device,Device->DcnFirmwareAddress,Device->DcnFirmwarePitch,4,format,FALSE,TRUE);
        if (!NT_SUCCESS(status)) return status;
        status=DcnResumeOwnedFetch(Device);
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
        status=DcnResumeOwnedFetch(Device);
        if (!NT_SUCCESS(status)) return status;
        status=DcnSetVisibility(Device,TRUE);
        if (!NT_SUCCESS(status)) return status;
    }
    Device->DcnCurrentAddress=Device->DcnFirmwareAddress;
    Device->DcnCurrentPitch=Device->DcnFirmwarePitch;
    if (Device->DcnPlaneFormats) Device->DcnPlaneFormat=BC250_PLANE_FORMAT_ARGB8888;
    Device->DcnDiverged=FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device)
{
    if (InterlockedCompareExchange(&Device->RuntimeDisplayChanged,0,0))
        return DcnRuntimeSurface(Device,TRUE);
    return DcnFirmwareSurfaceCore(Device,TRUE,FALSE);
}

// Only the OS's serialized bugcheck/reset/stop callbacks use this entry. A failed normal writer section never
// selects it, and high IRQL alone is not an authorization to bypass the writer protocol.
NTSTATUS DcnRestorePostDisplaySerialized(_Inout_ BC250_DEVICE* Device)
{
    if (InterlockedCompareExchange(&Device->RuntimeDisplayChanged,0,0))
        return DcnRuntimeSurfaceAuthorized(Device,TRUE,TRUE);
    return DcnFirmwareSurfaceCore(Device,TRUE,TRUE);
}

// modeset.c, before the viewport grows: the plane back on the firmware surface, which holds the native size, so that
// a larger viewport never reads past the end of the smaller surface it scanned. The source's visibility is the
// OS's business (SetVidPnSourceVisibility) and stays as it is. PASSIVE_LEVEL in practice; same rules as above.
NTSTATUS DcnFlipToFirmwareSurface(_Inout_ BC250_DEVICE* Device)
{
    if (InterlockedCompareExchange(&Device->RuntimeDisplayChanged,0,0))
        return DcnRuntimeSurface(Device,FALSE);
    return DcnFirmwareSurfaceCore(Device,FALSE,FALSE);
}

static long RuntimeRead(void* Context, unsigned long Offset, unsigned long* Value)
{
    BC250_DEVICE* device=Context;
    *Value=0;
    if (!device->Mmio) return STATUS_DEVICE_NOT_READY;
    if (!Bc250DispReadAllowed(Offset)) return STATUS_ACCESS_DENIED;
    *Value=READ_REGISTER_ULONG(device->Mmio+Offset/sizeof(ULONG));
    return STATUS_SUCCESS;
}

static long RuntimeWrite(void* Context, unsigned long Offset, unsigned long Value)
{
    BC250_DEVICE* device=Context;
    if (!device->Mmio || !device->DcnWriteEnabled) return STATUS_DEVICE_NOT_READY;
    if (!Bc250DispWriteAllowed(Offset)) return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG(device->Mmio+Offset/sizeof(ULONG),Value);
    return STATUS_SUCCESS;
}

static void RuntimeStall(void* Context, unsigned long Microseconds)
{
    UNREFERENCED_PARAMETER(Context);
    KeStallExecutionProcessor(Microseconds);
}

// BD-170 r6, the facts of docs/design/vidpn-flip.md section 7.2 that wddm.c reads inside L. No wait, no log: these run
// in the flip transaction at DIRQL and in the writer sections.
// DRAINED: one readback of HUBP0_DCHUBP_CNTL (flip_state.h, Bc250FlipDrained). 1 DRAINED, 0 not, -1 the read failed,
// which is not DRAINED either.
int DcnHubpDrained(_In_ const BC250_DEVICE* Device)
{
    ULONG value = 0;
    const NTSTATUS status = Device->Mmio != NULL ? MmioDcnRead(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &value) :
                                                   STATUS_DEVICE_NOT_READY;
    if (!NT_SUCCESS(status)) return -1;
    return Bc250FlipDrained(1u, (value & HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK) != 0,
                            (value & HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK) != 0,
                            (value & HUBP0_DCHUBP_CNTL__HUBP_NO_OUTSTANDING_REQ_MASK) != 0) ? 1 : 0;
}

// OTG running: OTG0_OTG_CONTROL.OTG_MASTER_EN. 1 running, 0 stopped, -1 the read failed (Bc250WriterEnd).
int DcnOtgRunning(_In_ const BC250_DEVICE* Device)
{
    ULONG value = 0;
    if (Device->Mmio == NULL || !NT_SUCCESS(MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_CONTROL, &value))) return -1;
    return (value & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) ? 1 : 0;
}

// E5 step 2: one read-modify-write of HUBP0_DCHUBP_CNTL that sets HUBP_BLANK_EN and HUBP_TTU_DISABLE, the two fields
// AMD hubp1_set_blank sets, and no wait for HUBP_NO_OUTSTANDING_REQ (the DPC records that, WddmFlipFaultDpc). The
// write goes through the pipe's own allow list (RuntimeWrite), as Bc250Dcn201PlaneBlank's does.
NTSTATUS DcnFaultBlank(_Inout_ BC250_DEVICE* Device)
{
    ULONG value = 0;
    long status;
    if (!Device->Mmio || !Device->DcnWriteEnabled) return STATUS_DEVICE_NOT_READY;
    status = RuntimeRead(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &value);
    if (status >= 0)
    {
        InterlockedExchange(&Device->RuntimePlaneBlank, 1);
        status = RuntimeWrite(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL,
                              value | HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK | HUBP0_DCHUBP_CNTL__HUBP_TTU_DISABLE_MASK);
    }
    return (NTSTATUS)status;
}

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
// BD-170 r6, the lab flavour's witnesses (bc250kmd.h). The flip event is the HUBP flip interrupt from which amdgpu
// sends its page-flip event (amdgpu_dm.c dm_pflip_high_irq; irq_service_dcn201.c maps HUBP0_FLIP_INTERRUPT to
// DC_IRQ_SOURCE_PFLIP1 with the ack SURFACE_FLIP_CLEAR, written as 1). The clear writes the four enable fields as
// found and both CLEAR fields 1, reads back, and writes a CLEAR field that reads back 1 to 0 again, so that it cannot
// hold the next event down: the sequence of BD-170 a04 (evidence 2026-10-14-bd170-a04-latch), which ran on the lab.
// No other code of this driver writes the register, and nothing sets SURFACE_FLIP_INT_MASK, so the IH gets no new
// source.
#define DCN_WITNESS_EVENTS (HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK | \
                            HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_OCCURRED_MASK)
#define DCN_WITNESS_CLEARS (HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_CLEAR_MASK | \
                            HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_CLEAR_MASK)
#define DCN_WITNESS_ENABLES (HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_MASK_MASK | \
                             HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_TYPE_MASK | \
                             HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_INT_MASK_MASK | \
                             HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_INT_TYPE_MASK)

#define DCN_WITNESS_EVENT_REG BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT
#define DCN_WITNESS_LOW_REG BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE
#define DCN_WITNESS_HIGH_REG BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH
#define DCN_WITNESS_LOW_MASK HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE__SURFACE_EARLIEST_INUSE_ADDRESS_MASK
#define DCN_WITNESS_HIGH_MASK HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK

void DcnWitnessArm(_In_ const BC250_DEVICE* Device, BOOLEAN Immediate, _Out_ BC250_DCN_WITNESS_ARM* Arm)
{
    ULONG value = 0;
    NTSTATUS status;

    RtlZeroMemory(Arm, sizeof(*Arm));
    Arm->Status = STATUS_NOT_SUPPORTED;
    if (!Immediate) return;
    if (Device->Mmio == NULL || !Device->DcnWriteEnabled) { Arm->Status = STATUS_DEVICE_NOT_READY; return; }
    // A clear event alone cannot attribute a later event to this request: an older pending flip may finish after
    // the clear. L excludes other software writers; both pending samples must also exclude that hardware work.
    status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &Arm->PendingBefore);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, DCN_WITNESS_EVENT_REG, &Arm->InterruptBefore);
    if (NT_SUCCESS(status))
        status = MmioDcnWriteEx(Device, DCN_WITNESS_EVENT_REG,
                                (Arm->InterruptBefore & DCN_WITNESS_ENABLES) | DCN_WITNESS_CLEARS, TRUE);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, DCN_WITNESS_EVENT_REG, &value);
    if (NT_SUCCESS(status) && (value & DCN_WITNESS_CLEARS))
    {
        status = MmioDcnWriteEx(Device, DCN_WITNESS_EVENT_REG, value & DCN_WITNESS_ENABLES, TRUE);
        if (NT_SUCCESS(status)) status = MmioDcnRead(Device, DCN_WITNESS_EVENT_REG, &value);
    }
    Arm->InterruptCleared = value;
    if (NT_SUCCESS(status))
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &Arm->PendingAfter);
    Arm->Status = status;
    Arm->Fresh = NT_SUCCESS(status) && !(value & DCN_WITNESS_EVENTS) &&
        !((Arm->PendingBefore | Arm->PendingAfter) & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK);
}

NTSTATUS DcnWitnessRead(_In_ const BC250_DEVICE* Device, _Out_ BC250_DCN_WITNESS_READ* Read)
{
    NTSTATUS status;

    RtlZeroMemory(Read, sizeof(*Read));
    if (Device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    status = MmioDcnRead(Device, DCN_WITNESS_EVENT_REG, &Read->Interrupt);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, DCN_WITNESS_LOW_REG, &Read->Low1);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, DCN_WITNESS_HIGH_REG, &Read->High1);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, DCN_WITNESS_HIGH_REG, &Read->High2);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, DCN_WITNESS_LOW_REG, &Read->Low2);
    Read->Event = (Read->Interrupt & HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK) != 0;
    Read->Earliest1 = ((ULONGLONG)(Read->High1 & DCN_WITNESS_HIGH_MASK) << 32) | (Read->Low1 & DCN_WITNESS_LOW_MASK);
    Read->Earliest2 = ((ULONGLONG)(Read->High2 & DCN_WITNESS_HIGH_MASK) << 32) | (Read->Low2 & DCN_WITNESS_LOW_MASK);
    return status;
}
#endif

// Caller owns the primary writer, or explicitly holds OS bugcheck/stop serialization.
// No allocation, software lock, logger or floating-point work is reachable.
static NTSTATUS DcnReservedSurfaceCore(_Inout_ BC250_DEVICE* Device, ULONG Width, ULONG Height, ULONG Pitch,
    BOOLEAN Visible, BOOLEAN PreserveVisibility, BOOLEAN Serialized)
{
    BC250_DISP_IO io={0};
    BC250_PLANE_REGISTERS format;
    ULONGLONG bytes,scanned=0;
    ULONG waited=0,pending=0,pitch=0,blank=0;
    BOOLEAN restoreVisible;
    NTSTATUS status;
    if (!Device->RuntimeFramebuffer || !Device->DcnPlaneFormats ||
        !DcnSurfaceBytes(Width,Height,Pitch,&bytes) ||
        bytes>Device->RuntimeFramebufferLength ||
        !DcnAddressFits((ULONGLONG)Device->RuntimeFramebufferPhysical.QuadPart,
            (ULONGLONG)Device->VramPhysical.QuadPart,Device->VramLength,bytes) ||
        !DcnPlaneFormatRegisters(Device,BC250_PLANE_FORMAT_ARGB8888,&format))
        return STATUS_DEVICE_NOT_READY;
    if (!Serialized) {
        status = WddmWriterResume(Device);
        if (!NT_SUCCESS(status)) return status;
    }
    io.Context=Device; io.Read=RuntimeRead; io.Write=RuntimeWrite; io.Stall=RuntimeStall;
    io.BudgetUs=BC250_DCNFLIP_POLL_MAX_US;
    // Read hardware, not the cached DcnBlanked flag. A pending blank request
    // is preserved as blank even if CURRENT_BLANK_STATE has not caught up.
    status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&blank);
    if (!NT_SUCCESS(status)) return status;
    restoreVisible=Visible || (PreserveVisibility && !(blank & (OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
        OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DE_MODE_MASK |
        OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK)));
    status=DcnSetVisibility(Device,FALSE);
    if (!NT_SUCCESS(status)) goto failed;
    InterlockedExchange(&Device->RuntimePlaneBlank,1);
    status=Bc250Dcn201PlaneBlank(&io,1);
    if (!NT_SUCCESS(status)) goto failed;
    // Begin already proved a driver-owned bridge before publishing the writer. Only reserved buffers can be
    // programmed outside L; application requests are refused while the writer owns the display.
    status=DcnFlipWriteSequence(Device,(ULONGLONG)Device->RuntimeFramebufferPhysical.QuadPart,
        Pitch,4,&format,FALSE,TRUE);
    if (!NT_SUCCESS(status)) goto failed;
    // DCN201 selects dcn10_program_pipe (Linux dcn201_init.c:101), which
    // programs the address then unblanks HUBP (dcn10_hwseq.c:3133-3136).
    // hubp1_set_blank also disables TTU; no in-use progress while blank is
    // guaranteed. Resume fetch from the validated reserved buffer with OTG
    // output still blank, then prove the frame-latched address took effect.
    status=Bc250Dcn201PlaneBlank(&io,0);
    if (!NT_SUCCESS(status)) goto failed;
    InterlockedExchange(&Device->RuntimePlaneBlank,0);
    for (;;) {
        status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,&pending);
        if (NT_SUCCESS(status)) status=DcnReadScanoutPhysical(Device,&scanned);
        if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&pitch);
        if (!NT_SUCCESS(status)) goto failed;
        if (!(pending&HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) &&
            scanned==(ULONGLONG)Device->RuntimeFramebufferPhysical.QuadPart &&
            ((pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)+1)*4==Pitch) break;
        if (waited>=BC250_DCNFLIP_POLL_MAX_US) { status=STATUS_IO_TIMEOUT; goto failed; }
        KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
        waited+=BC250_DCNFLIP_POLL_STEP_US;
    }
    if (restoreVisible) {
        status=DcnSetVisibility(Device,TRUE);
        if (!NT_SUCCESS(status)) goto failed;
    }
    Device->DcnCurrentAddress=(ULONGLONG)Device->RuntimeFramebufferPhysical.QuadPart;
    Device->DcnCurrentPitch=Pitch;
    Device->DcnPlaneFormat=BC250_PLANE_FORMAT_ARGB8888;
    Device->DcnDiverged=TRUE; // different from the immutable firmware surface
    return STATUS_SUCCESS;
failed:
    // Best-effort bounded cleanup. Preserve the original failure; a failed
    // MMIO cleanup cannot establish physical blanking or a drained plane.
    (void)DcnSetVisibility(Device,FALSE);
    InterlockedExchange(&Device->RuntimePlaneBlank,1);
    (void)Bc250Dcn201PlaneBlank(&io,1);
    return status;
}

static NTSTATUS DcnRuntimeSurfaceAuthorized(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible, BOOLEAN Serialized)
{
    if (!InterlockedCompareExchange(&Device->RuntimePostValid,0,0) ||
        Device->RuntimePost.PhysicAddress.QuadPart!=Device->RuntimeFramebufferPhysical.QuadPart)
        return STATUS_DEVICE_NOT_READY;
    return DcnReservedSurfaceCore(Device,Device->RuntimePost.Width,Device->RuntimePost.Height,
        Device->RuntimePost.Pitch,Visible,TRUE,Serialized);
}

NTSTATUS DcnRuntimeSurface(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible)
{
    return DcnRuntimeSurfaceAuthorized(Device,Visible,FALSE);
}

// Caller holds primary exclusion. No writes or ownership changes occur here.
NTSTATUS DcnLinkSurfaceCapture(BC250_DEVICE* Device, ULONG Width, ULONG Height, BC250_LINK_SURFACE* Surface)
{
    BC250_LINK_SURFACE saved={0};ULONGLONG bytes,again;
    ULONG pending=0,pitch=0,config=0,crossbar=0,cnvc=0;
    ULONG blank=0,hubp=0,blankAgain=0,hubpAgain=0;NTSTATUS status;
    if (!Surface) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Surface,sizeof(*Surface));
    if (!Device->VramEnabled || !Device->DcnPlaneFormats) return STATUS_DEVICE_NOT_READY;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,&pending);
    if (!NT_SUCCESS(status)) return status;
    if (pending&HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return STATUS_DEVICE_BUSY;
    status=DcnReadScanoutPhysical(Device,&saved.Physical);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&pitch);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG,&config);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL,&crossbar);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT,&cnvc);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&blank);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBP0_DCHUBP_CNTL,&hubp);
    if (!NT_SUCCESS(status)) return status;
    // A request/current disagreement is a transition, not a saved visibility.
    if (!!(blank&(OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
                  OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DE_MODE_MASK)) !=
        !!(blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK))
        return STATUS_DEVICE_BUSY;
    saved.Visible=!(blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK) &&
        !(hubp&HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK);
    saved.Format=DcnPlaneFormatDecodeRegisters(config,crossbar,cnvc);
    saved.Pitch=((pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)+1)*4;
    saved.Width=Width;saved.Height=Height;
    if (saved.Format!=BC250_PLANE_FORMAT_ARGB8888 || !DcnSurfaceBytes(Width,Height,saved.Pitch,&bytes) ||
        !DcnAddressFits(saved.Physical,(ULONGLONG)Device->VramPhysical.QuadPart,Device->VramLength,bytes))
        return STATUS_INVALID_PARAMETER;
    status=DcnReadScanoutPhysical(Device,&again);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,&pending);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,&blankAgain);
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBP0_DCHUBP_CNTL,&hubpAgain);
    if (!NT_SUCCESS(status)) return status;
    if (((blank^blankAgain)&(OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
          OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DE_MODE_MASK |
          OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK)) ||
        ((hubp^hubpAgain)&HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK))
        return STATUS_DEVICE_BUSY;
    if (again!=saved.Physical || (pending&HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK))
        return STATUS_DEVICE_BUSY;
    *Surface=saved;return STATUS_SUCCESS;
}

NTSTATUS DcnLinkSurfaceRestore(BC250_DEVICE* Device, const BC250_LINK_SURFACE* Surface)
{
    // Rollback restores the mode and its visibility, not a saved application allocation. The reserved runtime
    // buffer outlives the long writer; the latest saved application primary is selected only by End under L.
    if (!Surface || Surface->Format!=BC250_PLANE_FORMAT_ARGB8888 || Surface->Visible>1 ||
        Surface->Physical!=(ULONGLONG)Device->RuntimeFramebufferPhysical.QuadPart)
        return STATUS_DEVICE_NOT_READY;
    return DcnReservedSurfaceCore(Device,Surface->Width,Surface->Height,Surface->Pitch,
                                 Surface->Visible != 0 ? TRUE : FALSE,FALSE,FALSE);
}

NTSTATUS DcnStop(_Inout_ BC250_DEVICE* Device)
{
    NTSTATUS status;
    DcnUnmapScanout(Device);
    status=DcnRestorePostDisplaySerialized(Device);
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

// BD-197: whether DcnFlipSourceAddress programs a plane format other than the firmware's at this moment. wddm.c
// publishes the answer as BC250_SCANOUT_CAPS_PLANE_FORMATS, and the shells ask again for each primary, so the
// answer follows a mode change. The conditions are the flip path's own (DcnFlipSourceAddress): the start captured
// the firmware's format, and either a runtime mode of the connector is on screen with its surface valid, or the
// pipe is not the fixed inherited one. A runtime mode replaces the inherited pipe with the commit's own, which
// takes every format, and an enabled connector that still shows its inherited mode does too. While a commit or a
// detach has no valid runtime surface, the flip path refuses every format, so the answer is no. The trailer of
// 0.7.216.62 to 0.7.216.103 refused the other formats for every enabled connector, so on a DP screen an RGBA8
// chain was never a scan-out primary and DWM composed it.
BOOLEAN DcnPlaneFormatsOpen(_In_ BC250_DEVICE* Device)
{
    if (!Device->DcnPlaneFormats) return FALSE;
    if (InterlockedCompareExchange(&Device->RuntimeDisplayChanged, 0, 0))
        return InterlockedCompareExchange(&Device->RuntimePostValid, 0, 0) != 0;
    return !Device->Modeset.InheritedFixed;
}

void DcnPipeConnectorPlan(_Inout_ BC250_DEVICE* Device, ULONG DetPlane1)
{
    Device->DcnPipeDetPlane1 = DetPlane1;
    Device->DcnPipeConnector = TRUE;
}

// SetVidPnSourceAddress's own flip: the same M87 write sequence DcnFlipCore uses (DcnFlipWriteSequence), never
// PollFlipPending's 50 ms busy-wait - d3dkmddi.h allows this DDI up to PROFILE_LEVEL - 1, where a busy-wait of
// that length would stall the DPC that is supposed to report the flip's completion (WddmDcnVsync, wddm.c;
// DcnFlipPending below is that DPC's bounded, non-blocking observation). CardAddress is what dxgkrnl's
// DXGKARG_SETVIDPNSOURCEADDRESS.PrimaryAddress carries for this adapter's one segment - BaseAddress-relative
// (Device->VramMcBase + an offset, wddm.c's WddmQuerySegment4), not the physical address DCN wants - and
// DcnTranslateCardAddress (dcn_translate.c) is the inverse of that same arithmetic (facts M85), run from what
// VramStart measured, never a literal. PhysicalOut, if not NULL, gets the translated address for the caller's
// own log line.
// M15.14 (0.7.216.20): PlaneFormat (plane_format.h) gives the pitch's unit and, when it differs from the plane's
// current format, the three format registers the same update writes. The plane stays in a format until a flip of
// another one, so a run of game frames writes the format once and the desktop's next BGRA8 flip writes it back.
// BD-170: Immediate is the request's FlipImmediate; the write sequence above says what the hardware does with it.
NTSTATUS DcnFlipSourceAddress(_Inout_ BC250_DEVICE* Device, ULONGLONG CardAddress, ULONG Pitch, ULONG PlaneFormat,
    ULONGLONG AllocationBytes, BOOLEAN Immediate, _Out_opt_ ULONGLONG* PhysicalOut, _Out_ BOOLEAN* Wrote)
{
    ULONGLONG physical,bytes;
    NTSTATUS status;
    const BC250_PLANE_ENCODING* plane=Bc250PlaneEncoding(PlaneFormat);
    BC250_PLANE_REGISTERS registers;
    const BC250_PLANE_REGISTERS* format=NULL;
    ULONG current;

    *Wrote = FALSE;
    if (Device->Mmio == NULL || !Device->VidPnFlipEnabled) return STATUS_DEVICE_NOT_READY;
    // The scaled inherited path retains the GOP viewport and surface layout.
    // A new pitch or format needs a separately admitted pipe configuration.
    // BD-197: a runtime mode's pipe is the connector commit's own. Its DML plan is for a 32-bit 444 surface at
    // RuntimePost.Pitch (connector_link.c Plan), the source format of every plane encoding (plane_format.h, the
    // same-size rule), and the format registers keep its DET base (DcnPlaneFormatRegisters). So it takes every
    // format at that pitch, where 0.7.216.62 to 0.7.216.103 took ARGB8888 only.
    if (InterlockedCompareExchange(&Device->RuntimeDisplayChanged,0,0)) {
        if (!InterlockedCompareExchange(&Device->RuntimePostValid,0,0) ||
            Pitch!=Device->RuntimePost.Pitch ||
            DisplaySourceWidth(Device)!=Device->RuntimePost.Width ||
            DisplaySourceHeight(Device)!=Device->RuntimePost.Height) return STATUS_NOT_SUPPORTED;
    } else if (Device->Modeset.InheritedFixed && (!Device->Modeset.InheritedSupport ||
        Pitch != Device->Post.Pitch || PlaneFormat != BC250_PLANE_FORMAT_ARGB8888)) return STATUS_NOT_SUPPORTED;
    if (plane==NULL ||
        !DcnLinearSurfaceBytes(DisplaySourceWidth(Device),DisplaySourceHeight(Device),Pitch,plane->BytesPerPixel,
                               &bytes) ||
        bytes>AllocationBytes) return STATUS_INVALID_PARAMETER;
    // A format other than the firmware's needs the firmware's format captured at start: restore writes it back.
    // Refused before any register is touched, so the plane keeps its frame. wddm.c does not offer the flip to the
    // compositor's driver in that case (BC250_SCANOUT_CAPS_PLANE_FORMATS), so this is a guard, not a path.
    current=Device->DcnPlaneFormats ? Device->DcnPlaneFormat : BC250_PLANE_FORMAT_ARGB8888;
    if (PlaneFormat!=current)
    {
        if (!DcnPlaneFormatRegisters(Device,PlaneFormat,&registers))
        {
            InterlockedIncrement(&Device->DcnFormatRefused);
            return STATUS_NOT_SUPPORTED;
        }
        format=&registers;
    }
    status=CaptureFirmwareSurface(Device);
    if (!NT_SUCCESS(status)) return status;
    if (!Device->VramEnabled ||
        !DcnTranslateCardAddress(CardAddress, Device->VramMcBase, (ULONGLONG)Device->VramPhysical.QuadPart,
                                 Device->VramLength, &physical) ||
        !AddressAllowed(Device,physical,Pitch,DisplaySourceWidth(Device),DisplaySourceHeight(Device)))
    {
        InterlockedIncrement(&Device->DcnFlipRefused);
        return STATUS_ACCESS_DENIED;
    }
    if (PhysicalOut != NULL) *PhysicalOut = physical;


    // Quiet = TRUE (review 16 section 24): a real present, potentially once per frame for as long as the
    // desktop is up - MmioDcnWrite's per-write GuardLog was sized for the escape's occasional calls, not this.
    // Only compact counters change here. The caller emits diagnostics after leaving the interrupt section.
    InterlockedIncrement(&Device->DcnSurfaceSequence);
    // BD-170 r6: from here on a failure is not proof that nothing was written (an MMIO failure is unknown).
    *Wrote = TRUE;
    status=DcnFlipWriteSequence(Device,physical,Pitch,plane->BytesPerPixel,format,Immediate,TRUE);
    if (NT_SUCCESS(status))
    {
        if (Immediate) InterlockedIncrement(&Device->DcnImmediateFlips);
        // Publish address, pitch and format together under DcnSurfaceSequence. The
        // PASSIVE_LEVEL CPU mapper rejects a snapshot crossing this update.
        Device->DcnCurrentAddress=physical;
        Device->DcnCurrentPitch=Pitch;
        if (format!=NULL)
        {
            Device->DcnPlaneFormat=PlaneFormat;
            InterlockedIncrement(&Device->DcnFormatChanges);
        }
        Device->DcnDiverged=(physical!=Device->DcnFirmwareAddress || Pitch!=Device->DcnFirmwarePitch ||
                             PlaneFormat!=BC250_PLANE_FORMAT_ARGB8888);
        InterlockedIncrement(&Device->DcnFlipsHardware);
    }
    else InterlockedIncrement(&Device->DcnFlipRefused);
    InterlockedIncrement(&Device->DcnSurfaceSequence);
    return status;
}

static BOOLEAN DcnWriterBridgeCurrent(_In_ const BC250_DEVICE* Device, ULONGLONG Physical, ULONG Pitch)
{
    ULONG pending = 0, pitch = 0, config = 0, crossbar = 0, cnvc = 0;
    ULONGLONG scanned = 0, again = 0, primary = 0;
    NTSTATUS status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &pending);
    if (!NT_SUCCESS(status) || (pending & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK)) return FALSE;
    status = DcnReadScanoutPhysical(Device, &scanned);
    if (NT_SUCCESS(status)) status = DcnReadPrimaryPhysical(Device, &primary);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH, &pitch);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG, &config);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL, &crossbar);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT, &cnvc);
    if (NT_SUCCESS(status)) status = DcnReadScanoutPhysical(Device, &again);
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &pending);
    return NT_SUCCESS(status) && !(pending & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) &&
        scanned == Physical && again == Physical && primary == Physical &&
        ((pitch & HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) + 1) * 4 == Pitch &&
        DcnPlaneFormatDecodeRegisters(config, crossbar, cnvc) == BC250_PLANE_FORMAT_ARGB8888;
}

// The bridge pitch of the reserved runtime buffer: the runtime mode's once a link changed the display, else the
// pitch of the inherited or the committed source mode.
static ULONG DcnRuntimeBridgePitch(_In_ BC250_DEVICE* Device)
{
    return InterlockedCompareExchange(&Device->RuntimeDisplayChanged, 0, 0) ? Device->RuntimePost.Pitch :
        (Device->Modeset.InheritedFixed ? Device->Post.Pitch : DcnPrimaryPitch(DisplaySourceWidth(Device)));
}

// BD-195, invariant I3 (docs/design/vidpn-flip.md section 7.2): RuntimeDisplayChanged 1 needs a nonzero
// RuntimePost.Pitch. A link writer calls this right after its Begin, before it sets RuntimeDisplayChanged. Until
// the first Apply, RuntimePost is empty; 31v2's first detach set the flag over it, and every later bridge refused
// pitch 0 without a write or a log. This describes the reserved buffer at the pitch Begin's bridge used for it, so
// a later Begin finds that bridge current with HUBP blank. State only: no register access, and RuntimePostValid
// stays as it is, so presents and the system display paths stay refused until a commit applies a mode.
void DcnLinkSurfaceAdopt(BC250_DEVICE* Device)
{
    if (InterlockedCompareExchange(&Device->RuntimeDisplayChanged, 0, 0) || !Device->RuntimeFramebuffer ||
        !Device->RuntimeFramebufferLength) return;
    Device->RuntimePost = Device->Post;
    Device->RuntimePost.Width = DisplaySourceWidth(Device);
    Device->RuntimePost.Height = DisplaySourceHeight(Device);
    Device->RuntimePost.Pitch = DcnRuntimeBridgePitch(Device);
    Device->RuntimePost.PhysicAddress = Device->RuntimeFramebufferPhysical;
    Device->RuntimePost.ColorFormat = D3DDDIFMT_A8R8G8B8;
    Device->RuntimePost.TargetId = BC250_CHILD_UID;
}

// WriterActive and the odd generation are published under L before this call. The old application allocation
// stays alive while the bridge is programmed, and while Retire below waits outside L for its effective address.
// One stable already-owned surface can avoid programming, including an inherited pipe whose timing is stopped.
// Admission and the register sequence are bounded under L; no wait for EARLIEST to catch up happens here.
NTSTATUS DcnWriterBridge(_Inout_ BC250_DEVICE* Device, _In_ volatile LONG* Generation, ULONG ExpectedGeneration,
    _Out_ BOOLEAN* Wrote, _Out_ BC250_DCN_WRITER_BRIDGE* Bridge)
{
    ULONGLONG physical, length, bytes, offset, card, programmed = 0;
    ULONG pitch;
    BOOLEAN current;
    NTSTATUS status;
    LARGE_INTEGER start, now;

    *Wrote = FALSE;
    RtlZeroMemory(Bridge, sizeof(*Bridge));
    if (!Device->Mmio || !Device->VidPnFlipEnabled || !Device->VramEnabled) return STATUS_DEVICE_NOT_READY;
    if (Device->DcnQpcFrequency <= 0) return STATUS_DEVICE_NOT_READY;
    if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration) return STATUS_DEVICE_BUSY;
    start = KeQueryPerformanceCounter(NULL);
    // Firmware remains a valid owned bridge even when the preferred runtime buffer is available but has not yet
    // been latched. This covers a stopped inherited pipe without a write that would need the stopped timing.
    if (Device->Framebuffer && Device->DcnFirmwareKnown && Device->FramebufferLength &&
        Device->DcnFirmwareAddress == (ULONGLONG)Device->Post.PhysicAddress.QuadPart &&
        Device->DcnFirmwarePitch == Device->Post.Pitch &&
        DcnSurfaceBytes(DisplaySourceWidth(Device), DisplaySourceHeight(Device), Device->DcnFirmwarePitch, &bytes) &&
        bytes <= Device->FramebufferLength &&
        DcnAddressFits(Device->DcnFirmwareAddress, (ULONGLONG)Device->VramPhysical.QuadPart, Device->VramLength, bytes) &&
        DcnWriterBridgeCurrent(Device, Device->DcnFirmwareAddress, Device->DcnFirmwarePitch)) {
        now = KeQueryPerformanceCounter(NULL);
        if (DcnQpcUs(now.QuadPart - start.QuadPart, Device->DcnQpcFrequency) >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US)
            return STATUS_IO_TIMEOUT;
        if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration) return STATUS_DEVICE_BUSY;
        Bridge->Physical = Device->DcnFirmwareAddress;
        Bridge->Pitch = Device->DcnFirmwarePitch;
        Bridge->ProgramEndQpc = now.QuadPart;
        Bridge->Retired = TRUE;
        return STATUS_SUCCESS;
    }
    if (Device->RuntimeFramebuffer && Device->RuntimeFramebufferLength) {
        physical = (ULONGLONG)Device->RuntimeFramebufferPhysical.QuadPart;
        length = Device->RuntimeFramebufferLength;
        pitch = DcnRuntimeBridgePitch(Device);
        // I3 broken (no DcnLinkSurfaceAdopt before the flag): say so, never a size check's INVALID_PARAMETER.
        if (!pitch && InterlockedCompareExchange(&Device->RuntimeDisplayChanged, 0, 0))
            return STATUS_INVALID_DEVICE_STATE;
    } else if (Device->Framebuffer && Device->DcnFirmwareKnown && Device->FramebufferLength &&
               Device->DcnFirmwareAddress == (ULONGLONG)Device->Post.PhysicAddress.QuadPart &&
               Device->DcnFirmwarePitch == Device->Post.Pitch) {
        physical = Device->DcnFirmwareAddress;
        length = Device->FramebufferLength;
        pitch = Device->DcnFirmwarePitch;
    } else return STATUS_DEVICE_NOT_READY;
    if (!DcnSurfaceBytes(DisplaySourceWidth(Device), DisplaySourceHeight(Device), pitch, &bytes) || bytes > length ||
        !DcnAddressFits(physical, (ULONGLONG)Device->VramPhysical.QuadPart, Device->VramLength, bytes))
        return STATUS_INVALID_PARAMETER;
    // A prior writer or firmware may already own the effective surface, including while OTG is stopped. Require
    // the exact validated bridge, stable effective address, no pending request, and matching pitch and format.
    now = KeQueryPerformanceCounter(NULL);
    if (DcnQpcUs(now.QuadPart - start.QuadPart, Device->DcnQpcFrequency) >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US)
        return STATUS_IO_TIMEOUT;
    current = DcnWriterBridgeCurrent(Device, physical, pitch);
    now = KeQueryPerformanceCounter(NULL);
    if (DcnQpcUs(now.QuadPart - start.QuadPart, Device->DcnQpcFrequency) >= (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US)
        return STATUS_IO_TIMEOUT;
    if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration) return STATUS_DEVICE_BUSY;
    Bridge->Physical = physical;
    Bridge->Pitch = pitch;
    Bridge->ProgramEndQpc = now.QuadPart;
    if (current) {
        Bridge->Retired = TRUE;
        return STATUS_SUCCESS;
    }
    offset = physical - (ULONGLONG)Device->VramPhysical.QuadPart;
    card = Device->VramMcBase + offset;
    if (card < Device->VramMcBase) return STATUS_INVALID_PARAMETER;
    status = DcnFlipSourceAddress(Device, card, pitch, BC250_PLANE_FORMAT_ARGB8888, length, TRUE, &programmed, Wrote);
    now = KeQueryPerformanceCounter(NULL);
    Bridge->ProgramEndQpc = now.QuadPart;
    if (!NT_SUCCESS(status)) return status;
    if (programmed != physical) return STATUS_INVALID_DEVICE_STATE;
    if (DcnQpcUs(now.QuadPart - start.QuadPart, Device->DcnQpcFrequency) > (LONG)BC250_DCN_IMMEDIATE_OBSERVE_US) {
        InterlockedIncrement(&Device->DcnImmediateOverruns);
        return STATUS_IO_TIMEOUT;
    }
    if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration) return STATUS_DEVICE_BUSY;
    return STATUS_SUCCESS;
}

// EARLIEST can follow the immediate request at the next vblank (M902). Waiting for it under L within 50 us
// falsely faulted a normal one-frame delay. Keep the writer's exclusion gate, but wait only at PASSIVE_LEVEL.
// Every sample must fit the original deadline, including entry delay, read time and an overscheduled sleep.
NTSTATUS DcnWriterBridgeRetire(_Inout_ BC250_DEVICE* Device, _In_ volatile LONG* Generation,
    ULONG ExpectedGeneration, _In_ const BC250_DCN_WRITER_BRIDGE* Bridge)
{
    LARGE_INTEGER now;
    LONG elapsed;
    ULONG steps;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (!Device->Mmio || Device->DcnQpcFrequency <= 0 || Bridge->Physical == 0 || Bridge->Pitch == 0)
        return STATUS_DEVICE_NOT_READY;
    if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration) return STATUS_DEVICE_BUSY;
    if (Bridge->Retired) return STATUS_SUCCESS;
    for (steps = 0; steps < BC250_DCN_WRITER_RETIRE_US / 50u; steps++) {
        BOOLEAN effective;
        now = KeQueryPerformanceCounter(NULL);
        elapsed = DcnQpcUs(now.QuadPart - Bridge->ProgramEndQpc, Device->DcnQpcFrequency);
        if (elapsed < 0 || elapsed >= (LONG)BC250_DCN_WRITER_RETIRE_US) break;
        if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration) return STATUS_DEVICE_BUSY;
        effective = DcnWriterBridgeCurrent(Device, Bridge->Physical, Bridge->Pitch);
        now = KeQueryPerformanceCounter(NULL);
        elapsed = DcnQpcUs(now.QuadPart - Bridge->ProgramEndQpc, Device->DcnQpcFrequency);
        if (elapsed < 0 || elapsed > (LONG)BC250_DCN_WRITER_RETIRE_US) break;
        if ((ULONG)InterlockedCompareExchange(Generation, 0, 0) != ExpectedGeneration) return STATUS_DEVICE_BUSY;
        if (effective) return STATUS_SUCCESS;
        if (elapsed >= (LONG)BC250_DCN_WRITER_RETIRE_US) break;
        // A coarse scheduler timer can sleep from 15.6 ms to 31.2 ms and miss a one-frame retirement entirely.
        // This rare writer acquisition instead polls at PASSIVE with short stalls and no software lock held.
        // Interrupts/preemption remain enabled; the next sample earns its budget again after any scheduling delay.
        KeStallExecutionProcessor((ULONG)(((LONG)BC250_DCN_WRITER_RETIRE_US - elapsed < 50) ?
            (LONG)BC250_DCN_WRITER_RETIRE_US - elapsed : 50));
    }
    return STATUS_IO_TIMEOUT;
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
    ULONG pitch,format;
    BOOLEAN diverged;
    PHYSICAL_ADDRESS phys;
    SIZE_T length;
    *Mapping=NULL;*Length=0;*Pitch=0;
    if (generation&1) return FALSE;
    target=(ULONGLONG)InterlockedCompareExchange64((volatile LONG64*)&Device->DcnCurrentAddress,0,0);
    pitch=Device->DcnCurrentPitch;diverged=Device->DcnDiverged;
    // M15.14: the CPU copy writes B8G8R8A8 pixels 4 bytes apart, so it lands only on a plane of that format. A
    // client buffer of another format scanned out (0.7.216.20) gets no CPU copy; the next desktop flip ends it.
    format=Device->DcnPlaneFormats ? Device->DcnPlaneFormat : BC250_PLANE_FORMAT_ARGB8888;
    if (InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0)!=generation || !diverged ||
        format!=BC250_PLANE_FORMAT_ARGB8888 ||
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
