// DriverEntry: the gate of ADR 0008 point 1. One binary, two DDI tables.
//
//   <service key>\Parameters
//     EnableFullWddm  REG_DWORD  0 (default, and what every install writes) = the display-only table of M3,
//                                DxgkInitializeDisplayOnlyDriver, the driver that runs the owner's display today.
//                                1 = the full WDDM table of M7 stage A, DxgkInitialize (wddm.c).
//
// The gate is read before anything else is done with the driver object, so that at 0 not a line of M7's code runs.
// Both paths are counted by the same start budget (guard.c), because both can cost a boot.
#include "bc250kmd.h"

DRIVER_INITIALIZE DriverEntry;

// The display-only table, unchanged since M3. `Version` is a run-time value and stays at the display-only model's
// baseline; it is not the DXGKDDI_INTERFACE_VERSION the binary is compiled at (bc250kmd.h says why they differ).
static NTSTATUS InitializeDisplayOnly(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    KMDDOD_INITIALIZATION_DATA data;
    NTSTATUS status;

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
    if (!NT_SUCCESS(status)) GuardLog("DxgkInitializeDisplayOnlyDriver failed 0x%08X", status);
    return status;
}

// M7 stage A: the same 28 pointers plus what a full miniport may not leave out (wddm.c builds the table).
static NTSTATUS InitializeFullWddm(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    DRIVER_INITIALIZATION_DATA data;
    NTSTATUS status;

    WddmBuildTable(&data);
    status = DxgkInitialize(DriverObject, RegistryPath, &data);
    if (!NT_SUCCESS(status)) GuardLog("DxgkInitialize failed 0x%08X", status);
    return status;
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;

    status = GuardInit(RegistryPath);
    if (!NT_SUCCESS(status)) return status;
    PagingJournalInit();
    GuardStage(StageDriverEntry);

    status = WddmGateOpen() ? InitializeFullWddm(DriverObject, RegistryPath)
                            : InitializeDisplayOnly(DriverObject, RegistryPath);
    if (!NT_SUCCESS(status)) GuardCleanup();
    return status;
}
