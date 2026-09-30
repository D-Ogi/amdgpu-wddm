// Device-owned initialization. Sequences remain in the existing subsystem APIs.
#include "bc250kmd.h"
#include "startup.h"

// The callback itself only proves delivery under the interrupt synchronization
// mechanism. It neither enables hardware nor publishes DPC/ISR state.
static BOOLEAN GpuStartupInterruptProbe(PVOID Context)
{
    BOOLEAN* connected=(BOOLEAN*)Context;
    *connected=TRUE;
    return TRUE;
}

NTSTATUS GpuStartupInitialize(BC250_DEVICE* Device, BC250_START_REPORT* Report)
{
    struct _BC250_PSP_FIRMWARE* firmware=NULL;
    struct amdgpu_device* adev=NULL;
    PAGING_APERTURE aperture;
    BOOLEAN enabled=FALSE, synchronized=FALSE;
    NTSTATUS status=STATUS_DEVICE_NOT_READY;

    if (!Report) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Report,sizeof(*Report));
    if (!Device) { status=STATUS_INVALID_PARAMETER; goto Done; }
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL || Device->Started || Device->Wddm) {
        status=STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }
    if (Device->GpuStopUnconfirmed) { status=STATUS_DEVICE_HARDWARE_ERROR; goto Done; }
    if (!Device->FullWddm || !Device->Mmio || !Device->MmioGartEnabled ||
        !Device->MmioPspEnabled || !Device->MmioIhEnabled || !Device->MmioGfxEnabled ||
        !Device->VramWriteEnabled || !Device->Gart || !Device->Psp ||
        !Device->Ih || !Device->GpuMem || !Device->InterruptIsMessage ||
        !Device->Dxgk.DxgkCbSynchronizeExecution || !Device->Dxgk.DxgkCbNotifyInterrupt ||
        !Device->Dxgk.DxgkCbQueueDpc || !Device->Dxgk.DxgkCbNotifyDpc ||
        !Device->WddmAperture.bytes || !GfxStartupResources(Device,FALSE)) goto Done;

    // A non-NULL interface pointer does not prove the interrupt is connected.
    // MS documents STATUS_UNSUCCESSFUL when synchronization is unavailable.
    GuardLog("startup: entering interrupt connection");
    GuardLogKeep(); // KeepLog-gated, PASSIVE_LEVEL, before subsystem locks.
    status=Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle,
        GpuStartupInterruptProbe,&Report->InterruptConnected,0,&synchronized);
    if (!NT_SUCCESS(status)) goto Done;
    if (!synchronized || !Report->InterruptConnected) {
        status=STATUS_DEVICE_NOT_READY;
        goto Done;
    }

    // Planning only: refuse to re-enable/zero an already live GART, PSP or IH.
    ExAcquireFastMutex(&Device->GartLock);
    status=GartDevice(Device,&adev,&enabled);
    if (NT_SUCCESS(status) && (enabled || PspIsLoaded(Device) || IhIsActive(Device)))
        status=STATUS_INVALID_DEVICE_STATE;
    ExReleaseFastMutex(&Device->GartLock);
    if (!NT_SUCCESS(status)) goto Done;

    status=GartCaptureAperture(Device,&aperture);
    if (!NT_SUCCESS(status)) goto Done;
    if (aperture.mc!=Device->WddmAperture.mc || aperture.table!=Device->WddmAperture.table ||
        aperture.bytes!=Device->WddmAperture.bytes) {
        status=STATUS_INVALID_DEVICE_STATE;
        goto Done;
    }

    GuardLog("startup: entering firmware preparation");
    GuardLogKeep(); // KeepLog-gated, PASSIVE_LEVEL, before subsystem locks.
    status=PspPrepareFirmware(Device,&Report->Psp,&firmware);
    if (!NT_SUCCESS(status)) goto Done;

    // Before any translation/firmware/engine activation, never a later CLI.
    // The single native owner serializes this complete transaction with clients.
    Report->Attempted |= BC250_START_CLOCK;
    GuardLog("startup: entering native clock preparation");
    GuardLogKeep();
    status=SmuPrepareClock(&Device->Smu,&Report->Clock);
    GuardLog("startup: clock status 0x%08X ready %u request %uMHz/%umV initial %uMHz/VID%u observed %uMHz/VID%u temp %dmc staged %u",
        status,Report->Clock.ready,Report->Clock.requested_mhz,Report->Clock.requested_mv,
        Report->Clock.initial_mhz,Report->Clock.initial_vid,Report->Clock.observed_mhz,
        Report->Clock.observed_vid,Report->Clock.temperature_mc,Report->Clock.voltage_staged);
    GuardLogKeep();
    if (!NT_SUCCESS(status)) goto Done;
    if (!Report->Clock.ready) { status=STATUS_DEVICE_NOT_READY;goto Done; }
    Report->Completed |= BC250_START_CLOCK;

    status=GfxBeginTranslationBootstrap(Device);
    if (!NT_SUCCESS(status)) goto Done;

    Report->Attempted |= BC250_START_GART;
    GfxTraceRlcState(Device,"before-gart-initialization");
    GuardLog("startup: entering GART initialization");
    GuardLogKeep(); // KeepLog-gated, PASSIVE_LEVEL, before subsystem locks.
    status=GartInitializeHardware(Device,&Report->Gart);
    if (!NT_SUCCESS(status)) goto Unwind;
    Report->Completed |= BC250_START_GART;

    Report->Attempted |= BC250_START_PSP;
    GfxTraceRlcState(Device,"before-psp");
    GuardLog("startup: entering PSP initialization");
    GuardLogKeep(); // KeepLog-gated, PASSIVE_LEVEL, before subsystem locks.
    status=GfxPreparePspReload(Device);
    if (!NT_SUCCESS(status)) goto Unwind;
    GuardLogKeep(); // Persist the opt-in reset result before firmware loading.
    status=PspInitializePrepared(Device,firmware,&Report->Psp);
    if (!NT_SUCCESS(status)) goto Unwind;
    Report->Completed |= BC250_START_PSP;
    GfxTraceRlcState(Device,"after-psp");

    Report->Attempted |= BC250_START_IH;
    GuardLog("startup: entering IH initialization");
    GuardLogKeep(); // KeepLog-gated, PASSIVE_LEVEL, before subsystem locks.
    status=IhInitializeHardware(Device,&Report->Ih);
    if (!NT_SUCCESS(status)) goto Unwind;
    Report->Completed |= BC250_START_IH;

    Report->Attempted |= BC250_START_GFX;
    GuardLog("startup: entering GFX initialization");
    GuardLogKeep(); // KeepLog-gated, PASSIVE_LEVEL, before subsystem locks.
    // The CU mode's registry work brackets the stages: inside them GartLock holds APC_LEVEL.
    CuModePrepare(Device);
    status=GfxInitializeHardware(Device,&Report->Gfx);
    CuModeFinish(Device);
    if (!NT_SUCCESS(status)) goto Unwind;
    Report->Completed |= BC250_START_GFX;

    status=GartCaptureAperture(Device,&aperture);
    if (!NT_SUCCESS(status)) goto Unwind;
    if (aperture.mc!=Device->WddmAperture.mc || aperture.table!=Device->WddmAperture.table ||
        aperture.bytes!=Device->WddmAperture.bytes) {
        status=STATUS_INVALID_DEVICE_STATE;
        goto Unwind;
    }
    if (!GfxStartupResources(Device,TRUE)) {
        status=STATUS_DEVICE_NOT_READY;
        goto Unwind;
    }
    Report->Ready=TRUE;
    GuardLog("startup: both engines ready");
    GuardLogKeep();
    goto Done;

Unwind:
    // Even a failed first phase may own GPU-visible storage. Use the complete
    // dependency-aware stop chain, not only the successfully completed phases.
    // These stops preserve/quarantine backing on an unconfirmed hardware halt.
    IhStop(Device);
    GfxPrepareStop(Device);
    PspStop(Device);
    GartPrepareStop(Device);
    GfxStop(Device);
    GartStop(Device);
    Report->Unwound=TRUE;
Done:
    PspReleaseFirmware(firmware);
    Report->Status=status;
    Report->StopUnconfirmed=Device ? Device->GpuStopUnconfirmed : FALSE;
    if (Device) GuardLog("startup: attempted 0x%lX completed 0x%lX status 0x%08X ready %u unwind %u quarantine %u",
        Report->Attempted,Report->Completed,status,Report->Ready,Report->Unwound,Report->StopUnconfirmed);
    return status;
}
