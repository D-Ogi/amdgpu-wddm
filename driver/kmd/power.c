// Retained adapter power coordination; OS SetPowerState supplies Level Three.
#include "bc250kmd.h"
#include "hdmi_link.h"
#include "power_loss.h"

#define BC250_POWER_TAG 'wP2B'

C_ASSERT(PowerDeviceD3 == BC250_POWER_DEVICE_D3);
C_ASSERT(PowerActionSleep == BC250_POWER_ACTION_SLEEP);
C_ASSERT(PowerActionHibernate == BC250_POWER_ACTION_HIBERNATE);

// BD-188 D-2 (docs/design/power-loss.md section 4): the last act of a way down whose power goes. The pattern goes
// into the witness register and is read back; only a read-back that matches arms the witness. A device without a
// witness (power_loss.h) is never armed, so its D0 verdict is "no proof".
static void PowerWitnessArm(BC250_DEVICE* Device)
{
    ULONG value=0;
    NTSTATUS status=STATUS_NOT_SUPPORTED;
    Device->PowerWitnessArmed=FALSE;
    if (Device->PowerWitness!=BC250_POWER_WITNESS_NONE) {
        status=GfxPowerWitnessWrite(Device,Device->PowerWitness,BC250_POWER_WITNESS_PATTERN);
        if (NT_SUCCESS(status)) status=GfxPowerWitnessRead(Device,Device->PowerWitness,&value);
        Device->PowerWitnessArmed=NT_SUCCESS(status) && value==BC250_POWER_WITNESS_PATTERN;
    }
    GuardLog("power: witness %lu armed %u (status 0x%08X, read back 0x%08X)",Device->PowerWitness,
        (ULONG)Device->PowerWitnessArmed,(ULONG)status,value);
}

// D0, before any restore step: the verdict of the witness (BC250_POWER_WITNESS_LOST, KEPT or UNPROVEN), then 0 into
// the register, so that a later D0 cannot read a stale pattern. Unsaved: whether the way down was unsaved (the log
// line only; the caller acts on the verdict).
static ULONG PowerWitnessTake(BC250_DEVICE* Device, BOOLEAN Unsaved)
{
    ULONG value=0,verdict;
    NTSTATUS status=STATUS_NOT_SUPPORTED;
    if (Device->PowerWitnessArmed) status=GfxPowerWitnessRead(Device,Device->PowerWitness,&value);
    verdict=Bc250PowerWitnessVerdict(Device->PowerWitnessArmed,NT_SUCCESS(status),value);
    if (Device->PowerWitnessArmed && NT_SUCCESS(status) && value!=0)
        (void)GfxPowerWitnessWrite(Device,Device->PowerWitness,0);
    // verdict: 1 lost, 2 kept, 0 no proof (power_loss.h)
    GuardLog("power: D0 witness after a %s way down: armed %u, read 0x%08X (0x%08X), verdict %lu",
        Unsaved ? "unsaved" : "saved",(ULONG)Device->PowerWitnessArmed,value,(ULONG)status,verdict);
    Device->PowerWitnessArmed=FALSE;
    return verdict;
}

// One step of the way down: records a failure in Failed (bit Step-1) and keeps the first failure's status.
// Returns TRUE when the step failed.
static BOOLEAN PowerDownStepFailed(NTSTATUS Status, ULONG Step, ULONG* Failed, NTSTATUS* First)
{
    if (NT_SUCCESS(Status)) return FALSE;
    if (*Failed==0) *First=Status;
    *Failed|=1u<<(Step-1);
    GuardLog("power: way down step %lu failed 0x%08X",Step,(ULONG)Status);
    return TRUE;
}

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
    BOOLEAN unsaved;
    if (!Device || KeGetCurrentIrql()!=PASSIVE_LEVEL || !Device->Started || !Device->FullWddm)
        return STATUS_INVALID_DEVICE_STATE;
    if (State<PowerDeviceD0 || State>PowerDeviceD3) return STATUS_INVALID_PARAMETER;
    phase=InterlockedCompareExchange(&Device->RetainedPowerPhase,0,0);
    unsaved=phase==BC250_POWER_SUSPENDED_UNSAVED;
    if (State==PowerDeviceD0 && phase==BC250_POWER_ACTIVE) return STATUS_SUCCESS;
    if (State!=PowerDeviceD0 && (phase==BC250_POWER_SUSPENDED || unsaved)) return STATUS_SUCCESS;
    if ((State==PowerDeviceD0 && phase!=BC250_POWER_SUSPENDED && !unsaved) ||
        (State!=PowerDeviceD0 && phase!=BC250_POWER_ACTIVE)) {
        // A D0 after a failed transition (FAILED) restores nothing: the log must say so (BD-188).
        // BD-293: a way down in FAILED stops the owner that PowerFailedThermal started, before the power goes.
        if (State!=PowerDeviceD0 && phase==BC250_POWER_FAILED) SmuOwnerStop(&Device->Smu);
        GuardLog("power: state %u refused in phase %ld",(ULONG)State,phase);
        return STATUS_INVALID_DEVICE_STATE;
    }
    report=(BC250_ESCAPE_PSP*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*report),BC250_POWER_TAG);
    if (!report) {
        // BD-188: a way down that saves nothing must not stay ACTIVE. The D0
        // after it would find nothing to restore and report the engines as up.
        if (State!=PowerDeviceD0) {
            StartHealthClose(Device);
            SmuOwnerStop(&Device->Smu);
            InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_FAILED);
        }
        GuardLog("power: state %u no report memory, phase %ld",(ULONG)State,Device->RetainedPowerPhase);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(report,sizeof(*report));
    if (State!=PowerDeviceD0) {
        // BD-188 D-2 (docs/design/power-loss.md section 5). When the power goes all the same (power_loss.h), a
        // failed step no longer ends the way down: each later step still runs, in the form that its own module
        // allows after the earlier failure (a refused WDDM pause becomes the forced pause; PSP and GART refuse by
        // their own preconditions after a GFX halt that did not happen). The phase is then SUSPENDED_UNSAVED and the
        // call returns the first failure, as before. Otherwise the first failure is FAILED, as before.
        BOOLEAN lossExpected=Bc250PowerLossExpected((unsigned)State,(unsigned)Action) ? TRUE : FALSE;
        ULONG failed=0;
        NTSTATUS first=STATUS_SUCCESS;
        Device->RetainedDownState=State;
        Device->RetainedDownAction=Action;
        Device->RetainedDownFailed=0;
        Device->PowerWitnessArmed=FALSE;
        InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_SUSPENDING);
        StartHealthClose(Device);
        SmuOwnerStop(&Device->Smu); // joins no-synchronization telemetry before power loss
        status=BC250_POWER_DOWN_STEP(1,WddmSuspendRetained(Device));
        if (PowerDownStepFailed(status,1,&failed,&first)) {
            if (!lossExpected) goto Failed;
            WddmPauseForPowerLoss(Device);
        }
        status=BC250_POWER_DOWN_STEP(2,IhSetPowerRetained(Device,FALSE));
        if (PowerDownStepFailed(status,2,&failed,&first) && !lossExpected) goto Failed;
        status=BC250_POWER_DOWN_STEP(3,GfxSetPowerRetained(Device,FALSE));
        if (PowerDownStepFailed(status,3,&failed,&first) && !lossExpected) goto Failed;
        status=BC250_POWER_DOWN_STEP(4,PspSetPowerRetained(Device,FALSE,report));
        if (PowerDownStepFailed(status,4,&failed,&first) && !lossExpected) goto Failed;
        status=BC250_POWER_DOWN_STEP(5,GartSetPowerRetained(Device,FALSE));
        if (PowerDownStepFailed(status,5,&failed,&first) && !lossExpected) goto Failed;
        // The witness on every way down whose power goes, saved or not: after a saved one the D0 only logs it, which
        // is the positive control of the witness (lab row A3).
        if (lossExpected) PowerWitnessArm(Device);
        Device->RetainedDownFailed=failed;
        if (failed!=0) {
            status=first;
            InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_SUSPENDED_UNSAVED);
            GuardLog("power: unsaved way down to state %u, action %u: failed steps 0x%02lX, witness armed %u",
                (ULONG)State,(ULONG)Action,failed,(ULONG)Device->PowerWitnessArmed);
            goto Done;
        }
        InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_SUSPENDED);
        goto Done;
    }
    // Action on D0 is not authoritative: use the retained down-transition state.
    InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_RESTORING);
    // BD-188 D-2 (docs/design/power-loss.md section 6): the witness first, before the SMU owner and any restore step.
    // After an unsaved way down only "power lost" allows the restore: each block then takes its power-on software
    // state, and the normal restore below runs with the normal failure rule. Kept or no proof: FAILED, as before.
    if (PowerWitnessTake(Device,unsaved)!=BC250_POWER_WITNESS_LOST && unsaved) {
        GuardLog("power: unsaved way down (failed steps 0x%02lX) and no proof of power loss: FAILED",
            Device->RetainedDownFailed);
        status=STATUS_DEVICE_HARDWARE_ERROR;
        goto Failed;
    }
    if (unsaved) {
        IhPowerLost(Device);
        GfxPowerLost(Device);
        PspPowerLost(Device);
        GartPowerLost(Device);
    }
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
    InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_ACTIVE);
    if (unsaved)
        GuardLog("power: unsaved way down restored after power loss, failed steps 0x%02lX",Device->RetainedDownFailed);
    if (ConnectorIsEnabled(Device)) {
        status=ConnectorResumeNotifications(Device);
        if (!NT_SUCCESS(status)) goto Failed;
    }
    goto Done;
Failed:
    // No destructive PnP unwind and no synthetic completion of accepted work.
    StartHealthClose(Device);
    SmuOwnerStop(&Device->Smu);
    InterlockedExchange(&Device->RetainedPowerPhase,BC250_POWER_FAILED);
Done:
    GuardLog("power: state %u prior action %u phase %ld result 0x%08X",(ULONG)State,
        (ULONG)Device->RetainedDownAction,Device->RetainedPowerPhase,status);
    ExFreePoolWithTag(report,BC250_POWER_TAG);
    return status;
}

// BD-293: a D0 that ends in FAILED (a failed way down before it, or a failed restore) leaves the pauses of
// Bc250SetPowerState in force until the next start, and the transaction has stopped the SMU owner. The bus has given
// the power back, so the CPU runs at its firmware limits. The owner starts again here for the CPU surface alone:
// the read stage, the stored settings and the driver's temperature cap go in again. The governor, the fan and the
// hang detector stay paused, and every escape that tests the phase still refuses. PASSIVE_LEVEL.
void PowerFailedThermal(BC250_DEVICE* Device)
{
    NTSTATUS status=STATUS_SUCCESS;
    if (InterlockedCompareExchange(&Device->RetainedPowerPhase,0,0)!=BC250_POWER_FAILED) return;
    if (!Device->Smu.Online) status=SmuOwnerStart(&Device->Smu,Device->Mmio);
    if (!NT_SUCCESS(status)) {
        GuardLog("power: the failed transaction leaves no SMU owner (0x%08X): the CPU stays at its firmware limits",
            status);
        return;
    }
    CpuResumeAlone(Device);
    GuardLog("power: the failed transaction keeps the pauses; the SMU owner serves the CPU surface alone (BD-293)");
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
