// DriverEntry: hands the display-only DDI table to dxgkrnl (ADR 0006).
#include "bc250kmd.h"

DRIVER_INITIALIZE DriverEntry;

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    KMDDOD_INITIALIZATION_DATA data;
    NTSTATUS status;

    status = GuardInit(RegistryPath);
    if (!NT_SUCCESS(status)) return status;
    GuardStage(StageDriverEntry);

    RtlZeroMemory(&data, sizeof(data));
    data.Version = DXGKDDI_INTERFACE_VERSION_WIN8;      // the display-only model's baseline; nothing newer is used

    data.DxgkDdiAddDevice = Bc250AddDevice;
    data.DxgkDdiStartDevice = Bc250StartDevice;
    data.DxgkDdiStopDevice = Bc250StopDevice;
    data.DxgkDdiRemoveDevice = Bc250RemoveDevice;
    data.DxgkDdiResetDevice = Bc250ResetDevice;
    data.DxgkDdiDispatchIoRequest = Bc250DispatchIoRequest;
    data.DxgkDdiInterruptRoutine = Bc250InterruptRoutine;
    data.DxgkDdiDpcRoutine = Bc250DpcRoutine;
    data.DxgkDdiQueryChildRelations = Bc250QueryChildRelations;
    data.DxgkDdiQueryChildStatus = Bc250QueryChildStatus;
    data.DxgkDdiQueryDeviceDescriptor = Bc250QueryDeviceDescriptor;
    data.DxgkDdiSetPowerState = Bc250SetPowerState;
    data.DxgkDdiUnload = Bc250Unload;
    data.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = Bc250StopDeviceAndReleasePostDisplayOwnership;

    data.DxgkDdiQueryAdapterInfo = Bc250QueryAdapterInfo;
    data.DxgkDdiSetPointerPosition = Bc250SetPointerPosition;
    data.DxgkDdiSetPointerShape = Bc250SetPointerShape;
    data.DxgkDdiEscape = Bc250Escape;
    data.DxgkDdiIsSupportedVidPn = Bc250IsSupportedVidPn;
    data.DxgkDdiRecommendFunctionalVidPn = Bc250RecommendFunctionalVidPn;
    data.DxgkDdiEnumVidPnCofuncModality = Bc250EnumVidPnCofuncModality;
    data.DxgkDdiSetVidPnSourceVisibility = Bc250SetVidPnSourceVisibility;
    data.DxgkDdiCommitVidPn = Bc250CommitVidPn;
    data.DxgkDdiUpdateActiveVidPnPresentPath = Bc250UpdateActiveVidPnPresentPath;
    data.DxgkDdiRecommendMonitorModes = Bc250RecommendMonitorModes;
    data.DxgkDdiQueryVidPnHWCapability = Bc250QueryVidPnHWCapability;
    data.DxgkDdiPresentDisplayOnly = Bc250PresentDisplayOnly;
    data.DxgkDdiSystemDisplayEnable = Bc250SystemDisplayEnable;
    data.DxgkDdiSystemDisplayWrite = Bc250SystemDisplayWrite;

    status = DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &data);
    if (!NT_SUCCESS(status))
    {
        GuardLog("DxgkInitializeDisplayOnlyDriver failed 0x%08X", status);
        GuardCleanup();
    }
    return status;
}
