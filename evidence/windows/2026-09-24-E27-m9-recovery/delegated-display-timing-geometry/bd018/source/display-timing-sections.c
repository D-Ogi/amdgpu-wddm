#include "display_timing.h"
static NTSTATUS FillSignalInfo(_In_ const BC250_DEVICE* Device, _Out_ D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
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
    // Display-only retains its documented unspecified-timing path without MMIO.
    // Full WDDM must provide the actual inherited timing or refuse the mode;
    // nominal60Hz and zero blanking are not hardware measurements.
    if (Device->FullWddm) return DisplayReadInheritedTiming(Device,Signal);
    return STATUS_SUCCESS;
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
        status=FillSignalInfo(Device, &mode->VideoSignalInfo);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        if (NT_SUCCESS(status)) status = set->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(status)) set->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(status)) status = VidPn->pfnAssignTargetModeSet(hVidPn, TargetId, hSet);
    if (!NT_SUCCESS(status)) VidPn->pfnReleaseTargetModeSet(hVidPn, hSet);
    return status;
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

    status=FillSignalInfo(device, &mode->VideoSignalInfo);
    if (!NT_SUCCESS(status)) {
        Recommend->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(Recommend->hMonitorSourceModeSet, mode);
        return status;
    }
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
