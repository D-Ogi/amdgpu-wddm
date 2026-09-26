// Retained adapter power coordination; OS SetPowerState supplies Level Three.
#include "bc250kmd.h"

#define BC250_POWER_TAG 'wP2B'

static NTSTATUS PowerRestoreDisplay(BC250_DEVICE* Device)
{
    DXGK_DISPLAY_INFORMATION post;
    NTSTATUS status;
    RtlZeroMemory(&post,sizeof(post));
    status=Device->Dxgk.DxgkCbAcquirePostDisplayOwnership(Device->Dxgk.DeviceHandle,&post);
    if (!NT_SUCCESS(status)) return status;
    // Current inherited-mode implementation: never change allocation geometry
    // underneath retained OS handles. General mode initialization remains separate.
    if (post.Width!=Device->Post.Width || post.Height!=Device->Post.Height ||
        post.Pitch!=Device->Post.Pitch || post.ColorFormat!=Device->Post.ColorFormat ||
        post.PhysicAddress.QuadPart!=Device->Post.PhysicAddress.QuadPart)
        return STATUS_GRAPHICS_INVALID_VIDPN;
    status=DcnSetVisibility(Device,FALSE);
    if (!NT_SUCCESS(status)) return status;
    Device->SourceVisible=FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS GpuSetPowerRetained(BC250_DEVICE* Device, DEVICE_POWER_STATE State, POWER_ACTION Action)
{
    BC250_ESCAPE_PSP* report;
    struct bc250_clock_report clock;
    NTSTATUS status=STATUS_INVALID_DEVICE_STATE;
    LONG phase;
    if (!Device || KeGetCurrentIrql()!=PASSIVE_LEVEL || !Device->Started || !Device->FullWddm)
        return STATUS_INVALID_DEVICE_STATE;
    if (State<PowerDeviceD0 || State>PowerDeviceD3) return STATUS_INVALID_PARAMETER;
    phase=InterlockedCompareExchange(&Device->RetainedPowerPhase,0,0);
    if (State==PowerDeviceD0 && phase==0) return STATUS_SUCCESS;
    if (State!=PowerDeviceD0 && phase==2) return STATUS_SUCCESS;
    if ((State==PowerDeviceD0 && phase!=2) || (State!=PowerDeviceD0 && phase!=0))
        return STATUS_INVALID_DEVICE_STATE;
    report=(BC250_ESCAPE_PSP*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*report),BC250_POWER_TAG);
    if (!report) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(report,sizeof(*report));
    if (State!=PowerDeviceD0) {
        Device->RetainedDownState=State;
        Device->RetainedDownAction=Action;
        InterlockedExchange(&Device->RetainedPowerPhase,1);
        StartHealthClose(Device);
        SmuOwnerStop(&Device->Smu); // joins no-synchronization telemetry before power loss
        status=WddmSuspendRetained(Device);
        if (!NT_SUCCESS(status)) goto Failed;
        status=IhSetPowerRetained(Device,FALSE);
        if (!NT_SUCCESS(status)) goto Failed;
        status=GfxSetPowerRetained(Device,FALSE);
        if (!NT_SUCCESS(status)) goto Failed;
        status=PspSetPowerRetained(Device,FALSE,report);
        if (!NT_SUCCESS(status)) goto Failed;
        status=GartSetPowerRetained(Device,FALSE);
        if (!NT_SUCCESS(status)) goto Failed;
        InterlockedExchange(&Device->RetainedPowerPhase,2);
        goto Done;
    }
    // Action on D0 is not authoritative: use the retained down-transition state.
    InterlockedExchange(&Device->RetainedPowerPhase,3);
    status=SmuOwnerStart(&Device->Smu,Device->Mmio);
    if (!NT_SUCCESS(status)) goto Failed;
    status=SmuPrepareClock(&Device->Smu,&clock);
    if (!NT_SUCCESS(status)) goto Failed;
    if (!clock.ready) {status=STATUS_DEVICE_NOT_READY;goto Failed;}
    status=PowerRestoreDisplay(Device);
    if (!NT_SUCCESS(status)) goto Failed;
    status=GartSetPowerRetained(Device,TRUE);
    if (!NT_SUCCESS(status)) goto Failed;
    status=PspSetPowerRetained(Device,TRUE,report);
    if (!NT_SUCCESS(status)) goto Failed;
    status=IhSetPowerRetained(Device,TRUE);
    if (!NT_SUCCESS(status)) goto Failed;
    status=GfxSetPowerRetained(Device,TRUE);
    if (!NT_SUCCESS(status)) goto Failed;
    if (Device->GfxTlbBootstrap || !GfxStartupResources(Device,TRUE)) {
        status=STATUS_DEVICE_NOT_READY;goto Failed;
    }
    status=WddmResumeRetained(Device);
    if (!NT_SUCCESS(status)) goto Failed;
    StartHealthResumeReady(Device,Device->ModeActive);
    InterlockedExchange(&Device->RetainedPowerPhase,0);
    goto Done;
Failed:
    // No destructive PnP unwind and no synthetic completion of accepted work.
    StartHealthClose(Device);
    SmuOwnerStop(&Device->Smu);
    InterlockedExchange(&Device->RetainedPowerPhase,4);
Done:
    GuardLog("power: state %u prior action %u phase %ld result 0x%08X",(ULONG)State,
        (ULONG)Device->RetainedDownAction,Device->RetainedPowerPhase,status);
    ExFreePoolWithTag(report,BC250_POWER_TAG);
    return status;
}
