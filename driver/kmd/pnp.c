// Device life cycle, the one child (the monitor output), power. M3: no MMIO, no interrupt (ADR 0006).
#include "bc250kmd.h"
#include "adapter_owner.h"
#include "hdmi_link.h"
#include "state_kernel.h"

NTSTATUS Bc250AddDevice(_In_ const PDEVICE_OBJECT PhysicalDeviceObject, _Outptr_ PVOID* MiniportDeviceContext)
{
    BC250_DEVICE* device;
    NTSTATUS stateStatus;

    *MiniportDeviceContext = NULL;
    stateStatus=DriverStateValidateDevice(PhysicalDeviceObject);
    if(!NT_SUCCESS(stateStatus))return stateStatus;
    if (!AdapterOwnerClaim(PhysicalDeviceObject)) return STATUS_DEVICE_BUSY;
    device = (BC250_DEVICE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*device), BC250_TAG);
    if (device == NULL) {
        AdapterOwnerRelease(PhysicalDeviceObject, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    GuardStage(StageAddDevice);
    BoardMemoryInitialize(device);
    BoardMemoryIdentityClear(device);
    device->PhysicalDeviceObject = PhysicalDeviceObject;
    device->StopDone = TRUE; // No hardware start has been attempted yet.
    device->Rotation = D3DKMDT_VPPR_IDENTITY;
    StartHealthInitialize(device);
    CuModeInitialize(device);
    DpmInitialize(device);
    SmuMetricsInitialize(device);
    CpuInitialize(device);
    InteropInitialize(device);
    SmuOwnerInitialize(&device->Smu);
    HwmonInitialize(&device->Hwmon);
    FanInitialize(device);
    DpAudioInitialize(device);
    ModesetInitialize(device);
    ConnectorInitialize(device);
    ExInitializeFastMutex(&device->GartLock);
    KeInitializeMutex(&device->PspSessionLock,0);
    ExInitializePushLock(&device->GfxPagingLock);
    KeInitializeSpinLock(&device->GfxAccessLock);
    KeInitializeEvent(&device->GfxAccessDrained, NotificationEvent, TRUE);
    // KMD196: unsignalled, because nothing is held yet; a waiter clears it before every wait anyway.
    KeInitializeEvent(&device->GfxRetireEvent, NotificationEvent, FALSE);
    device->GfxAccessClosed = TRUE;
    *MiniportDeviceContext = device;
    return STATUS_SUCCESS;
}

NTSTATUS Bc250RemoveDevice(_In_ const PVOID MiniportDeviceContext)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    if (!device || !AdapterOwnerIs(device->PhysicalDeviceObject)) return STATUS_SUCCESS;
    // Remove may follow a failed start without an ordinary StopDevice.
    if (!device->StopDone) (void)Bc250StopDevice(device);
    BoardMemoryStop(device);
    HangDetectorStop();     // idempotent; a remove without a stop still joins the thread and the timer
    CpuStop(device);        // before DpmStop: a trial's revert still needs the mailbox and the governor's busy share
    DpmStop(device);        // idempotent, like the detector: its thread runs this image's code
    SmuMetricsStop(device); // after DpmStop: the governor thread was the one reading the page
    FanStop(device, BC250_FAN_REASON_STOP);    // idempotent; the fan goes back to the board before the memory does
    StartHealthRemove(device);
    InteropRemove(device);  // the power callback must not find the device once its memory goes
    DisplayUnmapFramebuffer(device);
    IhRemove(device);
    ConnectorDestroy(device);
    // Keep admission closed until every cleanup above has completed.
    AdapterOwnerRelease(device->PhysicalDeviceObject, device->GpuStopUnconfirmed);
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
    BOOLEAN displayBlankOwned = FALSE;

    *NumberOfVideoPresentSources = 0;
    *NumberOfChildren = 0;
    if (!device || !AdapterOwnerIs(device->PhysicalDeviceObject)) return STATUS_DEVICE_BUSY;
    // M455: durable admission belongs to StartDevice, not DriverEntry/AddDevice.
    // A failed commit must leave hardware untouched and the handoff retryable.
    status=DriverStateActivateDevice(device->PhysicalDeviceObject);
    if(!NT_SUCCESS(status))return status;
    BoardMemoryIdentityClear(device);
    GuardStage(StageStartEnter);
    // One kept log file for this whole start, however many checkpoints it writes (guard.c GuardLogKeep). Does
    // nothing unless KeepLog is 1 in the active state generation; the file goes to the Logs child of the driver's
    // shared state directory, which `bc250kmd_cli state paths` prints.
    GuardLogKeepEpisode(L"start");
    // BD-122: from here on the registry record belongs to THIS attempt, whatever an earlier boot left behind, and
    // every exit below that does not reach a started device writes its reason into it (guard.c, StartRecord). The
    // refusal above this line is not one: it is a foreign or absent device object, which this driver does not own
    // and must leave no trace for.
    GuardRecordStartAttempt();
    device->InheritedSignalValid=FALSE;
    if (device->GpuStopUnconfirmed) {
        GuardLog("start refused: previous GPU stop is unconfirmed for this device object");
        GuardStartStep(L"PreviousStopUnconfirmed");
        // This exit fails the start as surely as the goto failed paths do, so it leaves the stage every reader of
        // LastStage and StageHistory keys off, and the record's documented "Stage 91 for a failure" holds here too.
        GuardStage(StageStartFailed);
        GuardRecordStartFailed(STATUS_DEVICE_HARDWARE_ERROR);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }

    status = GuardCheckAndCountStart(WddmFullTableSelected());
    if (!NT_SUCCESS(status))
    {
        GuardStage(StageRefusedByGuard);
        GuardStartStep(L"BootLoopGuard");
        GuardRecordStartRefused(status);
        return status;
    }
    GuardStage(StageStartGuardPassed);
    device->StopDone=FALSE; // Only an admitted start may require hardware unwind.
    device->SourceVisible=FALSE;
    InterlockedExchange(&device->StartVisibilityPending,1);
    device->DisplayReservationEnd=0;
    device->RuntimeFramebufferLength=0;
    device->RuntimePostValid=0;
    device->RuntimeDisplayChanged=0;
    device->RuntimePlaneBlank=0;

    StartHealthBegin(device,WddmFullTableSelected());
    CuModeBegin(device);
    device->StartInfo = *DxgkStartInfo;
    // dxgkrnl hands out the interface at the size of the version we asked for (WIN8), while this structure is
    // compiled at the newest layout: copy what was given, not what we could hold.
    RtlZeroMemory(&device->Dxgk, sizeof(device->Dxgk));
    RtlCopyMemory(&device->Dxgk, DxgkInterface, min((SIZE_T)DxgkInterface->Size, sizeof(device->Dxgk)));

    GuardStartStep(L"DxgkCbGetDeviceInformation");
    status = device->Dxgk.DxgkCbGetDeviceInformation(device->Dxgk.DeviceHandle, &device->DeviceInfo);
    if (!NT_SUCCESS(status)) goto failed;
    GuardStage(StageStartDeviceInfo);
    BoardMemoryIdentityCapture(device);
    BoardProviderBind(device);

    // The firmware's mode and framebuffer. Without it there is nothing this driver could show: M3 sets no mode.
    GuardStartStep(L"AcquirePostDisplayOwnership");
    status = device->Dxgk.DxgkCbAcquirePostDisplayOwnership(device->Dxgk.DeviceHandle, &device->Post);
    if (!NT_SUCCESS(status)) goto failed;
    // Its own step: the firmware answered, and what it answered has no display in it. That is a BIOS or
    // seamless-boot question, not a driver one, and the two must not read the same in the record.
    GuardStartStep(L"PostDisplayGeometry");
    // BD-090: what Windows handed over, logged before the check, so that the record of a refused start names it.
    // After a PnP stop this is what the release below returned (pnputil /restart-device), or zeros after it failed.
    GuardLog("post display acquired %ux%u pitch %u format %u address %llX target %u", device->Post.Width,
             device->Post.Height, device->Post.Pitch, (ULONG)device->Post.ColorFormat,
             (ULONGLONG)device->Post.PhysicAddress.QuadPart, device->Post.TargetId);
    if (device->Post.Width == 0 || device->Post.Height == 0 || device->Post.PhysicAddress.QuadPart == 0)
    {
        status = STATUS_GRAPHICS_NO_VIDEO_MEMORY;       // honest: no firmware display to take over
        goto failed;
    }
    GuardLog("post display %ux%u pitch %u format %u target %u", device->Post.Width, device->Post.Height,
             device->Post.Pitch, (ULONG)device->Post.ColorFormat, device->Post.TargetId);
    GuardStage(StageStartPostDisplayAcquired);

    GuardStartStep(L"DisplayMapFramebuffer");
    status = DisplayMapFramebuffer(device);
    if (!NT_SUCCESS(status)) goto failed;
    GuardStage(StageStartFramebufferMapped);

    NoteInterruptResource(device);
    MmioStart(device);      // maps nothing unless the registry gate is open; never fails the start
    // WDDM 1.2 PnP start: keep sync, but send black regardless of the firmware surface's pixels.
    // A framebuffer clear cannot enforce that while another owner can still draw into it.
    GuardStartStep(L"StartDisplayBlank");
    status=DcnBeginStartVisibility(device,&displayBlankOwned);
    if (!NT_SUCCESS(status)) {
        // A read-only admission refusal must not make stop write to an unsupported firmware pipe.
        // No SMU/GPU/connector owner has started yet. Once a display write succeeds, keep BAR5 for unwind.
        if (!displayBlankOwned) MmioStop(device);
        goto failed;
    }
    HwmonStart(device);     // the board's hardware monitor, read only; same rule, and gated to off
    // Set only after removing the legacy bc250rd mailbox writer. No implicit
    // fallback: full-WDDM startup requires this owner through SmuPrepareClock.
    if (GuardReadSetting(L"EnableNativeSmu",0)==1) {
        // The mailbox is in BAR5, so this owner needs EnableMmio. Without the mapping SmuOwnerStart can only answer
        // STATUS_INVALID_PARAMETER, and until 0.7.216.10 that refusal failed the whole device start: an install that
        // closed EnableMmio and left EnableNativeSmu open (BD-091) gave Code 43 with nothing in the registry naming
        // the cause. Say it in one line instead and start without the owner, which is what a closed EnableNativeSmu
        // does: no clock control and no metrics, and SmuPrepareClock refuses a full WDDM start by itself.
        if (device->Mmio==NULL)
            GuardLog("smu: native owner start skipped, EnableNativeSmu 1 with EnableMmio 0 (no BAR5 mapping)");
        else {
            GuardStartStep(L"SmuOwnerStart");
            status=SmuOwnerStart(&device->Smu,device->Mmio);
            GuardLog("smu: native owner start status 0x%08X",status);
            if (!NT_SUCCESS(status)) {
                GuardStartFailureMark();        // BD-122: the record's log lines end here, not after the cleanup
                // The common stop must restore visibility before it unmaps BAR5.
                goto failed;
            }
        }
    }
    VramStart(device);      // same rule
    status=ConnectorBufferPrepare(device);
    GuardLog("connector: handoff reservation status %08lX",(ULONG)status);
    DisplayLogFramebufferSample(device);   // after VramStart, so the carve-out view exists when that gate is open
    // The sample keeps what firmware left. The logo keeps its default, but is drawn behind the startup blank;
    // only the later OS visibility request may expose a frame. Never fails the start (display.c).
    DisplayDrawBootLogo(device);
    GartStart(device);      // same rule
    PspStart(device);       // same rule
    GpuMemStart(device);    // same rule
    GfxStart(device);       // same rule
    IhStart(device);        // same rule
    // WddmStart and GpuStartupInitialize name their own sub-steps inside this one (GuardStartSubStep).
    GuardStartStep(L"WddmStart");
    status = WddmStart(device);
    if (!NT_SUCCESS(status)) {
        // BD-122: the failure point, before the whole device is torn down below. The stop of a GPU that got this
        // far writes dozens of its own log lines, and the record keeps the 64 that lead up to here.
        GuardStartFailureMark();
        // The common failure path preserves teardown ordering and retained/quarantined ownership.
        goto failed;
    }
    // Capture once before dxgkrnl can enumerate modes or describe a primary.
    // A single immutable signal keeps those two contracts exactly consistent.
    GuardStartStep(L"DisplayPrepareInheritedTiming");
    status=DisplayPrepareInheritedTiming(device);
    if (!NT_SUCCESS(status)) {
        GuardStartFailureMark();        // BD-122: before the teardown of a GPU that started, as above
        goto failed;
    }
    // Display modes (modeset.c): the EDID over DP AUX, the monitor descriptor, the source mode list. After the
    // inherited timing, which is the native size, and before DP audio, which programs the EDID's audio data. Never
    // fails the start: without an EDID the driver offers the one inherited mode, as before.
    ModesetStart(device);
    // The seamless-boot point: the firmware's DP stream is now ours, and Linux adds audio to such a stream here
    // (link_dpms.c). Never fails the start; refuses and writes nothing unless its preconditions hold (dpaudio.c).
    HdmiLinkStartAudio(device,FALSE);
    GuardStage(StageStartMmioDone);

    InterlockedExchange(&device->RetainedPowerPhase,0);
    // The initial admission keeps the inherited timing, with visibility held false. Subsequent HPD events
    // go through the passive owner and an OS-selected VidPN transaction.
    GuardStartStep(L"ConnectorStart");
    status=ConnectorStart(device);
    // A connector that cannot be admitted or planned has programmed nothing, so
    // the firmware's stream is still on the wire and the static inherited path
    // owns the display (connector.c, BD-169). Only a refusal of that path itself
    // leaves nothing to present, and that one still fails the start (#38).
    if (!NT_SUCCESS(status) && ConnectorRefusalKeepsDisplay(device)) {
        GuardLog("connector: start refused 0x%08lX; the static inherited path keeps the firmware stream",
                 (ULONG)status);
        status=STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) goto failed;
    // Connector setup may replace the pipe. Recheck the final output before publishing a successful start.
    GuardStartStep(L"StartDisplayBlankConfirm");
    status=DcnSetVisibility(device,FALSE);
    if (!NT_SUCCESS(status)) goto failed;
    device->Started = TRUE;
    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = 1;
    StartHealthReady(device,device->FullWddm && GfxStartupResources(device,TRUE));
    // Last: it watches a started device, and it never fails the start (EnableHangBugcheck, hang.c).
    HangDetectorStart(device);
    // Before the governor, whose thread runs its step, and after the last point where the start can fail: it arms a
    // watchdog and a bugcheck callback. Never fails the start (fan.c).
    FanStart(device);
    // Before the governor, whose thread reads the table: the page and the gate (smu_metrics.c). Sends nothing.
    SmuMetricsStart(device);
    // After it: the governor starts from the floor the start set, and never fails the start (dpm.c).
    DpmStart(device);
    // After the governor: the CPU surface reads its settings and, if CpuTune is 1, starts its worker (cpu.c).
    CpuStart(device);
    BoardMemoryStart(device); // Read-only self-check; never fails display start.
    GuardStage(StageStartDone);
    GuardRecordStartStarted();      // BD-122: this start is not a reason for anything
    return STATUS_SUCCESS;

failed:
    // BD-122, first of all and before the cleanup: the failure point of every step that did not mark one itself.
    GuardLog("start: failing step status 0x%08X before display handoff", status);
    GuardStartFailureMark();
    if (!device->StopDone) (void)Bc250StopDevice(device);
    // The PnP start contract requires a usable GOP mode on failure. Do not report an ordinary refusal when
    // we touched display state but failed to return it to the next display driver.
    if (displayBlankOwned && device->PostDisplayStopAttempted && !NT_SUCCESS(device->PostDisplayStopStatus)) {
        GuardLog("start: display handoff failed 0x%08X after start failure 0x%08X",device->PostDisplayStopStatus,status);
        status=STATUS_GRAPHICS_STALE_MODESET;
    }
    BoardMemoryStop(device);
    BoardMemoryIdentityClear(device);
    StartHealthClose(device);
    GuardLog("start failed 0x%08X", status);
    GuardStage(StageStartFailed);
    // BD-122, last and after the existing cleanup, which is unchanged: Stage is then the 91 a tester's
    // start-confirm.log already prints, and the kept ring lines are the ones up to the mark taken above.
    GuardRecordStartFailed(status);
    return status;
}

NTSTATUS Bc250StopDevice(_In_ const PVOID MiniportDeviceContext)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    BOOLEAN wasStarted;
    NTSTATUS connectorStatus;

    if (!device || !AdapterOwnerIs(device->PhysicalDeviceObject)) return STATUS_SUCCESS;
    BoardMemoryStop(device);

    // BD-090. When DxgkDdiStopDeviceAndReleasePostDisplayOwnership fails, dxgkrnl calls DxgkDdiStopDevice after it
    // (plug-and-play--pnp--start-and-stop-cases.md, "or after a call to DxgkDdiStopDeviceAndReleasePostDisplay-
    // Ownership fails"). The release already ran this whole teardown; a second pass would restore DCN through an
    // unmapped BAR5 and stop every block twice. The teardown is complete, so the second call only reports success.
    if (device->StopDone) {
        GuardLog("stop: teardown already done by the post-display release");
        return STATUS_SUCCESS;
    }
    // The next display owner needs a visible handoff. This also unwinds a failed start before the first OS TRUE.
    InterlockedExchange(&device->StartVisibilityPending,0);
    wasStarted=device->Started;
    HangDetectorStop();     // first: a stop may legitimately wait, and the detector must not judge it
    GuardRecordStopBegin(device); // closes health and saves the pre-teardown log without KeepLog
    GuardStage(StageStopEnter);
    // The stop gets its own kept log file, so that it is not appended to the start's (guard.c GuardLogKeep). A stop
    // that unwinds a failed start therefore leaves two files, which is how it should read.
    GuardLogKeepEpisode(L"stop");
    connectorStatus=ConnectorStop(device); // drains passive work before any MMIO/power teardown
    device->InheritedSignalValid=FALSE;
    CpuStop(device);        // first: a trial's revert needs the mailbox, which SmuOwnerStop below takes away
    DpmStop(device);        // the floor while the owner is still online, then no governor tick
    SmuMetricsStop(device); // after DpmStop: no tick reads the page any more
    FanStop(device, BC250_FAN_REASON_STOP);    // after DpmStop (no step runs), before HwmonStop: the fan to the board
    HwmonStop(&device->Hwmon);  // after DpmStop: the governor thread is the one that samples it
    DpAudioStop(device);        // before WddmStop/DcnStop, with BAR5 mapped: the next owner inherits AUDIO_ENABLED 0
    HdmiLinkStop(device);       // restore the inherited DVI encoder before BAR5 or power goes away
    SmuOwnerStop(&device->Smu); // join clients before any engine/translation teardown
    BoardProviderUnbind(device); // Only after fan hand-back and the final mailbox client.
    BoardMemoryIdentityClear(device);
    device->SystemDisplayReady=FALSE;
    device->PostDisplayStopAttempted=FALSE;
    device->PostDisplayStopStatus=STATUS_DEVICE_NOT_READY;
    device->Started = FALSE;
    device->ModeActive = FALSE;
    device->SourceVisible = FALSE;      // so that the next start writes its own first-commit and first-present breadcrumbs
    device->CommitSeen = FALSE;
    device->PresentSeen = FALSE;
    WddmStop(device);       // first: it logs what dxgkrnl called, and nothing below it is allowed to have run
    InteropStop(device);    // an orderly stop ends the GPU DWM session: its marker goes (registry only)
    // Display-only starts have no WDDM object and therefore no WddmStop restore.
    if (!device->PostDisplayStopAttempted) {
        device->PostDisplayStopStatus=DcnStop(device);
        device->PostDisplayStopAttempted=TRUE;
    }
    {
        NTSTATUS displayRestore, encoderRestore;
        displayRestore=device->PostDisplayStopStatus;
        encoderRestore=HdmiLinkRelease(device);
        if (!NT_SUCCESS(encoderRestore)) device->PostDisplayStopStatus=STATUS_DEVICE_DATA_ERROR;
        if (!NT_SUCCESS(connectorStatus)) device->PostDisplayStopStatus=connectorStatus;
        // BD-090: the three results that decide the display handoff, in one line of the stop's kept file.
        GuardLog("stop: display handoff restore %08lX hdmi %08lX connector %08lX result %08lX", (ULONG)displayRestore,
                 (ULONG)encoderRestore, (ULONG)connectorStatus, (ULONG)device->PostDisplayStopStatus);
    }
    IhStop(device);         // no interrupt of ours from here on
    GfxPrepareStop(device); // halt engines, retain storage through firmware/translation retirement
    PspStop(device);
    GartPrepareStop(device);
    GfxStop(device);        // destroy storage while the GART owner/table remain available
    GartStop(device);       // final firmware restore and owner destruction
    if (device->FullWddm) GfxTraceRlcState(device,"after-gart-stop");
    VramStop(device);
    ConnectorBufferRelease(device);
    MmioStop(device);
    DisplayUnmapFramebuffer(device);
    device->StopDone=TRUE;
    // BD-090: an orderly stop of a start that completed gives its boot-guard count back in a boot that was once
    // confirmed healthy (guard.c). A start that failed calls this function with Started still FALSE: it keeps it.
    if (wasStarted) GuardReleaseStart();
    GuardStage(StageStopDone);
    GuardRecordStopDone();
    // Last, so that the file holds the stop as well. Does nothing unless KeepLog is 1 (Data key of the active state
    // generation): dxgkrnl may end a full WDDM start by itself and unload the driver after, ring and all (E16 run 1).
    GuardLogKeep();
    return STATUS_SUCCESS;
}

// PnP stop with the screen kept: report the mode we leave behind so that the next owner (Basic Display on a
// rollback, this driver again after pnputil /restart-device) can continue on the same framebuffer.
NTSTATUS Bc250StopDeviceAndReleasePostDisplayOwnership(_In_ PVOID MiniportDeviceContext,
                                                       _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                       _Out_ PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    static const char* const linkAction[BC250_LINK_HANDOFF_ACTIONS]={"static","active","no-sink","suspended",
                                                                      "no-target","enabled","enable-failed"};
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    BC250_LINK_HANDOFF handoff;
    NTSTATUS status;

    // TargetId: one active output; the actual id is returned below.
    RtlZeroMemory(DisplayInfo,sizeof(*DisplayInfo));
    if (!device || !AdapterOwnerIs(device->PhysicalDeviceObject)) return STATUS_DEVICE_NOT_READY;
    // BD-090: the release's own lines, in the ring and so in the stop's kept file. After a pnputil restart that
    // leaves a black screen, the first question is whether dxgkrnl called this function at all.
    GuardLog("post-display release: target %lu started %u runtime changed %ld valid %ld", (ULONG)TargetId,
             (UINT)device->Started, InterlockedCompareExchange(&device->RuntimeDisplayChanged,0,0),
             InterlockedCompareExchange(&device->RuntimePostValid,0,0));
    // The next owner needs a visible handoff: the startup hold (BD-179) ends here, before the target below.
    InterlockedExchange(&device->StartVisibilityPending,0);
    // dispmprt.md, required steps 2, 5 and 6 (connector_link.c, ConnectorLinkHandoff): a target with no display
    // connected answers STATUS_NOT_SUPPORTED, and a link that an OS or a physical detach turned off gets a target
    // again, black, on the reserved buffer. Before the teardown, which takes the writer and BAR5 away.
    StartHealthEnter(device);
    (void)ConnectorLinkHandoff(device,&handoff);
    StartHealthLeave(device);
    GuardLog("post-display release: link %s detached-mode %lu timing %lux%lu %lu kHz status %08lX",
             handoff.Action<BC250_LINK_HANDOFF_ACTIONS?linkAction[handoff.Action]:"?",handoff.Detached,
             handoff.Width,handoff.Height,handoff.PixelKhz,(ULONG)handoff.Status);
    // WDDM 1.2 (plug-and-play--pnp--start-and-stop-cases.md): the surface the pipe scans out when this call returns
    // must be filled with black before source visibility goes TRUE. That surface is the POST framebuffer
    // (DcnRestorePostDisplay below puts the scanout back on it), which the memory segment is carved clear of
    // (VramFramebufferOffset), so nothing of ours lives in it. While DWM's primary is scanned out the fill is not
    // visible; without it the next owner shows whatever picture that surface last held until its first frame.
    if (device->Framebuffer != NULL && device->FramebufferLength != 0)
        RtlZeroMemory(device->Framebuffer, device->FramebufferLength);
    if (device->RuntimePostValid && device->RuntimeFramebuffer != NULL &&
        device->RuntimeFramebufferLength != 0)
        RtlZeroMemory(device->RuntimeFramebuffer,device->RuntimeFramebufferLength);
    // Local dispmprt contract: failure makes dxgkrnl call ordinary StopDevice.
    // Never publish a framebuffer that DCN failed to latch. Ordinary StopDevice
    // still finishes teardown and returns its own completion result.
    status=Bc250StopDevice(MiniportDeviceContext);
    if (NT_SUCCESS(status) && handoff.Action==BC250_LINK_HANDOFF_NO_SINK) status=STATUS_NOT_SUPPORTED;
    else if (NT_SUCCESS(status)) status=device->PostDisplayStopStatus;
    if (NT_SUCCESS(status)) {
        *DisplayInfo = device->RuntimePostValid ? device->RuntimePost : device->Post;
        DisplayInfo->TargetId = BC250_CHILD_UID;
        // BD-090, the self-check: what Windows keeps for the next owner. The next start logs the same numbers as
        // "post display acquired", or zeros when Windows kept nothing.
        GuardLog("post-display release: handed back %s %ux%u pitch %u address %llX target %u",
                 device->RuntimePostValid?"runtime":"firmware",DisplayInfo->Width,DisplayInfo->Height,
                 DisplayInfo->Pitch,(ULONGLONG)DisplayInfo->PhysicAddress.QuadPart,DisplayInfo->TargetId);
    }
    else GuardLog("post-display release: handed back nothing, status %08lX: %s",(ULONG)status,
                  handoff.Action==BC250_LINK_HANDOFF_NO_SINK?"no display connected":
                  handoff.Action==BC250_LINK_HANDOFF_FAILED?"the target enable failed":
                  "the display restore failed, see stop: display handoff");
    // A second keep of the same stop episode: it appends these lines to the file that the stop above wrote.
    GuardLogKeep();
    return status;
}

void Bc250ResetDevice(_In_ const PVOID MiniportDeviceContext)
{
    // Called at high IRQL on the way to a bugcheck or hibernate. The timing never changes; a smaller source mode
    // (modeset.c) changed the viewport and scaler of pipe 0, so the firmware surface and the native shape go back
    // here, with checked MMIO and bounded stalls only, as SystemDisplayEnable does. The case fan is the other thing
    // this driver takes from the board, so it goes back here too, with port writes only (fan.c).
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    if (device == NULL || !AdapterOwnerIs(device->PhysicalDeviceObject)) return;
    if (device->Modeset.PipeChanged || device->RuntimeDisplayChanged)
    {
        (void)DcnRestorePostDisplaySerialized(device);
        ModesetRestoreQuiet(device);
    }
    FanResetDevice(device);
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

// The display path enables no interrupt source; the two this driver ever enables are the IH ring (ih.c, behind
// its gate) and, since 0.7.24 (ADR 0011 point 3 step 3), OTG0's VUPDATE_NO_LOCK (dcn.c's DcnVsyncInterrupt,
// behind EnableVidPnFlip) - independent of each other and of GartLock (dcn.c's registers are not on any
// GART/PSP/GFX/IH sequence's set). Everything else that fires on a shared line is not ours. Both are called on
// every interrupt regardless of what the other found, so neither can suppress the other's ack or DPC request.
BOOLEAN Bc250InterruptRoutine(_In_ const PVOID MiniportDeviceContext, _In_ ULONG MessageNumber)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    BOOLEAN ih, vsync, hpd;

    ProgressEnter(ProgressSiteIsr);     // interlocked stores only (hang.c): allowed at DIRQL
    InterlockedExchange64(&device->InterruptLastTime, (LONG64)KeQueryInterruptTime());
    InterlockedIncrement(&device->InterruptCount);
    InterlockedExchange(&device->LastMessageNumber, (LONG)MessageNumber);
    ih = IhInterrupt(device);
    vsync = DcnVsyncInterrupt(device);
    hpd = ConnectorInterrupt(device);
    ProgressExit(ProgressSiteIsr, (LONG)MessageNumber);
    return ih || vsync || hpd;
}

void Bc250DpcRoutine(_In_ const PVOID MiniportDeviceContext)
{
    ProgressEnter(ProgressSiteDeviceDpc);
    IhDpc((BC250_DEVICE*)MiniportDeviceContext);
    WddmGpuFence((BC250_DEVICE*)MiniportDeviceContext); // stage C: the vectors are consumed, the fence memory says whose they were
    // ADR 0008 stage D: node 1's own poll, unconditional like the one above - which vector woke this DPC does not
    // matter to either read, only whether the fence slot it polls now holds the value it is waiting for.
    WddmGpuFencePaging((BC250_DEVICE*)MiniportDeviceContext);
    // ADR 0011 point 3 step 3: the hardware vsync's own report, same shape - a no-op unless Device->DcnVsyncAcked
    // says an interrupt found a real VUPDATE_NO_LOCK event since the last time this ran.
    if (IhTakeVsync((BC250_DEVICE*)MiniportDeviceContext))
        DcnVsyncFromVector((BC250_DEVICE*)MiniportDeviceContext);
    WddmDcnVsync((BC250_DEVICE*)MiniportDeviceContext);
    // BD-170 r6: E5's notification and drain record, a no-op unless E5 queued this DPC (wddm.c's WddmFlipFault).
    WddmFlipFaultDpc((BC250_DEVICE*)MiniportDeviceContext);
    WddmDpc((BC250_DEVICE*)MiniportDeviceContext);      // returns at once unless the full table is in use
    ConnectorDpc((BC250_DEVICE*)MiniportDeviceContext);
    ProgressExit(ProgressSiteDeviceDpc, 0);
}

NTSTATUS Bc250QueryChildRelations(_In_ const PVOID MiniportDeviceContext,
                                  _Inout_updates_bytes_(ChildRelationsSize) PDXGK_CHILD_DESCRIPTOR ChildRelations,
                                  _In_ ULONG ChildRelationsSize)
{
    // dxgkrnl sizes the array for NumberOfChildren + 1; the terminating entry stays zeroed.
    if (ChildRelationsSize < 2 * sizeof(DXGK_CHILD_DESCRIPTOR)) return STATUS_BUFFER_TOO_SMALL;
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.HpdAwareness =
        ConnectorIsEnabled((BC250_DEVICE*)MiniportDeviceContext) ? HpdAwarenessInterruptible : HpdAwarenessAlwaysConnected;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology =
        ConnectorIsEnabled((BC250_DEVICE*)MiniportDeviceContext) ? D3DKMDT_VOT_DISPLAYPORT_EXTERNAL : D3DKMDT_VOT_OTHER;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = BC250_CHILD_UID;
    if (((BC250_DEVICE*)MiniportDeviceContext)->Modeset.InheritedFixed)
        GuardLog("display: inherited child reported uid %lu hpd %u interface %u",
            ChildRelations[0].ChildUid,(UINT)ChildRelations[0].ChildCapabilities.HpdAwareness,
            (UINT)ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology);
    return STATUS_SUCCESS;
}

NTSTATUS Bc250QueryChildStatus(_In_ const PVOID MiniportDeviceContext, _Inout_ PDXGK_CHILD_STATUS ChildStatus,
                               _In_ BOOLEAN NonDestructiveOnly)
{
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
        ChildStatus->HotPlug.Connected = ConnectorIsEnabled((BC250_DEVICE*)MiniportDeviceContext) ?
            ConnectorIsConnected((BC250_DEVICE*)MiniportDeviceContext) : TRUE;
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
    BC250_DEVICE* device=MiniportDeviceContext;
    // The EDID of the monitor as modeset.c read it over DP AUX at start (EnableDisplayModes 1 or 2), in the pieces
    // dxgkrnl and Monitor.sys ask for; STATUS_MONITOR_NO_DESCRIPTOR without one, as before display modes.
    if (ChildUid != BC250_CHILD_UID) return STATUS_INVALID_PARAMETER;
    if (ConnectorIsEnabled(device)) {
        BC250_CONNECTOR_SNAPSHOT* snapshot=ConnectorSnapshotAcquire(device);
        NTSTATUS status=STATUS_MONITOR_NO_DESCRIPTOR;
        if (snapshot && snapshot->Sink.Kind!=BC250_SINK_NONE) {
            ULONG offset=DeviceDescriptor->DescriptorOffset;
            ULONG length=DeviceDescriptor->DescriptorLength;
            if (!DeviceDescriptor->DescriptorBuffer || !length) status=STATUS_INVALID_PARAMETER;
            else if (offset>=snapshot->Sink.EdidLength) status=STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
            else {
                ULONG copy=min(length,snapshot->Sink.EdidLength-offset);
                RtlCopyMemory(DeviceDescriptor->DescriptorBuffer,snapshot->Sink.Edid+offset,copy);
                if (copy<length) RtlZeroMemory((PUCHAR)DeviceDescriptor->DescriptorBuffer+copy,length-copy);
                status=ConnectorGenerationCurrent(device,snapshot->Generation) ? STATUS_SUCCESS : STATUS_RETRY;
            }
        }
        ConnectorSnapshotRelease(snapshot);
        return status;
    }
    return ModesetQueryDescriptor((BC250_DEVICE*)MiniportDeviceContext, DeviceDescriptor);
}

// The container ID of the one child. dxgkrnl makes a default one out of the EDID that Bc250QueryDeviceDescriptor
// answered and offers it here; the driver keeps it (STATUS_MONITOR_NO_DESCRIPTOR, and the structure is not
// touched, as the DDI reference requires of that answer). The hardware carries no container ID of its own, so
// there is nothing better to put there.
//
// What the driver does take is the EldInfo next to it: the port ID the operating system made for this child and
// the monitor's manufacturer and product. DP audio writes the port ID into the ELD of the Azalia endpoint, so
// that the audio endpoint and the monitor land in one device container (docs/design/dp-audio.md step 4).
NTSTATUS Bc250GetChildContainerId(_In_ const PVOID MiniportDeviceContext, _In_ ULONG ChildUid,
                                  _Inout_ PDXGK_CHILD_CONTAINER_ID ContainerId)
{
    if (ChildUid != BC250_CHILD_UID || ContainerId == NULL) return STATUS_INVALID_PARAMETER;
    DpAudioContainerId((BC250_DEVICE*)MiniportDeviceContext, ContainerId->EldInfo.PortId,
                       ContainerId->EldInfo.ManufacturerName, ContainerId->EldInfo.ProductCode);
    return STATUS_MONITOR_NO_DESCRIPTOR;
}

NTSTATUS Bc250SetPowerState(_In_ const PVOID MiniportDeviceContext, _In_ ULONG DeviceUid,
                            _In_ DEVICE_POWER_STATE DevicePowerState, _In_ POWER_ACTION ActionType)
{
    BC250_DEVICE* device=(BC250_DEVICE*)MiniportDeviceContext;
    if (!device || !AdapterOwnerIs(device->PhysicalDeviceObject)) return STATUS_DEVICE_NOT_READY;
    // BD-059: down for a system sleep or shutdown ends the GPU DWM session while the registry is still up (registry
    // only, before any hardware step); the way back to D0 marks it again if devices still use the path.
    if (DeviceUid==DISPLAY_ADAPTER_HW_ID && DevicePowerState!=PowerDeviceD0)
        InteropAdapterPower(device,DevicePowerState,ActionType);
    if (DeviceUid==DISPLAY_ADAPTER_HW_ID && device->FullWddm && device->Started) {
        NTSTATUS status;
        // The hang detector judges a started device in D0 only. A failed transition down leaves it paused:
        // silence is the safe side of a diagnostic that bugchecks.
        // FanPause after DpmPause: no fan step runs while the governor is paused, and the board has the fan out of D0.
        if (DevicePowerState!=PowerDeviceD0) {
            // BD-170 r6: dxgmms2 flushed the flip queue before this call (facts M904), so the power writers below owe
            // no flip (wddm.c's WddmFlipFlushed).
            WddmFlipFlushed(device);
            HangDetectorPause(); CpuPause(device); DpmPause(device); FanPause(device);
            DpAudioStop(device);    // AUDIO_ENABLED 0 before the display block goes down
            if (ConnectorIsEnabled(device)) {
                status=ConnectorSuspend(device);
                if (!NT_SUCCESS(status)) return status;
            }
            if (!InterlockedCompareExchange(&device->RuntimeDisplayChanged,0,0)) {
                if (!HdmiLinkStop(device)) return STATUS_DEVICE_DATA_ERROR;
                ModesetPowerDown(device);
            }
        }
        status=GpuSetPowerRetained(device,DevicePowerState,ActionType);
        // A failed transition down keeps the device in D0 with the desktop up: the committed shape goes back at once,
        // or the smaller surface of that desktop would be read through the native viewport.
        if (DevicePowerState!=PowerDeviceD0 && !NT_SUCCESS(status) && !ConnectorIsEnabled(device)) {
            ModesetPowerUp(device);
            HdmiLinkStartAudio(device,TRUE);
        }
        if (DevicePowerState==PowerDeviceD0 && NT_SUCCESS(status)) {
            if (!InterlockedCompareExchange(&device->RuntimeDisplayChanged,0,0) &&
                !InterlockedCompareExchange(&device->Connector.Pending,0,0)) ModesetPowerUp(device);
            DpmResume(device);
            FanResume(device);
            CpuResume(device);  // nothing of the CPU surface survives D3 in the chip: read it again (0.7.211)
            if (!ConnectorIsEnabled(device)) HdmiLinkStartAudio(device,TRUE);
            else if (!InterlockedCompareExchange(&device->Connector.Pending,0,0)) DpAudioStart(device);
            HangDetectorResume();
            InteropAdapterPower(device,DevicePowerState,ActionType);
        }
        return status;
    }
    // Display-only/initial PnP handling retains its existing ownership boundary.
    if (DeviceUid==DISPLAY_ADAPTER_HW_ID && DevicePowerState!=PowerDeviceD0) {
        StartHealthClose(device);
        CpuStop(device);
        DpmStop(device);
        SmuMetricsStop(device);
        FanStop(device, BC250_FAN_REASON_POWER);
        DpAudioStop(device);    // like the governor: stopped here, started again only by the next start
        HdmiLinkStop(device);
        SmuOwnerStop(&device->Smu);
    }
    return STATUS_SUCCESS;
}

void Bc250Unload(void)
{
    HangDetectorStop();     // its thread and DPC run this image's code: joined before the image goes
    InteropDriverUnload();  // likewise the power callback
    FanDriverUnload();      // likewise the fan watchdog and its bugcheck callback; the fan goes back to the board
    PspDriverCleanup();     // retained images and device workers have drained before driver unload
    GuardCleanup();
}
