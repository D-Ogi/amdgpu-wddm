// Display modes, stage A (docs/design/display-modes.md): the EDID over DP AUX at start, the monitor descriptor, the
// source modes, and the commit that puts a source mode on pipe 0 through the scaler at the unchanged native timing.
// The register work is in plain C files that the host tests compile as they are: dpaux.c (AUX and EDID), edid.c
// (parser and mode list), dcn_scale.c (scaler math and register sequence), display_io.c (the checked accessors).
// This file gives them BAR5 and a busy stall, and owns the state in BC250_MODESET (modeset.h).
//
// The switch EnableDisplayModes (Parameters, REG_DWORD, read at start): 0 = the driver before this feature (no AUX
// access, no descriptor, one source mode, no scaler write), 1 = the EDID and the descriptor, smaller modes
// centered 1:1, 2 = the default: smaller modes scaled as the VidPN path asks (aspect ratio kept, stretched or
// centered). A value above 2 counts as 0.
#include "bc250kmd.h"
#include "dpaux.h"
#include "dcn_2_0_1_sh_mask.h"
#include <ntstrsafe.h>

#define MODESET_SETTING L"EnableDisplayModes"
#define MODESET_AUX_BUDGET_US 200000ul      // the whole EDID read: E03 measured 780-920 us per transaction, ~40 here
#define MODESET_PIPE_BUDGET_US 1000ul       // one pipe update: the lock acknowledgement takes at most ten 1 us stalls
#define MODESET_LOG_MODES 6ul               // modes per log line

// ---- the accessors display_io.c calls -----------------------------------------------------------------------
//
// display_io.c admits only the offsets of gen_regs.py's display tables; checked again here, as dpaudio.c does,
// so that nothing in this file reaches BAR5 past them. A write needs EnableDcnWrite (Device->DcnWriteEnabled), the
// same gate as every other DCN write of this driver. Neither callback logs or locks: the quiet restore runs them at
// high IRQL on the way to a bugcheck.
static long ModesetRead(void* Context, unsigned long Offset, unsigned long* Value)
{
    const BC250_DEVICE* device = (const BC250_DEVICE*)Context;

    *Value = 0;
    if (device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    if (!Bc250DispReadAllowed(Offset)) return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG((PULONG)&device->Mmio[Offset / 4]);
    return STATUS_SUCCESS;
}

static long ModesetWrite(void* Context, unsigned long Offset, unsigned long Value)
{
    const BC250_DEVICE* device = (const BC250_DEVICE*)Context;

    if (device->Mmio == NULL || !device->DcnWriteEnabled) return STATUS_DEVICE_NOT_READY;
    if (!Bc250DispWriteAllowed(Offset)) return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG((PULONG)&device->Mmio[Offset / 4], Value);
    return STATUS_SUCCESS;
}

static void ModesetStall(void* Context, unsigned long Microseconds)
{
    UNREFERENCED_PARAMETER(Context);
    KeStallExecutionProcessor(Microseconds);
}

static void IoOpen(_In_ BC250_DEVICE* Device, _Out_ BC250_DISP_IO* Io, ULONG BudgetUs)
{
    RtlZeroMemory(Io, sizeof(*Io));
    Io->Context = Device;
    Io->Read = ModesetRead;
    Io->Write = ModesetWrite;
    Io->Stall = ModesetStall;
    Io->BudgetUs = BudgetUs;
}

// ---- start ------------------------------------------------------------------------------------------------

void ModesetInitialize(_Inout_ BC250_DEVICE* Device)
{
    RtlZeroMemory(&Device->Modeset, sizeof(Device->Modeset));
    ExInitializeFastMutex(&Device->Modeset.Lock);
}

// The EDID and the DPCD receiver capabilities over AUX. Under the mutex, PASSIVE_LEVEL (APC_LEVEL in the mutex).
static void ReadMonitor(_Inout_ BC250_DEVICE* Device)
{
    BC250_MODESET* m = &Device->Modeset;
    BC250_DISP_IO io;
    BC250_AUX_SESSION aux;
    unsigned long length = 0;
    long status, close;

    RtlZeroMemory(&aux, sizeof(aux));
    IoOpen(Device, &io, MODESET_AUX_BUDGET_US);
    status = Bc250AuxOpen(&aux, &io);
    if (status < 0) {
        m->AuxStatus = status;
        GuardLog("display modes: AUX not opened 0x%08X (AUX_CONTROL 0x%08X)", (ULONG)status, aux.AuxControl);
        return;
    }
    // The DPCD first: a sink that answers it is a DP sink, and its link fields are stage B's starting point.
    m->DpcdValid = Bc250AuxDpcdRead(&aux, 0, m->Dpcd, BC250_MODESET_DPCD_BYTES) >= 0;
    status = Bc250AuxReadEdid(&aux, m->Edid, sizeof(m->Edid), &length);
    close = Bc250AuxClose(&aux);
    m->AuxStatus = status < 0 ? status : close;
    m->EdidLength = length;
    m->AuxStallUs = io.StalledUs;
    GuardLog("display modes: AUX %lu transactions %lu attempts, %lu defers %lu timeouts %lu invalid %lu nacks %lu busy",
             aux.Transactions, aux.Attempts, aux.Defers, aux.Timeouts, aux.Invalid, aux.Nacks, aux.Busy);
    GuardLog("display modes: AUX %lu us stalled, EDID %lu bytes, status 0x%08X close 0x%08X",
             io.StalledUs, length, (ULONG)status, (ULONG)close);
    if (m->DpcdValid)
        GuardLog("display modes: DPCD rev 0x%02X max link rate 0x%02X max lanes %u (0x%02X) downspread 0x%02X",
                 m->Dpcd[0], m->Dpcd[1], (UINT)(m->Dpcd[2] & 0x1Fu), m->Dpcd[2], m->Dpcd[3]);
}

static void LogModes(_In_ const BC250_MODESET* Modes)
{
    char line[160];
    ULONG i, at;

    for (i = 0; i < Modes->Modes.Count; i += MODESET_LOG_MODES)
    {
        size_t used = 0;
        line[0] = '\0';
        for (at = i; at < Modes->Modes.Count && at < i + MODESET_LOG_MODES; at++)
        {
            const BC250_EDID_MODE* mode = &Modes->Modes.Modes[at];
            if (!NT_SUCCESS(RtlStringCbPrintfA(line + used, sizeof(line) - used, " %lux%lu/0x%02lX",
                                               mode->Width, mode->Height, mode->Source)))
                break;
            (void)RtlStringCbLengthA(line, sizeof(line), &used);
        }
        GuardLog("display modes: list %lu..%lu:%s", i, at - 1, line);
    }
}

void ModesetStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_MODESET* m = &Device->Modeset;
    ULONG requested = GuardReadSetting(MODESET_SETTING, BC250_DISPLAY_MODES_SCALED);    // PASSIVE_LEVEL: before the mutex
    ULONG maxLevel = BC250_DISPLAY_MODES_OFF;

    ExAcquireFastMutex(&m->Lock);
    m->Requested = requested <= BC250_DISPLAY_MODES_SCALED ? requested : BC250_DISPLAY_MODES_OFF;
    m->Level = BC250_DISPLAY_MODES_OFF;
    m->PipeVerdict = BC250_PIPE_VERDICT_COUNT;
    m->AuxStatus = STATUS_NOT_SUPPORTED;
    m->EdidReason = BC250_EDID_SHORT;
    m->EdidValid = m->DescriptorServed = m->DpcdValid = FALSE;
    m->PipeChanged = m->Suspended = FALSE;
    m->EdidLength = 0;
    RtlZeroMemory(m->Edid, sizeof(m->Edid));
    RtlZeroMemory(m->Dpcd, sizeof(m->Dpcd));
    RtlZeroMemory(&m->Info, sizeof(m->Info));
    RtlZeroMemory(&m->Plan, sizeof(m->Plan));
    RtlZeroMemory(&m->LastResult, sizeof(m->LastResult));
    m->LastStatus = STATUS_SUCCESS;
    m->Scaling = BC250_SCALING_IDENTITY;
    InterlockedExchange(&m->SourceWidth, (LONG)Device->Post.Width);
    InterlockedExchange(&m->SourceHeight, (LONG)Device->Post.Height);

    if (m->Requested == BC250_DISPLAY_MODES_OFF)
        GuardLog("display modes: off (EnableDisplayModes %lu): no AUX access, no descriptor, one mode", requested);
    else if (Device->Mmio == NULL || !Device->DcnWriteEnabled)
        GuardLog("display modes: no EDID: EnableMmio and EnableDcnWrite must both be 1 (BAR5 %u, DCN write %u)",
                 (UINT)(Device->Mmio != NULL), (UINT)Device->DcnWriteEnabled);
    else
    {
        BC250_DISP_IO io;
        BC250_PIPE_SHAPE shape;
        long status;

        ReadMonitor(Device);
        if (m->EdidLength >= BC250_EDID_BLOCK)
        {
            m->EdidReason = Bc250EdidParse(m->Edid, m->EdidLength, &m->Info);
            m->EdidValid = m->EdidReason == BC250_EDID_OK || m->EdidReason == BC250_EDID_EXTENSION_DROPPED;
            // A dropped extension is not served: the descriptor ends with the base block (Monitor.sys then gets
            // STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA for the extension the base block announces).
            if (m->EdidReason == BC250_EDID_EXTENSION_DROPPED) m->EdidLength = BC250_EDID_BLOCK;
        }
        // The identity, never the serial number: the parser does not keep it, and nothing here prints raw bytes.
        GuardLog("display modes: EDID %s, %lu bytes, %lu blocks, version %lu.%lu", Bc250EdidReasonText(m->EdidReason),
                 m->EdidLength, m->Info.Blocks, m->Info.Version, m->Info.Revision);
        GuardLog("display modes: monitor %s product 0x%04lX name '%s'", m->Info.Vendor, m->Info.ProductCode,
                 m->Info.HasName ? m->Info.Name : "");
        if (m->EdidValid && m->Info.HasPreferred)
            GuardLog("display modes: preferred %lux%lu %lu.%03lu Hz, pixel clock %lu0 kHz; native (firmware) %ux%u",
                     m->Info.Preferred.HActive, m->Info.Preferred.VActive, m->Info.PreferredRefreshMilliHz / 1000ul,
                     m->Info.PreferredRefreshMilliHz % 1000ul, m->Info.Preferred.PixelClock10Khz,
                     Device->Post.Width, Device->Post.Height);
        m->DescriptorServed = m->EdidValid;

        // The pipe the firmware lit. Reads only (no budget: no stall).
        RtlZeroMemory(&shape, sizeof(shape));
        IoOpen(Device, &io, 0);
        status = Bc250PipeShapeRead(&io, &shape);
        if (status >= 0) m->PipeVerdict = Bc250PipeShapeCheck(&shape, Device->Post.Width, Device->Post.Height, &maxLevel);
        GuardLog("display modes: pipe 0 %s (0x%08X): viewport 0x%08X/0x%08X recout 0x%08X/0x%08X mpc 0x%08X",
                 Bc250PipeVerdictText(m->PipeVerdict), (ULONG)status, shape.ViewportStart, shape.ViewportDimension,
                 shape.RecoutStart, shape.RecoutSize, shape.MpcSize);
        GuardLog("display modes: pipe 0 scl 0x%08X mem 0x%08X top %lu odm %lu hubp 0x%08X",
                 shape.SclMode, shape.MemPwrCtrl, shape.MpccTopSel, shape.OdmSource, shape.HubpCntl);
        if (status < 0) maxLevel = BC250_DISPLAY_MODES_OFF;
        // Scaled modes need the full table (the display-only table copies 4-byte pixels at the firmware's pitch).
        if (Device->FullWddm) m->Level = m->Requested < maxLevel ? m->Requested : maxLevel;
    }
    Bc250ModeListBuild(Device->Post.Width, Device->Post.Height, m->EdidValid ? &m->Info : NULL, m->Level, &m->Modes);
    GuardLog("display modes: requested %lu level %lu, %lu source modes, descriptor %s",
             m->Requested, m->Level, m->Modes.Count, m->DescriptorServed ? "served" : "not served");
    LogModes(m);
    ExReleaseFastMutex(&m->Lock);
}

// ---- commit -----------------------------------------------------------------------------------------------

ULONG ModesetScalingSupport(_In_ const BC250_DEVICE* Device, ULONG Width, ULONG Height)
{
    const BC250_MODESET* m = &Device->Modeset;

    if (Width == Device->Post.Width && Height == Device->Post.Height)
        return Bc250ScalingSupport(m->Level, Width, Height, Device->Post.Width, Device->Post.Height);
    if (!Bc250ModeListHas(&m->Modes, Width, Height)) return 0;
    return Bc250ScalingSupport(m->Level, Width, Height, Device->Post.Width, Device->Post.Height);
}

// The register work of a change, under the mutex and the primary transaction. Grow: the viewport becomes larger
// than the surface the plane may be reading (dxgkrnl's previous primary, or a game's swap-chain buffer), so the
// plane goes to the firmware surface first, which holds the native size. A shrink reads a part of what the plane
// reads now and needs no flip.
static long ProgramLocked(_Inout_ BC250_DEVICE* Device, _In_ const BC250_SCALER_PLAN* Plan, BOOLEAN Grow,
                          _Out_ BOOLEAN* Invalidate)
{
    BC250_MODESET* m = &Device->Modeset;
    BC250_DISP_IO io;
    long status;

    *Invalidate = FALSE;
    if (Grow && Device->DcnDiverged)
    {
        // Black, not the boot picture, for the frames before dxgkrnl's next flip.
        if (Device->Framebuffer != NULL) RtlZeroMemory(Device->Framebuffer, Device->FramebufferLength);
        status = DcnFlipToFirmwareSurface(Device);
        if (!NT_SUCCESS(status)) return status;
        *Invalidate = TRUE;
    }
    IoOpen(Device, &io, MODESET_PIPE_BUDGET_US);
    status = Bc250ScalerProgram(&io, Plan, &m->LastResult);
    if ((m->LastResult.HubpCntlAfter & HUBP0_DCHUBP_CNTL__HUBP_UNDERFLOW_STATUS_MASK) != 0) m->Underflows++;
    return status;
}

NTSTATUS ModesetCommit(_Inout_ BC250_DEVICE* Device, ULONG Width, ULONG Height, ULONG Scaling)
{
    BC250_MODESET* m = &Device->Modeset;
    const ULONG postWidth = Device->Post.Width, postHeight = Device->Post.Height;
    BC250_SCALER_PLAN plan;
    ULONG generation, support;
    BOOLEAN native = (Width == postWidth && Height == postHeight), invalidate = FALSE, grow;
    long status;

    support = ModesetScalingSupport(Device, Width, Height);
    if (support == 0) { m->Refusals++; return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE; }
    // At the native size every scaling is the 1:1 shape.
    if (native) Scaling = BC250_SCALING_IDENTITY;
    else if (Scaling >= BC250_SCALING_COUNT || !(support & BC250_SCALING_BIT(Scaling)))
        Scaling = Bc250ScalingDefault(m->Level, Width, Height, postWidth, postHeight);

    ExAcquireFastMutex(&m->Lock);
    m->Commits++;
    if (DisplaySourceWidth(Device) == Width && DisplaySourceHeight(Device) == Height && m->Scaling == Scaling)
    {
        ExReleaseFastMutex(&m->Lock);
        return STATUS_SUCCESS;          // the shape pipe 0 has: a redundant commit writes nothing
    }
    status = Bc250ScalerPlan(Width, Height, postWidth, postHeight, Scaling, &plan);
    if (status < 0 || m->Level == BC250_DISPLAY_MODES_OFF)
    {
        m->Refusals++;
        ExReleaseFastMutex(&m->Lock);
        GuardLog("display modes: commit %lux%lu %s refused: plan 0x%08X level %lu", Width, Height,
                 Bc250ScalingName(Scaling), (ULONG)status, m->Level);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    if (m->Suspended)
    {
        // D3: the committed shape is programmed again at D0 (ModesetPowerUp); remember it only.
        InterlockedExchange(&m->SourceWidth, (LONG)Width);
        InterlockedExchange(&m->SourceHeight, (LONG)Height);
        m->Scaling = Scaling;
        m->Plan = plan;
        ExReleaseFastMutex(&m->Lock);
        return STATUS_SUCCESS;
    }
    if (!WddmPrimaryExclusiveBegin(Device, &generation))
    {
        m->Busy++;
        ExReleaseFastMutex(&m->Lock);
        GuardLog("display modes: commit %lux%lu refused: a flip held the primary transaction for 2 ms", Width, Height);
        return STATUS_DEVICE_BUSY;
    }
    grow = Width > DisplaySourceWidth(Device) || Height > DisplaySourceHeight(Device);
    status = ProgramLocked(Device, &plan, grow, &invalidate);
    m->LastStatus = status;
    if (status >= 0)
    {
        InterlockedExchange(&m->SourceWidth, (LONG)Width);
        InterlockedExchange(&m->SourceHeight, (LONG)Height);
        m->Scaling = Scaling;
        m->Plan = plan;
        m->PipeChanged = !native;
        m->Changes++;
    }
    WddmPrimaryExclusiveEnd(Device, generation, invalidate);
    ExReleaseFastMutex(&m->Lock);
    GuardLog("display modes: commit %lux%lu %s -> 0x%08X%s", Width, Height, Bc250ScalingName(Scaling), (ULONG)status,
             invalidate ? ", firmware surface first" : "");
    GuardLog("display modes: viewport %lux%lu recout %lux%lu at %lu,%lu", plan.Viewport.Width, plan.Viewport.Height,
             plan.Recout.Width, plan.Recout.Height, plan.Recout.X, plan.Recout.Y);
    GuardLog("display modes: ratio 0x%05lX/0x%05lX taps %lu/%lu dscl %lu lb %lu", plan.RatioH19, plan.RatioV19,
             plan.TapsH, plan.TapsV, plan.DsclMode, plan.LbConfig);
    GuardLog("display modes: lock %lu us, %lu coefficients, scl 0x%08X -> 0x%08X, hubp 0x%08X", m->LastResult.LockWaitUs,
             m->LastResult.CoefWrites, m->LastResult.SclModeBefore, m->LastResult.SclModeAfter, m->LastResult.HubpCntlAfter);
    return status >= 0 ? STATUS_SUCCESS : (NTSTATUS)status;
}

// ---- restore ----------------------------------------------------------------------------------------------

// The native shape back on pipe 0. No log, no lock: the callers decide both.
static long RestoreNative(_Inout_ BC250_DEVICE* Device, _Out_ BC250_SCALER_RESULT* Result)
{
    BC250_DISP_IO io;

    IoOpen(Device, &io, MODESET_PIPE_BUDGET_US);
    return Bc250ScalerRestoreNative(&io, Device->Post.Width, Device->Post.Height, Result);
}

void ModesetStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_MODESET* m = &Device->Modeset;
    BC250_SCALER_RESULT result;
    long status;

    ExAcquireFastMutex(&m->Lock);
    if (m->PipeChanged)
    {
        status = RestoreNative(Device, &result);
        m->Restores++;
        m->PipeChanged = FALSE;
        GuardLog("display modes: stop: native %ux%u shape restored 0x%08X (scl 0x%08X -> 0x%08X)",
                 Device->Post.Width, Device->Post.Height, (ULONG)status, result.SclModeBefore, result.SclModeAfter);
    }
    InterlockedExchange(&m->SourceWidth, (LONG)Device->Post.Width);
    InterlockedExchange(&m->SourceHeight, (LONG)Device->Post.Height);
    m->Scaling = BC250_SCALING_IDENTITY;
    GuardLog("display modes: stop: %lu commits, %lu changes, %lu refusals, %lu busy, %lu underflows, %lu restores",
             m->Commits, m->Changes, m->Refusals, m->Busy, m->Underflows, m->Restores);
    ExReleaseFastMutex(&m->Lock);
}

// Bugcheck (SystemDisplayEnable, after the firmware surface is back) and ResetDevice: any IRQL. The mutex may be
// held by the processor that crashed; this path does not take it, and writes the native shape whatever a commit in
// progress left.
void ModesetRestoreQuiet(_Inout_ BC250_DEVICE* Device)
{
    BC250_SCALER_RESULT result;

    if (!Device->Modeset.PipeChanged || Device->Mmio == NULL) return;
    (void)RestoreNative(Device, &result);
    Device->Modeset.PipeChanged = FALSE;
    InterlockedExchange(&Device->Modeset.SourceWidth, (LONG)Device->Post.Width);
    InterlockedExchange(&Device->Modeset.SourceHeight, (LONG)Device->Post.Height);
}

// D3: the plane to the firmware surface and the native shape, so that whatever reads the display while the device
// is down (the hibernation screen, the firmware at resume) finds the firmware's own picture. The committed mode
// stays in SourceWidth/SourceHeight/Plan; D0 programs it again.
void ModesetPowerDown(_Inout_ BC250_DEVICE* Device)
{
    BC250_MODESET* m = &Device->Modeset;
    BC250_SCALER_RESULT result;
    ULONG generation;
    long status = STATUS_SUCCESS, flip = STATUS_SUCCESS;

    ExAcquireFastMutex(&m->Lock);
    if (m->PipeChanged && !m->Suspended)
    {
        if (WddmPrimaryExclusiveBegin(Device, &generation))
        {
            if (Device->DcnDiverged) flip = DcnFlipToFirmwareSurface(Device);
            status = RestoreNative(Device, &result);
            WddmPrimaryExclusiveEnd(Device, generation, TRUE);
        }
        else status = STATUS_DEVICE_BUSY;
        m->Restores++;
        GuardLog("display modes: D3: firmware surface 0x%08X, native shape 0x%08X", (ULONG)flip, (ULONG)status);
    }
    m->Suspended = TRUE;
    ExReleaseFastMutex(&m->Lock);
}

void ModesetPowerUp(_Inout_ BC250_DEVICE* Device)
{
    BC250_MODESET* m = &Device->Modeset;
    BC250_DISP_IO io;
    BC250_PIPE_SHAPE shape;
    ULONG generation, maxLevel = BC250_DISPLAY_MODES_OFF, verdict = BC250_PIPE_VERDICT_COUNT;
    long status;

    ExAcquireFastMutex(&m->Lock);
    m->Suspended = FALSE;
    if (!m->PipeChanged)
    {
        ExReleaseFastMutex(&m->Lock);
        return;
    }
    // The pipe as the way back to D0 left it: the firmware's shape after a power loss, ours (restored to native at
    // D3) otherwise. Anything else is not programmed over.
    IoOpen(Device, &io, 0);
    status = Bc250PipeShapeRead(&io, &shape);
    if (status >= 0) verdict = Bc250PipeShapeCheck(&shape, Device->Post.Width, Device->Post.Height, &maxLevel);
    if (status < 0 || maxLevel < m->Level || !WddmPrimaryExclusiveBegin(Device, &generation))
    {
        m->PipeChanged = FALSE;
        InterlockedExchange(&m->SourceWidth, (LONG)Device->Post.Width);
        InterlockedExchange(&m->SourceHeight, (LONG)Device->Post.Height);
        m->Scaling = BC250_SCALING_IDENTITY;
        ExReleaseFastMutex(&m->Lock);
        GuardLog("display modes: D0: committed mode not programmed again: pipe %s (0x%08X)",
                 Bc250PipeVerdictText(verdict), (ULONG)status);
        return;
    }
    IoOpen(Device, &io, MODESET_PIPE_BUDGET_US);
    status = Bc250ScalerProgram(&io, &m->Plan, &m->LastResult);
    WddmPrimaryExclusiveEnd(Device, generation, FALSE);
    ExReleaseFastMutex(&m->Lock);
    GuardLog("display modes: D0: %lux%lu %s programmed again 0x%08X", m->Plan.SourceWidth, m->Plan.SourceHeight,
             Bc250ScalingName(m->Scaling), (ULONG)status);
}

// ---- descriptor and audio ---------------------------------------------------------------------------------

NTSTATUS ModesetQueryDescriptor(_In_ BC250_DEVICE* Device, _Inout_ PDXGK_DEVICE_DESCRIPTOR Descriptor)
{
    const BC250_MODESET* m = &Device->Modeset;
    ULONG copy;

    if (!m->DescriptorServed) return STATUS_MONITOR_NO_DESCRIPTOR;
    if (Descriptor->DescriptorOffset >= m->EdidLength) return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    if (Descriptor->DescriptorBuffer == NULL || Descriptor->DescriptorLength == 0) return STATUS_INVALID_PARAMETER;
    // Never more than DescriptorLength bytes (dispmprt contract); a request past the end gets the rest, zero filled.
    copy = m->EdidLength - Descriptor->DescriptorOffset;
    if (copy > Descriptor->DescriptorLength) copy = Descriptor->DescriptorLength;
    RtlCopyMemory(Descriptor->DescriptorBuffer, m->Edid + Descriptor->DescriptorOffset, copy);
    if (copy < Descriptor->DescriptorLength)
        RtlZeroMemory((PUCHAR)Descriptor->DescriptorBuffer + copy, Descriptor->DescriptorLength - copy);
    return STATUS_SUCCESS;
}

const BC250_EDID_INFO* ModesetEdidForAudio(_In_ const BC250_DEVICE* Device)
{
    return Device->Modeset.EdidValid ? &Device->Modeset.Info : NULL;
}
