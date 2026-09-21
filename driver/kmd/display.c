// Display side of the M3 miniport: one source, one target, one mode (the one the firmware left), present by
// CPU copy into the firmware's framebuffer. No MMIO. Written against the documented display-only DDI.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"

static ULONG g_Presents;

static BOOLEAN IsPostFormatSupported(D3DDDIFORMAT Format)
{
    return Format == D3DDDIFMT_X8R8G8B8 || Format == D3DDDIFMT_A8R8G8B8;
}

NTSTATUS DisplayMapFramebuffer(_Inout_ BC250_DEVICE* Device)
{
    // M3 copies 32-bit pixels and nothing else. Any other firmware format is refused, not converted.
    if (!IsPostFormatSupported(Device->Post.ColorFormat)) return STATUS_GRAPHICS_INVALID_PIXELFORMAT;
    if (Device->Post.Pitch < Device->Post.Width * 4) return STATUS_GRAPHICS_INVALID_STRIDE;

    Device->FramebufferLength = (SIZE_T)Device->Post.Pitch * Device->Post.Height;
    Device->Framebuffer = MmMapIoSpaceEx(Device->Post.PhysicAddress, Device->FramebufferLength,
                                         PAGE_READWRITE | PAGE_WRITECOMBINE);
    // Write-combining can be refused when the range already carries another cache attribute (a framebuffer
    // carved out of system RAM, as on this APU, is a candidate). Slower but always mappable: uncached.
    if (Device->Framebuffer == NULL)
        Device->Framebuffer = MmMapIoSpaceEx(Device->Post.PhysicAddress, Device->FramebufferLength,
                                             PAGE_READWRITE | PAGE_NOCACHE);
    if (Device->Framebuffer == NULL)
    {
        Device->FramebufferLength = 0;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

void DisplayUnmapFramebuffer(_Inout_ BC250_DEVICE* Device)
{
    if (Device->Framebuffer != NULL) MmUnmapIoSpace(Device->Framebuffer, Device->FramebufferLength);
    Device->Framebuffer = NULL;
    Device->FramebufferLength = 0;
}

// ---- adapter ----------------------------------------------------------------------------------------------

NTSTATUS Bc250QueryAdapterInfo(_In_ const HANDLE hAdapter, _In_ const DXGKARG_QUERYADAPTERINFO* QueryAdapterInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);

    if (QueryAdapterInfo->Type != DXGKQAITYPE_DRIVERCAPS) return STATUS_NOT_SUPPORTED;
    if (QueryAdapterInfo->OutputDataSize < RTL_SIZEOF_THROUGH_FIELD(DXGK_DRIVERCAPS, SupportSmoothRotation))
        return STATUS_BUFFER_TOO_SMALL;

    {
        DXGK_DRIVERCAPS* caps = (DXGK_DRIVERCAPS*)QueryAdapterInfo->pOutputData;
        RtlZeroMemory(caps, QueryAdapterInfo->OutputDataSize);
        caps->WDDMVersion = DXGKDDI_WDDMv1_2;
        caps->HighestAcceptableAddress.QuadPart = -1;
        caps->SupportNonVGA = TRUE;
        // No hardware pointer (MaxPointerWidth/Height stay 0): dxgkrnl draws the cursor into the image it presents.
    }
    return STATUS_SUCCESS;
}

NTSTATUS Bc250SetPointerPosition(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETPOINTERPOSITION* SetPointerPosition)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(SetPointerPosition);
    return STATUS_SUCCESS;      // only "hide" can arrive while we report no pointer capability
}

NTSTATUS Bc250SetPointerShape(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETPOINTERSHAPE* SetPointerShape)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(SetPointerShape);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS Bc250Escape(_In_ const HANDLE hAdapter, _In_ const DXGKARG_ESCAPE* Escape)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_ESCAPE* data = (BC250_ESCAPE*)Escape->pPrivateDriverData;

    if (Escape->PrivateDriverDataSize < sizeof(BC250_ESCAPE) || data == NULL || data->Magic != BC250_ESCAPE_MAGIC)
        return STATUS_INVALID_PARAMETER;
    if (data->Command != BC250_ESCAPE_GET_INFO)
    {
        data->Status = 1;
        return STATUS_SUCCESS;
    }
    data->Status = 0;
    data->Version = BC250_KMD_VERSION;
    data->LastStage = (unsigned long)GuardLastStage();
    data->Width = device->Post.Width;
    data->Height = device->Post.Height;
    data->Pitch = device->Post.Pitch;
    data->ColorFormat = (unsigned long)device->Post.ColorFormat;
    data->Presents = g_Presents;
    return STATUS_SUCCESS;
}

// ---- VidPN ------------------------------------------------------------------------------------------------

static void FillSignalInfo(_In_ const BC250_DEVICE* Device, _Out_ D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    // We did not set this mode and cannot read its timing without display-core MMIO: say so.
    RtlZeroMemory(Signal, sizeof(*Signal));
    Signal->VideoStandard = D3DKMDT_VSS_OTHER;
    Signal->TotalSize.cx = Device->Post.Width;
    Signal->TotalSize.cy = Device->Post.Height;
    Signal->ActiveSize = Signal->TotalSize;
    Signal->VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    Signal->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS OfferSourceMode(_In_ const BC250_DEVICE* Device, _In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE* set = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE* mode = NULL;
    NTSTATUS status;

    status = VidPn->pfnCreateNewSourceModeSet(hVidPn, SourceId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;

    status = set->pfnCreateNewModeInfo(hSet, &mode);
    if (NT_SUCCESS(status))
    {
        mode->Type = D3DKMDT_RMT_GRAPHICS;
        mode->Format.Graphics.PrimSurfSize.cx = Device->Post.Width;
        mode->Format.Graphics.PrimSurfSize.cy = Device->Post.Height;
        mode->Format.Graphics.VisibleRegionSize = mode->Format.Graphics.PrimSurfSize;
        mode->Format.Graphics.Stride = Device->Post.Pitch;
        mode->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        mode->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        mode->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        status = set->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(status)) status = VidPn->pfnAssignSourceModeSet(hVidPn, SourceId, hSet);
    if (!NT_SUCCESS(status)) VidPn->pfnReleaseSourceModeSet(hVidPn, hSet);
    return status;
}

static NTSTATUS OfferTargetMode(_In_ const BC250_DEVICE* Device, _In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE* set = NULL;
    D3DKMDT_VIDPN_TARGET_MODE* mode = NULL;
    NTSTATUS status;

    status = VidPn->pfnCreateNewTargetModeSet(hVidPn, TargetId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;

    status = set->pfnCreateNewModeInfo(hSet, &mode);
    if (NT_SUCCESS(status))
    {
        FillSignalInfo(Device, &mode->VideoSignalInfo);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        status = set->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(status)) status = VidPn->pfnAssignTargetModeSet(hVidPn, TargetId, hSet);
    if (!NT_SUCCESS(status)) VidPn->pfnReleaseTargetModeSet(hVidPn, hSet);
    return status;
}

static NTSTATUS SourceModeIsPinned(_In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                   D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId, _Out_ BOOLEAN* Pinned,
                                   _Out_opt_ D3DKMDT_VIDPN_SOURCE_MODE* Copy)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE* set = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE* pinned = NULL;
    NTSTATUS status;

    *Pinned = FALSE;
    status = VidPn->pfnAcquireSourceModeSet(hVidPn, SourceId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;
    status = set->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(status) && pinned != NULL)
    {
        *Pinned = TRUE;
        if (Copy != NULL) *Copy = *pinned;
        set->pfnReleaseModeInfo(hSet, pinned);
    }
    VidPn->pfnReleaseSourceModeSet(hVidPn, hSet);
    return (status == STATUS_GRAPHICS_MODE_NOT_PINNED) ? STATUS_SUCCESS : status;
}

static NTSTATUS TargetModeIsPinned(_In_ const DXGK_VIDPN_INTERFACE* VidPn, D3DKMDT_HVIDPN hVidPn,
                                   D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, _Out_ BOOLEAN* Pinned)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE* set = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE* pinned = NULL;
    NTSTATUS status;

    *Pinned = FALSE;
    status = VidPn->pfnAcquireTargetModeSet(hVidPn, TargetId, &hSet, &set);
    if (!NT_SUCCESS(status)) return status;
    status = set->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(status) && pinned != NULL)
    {
        *Pinned = TRUE;
        set->pfnReleaseModeInfo(hSet, pinned);
    }
    VidPn->pfnReleaseTargetModeSet(hVidPn, hSet);
    return (status == STATUS_GRAPHICS_MODE_NOT_PINNED) ? STATUS_SUCCESS : status;
}

NTSTATUS Bc250IsSupportedVidPn(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_ISSUPPORTEDVIDPN* IsSupportedVidPn)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    SIZE_T paths = 0;
    NTSTATUS status;

    IsSupportedVidPn->IsVidPnSupported = FALSE;
    if (IsSupportedVidPn->hDesiredVidPn == NULL)        // the empty VidPN is always supported
    {
        IsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }
    status = device->Dxgk.DxgkCbQueryVidPnInterface(IsSupportedVidPn->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) return status;
    status = vidpn->pfnGetTopology(IsSupportedVidPn->hDesiredVidPn, &hTopology, &topology);
    if (!NT_SUCCESS(status)) return status;
    status = topology->pfnGetNumPaths(hTopology, &paths);
    if (!NT_SUCCESS(status)) return status;

    // One source, one target: zero or one path. Modes are constrained in EnumVidPnCofuncModality.
    IsSupportedVidPn->IsVidPnSupported = (paths <= 1);
    return STATUS_SUCCESS;
}

NTSTATUS Bc250RecommendFunctionalVidPn(_In_ const HANDLE hAdapter, _In_ const DXGKARG_RECOMMENDFUNCTIONALVIDPN* const Recommend)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(Recommend);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS Bc250EnumVidPnCofuncModality(_In_ const HANDLE hAdapter, _In_ const DXGKARG_ENUMVIDPNCOFUNCMODALITY* const Enum)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH* path = NULL;
    NTSTATUS status;

    status = device->Dxgk.DxgkCbQueryVidPnInterface(Enum->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) return status;
    status = vidpn->pfnGetTopology(Enum->hConstrainingVidPn, &hTopology, &topology);
    if (!NT_SUCCESS(status)) return status;

    status = topology->pfnAcquireFirstPathInfo(hTopology, &path);
    while (NT_SUCCESS(status) && status != STATUS_GRAPHICS_DATASET_IS_EMPTY && path != NULL)
    {
        const D3DKMDT_VIDPN_PRESENT_PATH* next = NULL;
        D3DKMDT_VIDPN_PRESENT_PATH update = *path;
        BOOLEAN pinned;
        BOOLEAN changed = FALSE;
        NTSTATUS step;

        // Source mode set: offer our one mode unless a mode is pinned or this set is the pivot.
        step = SourceModeIsPinned(vidpn, Enum->hConstrainingVidPn, path->VidPnSourceId, &pinned, NULL);
        if (NT_SUCCESS(step) && !pinned &&
            !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE && Enum->EnumPivot.VidPnSourceId == path->VidPnSourceId))
            step = OfferSourceMode(device, vidpn, Enum->hConstrainingVidPn, path->VidPnSourceId);

        // Target mode set: the same.
        if (NT_SUCCESS(step)) step = TargetModeIsPinned(vidpn, Enum->hConstrainingVidPn, path->VidPnTargetId, &pinned);
        if (NT_SUCCESS(step) && !pinned &&
            !(Enum->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET && Enum->EnumPivot.VidPnTargetId == path->VidPnTargetId))
            step = OfferTargetMode(device, vidpn, Enum->hConstrainingVidPn, path->VidPnTargetId);

        // Path transformations: identity only, reported where nothing is pinned yet.
        if (NT_SUCCESS(step))
        {
            if (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_SCALING)
            {
                RtlZeroMemory(&update.ContentTransformation.ScalingSupport, sizeof(update.ContentTransformation.ScalingSupport));
                update.ContentTransformation.ScalingSupport.Identity = 1;
                changed = TRUE;
            }
            if (path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED && Enum->EnumPivotType != D3DKMDT_EPT_ROTATION)
            {
                RtlZeroMemory(&update.ContentTransformation.RotationSupport, sizeof(update.ContentTransformation.RotationSupport));
                update.ContentTransformation.RotationSupport.Identity = 1;
                changed = TRUE;
            }
            if (changed) step = topology->pfnUpdatePathSupportInfo(hTopology, &update);
        }

        if (!NT_SUCCESS(step))
        {
            topology->pfnReleasePathInfo(hTopology, path);
            return step;
        }
        status = topology->pfnAcquireNextPathInfo(hTopology, path, &next);
        topology->pfnReleasePathInfo(hTopology, path);
        path = next;
        if (status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) { status = STATUS_SUCCESS; break; }
    }
    return (status == STATUS_GRAPHICS_DATASET_IS_EMPTY) ? STATUS_SUCCESS : status;
}

NTSTATUS Bc250SetVidPnSourceVisibility(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETVIDPNSOURCEVISIBILITY* Visibility)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;

    if (Visibility->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (!Visibility->Visible && device->SourceVisible && device->Framebuffer != NULL)
        RtlZeroMemory(device->Framebuffer, device->FramebufferLength);      // blank: there is no plane to disable in M3
    device->SourceVisible = Visibility->Visible;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250CommitVidPn(_In_ const HANDLE hAdapter, _In_ const DXGKARG_COMMITVIDPN* const Commit)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    const DXGK_VIDPN_INTERFACE* vidpn = NULL;
    D3DKMDT_HVIDPNTOPOLOGY hTopology = NULL;
    const DXGK_VIDPNTOPOLOGY_INTERFACE* topology = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE mode;
    SIZE_T paths = 0;
    BOOLEAN pinned;
    NTSTATUS status;

    if (!device->CommitSeen) { device->CommitSeen = TRUE; GuardStage(StageFirstCommitVidPn); }
    if (Commit->Flags.PathPowerTransition) return STATUS_SUCCESS;       // no mode change, power only

    status = device->Dxgk.DxgkCbQueryVidPnInterface(Commit->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(status)) return status;
    status = vidpn->pfnGetTopology(Commit->hFunctionalVidPn, &hTopology, &topology);
    if (!NT_SUCCESS(status)) return status;
    status = topology->pfnGetNumPaths(hTopology, &paths);
    if (!NT_SUCCESS(status)) return status;
    // From here on the source counts as unusable until this commit has validated its mode: presents are
    // dropped while ModeActive is clear, so a surface of another size can never be copied from.
    device->ModeActive = FALSE;
    if (paths == 0) return STATUS_SUCCESS;                               // source detached: nothing to program

    status = SourceModeIsPinned(vidpn, Commit->hFunctionalVidPn, 0, &pinned, &mode);
    if (!NT_SUCCESS(status)) return status;
    if (!pinned) return STATUS_SUCCESS;     // a path without a pinned source mode: nothing to show, not an error
                                            // (STATUS_GRAPHICS_MODE_NOT_PINNED has success severity anyway)

    // The only mode we can show is the one already on the wire. Accept exactly that.
    if (mode.Type != D3DKMDT_RMT_GRAPHICS ||
        mode.Format.Graphics.PrimSurfSize.cx != device->Post.Width ||
        mode.Format.Graphics.PrimSurfSize.cy != device->Post.Height)
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    device->ModeActive = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250UpdateActiveVidPnPresentPath(_In_ const HANDLE hAdapter, _In_ const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* const Update)
{
    UNREFERENCED_PARAMETER(hAdapter);
    if (Update->VidPnPresentPathInfo.ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY &&
        Update->VidPnPresentPathInfo.ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED)
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250RecommendMonitorModes(_In_ const HANDLE hAdapter, _In_ const DXGKARG_RECOMMENDMONITORMODES* const Recommend)
{
    // The monitor has no descriptor in M3 (no EDID), so the OS would assume a default monitor that may not
    // list the firmware's resolution. Tell it that this monitor can do the mode it is showing right now.
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    D3DKMDT_MONITOR_SOURCE_MODE* mode = NULL;
    NTSTATUS status;

    status = Recommend->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(Recommend->hMonitorSourceModeSet, &mode);
    if (!NT_SUCCESS(status)) return status;

    FillSignalInfo(device, &mode->VideoSignalInfo);
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 8;
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;

    status = Recommend->pMonitorSourceModeSetInterface->pfnAddMode(Recommend->hMonitorSourceModeSet, mode);
    if (!NT_SUCCESS(status))
    {
        Recommend->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Recommend->hMonitorSourceModeSet, mode);
        if (status == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) status = STATUS_SUCCESS;
    }
    return status;
}

NTSTATUS Bc250QueryVidPnHWCapability(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY* Capability)
{
    UNREFERENCED_PARAMETER(hAdapter);
    RtlZeroMemory(&Capability->VidPnHWCaps, sizeof(Capability->VidPnHWCaps));     // no hardware rotation, scaling or cloning
    return STATUS_SUCCESS;
}

// ---- present ----------------------------------------------------------------------------------------------

static void CopyRect(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_PRESENT_DISPLAYONLY* Present, RECT Rect)
{
    LONG y;

    if (Rect.left < 0) Rect.left = 0;
    if (Rect.top < 0) Rect.top = 0;
    if (Rect.right > (LONG)Device->Post.Width) Rect.right = (LONG)Device->Post.Width;
    if (Rect.bottom > (LONG)Device->Post.Height) Rect.bottom = (LONG)Device->Post.Height;
    if (Rect.left >= Rect.right || Rect.top >= Rect.bottom) return;

    for (y = Rect.top; y < Rect.bottom; y++)
    {
        const UCHAR* from = (const UCHAR*)Present->pSource + (SIZE_T)y * Present->Pitch + (SIZE_T)Rect.left * 4;
        UCHAR* to = (UCHAR*)Device->Framebuffer + (SIZE_T)y * Device->Post.Pitch + (SIZE_T)Rect.left * 4;
        RtlCopyMemory(to, from, (SIZE_T)(Rect.right - Rect.left) * 4);
    }
}

NTSTATUS Bc250PresentDisplayOnly(_In_ const HANDLE hAdapter, _In_ const DXGKARG_PRESENT_DISPLAYONLY* Present)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    ULONG i;

    if (Present->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (Present->BytesPerPixel != 4 || Present->Pitch <= 0) return STATUS_GRAPHICS_INVALID_PIXELFORMAT;
    if (!device->Started || device->Framebuffer == NULL || !device->SourceVisible || !device->ModeActive)
        return STATUS_SUCCESS;

    if (!device->PresentSeen) GuardStage(StageFirstPresent);

    // The source image is the complete new frame, so a move is just one more rectangle that changed.
    for (i = 0; i < Present->NumMoves; i++) CopyRect(device, Present, Present->pMoves[i].DestRect);
    for (i = 0; i < Present->NumDirtyRects; i++) CopyRect(device, Present, Present->pDirtyRect[i]);
    g_Presents++;

    if (!device->PresentSeen) { device->PresentSeen = TRUE; GuardStage(StageFirstPresentDone); }
    return STATUS_SUCCESS;
}

// ---- system display (bugcheck screen): high IRQL, no pool, no registry ---------------------------------------

NTSTATUS Bc250SystemDisplayEnable(_In_ const PVOID MiniportDeviceContext, _In_ const D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                  _In_ const PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, _Out_ UINT* Width, _Out_ UINT* Height,
                                  _Out_ D3DDDIFORMAT* ColorFormat)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Flags);
    *Width = device->Post.Width;
    *Height = device->Post.Height;
    *ColorFormat = device->Post.ColorFormat;
    return (device->Framebuffer != NULL) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

void Bc250SystemDisplayWrite(_In_ const PVOID MiniportDeviceContext, _In_ const PVOID Source, _In_ const UINT SourceWidth,
                             _In_ const UINT SourceHeight, _In_ const UINT SourceStride, _In_ const UINT PositionX,
                             _In_ const UINT PositionY)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    UINT y, width, height;

    if (device->Framebuffer == NULL || PositionX >= device->Post.Width || PositionY >= device->Post.Height) return;
    width = min(SourceWidth, device->Post.Width - PositionX);
    height = min(SourceHeight, device->Post.Height - PositionY);
    for (y = 0; y < height; y++)
        RtlCopyMemory((UCHAR*)device->Framebuffer + (SIZE_T)(PositionY + y) * device->Post.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR*)Source + (SIZE_T)y * SourceStride, (SIZE_T)width * 4);
}
