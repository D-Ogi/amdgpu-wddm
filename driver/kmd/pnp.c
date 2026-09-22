// Device life cycle, the one child (the monitor output), power. M3: no MMIO, no interrupt (ADR 0006).
#include "bc250kmd.h"

NTSTATUS Bc250AddDevice(_In_ const PDEVICE_OBJECT PhysicalDeviceObject, _Outptr_ PVOID* MiniportDeviceContext)
{
    BC250_DEVICE* device;

    GuardStage(StageAddDevice);
    device = (BC250_DEVICE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*device), BC250_TAG);
    if (device == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    device->PhysicalDeviceObject = PhysicalDeviceObject;
    device->Rotation = D3DKMDT_VPPR_IDENTITY;
    ExInitializeFastMutex(&device->GartLock);
    *MiniportDeviceContext = device;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250RemoveDevice(_In_ const PVOID MiniportDeviceContext)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    DisplayUnmapFramebuffer(device);
    IhRemove(device);
    ExFreePoolWithTag(device, BC250_TAG);
    return STATUS_SUCCESS;
}

static void NoteInterruptResource(_Inout_ BC250_DEVICE* Device);

NTSTATUS Bc250StartDevice(_In_ const PVOID MiniportDeviceContext, _In_ PDXGK_START_INFO DxgkStartInfo,
                          _In_ PDXGKRNL_INTERFACE DxgkInterface, _Out_ PULONG NumberOfVideoPresentSources,
                          _Out_ PULONG NumberOfChildren)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    NTSTATUS status;

    *NumberOfVideoPresentSources = 0;
    *NumberOfChildren = 0;
    GuardStage(StageStartEnter);

    status = GuardCheckAndCountStart();
    if (!NT_SUCCESS(status))
    {
        GuardStage(StageRefusedByGuard);
        return status;
    }
    GuardStage(StageStartGuardPassed);

    device->StartInfo = *DxgkStartInfo;
    // dxgkrnl hands out the interface at the size of the version we asked for (WIN8), while this structure is
    // compiled at the newest layout: copy what was given, not what we could hold.
    RtlZeroMemory(&device->Dxgk, sizeof(device->Dxgk));
    RtlCopyMemory(&device->Dxgk, DxgkInterface, min((SIZE_T)DxgkInterface->Size, sizeof(device->Dxgk)));

    status = device->Dxgk.DxgkCbGetDeviceInformation(device->Dxgk.DeviceHandle, &device->DeviceInfo);
    if (!NT_SUCCESS(status)) goto failed;
    GuardStage(StageStartDeviceInfo);

    // The firmware's mode and framebuffer. Without it there is nothing this driver could show: M3 sets no mode.
    status = device->Dxgk.DxgkCbAcquirePostDisplayOwnership(device->Dxgk.DeviceHandle, &device->Post);
    if (!NT_SUCCESS(status)) goto failed;
    if (device->Post.Width == 0 || device->Post.Height == 0 || device->Post.PhysicAddress.QuadPart == 0)
    {
        status = STATUS_GRAPHICS_NO_VIDEO_MEMORY;       // honest: no firmware display to take over
        goto failed;
    }
    GuardLog("post display %ux%u pitch %u format %u target %u", device->Post.Width, device->Post.Height,
             device->Post.Pitch, (ULONG)device->Post.ColorFormat, device->Post.TargetId);
    GuardStage(StageStartPostDisplayAcquired);

    status = DisplayMapFramebuffer(device);
    if (!NT_SUCCESS(status)) goto failed;
    GuardStage(StageStartFramebufferMapped);

    NoteInterruptResource(device);
    MmioStart(device);      // maps nothing unless the registry gate is open; never fails the start
    VramStart(device);      // same rule
    GartStart(device);      // same rule
    PspStart(device);       // same rule
    GpuMemStart(device);    // same rule
    GfxStart(device);       // same rule
    IhStart(device);        // same rule
    WddmStart(device);      // same rule: does nothing at all unless EnableFullWddm opened the gate in DriverEntry
    GuardStage(StageStartMmioDone);

    device->Started = TRUE;
    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = 1;
    GuardStage(StageStartDone);
    return STATUS_SUCCESS;

failed:
    GuardLog("start failed 0x%08X", status);
    GuardStage(StageStartFailed);
    return status;
}

NTSTATUS Bc250StopDevice(_In_ const PVOID MiniportDeviceContext)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    GuardStage(StageStopEnter);
    device->Started = FALSE;
    device->ModeActive = FALSE;
    device->SourceVisible = FALSE;      // so that the next start writes its own first-commit and first-present breadcrumbs
    device->CommitSeen = FALSE;
    device->PresentSeen = FALSE;
    WddmStop(device);       // first: it logs what dxgkrnl called, and nothing below it is allowed to have run
    IhStop(device);         // no interrupt of ours from here on
    GfxStop(device);        // then, in amdgpu's order: engines halted, then their memory (gpumem.c) given back or kept
    PspStop(device);        // then, in amdgpu's order: the PSP forgets our ring and TMR while the GART state still stands
    GartStop(device);       // while the registers are still mapped: it may have a state to give back
    VramStop(device);
    // ADR 0011, consequences: if a flip ever moved HUBP0 off the firmware's own address, put it back before
    // BAR5 goes away. A no-op, logged as one, when nothing ever flipped (dcn.c's DcnStop).
    DcnStop(device);
    MmioStop(device);
    DisplayUnmapFramebuffer(device);
    GuardStage(StageStopDone);
    // Last, so that the file holds the stop as well. Does nothing unless Parameters\KeepLog is set: dxgkrnl may
    // end a full WDDM start by itself and unload the driver after, ring and all (E16 run 1).
    GuardLogKeep();
    return STATUS_SUCCESS;
}

// PnP stop with the screen kept: report the mode we leave behind so that the next owner (Basic Display on a
// rollback) can continue on the same framebuffer.
NTSTATUS Bc250StopDeviceAndReleasePostDisplayOwnership(_In_ PVOID MiniportDeviceContext,
                                                       _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                       _Out_ PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    // The mode is the one we were given; the target is the one dxgkrnl is releasing now. Post.TargetId is the
    // previous owner's name for it, or D3DDDI_ID_UNINITIALIZED after a reload (E16 step 1), and means nothing to
    // the next owner.
    *DisplayInfo = device->Post;
    DisplayInfo->TargetId = TargetId;
    return Bc250StopDevice(MiniportDeviceContext);
}

void Bc250ResetDevice(_In_ const PVOID MiniportDeviceContext)
{
    // Called at high IRQL on the way to a bugcheck or hibernate. We never changed the mode: nothing to restore.
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
}

NTSTATUS Bc250DispatchIoRequest(_In_ const PVOID MiniportDeviceContext, _In_ ULONG VidPnSourceId,
                                _In_ PVIDEO_REQUEST_PACKET VideoRequestPacket)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(VideoRequestPacket);
    return STATUS_NOT_SUPPORTED;
}

// What Windows assigned: a message (INF, MessageSignaledInterruptProperties) or the line. For the log and the IH state.
static void NoteInterruptResource(_Inout_ BC250_DEVICE* Device)
{
    PCM_RESOURCE_LIST list = Device->DeviceInfo.TranslatedResourceList;
    ULONG i, j, found = 0;

    InterlockedExchange(&Device->InterruptCount, 0);
    for (i = 0; list != NULL && i < list->Count; i++)
    {
        PCM_PARTIAL_RESOURCE_LIST partial = &list->List[i].PartialResourceList;
        for (j = 0; j < partial->Count; j++)
        {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &partial->PartialDescriptors[j];
            if (d->Type != CmResourceTypeInterrupt) continue;
            found++;
            Device->InterruptIsMessage = (d->Flags & CM_RESOURCE_INTERRUPT_MESSAGE) != 0;
            Device->InterruptVector = Device->InterruptIsMessage ? d->u.MessageInterrupt.Translated.Vector : d->u.Interrupt.Vector;
            GuardLog("interrupt resource %u: %s, vector 0x%X, flags 0x%X", found, Device->InterruptIsMessage ? "message" : "line",
                     Device->InterruptVector, (ULONG)d->Flags);
        }
    }
    if (found == 0) GuardLog("no interrupt resource");
}

// The display path enables no interrupt source; the only one this driver ever enables is the IH ring (ih.c, behind its
// gate). Everything else that fires on a shared line is not ours. Counted either way: the count is evidence.
BOOLEAN Bc250InterruptRoutine(_In_ const PVOID MiniportDeviceContext, _In_ ULONG MessageNumber)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    InterlockedIncrement(&device->InterruptCount);
    InterlockedExchange(&device->LastMessageNumber, (LONG)MessageNumber);
    return IhInterrupt(device);
}

void Bc250DpcRoutine(_In_ const PVOID MiniportDeviceContext)
{
    IhDpc((BC250_DEVICE*)MiniportDeviceContext);
    WddmGpuFence((BC250_DEVICE*)MiniportDeviceContext); // stage C: the vectors are consumed, the fence memory says whose they were
    WddmDpc((BC250_DEVICE*)MiniportDeviceContext);      // returns at once unless the full table is in use
}

NTSTATUS Bc250QueryChildRelations(_In_ const PVOID MiniportDeviceContext,
                                  _Inout_updates_bytes_(ChildRelationsSize) PDXGK_CHILD_DESCRIPTOR ChildRelations,
                                  _In_ ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);

    // dxgkrnl sizes the array for NumberOfChildren + 1; the terminating entry stays zeroed.
    if (ChildRelationsSize < 2 * sizeof(DXGK_CHILD_DESCRIPTOR)) return STATUS_BUFFER_TOO_SMALL;
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;    // M3 cannot sense hot plug
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_OTHER;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = BC250_CHILD_UID;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250QueryChildStatus(_In_ const PVOID MiniportDeviceContext, _Inout_ PDXGK_CHILD_STATUS ChildStatus,
                               _In_ BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);

    // This buffer belongs to dxgkrnl and is sized for the version this driver declares, WIN8. DXGK_CHILD_STATUS
    // grows from 12 to 16 bytes at WDDM 1.3 and later, where the union gains its Miracast arm
    // (dispmprt.h:338-343), and this driver's compiled view of the structure is the larger one. So: write only
    // inside the first 12 bytes, which is what the two arms below do, and never zero, copy or assign the whole
    // structure - that would write past the end of what dxgkrnl allocated.
    if (ChildStatus->ChildUid != BC250_CHILD_UID) return STATUS_INVALID_PARAMETER;
    switch (ChildStatus->Type)
    {
    case StatusConnection:
        ChildStatus->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        ChildStatus->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

NTSTATUS Bc250QueryDeviceDescriptor(_In_ const PVOID MiniportDeviceContext, _In_ ULONG ChildUid,
                                    _Inout_ PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    // No EDID in M3: reading it needs the AUX channel, which is display-core MMIO (facts M24).
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(ChildUid);
    UNREFERENCED_PARAMETER(DeviceDescriptor);
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

NTSTATUS Bc250SetPowerState(_In_ const PVOID MiniportDeviceContext, _In_ ULONG DeviceUid,
                            _In_ DEVICE_POWER_STATE DevicePowerState, _In_ POWER_ACTION ActionType)
{
    // Nothing of ours to save or restore: the hardware state belongs to the firmware and, for clocks, to the
    // SMU requests made through tools/win/bc250rd.
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(DeviceUid);
    UNREFERENCED_PARAMETER(DevicePowerState);
    UNREFERENCED_PARAMETER(ActionType);
    return STATUS_SUCCESS;
}

void Bc250Unload(void)
{
    GuardCleanup();
}
