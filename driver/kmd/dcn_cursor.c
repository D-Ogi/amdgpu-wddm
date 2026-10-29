// The hardware pointer of pipe 0 (docs/design/hardware-pointer.md): DxgkDdiSetPointerShape and
// DxgkDdiSetPointerPosition on the DCN 2.0.1 cursor of HUBP0 and DPP0. The shape conversion and the placement are
// plain C in dcn_cursor_shape.h (host test: test/dcn_cursor_test.c). This file owns the VRAM surface, the registers
// and the life of the cursor across mode changes, power transitions and the stop.
//
// Locks, in this order and never the reverse: the connector's transaction (connector_link.c), Cursor->ShapeLock, then
// Cursor->Lock. The shape DDI takes ShapeLock and Lock, the position DDI takes Lock only (a spin lock: no wait, no
// allocation, no paged code), and the connector calls DcnCursorPipeGone (Lock only) and DcnCursorPipeReady
// (ShapeLock and Lock) from inside its transaction. Every register batch runs under Lock, inside the cursor's own
// update lock (AMD mpc1_cursor_lock: CUR_VUPDATE_LOCK_SET of OPP0), so that the hardware latches a whole batch at
// one VUPDATE and never half of it.
//
// No tear on a shape change: two surfaces, and a shape is drawn into the one the registers do not name. That one
// can still be in use for up to one frame after the previous change, until VUPDATE latches the address that left it.
// CNVC_CUR0_CURSOR0_CONTROL.CUR0_UPDATE_PENDING, read back under Lock, says when that latch happened; a change older
// than DCN_CURSOR_LATCH_MS is taken as latched (a frame of a 24 Hz mode is 42 ms), which covers an OTG that does
// not run (the display is off, so nothing is scanned out).
#include "bc250kmd.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "display_io.h"             // Bc250DispField
#include "display_rotation.h"       // Bc250RotationAngle
#include "dcn_cursor_shape.h"

#define DCN_CURSOR_TAG 'cK52'
#define DCN_CURSOR_LATCH_MS 100ull
#define DCN_CURSOR_WAIT_MS 120ull
#define DCN_CURSOR_LOG_LINES 32
#define DCN_CURSOR_FP_ONE 0x3C00u      // FP16 1.0: dcn10_set_cursor_sdr_white_level's scale for an SDR plane

typedef struct _DCN_CURSOR {
    KSPIN_LOCK Lock;
    FAST_MUTEX ShapeLock;
    ULONG Setting;                      // EnableHardwarePointer of the Parameters key, read in DriverEntry
    PHYSICAL_ADDRESS Physical;          // surface 0; surface 1 is BC250_CURSOR_BUFFER_BYTES above it
    PUCHAR Cpu;                         // the CPU view of both surfaces
    PUCHAR Raw;                         // the copy of the last shape, BC250_CURSOR_RAW_MAX_BYTES
    // Under ShapeLock.
    BC250_CURSOR_SHAPE Shape;           // Pixels points into Raw
    BOOLEAN ShapeValid;
    // Under Lock.
    BOOLEAN Ready;                      // a pipe that DcnCursorPipeReady described is on the screen
    BOOLEAN Stopped;                    // DcnCursorStop ran: nothing is programmed again until the next start
    BOOLEAN EverReady;                  // for the caps: this start had a cursor-capable pipe
    BOOLEAN ImageValid;                 // the registers name a rendered surface
    BOOLEAN Enabled;                    // CURSOR_ENABLE and CUR0_ENABLE are 1 in the last batch
    BOOLEAN Visible;                    // the last SetPointerPosition's Visible
    BOOLEAN SwitchOpen;                 // the last change of surface may not be latched yet
    BC250_CURSOR_PIPE Pipe;
    BC250_CURSOR_IMAGE Image;
    ULONG ImageAngle, ImageWidth, ImageHeight, XHot, YHot;
    ULONG Current;                      // the surface the registers name
    ULONGLONG SwitchTime;               // KeQueryInterruptTime of the last change of surface
    LONG X, Y;
    BC250_CURSOR_PLACE Place;           // the last placement written
    // Counters, for the stop line and the lab.
    volatile LONG Shapes, ShapeRefusals, Positions, Batches, BatchFailures, LatchWaits, LatchTimeouts, PipeReadies,
        PipeGones, TtuWrites, Logged;
} DCN_CURSOR;

static DCN_CURSOR* CursorOf(_In_ const BC250_DEVICE* Device)
{
    return (DCN_CURSOR*)Device->Cursor;
}

static BOOLEAN CursorLogLine(_Inout_ DCN_CURSOR* c)
{
    return InterlockedIncrement(&c->Logged) <= DCN_CURSOR_LOG_LINES;
}

// ---- registers (under Lock) ------------------------------------------------------------------------------------

static NTSTATUS CursorUpdate(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Mask, ULONG Value)
{
    ULONG old = 0;
    NTSTATUS status = MmioDcnRead(Device, Offset, &old);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, Offset, (old & ~Mask) | (Value & Mask), TRUE);
    return status;
}

static NTSTATUS CursorLock(_In_ const BC250_DEVICE* Device, BOOLEAN Lock)
{
    return MmioDcnWriteEx(Device, BC250_REG_DMU_CUR_VUPDATE_LOCK_SET0,
        Bc250DispField(CUR_VUPDATE_LOCK_SET0__CUR_VUPDATE_LOCK_SET_MASK, Lock ? 1u : 0u), TRUE);
}

// hubp2_cursor_set_attributes, dpp2_set_cursor_attributes and dpp1_cnv_set_optional_cursor_attributes for one
// surface. The cursor stays in plane space (CURSOR_XY_POSITION_ROTATION_AND_MIRRORING_BYPASS is not touched: the
// plane is never rotated or mirrored here).
static NTSTATUS CursorAttributes(_In_ const BC250_DEVICE* Device, _In_ const DCN_CURSOR* c, ULONG Surface,
    _In_ const BC250_CURSOR_IMAGE* Image)
{
    const ULONGLONG address = (ULONGLONG)c->Physical.QuadPart + (ULONGLONG)Surface * BC250_CURSOR_BUFFER_BYTES;
    const ULONG control = CURSOR0_0_CURSOR_CONTROL__CURSOR_MODE_MASK | CURSOR0_0_CURSOR_CONTROL__CURSOR_2X_MAGNIFY_MASK |
        CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH_MASK | CURSOR0_0_CURSOR_CONTROL__CURSOR_LINES_PER_CHUNK_MASK;
    const ULONG cnvc = CNVC_CUR0_CURSOR0_CONTROL__CUR0_MODE_MASK | CNVC_CUR0_CURSOR0_CONTROL__CUR0_EXPANSION_MODE_MASK |
        CNVC_CUR0_CURSOR0_CONTROL__CUR0_ROM_EN_MASK;
    NTSTATUS status;

    status = CursorUpdate(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH,
        CURSOR0_0_CURSOR_SURFACE_ADDRESS_HIGH__CURSOR_SURFACE_ADDRESS_HIGH_MASK, (ULONG)(address >> 32));
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_SURFACE_ADDRESS,
        (ULONG)(address & 0xFFFFFFFFull), TRUE);
    if (NT_SUCCESS(status)) status = CursorUpdate(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_SIZE,
        CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH_MASK | CURSOR0_0_CURSOR_SIZE__CURSOR_HEIGHT_MASK,
        Bc250DispField(CURSOR0_0_CURSOR_SIZE__CURSOR_WIDTH_MASK, Image->Width) |
        Bc250DispField(CURSOR0_0_CURSOR_SIZE__CURSOR_HEIGHT_MASK, Image->Height));
    // CURSOR_PITCH 2 is 256 pixels (hubp1_get_cursor_pitch).
    if (NT_SUCCESS(status)) status = CursorUpdate(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL, control,
        Bc250DispField(CURSOR0_0_CURSOR_CONTROL__CURSOR_MODE_MASK, Image->Mode) |
        Bc250DispField(CURSOR0_0_CURSOR_CONTROL__CURSOR_PITCH_MASK, 2u) |
        Bc250DispField(CURSOR0_0_CURSOR_CONTROL__CURSOR_LINES_PER_CHUNK_MASK,
                       Bc250CursorLinesPerChunk(Image->Width, Image->Mode)));
    // No shift of the cursor's schedule, the chunk deadline adjust of AMD's code (3).
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_CURSOR_SETTINGS,
        Bc250DispField(HUBPREQ0_CURSOR_SETTINGS__CURSOR0_DST_Y_OFFSET_MASK, 0u) |
        Bc250DispField(HUBPREQ0_CURSOR_SETTINGS__CURSOR0_CHUNK_HDL_ADJUST_MASK, 3u), TRUE);
    // No degamma ROM and no expansion, as AMD programs an 8-bit cursor.
    if (NT_SUCCESS(status)) status = CursorUpdate(Device, BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL, cnvc,
        Bc250DispField(CNVC_CUR0_CURSOR0_CONTROL__CUR0_MODE_MASK, Image->Mode));
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_CNVC_CUR0_CURSOR0_FP_SCALE_BIAS,
        Bc250DispField(CNVC_CUR0_CURSOR0_FP_SCALE_BIAS__CUR0_FP_SCALE_MASK, DCN_CURSOR_FP_ONE) |
        Bc250DispField(CNVC_CUR0_CURSOR0_FP_SCALE_BIAS__CUR0_FP_BIAS_MASK, 0u), TRUE);
    return status;
}

// hubp2_cursor_set_position and dpp1_set_cursor_position.
static NTSTATUS CursorPosition(_In_ const BC250_DEVICE* Device, _In_ const BC250_CURSOR_PLACE* Place)
{
    NTSTATUS status = CursorUpdate(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL,
        CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK,
        Bc250DispField(CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK, Place->Enable));
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_POSITION,
        Bc250DispField(CURSOR0_0_CURSOR_POSITION__CURSOR_X_POSITION_MASK, Place->X) |
        Bc250DispField(CURSOR0_0_CURSOR_POSITION__CURSOR_Y_POSITION_MASK, Place->Y), TRUE);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_HOT_SPOT,
        Bc250DispField(CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_X_MASK, Place->HotX) |
        Bc250DispField(CURSOR0_0_CURSOR_HOT_SPOT__CURSOR_HOT_SPOT_Y_MASK, Place->HotY), TRUE);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_DST_OFFSET,
        Bc250DispField(CURSOR0_0_CURSOR_DST_OFFSET__CURSOR_DST_X_OFFSET_MASK, Place->DstX), TRUE);
    if (NT_SUCCESS(status)) status = CursorUpdate(Device, BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL,
        CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK,
        Bc250DispField(CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK, Place->Enable));
    return status;
}

// The plane the cursor is on now. The committed source size is read at any IRQL (DisplaySourceWidth). A scaled
// multiplane overlay plane (dcn.c DcnPlaneShape, published under DcnSurfaceSequence) maps the source into the
// plane; the source-mode scaler of modeset.c needs no map (the cursor scales with the plane it is drawn on), only
// its ratio for the destination offset.
static void CursorPlaneNow(_In_ const BC250_DEVICE* Device, _In_ const DCN_CURSOR* c, _Out_ BC250_CURSOR_PLANE* Plane)
{
    BC250_SCALER_PLAN shape;
    BOOLEAN scaled = FALSE, stable = FALSE;
    LONG sequence;
    ULONG tries;

    RtlZeroMemory(Plane, sizeof(*Plane));
    Plane->SourceWidth = Plane->ViewportWidth = DisplaySourceWidth(Device);
    Plane->SourceHeight = Plane->ViewportHeight = DisplaySourceHeight(Device);
    Plane->RefClockKhz = c->Pipe.RefClockKhz;
    Plane->PixelClockKhz = c->Pipe.PixelClockKhz;
    RtlZeroMemory(&shape, sizeof(shape));
    for (tries = 0; tries < 3 && !stable; tries++) {
        sequence = InterlockedCompareExchange((volatile LONG*)&Device->DcnSurfaceSequence, 0, 0);
        if (sequence & 1) continue;
        scaled = Device->DcnPlaneScaled;
        shape = Device->DcnPlaneShape;
        stable = InterlockedCompareExchange((volatile LONG*)&Device->DcnSurfaceSequence, 0, 0) == sequence;
    }
    if (stable && scaled && shape.Scaling == BC250_SCALING_PLANE && shape.Viewport.Width && shape.Viewport.Height &&
        shape.Recout.Width && shape.Recout.Height) {
        Plane->Map = 1;
        Plane->ViewportWidth = shape.Viewport.Width;
        Plane->ViewportHeight = shape.Viewport.Height;
        Plane->ViewportX = (int)shape.Viewport.X;
        Plane->ViewportY = (int)shape.Viewport.Y;
        Plane->RecoutX = (int)shape.Recout.X;
        Plane->RecoutY = (int)shape.Recout.Y;
        Plane->RecoutWidth = shape.Recout.Width;
        Plane->RecoutHeight = shape.Recout.Height;
        Plane->DstNumerator = shape.Recout.Width;
        Plane->DstDenominator = shape.Viewport.Width;
    } else if (Device->Modeset.PipeChanged && Device->Modeset.Plan.Viewport.Width && Device->Modeset.Plan.Recout.Width) {
        Plane->DstNumerator = Device->Modeset.Plan.Recout.Width;
        Plane->DstDenominator = Device->Modeset.Plan.Viewport.Width;
    }
}

static ULONG CursorAngle(_In_ const BC250_DEVICE* Device)
{
    const ULONG angle = Bc250RotationAngle((ULONG)Device->Rotation, Device->FullWddm);
    return Bc250CursorAngleValid(angle) ? angle : 0u;
}

// The placement of the image on the registers at the last position, with the hot spot rule of the setting.
static void CursorPlaceNow(_In_ const BC250_DEVICE* Device, _In_ const DCN_CURSOR* c, _Out_ BC250_CURSOR_PLACE* Place)
{
    BC250_CURSOR_PLANE plane;
    int x, y;
    Bc250CursorTopLeft(c->Setting, (int)c->X, (int)c->Y, c->XHot, c->YHot, &x, &y);
    CursorPlaneNow(Device, c, &plane);
    Bc250CursorPlace(&plane, c->ImageAngle, c->ImageWidth, c->ImageHeight, &c->Image,
        c->Ready && c->ImageValid && c->Visible && !c->Stopped, x, y, Place);
}

// One batch inside the cursor lock: the attributes of Surface when Attributes is TRUE, and the position. The lock is
// released on every path, so a failed write never leaves the cursor's updates held.
static NTSTATUS CursorBatch(_In_ const BC250_DEVICE* Device, _Inout_ DCN_CURSOR* c, BOOLEAN Attributes, ULONG Surface)
{
    BC250_CURSOR_PLACE place;
    NTSTATUS status, unlock;

    CursorPlaceNow(Device, c, &place);
    status = CursorLock(Device, TRUE);
    if (NT_SUCCESS(status) && Attributes) status = CursorAttributes(Device, c, Surface, &c->Image);
    if (NT_SUCCESS(status)) status = CursorPosition(Device, &place);
    unlock = CursorLock(Device, FALSE);
    if (NT_SUCCESS(status)) status = unlock;
    InterlockedIncrement(&c->Batches);
    if (!NT_SUCCESS(status)) {
        InterlockedIncrement(&c->BatchFailures);
        // Unknown after a failed write: count the cursor as shown, so the next batch writes the enable again.
        c->Enabled = TRUE;
        return status;
    }
    c->Place = place;
    c->Enabled = place.Enable != 0;
    return STATUS_SUCCESS;
}

// The cursor off in its own batch. Lock held.
static void CursorOffLocked(_In_ const BC250_DEVICE* Device, _Inout_ DCN_CURSOR* c)
{
    BC250_CURSOR_PLACE off;
    NTSTATUS status;
    if (!c->Enabled || Device->Mmio == NULL) { c->Enabled = FALSE; return; }
    RtlZeroMemory(&off, sizeof(off));
    status = CursorLock(Device, TRUE);
    if (NT_SUCCESS(status)) status = CursorPosition(Device, &off);
    if (NT_SUCCESS(CursorLock(Device, FALSE)) && NT_SUCCESS(status)) { c->Enabled = FALSE; c->Place = off; }
    else InterlockedIncrement(&c->BatchFailures);
}

// The surface that is not on the registers can be drawn into once the hardware latched the change that left it.
static BOOLEAN CursorSpareFreeLocked(_In_ const BC250_DEVICE* Device, _Inout_ DCN_CURSOR* c)
{
    ULONG control = 0;
    if (!c->SwitchOpen || !c->Enabled) { c->SwitchOpen = FALSE; return TRUE; }
    if (KeQueryInterruptTime() - c->SwitchTime >= DCN_CURSOR_LATCH_MS * 10000ull) { c->SwitchOpen = FALSE; return TRUE; }
    if (NT_SUCCESS(MmioDcnRead(Device, BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL, &control)) &&
        (control & CNVC_CUR0_CURSOR0_CONTROL__CUR0_UPDATE_PENDING_MASK) == 0) {
        c->SwitchOpen = FALSE;
        return TRUE;
    }
    return FALSE;
}

// ---- rendering (under ShapeLock, Lock not held) -------------------------------------------------------------------

// Wait, outside Lock, until the spare surface is free. PASSIVE_LEVEL or APC_LEVEL (ShapeLock). A wait that runs
// out is counted and the draw goes ahead: one frame of a mixed image is better than no pointer.
static ULONG CursorSpare(_In_ const BC250_DEVICE* Device, _Inout_ DCN_CURSOR* c)
{
    const ULONGLONG deadline = KeQueryInterruptTime() + DCN_CURSOR_WAIT_MS * 10000ull;
    LARGE_INTEGER delay;
    KIRQL irql;
    BOOLEAN free, waited = FALSE;
    ULONG spare;

    delay.QuadPart = -10000ll;      // 1 ms, relative
    for (;;) {
        KeAcquireSpinLock(&c->Lock, &irql);
        free = CursorSpareFreeLocked(Device, c);
        spare = c->Current ^ 1u;
        KeReleaseSpinLock(&c->Lock, irql);
        if (free) break;
        if (!waited) { waited = TRUE; InterlockedIncrement(&c->LatchWaits); }
        if (KeQueryInterruptTime() >= deadline) { InterlockedIncrement(&c->LatchTimeouts); break; }
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    return spare;
}

// Draw the stored shape into Surface for Angle. ShapeLock held.
static unsigned int CursorDraw(_In_ DCN_CURSOR* c, ULONG Surface, ULONG Angle, _Out_ BC250_CURSOR_IMAGE* Image)
{
    unsigned int verdict = Bc250CursorRender(&c->Shape, Angle, (c->Setting & BC250_CURSOR_SETTING_PREMULTIPLIED) != 0,
        (unsigned int*)(c->Cpu + (SIZE_T)Surface * BC250_CURSOR_BUFFER_BYTES), Image);
    // The surface is write-combined VRAM: the stores reach memory before the registers name it.
    KeMemoryBarrier();
    return verdict;
}

// Draw the stored shape into the spare surface and put that surface on the registers. ShapeLock held, PASSIVE.
static NTSTATUS CursorShow(_In_ BC250_DEVICE* Device, _Inout_ DCN_CURSOR* c, _Out_opt_ ULONG* SurfaceOut,
    _Out_opt_ BC250_CURSOR_IMAGE* ImageOut)
{
    BC250_CURSOR_IMAGE image;
    ULONG surface, angle = CursorAngle(Device);
    unsigned int verdict;
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL irql;

    surface = CursorSpare(Device, c);
    verdict = CursorDraw(c, surface, angle, &image);
    if (SurfaceOut) *SurfaceOut = surface;
    if (ImageOut) *ImageOut = image;
    if (verdict != BC250_CURSOR_SHAPE_OK) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&c->Lock, &irql);
    c->Image = image;
    c->ImageAngle = angle;
    c->ImageWidth = c->Shape.Width;
    c->ImageHeight = c->Shape.Height;
    c->XHot = c->Shape.XHot;
    c->YHot = c->Shape.YHot;
    c->ImageValid = TRUE;
    if (c->Ready && !c->Stopped) {
        status = CursorBatch(Device, c, TRUE, surface);
        if (NT_SUCCESS(status)) {
            c->Current = surface;
            c->SwitchOpen = TRUE;
            c->SwitchTime = KeQueryInterruptTime();
        }
    }
    KeReleaseSpinLock(&c->Lock, irql);
    return status;
}

// ---- start and stop -------------------------------------------------------------------------------------------------

// StartDevice, after ConnectorBufferPrepare and before WddmStart (QUERYSEGMENT reads DisplayReservationEnd): two
// cursor surfaces after the connector's handoff buffer, 64 KiB aligned, out of every VidMm segment, and the copy of
// one shape. Never fails the start: without it the caps report no hardware pointer and dxgkrnl draws the cursor.
NTSTATUS DcnCursorPrepare(_Inout_ BC250_DEVICE* Device)
{
    const ULONGLONG bytes = 2ull * BC250_CURSOR_BUFFER_BYTES;
    ULONGLONG offset, limit;
    DCN_CURSOR* c;
    ULONG setting;
    NTSTATUS status = STATUS_SUCCESS;

    if (Device->Cursor) DcnCursorRelease(Device);
    setting = GuardHardwarePointerSetting() & BC250_CURSOR_SETTING_MASK;
    if (!(setting & BC250_CURSOR_SETTING_ON)) status = STATUS_NOT_SUPPORTED;
    // Before WddmStart, which sets Device->FullWddm from the same selection.
    else if (!WddmFullTableSelected() || !Device->DcnWriteEnabled || !Device->RuntimeFramebuffer ||
             !Device->DisplayReservationEnd ||
             Device->VramLength <= BC250_VRAM_TOP_RESERVED) status = STATUS_DEVICE_NOT_READY;
    if (!NT_SUCCESS(status)) goto done;
    limit = Device->VramLength - BC250_VRAM_TOP_RESERVED;
    if (Device->DisplayReservationEnd > MAXULONGLONG - 65535ull) { status = STATUS_INVALID_PARAMETER; goto done; }
    offset = (Device->DisplayReservationEnd + 65535ull) & ~65535ull;
    if (offset > limit || bytes > limit - offset) { status = STATUS_NO_MEMORY; goto done; }
    if (Device->VramPhysical.QuadPart < 0 || offset > (ULONGLONG)MAXLONGLONG - (ULONGLONG)Device->VramPhysical.QuadPart ||
        bytes > (ULONGLONG)MAXLONGLONG - (ULONGLONG)Device->VramPhysical.QuadPart - offset) {
        status = STATUS_INVALID_PARAMETER;
        goto done;
    }
    c = (DCN_CURSOR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), DCN_CURSOR_TAG);
    if (!c) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    c->Raw = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, BC250_CURSOR_RAW_MAX_BYTES, DCN_CURSOR_TAG);
    c->Physical.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)offset;
    c->Cpu = c->Raw ? (PUCHAR)VramMapCpuRange(Device, c->Physical, (SIZE_T)bytes, PAGE_READWRITE) : NULL;
    if (!c->Cpu) {
        if (c->Raw) ExFreePoolWithTag(c->Raw, DCN_CURSOR_TAG);
        ExFreePoolWithTag(c, DCN_CURSOR_TAG);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto done;
    }
    KeInitializeSpinLock(&c->Lock);
    ExInitializeFastMutex(&c->ShapeLock);
    c->Setting = setting;
    RtlZeroMemory(c->Cpu, (SIZE_T)bytes);
    KeMemoryBarrier();
    Device->DisplayReservationEnd = offset + bytes;
    Device->Cursor = c;
done:
    GuardLog("cursor: prepare setting %lu surfaces %llu bytes status %08lX", setting, bytes, (ULONG)status);
    return status;
}

// The width the connector plans its pipes with (Plan's CursorWidth): AMD's display manager plans every RGB pipe
// with a 256-wide 32-bit cursor. 0 when this start has no hardware pointer, so the plans stay as they were.
ULONG DcnCursorPlanWidth(_In_ const BC250_DEVICE* Device)
{
    const DCN_CURSOR* c = CursorOf(Device);
    return c && (c->Setting & BC250_CURSOR_SETTING_ON) ? BC250_CURSOR_MAX_SIZE : 0u;
}

// A pipe of this start had the cursor, the connector still drives the link, and no stop ran. A gap without a ready pipe
// is then a mode change, a power transition or a detach, and the next DcnCursorPipeReady shows the stored shape.
static BOOLEAN CursorExpectedLocked(_In_ BC250_DEVICE* Device, _In_ const DCN_CURSOR* c)
{
    return c->EverReady && !c->Stopped && ConnectorIsEnabled(Device);
}

// DXGK_DRIVERCAPS: a hardware pointer only on a start whose pipe this driver drives with a cursor in its plan, on a
// connector that is enabled (a start refused after the inherited pipe was adopted releases the link and keeps the
// static path: no pointer there). dxgkrnl reads the caps at the adapter start, after StartDevice: a start whose first
// pipe has no cursor in its plan keeps the software pointer to its end, because a later pipe cannot change the caps.
BOOLEAN DcnCursorCaps(_In_ BC250_DEVICE* Device)
{
    DCN_CURSOR* c = CursorOf(Device);
    BOOLEAN caps;
    KIRQL irql;
    if (!c || !Device->FullWddm || !Device->DcnWriteEnabled || !Device->VidPnFlipEnabled) return FALSE;
    KeAcquireSpinLock(&c->Lock, &irql);
    caps = CursorExpectedLocked(Device, c);
    KeReleaseSpinLock(&c->Lock, irql);
    if (CursorLogLine(c)) GuardLog("cursor: caps %u setting %lu", (UINT)caps, c->Setting);
    return caps;
}

// Every exit path: the start's failure, StopDevice, the post-display release, DcnStop. The cursor goes off and
// nothing programs it again until the next start.
void DcnCursorStop(_Inout_ BC250_DEVICE* Device)
{
    DCN_CURSOR* c = CursorOf(Device);
    KIRQL irql;
    BOOLEAN first;
    if (!c) return;
    KeAcquireSpinLock(&c->Lock, &irql);
    first = !c->Stopped;
    c->Stopped = TRUE;
    c->Ready = FALSE;
    c->Enabled = TRUE;      // write the off batch even if the last one said off: the next owner reads these registers
    CursorOffLocked(Device, c);
    KeReleaseSpinLock(&c->Lock, irql);
    if (first) {
        GuardLog("cursor: stop shapes %ld refused %ld positions %ld batches %ld failed %ld", c->Shapes,
            c->ShapeRefusals, c->Positions, c->Batches, c->BatchFailures);
        GuardLog("cursor: stop waits %ld timeouts %ld ready %ld gone %ld ttu %ld", c->LatchWaits, c->LatchTimeouts,
            c->PipeReadies, c->PipeGones, c->TtuWrites);
    }
}

// StopDevice, with ConnectorBufferRelease: after DcnStop, before BAR5 goes.
void DcnCursorRelease(_Inout_ BC250_DEVICE* Device)
{
    DCN_CURSOR* c = CursorOf(Device);
    if (!c) return;
    DcnCursorStop(Device);
    Device->Cursor = NULL;
    MmUnmapIoSpace(c->Cpu, (SIZE_T)(2ull * BC250_CURSOR_BUFFER_BYTES));
    ExFreePoolWithTag(c->Raw, DCN_CURSOR_TAG);
    ExFreePoolWithTag(c, DCN_CURSOR_TAG);
}

// The bugcheck screen and DxgkDdiResetDevice: any IRQL, no lock, no log. The cursor would otherwise stay over the
// firmware surface. The cursor's update lock is cleared first, in case the bugcheck stopped a batch that held it.
void DcnCursorOffQuiet(_Inout_ BC250_DEVICE* Device)
{
    const DCN_CURSOR* c = CursorOf(Device);
    if (!c || Device->Mmio == NULL) return;
    (void)CursorLock(Device, FALSE);
    (void)CursorUpdate(Device, BC250_REG_DMU_CURSOR0_0_CURSOR_CONTROL, CURSOR0_0_CURSOR_CONTROL__CURSOR_ENABLE_MASK, 0);
    (void)CursorUpdate(Device, BC250_REG_DMU_CNVC_CUR0_CURSOR0_CONTROL, CNVC_CUR0_CURSOR0_CONTROL__CUR0_ENABLE_MASK, 0);
}

// ---- the pipe (connector_link.c, modeset.c, display.c) ---------------------------------------------------------------

// The pipe goes away or is about to be programmed: the cursor off, nothing programmed until DcnCursorPipeReady.
// Lock only, so a mode change never waits for a shape.
void DcnCursorPipeGone(_Inout_ BC250_DEVICE* Device)
{
    DCN_CURSOR* c = CursorOf(Device);
    KIRQL irql;
    if (!c) return;
    KeAcquireSpinLock(&c->Lock, &irql);
    if (c->Ready) InterlockedIncrement(&c->PipeGones);
    c->Ready = FALSE;
    CursorOffLocked(Device, c);
    KeReleaseSpinLock(&c->Lock, irql);
}

// A pipe is on the screen with a cursor in its plan. The connector calls this at the end of a successful Apply and for
// the inherited timing it adopted. On the inherited pipe the connector programmed no requestor, so the two cursor TTU
// registers get the plan's values here (hubp2's program_deadline), while the cursor is off. Then the stored shape
// goes back on (a mode set does not always bring a new SetPointerShape). PASSIVE, inside the connector's transaction.
void DcnCursorPipeReady(_Inout_ BC250_DEVICE* Device, _In_ const BC250_CURSOR_PIPE* Pipe)
{
    DCN_CURSOR* c = CursorOf(Device);
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN ready = FALSE, log;
    KIRQL irql;

    if (!c || !Pipe || KeGetCurrentIrql() > APC_LEVEL) return;
    ExAcquireFastMutex(&c->ShapeLock);
    KeAcquireSpinLock(&c->Lock, &irql);
    c->Ready = FALSE;
    CursorOffLocked(Device, c);
    if (!c->Stopped && Pipe->CursorWidth >= BC250_CURSOR_MAX_SIZE && Pipe->RefClockKhz && Pipe->PixelClockKhz) {
        if (Pipe->WriteTtu) {
            if (Pipe->Delivery > HUBPREQ0_DCN_CUR0_TTU_CNTL0__REFCYC_PER_REQ_DELIVERY_MASK ||
                Pipe->DeliveryPre > HUBPREQ0_DCN_CUR0_TTU_CNTL1__REFCYC_PER_REQ_DELIVERY_PRE_MASK ||
                Pipe->QosLevelFixed > 0xFu || Pipe->QosRampDisable > 1u) status = STATUS_INVALID_PARAMETER;
            if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL0,
                Bc250DispField(HUBPREQ0_DCN_CUR0_TTU_CNTL0__REFCYC_PER_REQ_DELIVERY_MASK, Pipe->Delivery) |
                Bc250DispField(HUBPREQ0_DCN_CUR0_TTU_CNTL0__QoS_LEVEL_FIXED_MASK, Pipe->QosLevelFixed) |
                Bc250DispField(HUBPREQ0_DCN_CUR0_TTU_CNTL0__QoS_RAMP_DISABLE_MASK, Pipe->QosRampDisable), TRUE);
            if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCN_CUR0_TTU_CNTL1,
                Bc250DispField(HUBPREQ0_DCN_CUR0_TTU_CNTL1__REFCYC_PER_REQ_DELIVERY_PRE_MASK, Pipe->DeliveryPre), TRUE);
            if (NT_SUCCESS(status)) InterlockedIncrement(&c->TtuWrites);
        }
        if (NT_SUCCESS(status)) {
            c->Pipe = *Pipe;
            c->Ready = TRUE;
            c->EverReady = TRUE;
            c->ImageValid = FALSE;      // drawn again below, for this pipe's rotation
            ready = TRUE;
            InterlockedIncrement(&c->PipeReadies);
        }
    }
    KeReleaseSpinLock(&c->Lock, irql);
    if (ready && c->ShapeValid) status = CursorShow(Device, c, NULL, NULL);
    ExReleaseFastMutex(&c->ShapeLock);
    log = CursorLogLine(c);
    if (log)
        GuardLog("cursor: pipe ready %u width %lu ref %lu kHz pixel %lu kHz ttu %u shape %u status %08lX",
            (UINT)ready, Pipe->CursorWidth, Pipe->RefClockKhz, Pipe->PixelClockKhz, (UINT)Pipe->WriteTtu,
            (UINT)c->ShapeValid, (ULONG)status);
}

// The rotation or the source size changed (display.c, modeset.c): the image is drawn again when the angle changed,
// and the position is placed again on the new plane. PASSIVE. Takes ShapeLock.
void DcnCursorRefresh(_Inout_ BC250_DEVICE* Device)
{
    DCN_CURSOR* c = CursorOf(Device);
    KIRQL irql;
    BOOLEAN redraw;
    if (!c || KeGetCurrentIrql() > APC_LEVEL) return;
    ExAcquireFastMutex(&c->ShapeLock);
    KeAcquireSpinLock(&c->Lock, &irql);
    redraw = c->ShapeValid && c->ImageAngle != CursorAngle(Device);
    if (!redraw && c->Ready && !c->Stopped && c->ImageValid) (void)CursorBatch(Device, c, FALSE, c->Current);
    KeReleaseSpinLock(&c->Lock, irql);
    if (redraw) (void)CursorShow(Device, c, NULL, NULL);
    ExReleaseFastMutex(&c->ShapeLock);
}

// ---- the DDIs ---------------------------------------------------------------------------------------------------------

// DxgkDdiSetPointerShape: PASSIVE_LEVEL, the pointer class of Level One (serialized with SetPointerPosition only).
// A shape this cursor cannot show is refused, and dxgkrnl then draws the pointer itself. A valid shape that arrives
// while the pipe changes (CursorExpectedLocked) is stored and answered with success, and DcnCursorPipeReady shows it:
// the same result as a shape that came just before the change, which DcnCursorPipeReady shows again. A refusal there
// would keep the software pointer until the next shape change. The decision is made under ShapeLock, which
// DcnCursorPipeReady holds for its whole run, so a shape is never stored after that run without being shown. A shape
// with no pipe expected (no cursor pipe in this start, a released link, the stop) is refused.
NTSTATUS DcnCursorSetShape(_Inout_ BC250_DEVICE* Device, _In_ const DXGKARG_SETPOINTERSHAPE* Arg)
{
    DCN_CURSOR* c = CursorOf(Device);
    BC250_CURSOR_SHAPE in;
    BC250_CURSOR_IMAGE image;
    unsigned int verdict, bytes;
    ULONG surface = 0;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN ready = FALSE, expected = FALSE;
    KIRQL irql;

    RtlZeroMemory(&image, sizeof(image));
    if (!c || !Arg) return STATUS_NOT_SUPPORTED;
    if (Arg->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    in.Flags = Arg->Flags.Value;
    in.Width = Arg->Width;
    in.Height = Arg->Height;
    in.Pitch = Arg->Pitch;
    in.XHot = Arg->XHot;
    in.YHot = Arg->YHot;
    in.Pixels = (const unsigned char*)Arg->pPixels;
    verdict = Bc250CursorShapeCheck(&in);
    if (verdict == BC250_CURSOR_SHAPE_OK) {
        ExAcquireFastMutex(&c->ShapeLock);
        KeAcquireSpinLock(&c->Lock, &irql);
        ready = c->Ready && !c->Stopped;
        expected = CursorExpectedLocked(Device, c);
        KeReleaseSpinLock(&c->Lock, irql);
        if (ready || expected) {
            bytes = Bc250CursorShapeBytes(&in);
            RtlCopyMemory(c->Raw, in.Pixels, bytes);
            c->Shape = in;
            c->Shape.Pixels = c->Raw;
            c->ShapeValid = TRUE;
            // Drawn only on a ready pipe: between the pipes the surfaces stay as they are (the device may be on its
            // way to D3), and DcnCursorPipeReady draws the stored shape for the new pipe.
            if (ready) status = CursorShow(Device, c, &surface, &image);
        } else verdict = BC250_CURSOR_SHAPE_VERDICTS;     // no pipe in this start: refused
        ExReleaseFastMutex(&c->ShapeLock);
    }
    if (verdict != BC250_CURSOR_SHAPE_OK) {
        InterlockedIncrement(&c->ShapeRefusals);
        if (CursorLogLine(c))
            GuardLog("cursor: shape refused verdict %u flags 0x%X %lux%lu pitch %lu hot %lu,%lu ready %u", verdict,
                in.Flags, (ULONG)in.Width, (ULONG)in.Height, (ULONG)in.Pitch, (ULONG)in.XHot, (ULONG)in.YHot,
                (UINT)ready);
        return verdict == BC250_CURSOR_SHAPE_VERDICTS ? STATUS_DEVICE_NOT_READY : STATUS_NOT_SUPPORTED;
    }
    InterlockedIncrement(&c->Shapes);
    if (CursorLogLine(c))
        GuardLog("cursor: shape 0x%X %lux%lu hot %lu,%lu mode %u xor %u at %lu ready %u %08lX",
            in.Flags, (ULONG)in.Width, (ULONG)in.Height, (ULONG)in.XHot, (ULONG)in.YHot,
            image.Mode, image.XorPixels, surface, (UINT)ready, (ULONG)status);
    return status;
}

// DxgkDdiSetPointerPosition: no wait, no allocation, no paged code; the spin lock and the registers only. Visible 0
// hides the cursor and keeps the last X and Y (the DDI says to ignore them). A position that arrives while no pipe is
// ready is kept for DcnCursorPipeReady and answered with success, as for a disabled topology.
NTSTATUS DcnCursorSetPosition(_Inout_ BC250_DEVICE* Device, _In_ const DXGKARG_SETPOINTERPOSITION* Arg)
{
    DCN_CURSOR* c = CursorOf(Device);
    NTSTATUS status = STATUS_SUCCESS;
    KIRQL irql;
    if (!c || !Arg || Arg->VidPnSourceId != 0) return STATUS_SUCCESS;
    KeAcquireSpinLock(&c->Lock, &irql);
    c->Visible = Arg->Flags.Visible != 0;
    if (c->Visible) { c->X = Arg->X; c->Y = Arg->Y; }
    if (c->Ready && !c->Stopped && c->ImageValid) status = CursorBatch(Device, c, FALSE, c->Current);
    KeReleaseSpinLock(&c->Lock, irql);
    InterlockedIncrement(&c->Positions);
    // A register failure is counted (BatchFailures); the pointer class has no recovery for it, and a failure
    // here would not bring the software pointer back.
    UNREFERENCED_PARAMETER(status);
    return STATUS_SUCCESS;
}
