// Retained adapter power coordination; OS SetPowerState supplies Level Three.
#include "bc250kmd.h"
#include "hdmi_link.h"

#define BC250_POWER_TAG 'wP2B'

static NTSTATUS PowerRestoreDisplay(BC250_DEVICE* Device)
{
    DXGK_DISPLAY_INFORMATION post;
    NTSTATUS status;
    RtlZeroMemory(&post,sizeof(post));
    status=Device->Dxgk.DxgkCbAcquirePostDisplayOwnership(Device->Dxgk.DeviceHandle,&post);
    if (ConnectorIsEnabled(Device)) {
        // The cold path reinitializes a positively identified sink using its
        // private reserved surface, never the OS-returned address. POST may
        // describe a different firmware mode or have Width zero on resume.
        // BD-188: without a POST answer the driver "should not assume that any
        // specific display mode is currently enabled" and initializes the
        // display (dispmprt.md): the sink is found again after phase 0, and
        // the engines resume behind a black output instead of staying down.
        NTSTATUS display=ConnectorResumeDisplay(Device);
        if (!NT_SUCCESS(display)) return display;
        if (!NT_SUCCESS(status)) {
            GuardLog("power: POST display ownership 0x%08X at D0; the sink is found again",(ULONG)status);
            InterlockedExchange(&Device->Connector.Pending,1);
            Device->ModeActive=FALSE;
        }
        if (InterlockedCompareExchange(&Device->RuntimeDisplayChanged,0,0) ||
            InterlockedCompareExchange(&Device->Connector.Pending,0,0)) {
            // A changed or unreadable sink must not replay its old timing, but
            // WDDM still needs a confirmed black output before reopening work.
            status=DcnSetVisibility(Device,FALSE);
            if (!NT_SUCCESS(status)) return status;
            Device->SourceVisible=FALSE;
            return STATUS_SUCCESS;
        }
    }
    if (!NT_SUCCESS(status)) return status;
    // Current inherited-mode implementation: never change allocation geometry
    // underneath retained OS handles. General mode initialization remains separate.
    if (post.Width!=Device->Post.Width || post.Height!=Device->Post.Height ||
        post.Pitch!=Device->Post.Pitch || post.ColorFormat!=Device->Post.ColorFormat ||
        post.PhysicAddress.QuadPart!=Device->Post.PhysicAddress.QuadPart)
        return STATUS_GRAPHICS_INVALID_VIDPN;
    status=ModesetValidateInherited(Device);
    if (!NT_SUCCESS(status)) return status;
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
    if ((State==PowerDeviceD0 && phase!=2) || (State!=PowerDeviceD0 && phase!=0)) {
        // A D0 after a failed transition (phase 4) restores nothing: the log must say so (BD-188).
        GuardLog("power: state %u refused in phase %ld",(ULONG)State,phase);
        return STATUS_INVALID_DEVICE_STATE;
    }
    report=(BC250_ESCAPE_PSP*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*report),BC250_POWER_TAG);
    if (!report) {
        // BD-188: a way down that saves nothing must not leave phase 0. The D0
        // after it would find nothing to restore and report the engines as up.
        if (State!=PowerDeviceD0) {
            StartHealthClose(Device);
            SmuOwnerStop(&Device->Smu);
            InterlockedExchange(&Device->RetainedPowerPhase,4);
        }
        GuardLog("power: state %u no report memory, phase %ld",(ULONG)State,Device->RetainedPowerPhase);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
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
    if (ConnectorIsEnabled(Device)) {
        status=ConnectorResumeNotifications(Device);
        if (!NT_SUCCESS(status)) goto Failed;
    }
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

// The way down of a started full-WDDM adapter (pnp.c's Bc250SetPowerState, after the pauses): the display half,
// then the engines. A failed display step is logged and never stops the save of the engines. DxgkDdiSetPowerState
// "should never fail" (dispmprt.md), and the bus driver takes the adapter's power after this call whatever it
// returns, so an engine that stays running in software meets a chip without firmware at D0.
// BD-188 (lab 2026-10-19, tester.31): the refused HUBP write of the firmware surface restore (BD-090) failed
// ConnectorSuspend with 0xC0000022, Bc250SetPowerState returned before GpuSetPowerRetained, the D0 after the
// hibernation found phase 0 and restored nothing, and the first paging job never completed on SDMA0 (0x116).
NTSTATUS PowerAdapterDown(BC250_DEVICE* Device, DEVICE_POWER_STATE State, POWER_ACTION Action)
{
    NTSTATUS display=STATUS_SUCCESS;
    // The hardware pointer off before anything else: the static inherited path has no connector suspend, and the
    // connector's suspend can stop early. A pointer left enabled would be written again over reset registers at D0.
    DcnCursorPipeGone(Device);
    if (ConnectorIsEnabled(Device)) display=ConnectorSuspend(Device);
    if (!InterlockedCompareExchange(&Device->RuntimeDisplayChanged,0,0)) {
        if (!HdmiLinkStop(Device) && NT_SUCCESS(display)) display=STATUS_DEVICE_DATA_ERROR;
        ModesetPowerDown(Device);
    }
    if (!NT_SUCCESS(display))
        GuardLog("power: display suspend for action %u failed 0x%08X; the engines are saved all the same",
            (ULONG)Action,(ULONG)display);
    return GpuSetPowerRetained(Device,State,Action);
}
