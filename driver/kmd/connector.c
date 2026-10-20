// Discovery is passive and generation-guarded. Windows callbacks run without the
// operation mutex: IndicateChildStatus can cause another VidPN transaction.
#include "bc250kmd.h"
#include "connector.h"
#include "../shim/bc250_dp_train.h"

#define CONNECTOR_TAG 'cC2B'
#define CONNECTOR_DEBOUNCE_100NS (-500000LL) // one 50 ms settling interval
#define CONNECTOR_DISCOVERY_US 250000ul
#define CONNECTOR_BUSY_REARMS 3u // worker re-arms of one generation after a BUSY disable
// BD-195: a DP sink can raise HPD before its AUX receiver answers (a monitor that wakes from standby on the plug).
// Linux retrieve_link_cap waits for it: wake_up_aux_channel, then up to 20 DPCD reads 30 ms apart. The worker does the
// same with whole discoveries, the operation mutex released between them, about 2 s in all.
#define CONNECTOR_DISCOVERY_TRIES 20u
#define CONNECTOR_DISCOVERY_RETRY_100NS (-1000000LL) // 100 ms between two discoveries of one generation

static LONG ReadAtomic(volatile LONG* Value)
{
    return InterlockedCompareExchange(Value, 0, 0);
}

// BD-195: a disable that answered BUSY (the writer refused or another writer
// held the plane) is tried again by the same worker path after the debounce,
// at most CONNECTOR_BUSY_REARMS times for one generation. Before this, the
// worker stopped there and a re-plug got no Indicate until the next HPD edge.
// A new generation starts a new count. Operation held.
static BOOLEAN ConnectorBusyRearm(BC250_CONNECTOR* c, ULONG Generation)
{
    if (c->BusyGeneration != Generation) {
        c->BusyGeneration = Generation;
        c->BusyRearms = 0;
    }
    if (c->BusyRearms >= CONNECTOR_BUSY_REARMS || !ReadAtomic(&c->Running)) return FALSE;
    c->BusyRearms++;
    InterlockedExchange(&c->Pending, 1);
    return TRUE;
}

// A discovery that found no answer yet is tried again: an AUX or DDC timeout, a NACK, a spent stall budget. Not a
// failed pad cleanup (no second protocol after it), a refused register, a stale generation or a sink that answered
// with something this driver does not drive.
static BOOLEAN ConnectorDiscoveryRetryable(long Status, long CleanupStatus)
{
    return !CleanupStatus && (Status == BC250_DISP_STATUS_TIMEOUT || Status == BC250_DISP_STATUS_DEVICE ||
        Status == BC250_DISP_STATUS_BUDGET);
}

static long ConnectorRead(void* Context, unsigned long Offset, unsigned long* Value)
{
    BC250_DEVICE* device = Context;
    *Value = 0;
    if (!device->Mmio) return STATUS_DEVICE_NOT_READY;
    if (!Bc250DispReadAllowed(Offset)) return STATUS_ACCESS_DENIED;
    *Value = READ_REGISTER_ULONG(device->Mmio + Offset / sizeof(ULONG));
    return STATUS_SUCCESS;
}

static long ConnectorWrite(void* Context, unsigned long Offset, unsigned long Value)
{
    BC250_DEVICE* device = Context;
    // Cleanup remains possible after a generation change. Stop drains the worker
    // before MMIO is removed; discovery's guarded wrapper rejects stale work.
    if (!device->Mmio) return STATUS_DEVICE_NOT_READY;
    if (!device->DcnWriteEnabled || !Bc250DispWriteAllowed(Offset)) return STATUS_ACCESS_DENIED;
    WRITE_REGISTER_ULONG(device->Mmio + Offset / sizeof(ULONG), Value);
    return STATUS_SUCCESS;
}

static void ConnectorStall(void* Context, unsigned long Microseconds)
{
    UNREFERENCED_PARAMETER(Context);
    KeStallExecutionProcessor(Microseconds);
}

BOOLEAN ConnectorGenerationCurrent(BC250_DEVICE* Device, ULONG Generation)
{
    BC250_CONNECTOR* c = &Device->Connector;
    return (ReadAtomic(&c->Running) != 0 || ReadAtomic(&c->Restoring) != 0) &&
        (ULONG)ReadAtomic(&c->Generation) == Generation &&
        (ReadAtomic(&Device->RetainedPowerPhase) == 0 ||
         (ReadAtomic(&c->Restoring) && ReadAtomic(&Device->RetainedPowerPhase) == 3));
}

static int DiscoveryCurrent(void* Context)
{
    BC250_DEVICE* device = Context;
    return ConnectorGenerationCurrent(device, device->Connector.OperationGeneration);
}

void ConnectorOperationAcquire(BC250_DEVICE* Device)
{
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    (void)KeWaitForSingleObject(&Device->Connector.Operation, Executive, KernelMode, FALSE, NULL);
}

void ConnectorOperationRelease(BC250_DEVICE* Device)
{
    KeReleaseMutex(&Device->Connector.Operation, FALSE);
}

BC250_CONNECTOR_SNAPSHOT* ConnectorSnapshotAcquire(BC250_DEVICE* Device)
{
    BC250_CONNECTOR_SNAPSHOT* snapshot;
    KIRQL irql;
    KeAcquireSpinLock(&Device->Connector.Lock, &irql);
    snapshot = Device->Connector.Snapshot;
    if (snapshot && (ReadAtomic(&Device->Connector.Suspended) ||
        snapshot->Generation != (ULONG)ReadAtomic(&Device->Connector.Generation)))
        snapshot = NULL;
    if (snapshot) InterlockedIncrement(&snapshot->References);
    KeReleaseSpinLock(&Device->Connector.Lock, irql);
    return snapshot;
}

void ConnectorSnapshotRelease(BC250_CONNECTOR_SNAPSHOT* Snapshot)
{
    if (Snapshot && InterlockedDecrement(&Snapshot->References) == 0)
        ExFreePoolWithTag(Snapshot, CONNECTOR_TAG);
}

BOOLEAN ConnectorIsEnabled(BC250_DEVICE* Device)
{
    return ReadAtomic(&Device->Connector.Enabled) != 0;
}

BOOLEAN ConnectorIsConnected(BC250_DEVICE* Device)
{
    BC250_CONNECTOR_SNAPSHOT* s = ConnectorSnapshotAcquire(Device);
    BOOLEAN connected = s && s->Sink.Kind != BC250_SINK_NONE;
    ConnectorSnapshotRelease(s);
    return connected;
}

static BC250_CONNECTOR_SNAPSHOT* AllocateSnapshot(ULONG Generation)
{
    BC250_CONNECTOR_SNAPSHOT* s = ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*s), CONNECTOR_TAG);
    if (s) {
        s->References = 1;
        s->Generation = Generation;
    }
    return s;
}

static BOOLEAN Publish(BC250_DEVICE* Device, BC250_CONNECTOR_SNAPSHOT* New)
{
    BC250_CONNECTOR* c = &Device->Connector;
    BC250_CONNECTOR_SNAPSHOT* old;
    KIRQL irql;
    KeAcquireSpinLock(&c->Lock, &irql);
    if (!ConnectorGenerationCurrent(Device, New->Generation)) {
        KeReleaseSpinLock(&c->Lock, irql);
        InterlockedIncrement(&c->Stale);
        return FALSE;
    }
    old = c->Snapshot;
    c->Snapshot = New;
    InterlockedIncrement(&c->Publications);
    KeReleaseSpinLock(&c->Lock, irql);
    ConnectorSnapshotRelease(old);
    return TRUE;
}

static void Indicate(BC250_DEVICE* Device, BOOLEAN Connected, ULONG Generation)
{
    DXGK_CHILD_STATUS child = {0};
    NTSTATUS status;
    if (!ConnectorGenerationCurrent(Device, Generation)) return;
    child.Type = StatusConnection;
    child.ChildUid = BC250_CHILD_UID;
    child.HotPlug.Connected = Connected;
    status = Device->Dxgk.DxgkCbIndicateChildStatus(Device->Dxgk.DeviceHandle, &child);
    InterlockedIncrement(&Device->Connector.Notifications);
    if (!NT_SUCCESS(status)) InterlockedIncrement(&Device->Connector.NotifyFailures);
    GuardLog("connector: indicate generation %lu connected %u status %08lX",
        Generation, Connected, (ULONG)status);
}

static void ConnectorWorker(PDEVICE_OBJECT Object, PVOID Context);

void ConnectorDpc(BC250_DEVICE* Device)
{
    BC250_CONNECTOR* c = &Device->Connector;
    KIRQL irql;
    KeAcquireSpinLock(&c->Lock, &irql);
    if (c->Running && c->Enabled && !c->Suspended && !c->Queued && c->WorkItem &&
        (ReadAtomic(&c->Pending) || ReadAtomic(&c->RxPending))) {
        c->Queued = 1;
        KeClearEvent(&c->Idle);
        IoQueueWorkItem(c->WorkItem, ConnectorWorker, DelayedWorkQueue, Device);
    }
    KeReleaseSpinLock(&c->Lock, irql);
}

static void DebounceDpc(PKDPC Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    ConnectorDpc(Context);
}

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
NTSTATUS ConnectorReplug(BC250_DEVICE* Device, ULONG* Generation, BOOLEAN* DpcQueued)
{
    BC250_CONNECTOR* c = &Device->Connector;
    KIRQL irql;
    NTSTATUS status = STATUS_DEVICE_NOT_READY;
    *Generation = 0;
    *DpcQueued = FALSE;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    // Same admission lock as ConnectorDrain. Queue before releasing it, so stop
    // cannot finish its DPC drain and then receive this request. No MMIO or
    // operation mutex here: the existing PASSIVE worker owns disable/discovery.
    KeAcquireSpinLock(&c->Lock, &irql);
    if (Device->FullWddm && c->Running && c->Enabled && !c->Suspended && c->WorkItem &&
        !ReadAtomic(&Device->RetainedPowerPhase) && Device->Dxgk.DxgkCbQueueDpc) {
        *Generation = (ULONG)InterlockedIncrement(&c->Generation);
        InterlockedExchange(&c->Pending, 1);
        *DpcQueued = Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
        // FALSE also means that the adapter already has a queued DPC. Pending
        // remains set for that DPC (or the running worker's debounce rearm).
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&c->Lock, irql);
    return status;
}
#endif

BOOLEAN ConnectorInterrupt(BC250_DEVICE* Device)
{
    BC250_CONNECTOR* c = &Device->Connector;
    int sense, rx, handled;
    if (!ReadAtomic(&c->Running) || !ReadAtomic(&c->Enabled) || ReadAtomic(&c->Suspended)) return FALSE;
    handled = Bc250HpdInterrupt(&c->Hpd, &sense, &rx);
    if (handled <= 0) {
        if (handled < 0) InterlockedIncrement(&c->Refusals);
        return FALSE;
    }
    InterlockedIncrement(&c->Interrupts);
    if (c->Hpd.Events & 1u) {
        InterlockedIncrement(&c->Generation);
        InterlockedExchange(&c->Pending, 1);
    }
    if (rx) {
        InterlockedIncrement(&c->RxInterrupts);
        InterlockedExchange(&c->RxPending, 1);
    }
    (void)Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
    return TRUE;
}

typedef struct _CONNECTOR_SYNC {
    BC250_DEVICE* Device;
    BOOLEAN Arm, Invoked;
    NTSTATUS Status;
} CONNECTOR_SYNC;

static BOOLEAN HpdSynchronized(PVOID Context)
{
    CONNECTOR_SYNC* sync = Context;
    BC250_CONNECTOR* c = &sync->Device->Connector;
    sync->Invoked = TRUE;
    sync->Status = sync->Arm ? Bc250HpdArm(&c->Hpd, &c->HpdIo) : Bc250HpdDisarm(&c->Hpd);
    if (sync->Arm && NT_SUCCESS(sync->Status)) InterlockedExchange(&c->Enabled, 1);
    return TRUE;
}

static NTSTATUS SynchronizeHpd(BC250_DEVICE* Device, BOOLEAN Arm)
{
    CONNECTOR_SYNC sync = {0};
    BOOLEAN returned = FALSE;
    NTSTATUS status;
    sync.Device = Device;
    sync.Arm = Arm;
    status = Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle,
        HpdSynchronized, &sync, 0, &returned);
    if (!NT_SUCCESS(status)) return status;
    if (!returned || !sync.Invoked) return STATUS_DEVICE_NOT_READY;
    return sync.Status;
}

// BD-195: with KeepLog set, the worker's steps reach the disk before they run (GuardKeepActive), so a step that
// stops the whole machine is named by the last line of the keep file after the power cycle.
static void WorkerKeep(const char* Step, ULONG Generation, int Sense)
{
    if (!GuardKeepActive()) return;
    GuardLog("connector: keep %s generation %lu sense %d", Step, Generation, Sense);
    GuardLogKeep();
}

static void ConnectorWorker(PDEVICE_OBJECT Object, PVOID Context)
{
    BC250_DEVICE* device = Context;
    BC250_CONNECTOR* c = &device->Connector;
    BC250_CONNECTOR_SNAPSHOT* next = NULL;
    BC250_CONNECTOR_SNAPSHOT* old = NULL;
    BC250_SINK_GUARD guard = {device, DiscoveryCurrent};
    LONG changed = InterlockedExchange(&c->Pending, 0);
    LONG rx = InterlockedExchange(&c->RxPending, 0);
    ULONG generation = (ULONG)ReadAtomic(&c->Generation);
    LARGE_INTEGER delay;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN notify = FALSE, connected = FALSE;
    int sense = 0;
    UNREFERENCED_PARAMETER(Object);
    InterlockedIncrement(&c->Workers);
    if (changed) {
        delay.QuadPart = CONNECTOR_DEBOUNCE_100NS;
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    ConnectorOperationAcquire(device);
    if (!ConnectorGenerationCurrent(device, generation)) goto stale;
    c->OperationGeneration = generation;
    old = ConnectorSnapshotAcquire(device);
    if (!changed && rx) {
        if (!old || old->Sink.Kind != BC250_SINK_DP) {
            GuardLog("connector: RX generation %lu without DP snapshot; no unplug", generation);
            goto done;
        }
        status = ConnectorLinkCheck(device, &old->Sink, generation);
        GuardLog("connector: RX generation %lu status %08lX", generation, (ULONG)status);
        if (status != STATUS_RETRY) goto done;
        generation = (ULONG)InterlockedIncrement(&c->Generation);
        c->OperationGeneration = generation;
    }
    status = Bc250HpdSense(&c->Hpd, &sense);
    if (!NT_SUCCESS(status)) goto done;
    next = AllocateSnapshot(generation);
    if (!next) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    WorkerKeep("disable", generation, sense);
    status = ConnectorLinkDisable(device, generation);
    GuardLog("connector: disable generation %lu status %08lX", generation, (ULONG)status);
    if (status == STATUS_DEVICE_BUSY && ConnectorBusyRearm(c, generation))
        GuardLog("connector: disable busy generation %lu re-arm %lu of %lu",
            generation, c->BusyRearms, (ULONG)CONNECTOR_BUSY_REARMS);
    if (!NT_SUCCESS(status)) goto done;
    if (!ConnectorGenerationCurrent(device, generation)) goto stale;
    // Publish the detached state before probing another sink. A failed discovery
    // must not leave the old EDID advertised after its stream was disabled.
    if (!Publish(device, next)) goto stale;
    next = NULL;
    ConnectorOperationRelease(device);
    Indicate(device, FALSE, generation);
    ConnectorOperationAcquire(device);
    if (!ConnectorGenerationCurrent(device, generation)) goto stale;
    if (!sense) goto done;
    next = AllocateSnapshot(generation);
    if (!next) { status = STATUS_INSUFFICIENT_RESOURCES; goto done; }
    if (sense) {
        ULONG tries = 0;
        for (;;) {
            c->DiscoveryIo.StalledUs = 0;
            InterlockedIncrement(&c->Discoveries);
            WorkerKeep("discovery", generation, sense);
            status = Bc250SinkDiscover(&c->DiscoveryIo, &guard, 300000ul, 4096ul, 2160ul, &next->Sink);
            tries++;
            GuardLog("connector: discovery generation %lu kind %u phase %u edid %lu targets %lu status %08lX cleanup %08lX",
                generation, next->Sink.Kind, next->Sink.Phase, next->Sink.EdidLength,
                next->Sink.TargetCount, (ULONG)status, (ULONG)next->Sink.CleanupStatus);
            GuardLog("connector: discovery generation %lu try %lu aux %08lX ddc %08lX", generation, tries,
                (ULONG)next->Sink.AuxStatus, (ULONG)next->Sink.DdcStatus);
            if (NT_SUCCESS(status) || tries >= CONNECTOR_DISCOVERY_TRIES ||
                !ConnectorDiscoveryRetryable(status, next->Sink.CleanupStatus)) break;
            // Wait without the mutex, so a VidPN commit or a power transition is not held up, then start again
            // only for the same generation with HPD still high.
            InterlockedIncrement(&c->DiscoveryRetries);
            ConnectorOperationRelease(device);
            delay.QuadPart = CONNECTOR_DISCOVERY_RETRY_100NS;
            (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
            ConnectorOperationAcquire(device);
            if (!ConnectorGenerationCurrent(device, generation)) goto stale;
            status = Bc250HpdSense(&c->Hpd, &sense);
            if (!NT_SUCCESS(status)) goto done;
            if (!sense) {
                GuardLog("connector: discovery generation %lu HPD low after try %lu", generation, tries);
                goto done;
            }
        }
        if (!NT_SUCCESS(status)) goto done;
        WorkerKeep("prepare", generation, sense);
        status = ConnectorLinkPrepare(device, &next->Sink, generation);
        if (!NT_SUCCESS(status)) goto done;
        WorkerKeep("indicate", generation, sense);
    }
    connected = next->Sink.Kind != BC250_SINK_NONE;
    if (!Publish(device, next)) goto stale;
    next = NULL;
    notify = TRUE;
    goto done;
stale:
    status = STATUS_RETRY;
    InterlockedIncrement(&c->Stale);
    if (ReadAtomic(&c->Running)) InterlockedExchange(&c->Pending, 1);
done:
    c->LastStatus = status;
    if (!NT_SUCCESS(status)) {
        InterlockedIncrement(&c->Refusals);
        GuardLog("connector: refused generation %lu status %08lX", generation, (ULONG)status);
    }
    ConnectorSnapshotRelease(next);
    ConnectorSnapshotRelease(old);
    ConnectorOperationRelease(device);
    if (notify) Indicate(device, connected, generation);
    // Finish under the queue lock. Stop takes this lock after waiting on Idle,
    // ensuring no worker touches the embedded state after its final release.
    KeAcquireSpinLock(&c->Lock, &irql);
    c->Queued = 0;
    if (c->Running && (ReadAtomic(&c->Pending) || ReadAtomic(&c->RxPending))) {
        delay.QuadPart = CONNECTOR_DEBOUNCE_100NS;
        KeSetTimer(&c->Debounce, delay, &c->DebounceDpc);
        InterlockedIncrement(&c->Rearms);
    }
    KeSetEvent(&c->Idle, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&c->Lock, irql);
}

void ConnectorInitialize(BC250_DEVICE* Device)
{
    BC250_CONNECTOR* c = &Device->Connector;
    RtlZeroMemory(c, sizeof(*c));
    KeInitializeMutex(&c->Operation, 0);
    KeInitializeSpinLock(&c->Lock);
    KeInitializeEvent(&c->Idle, NotificationEvent, TRUE);
    KeInitializeTimer(&c->Debounce);
    KeInitializeDpc(&c->DebounceDpc, DebounceDpc, Device);
    c->HpdIo.Context = Device;
    c->HpdIo.Read = ConnectorRead;
    c->HpdIo.Write = ConnectorWrite;
    c->HpdIo.Stall = ConnectorStall;
    c->DiscoveryIo = c->HpdIo;
    c->DiscoveryIo.BudgetUs = CONNECTOR_DISCOVERY_US;
}

// Static VidPN has its own source/target contract and preserves the GOP scaler.
// Do not feed that tuple to Plan(), which describes a newly programmed 1:1 pipe.
// This route also covers unavailable discovery/programming prerequisites. It is
// selected before any connector transaction; a failed transaction is not hidden.
static BOOLEAN ConnectorNeedsInherited(const BC250_DEVICE* Device)
{
    return Device->Modeset.InheritedFixed || Device->Modeset.Requested==BC250_DISPLAY_MODES_OFF ||
        !Device->Mmio || !Device->DcnWriteEnabled || !Device->Modeset.EdidValid ||
        (!Device->Modeset.DpcdValid && Device->Modeset.Adapter.type==BC250_ADAPTER_UNKNOWN) ||
        !Device->Dxgk.DxgkCbSynchronizeExecution || !Device->Dxgk.DxgkCbQueueDpc ||
        !Device->Dxgk.DxgkCbIndicateChildStatus;
}

static NTSTATUS ConnectorStartInherited(BC250_DEVICE* Device)
{
    BC250_MODESET* m=&Device->Modeset;
    ULONG scaling;
    if (!Device->InheritedSignalValid || !NT_SUCCESS(m->InheritedPipeStatus) || !m->InheritedSupport)
        return STATUS_NOT_SUPPORTED;
    for(scaling=0;scaling<BC250_SCALING_COUNT;scaling++)
        if(m->InheritedSupport & BC250_SCALING_BIT(scaling)) break;
    if(scaling==BC250_SCALING_COUNT) return STATUS_NOT_SUPPORTED;
    // A native pipe may have advertised extra modes before discovery failed or
    // a programming callback proved unavailable. Publish only the saved tuple.
    Bc250ModeListBuild(Device->Post.Width,Device->Post.Height,NULL,BC250_DISPLAY_MODES_OFF,&m->Modes);
    if (m->Modes.Count!=1 || m->Modes.Modes[0].Width!=Device->Post.Width ||
        m->Modes.Modes[0].Height!=Device->Post.Height) return STATUS_NOT_SUPPORTED;
    m->InheritedFixed=TRUE;
    m->InheritedSupportCalls=m->InheritedCofuncCalls=m->InheritedCommitCalls=0;
    m->Level=BC250_DISPLAY_MODES_OFF;
    m->Scaling=scaling;
    Device->Connector.StartAccepted=1;
    GuardLog("connector: inherited-only child %lu source %ux%u pitch %u target %ux%u pixel %Iu support %lX",
        (ULONG)BC250_CHILD_UID,Device->Post.Width,Device->Post.Height,Device->Post.Pitch,
        Device->InheritedSignal.ActiveSize.cx,Device->InheritedSignal.ActiveSize.cy,
        Device->InheritedSignal.PixelRate,m->InheritedSupport);
    GuardLog("connector: inherited-only keeps scaler and output timing; no mode change or hotplug programming");
    return STATUS_SUCCESS;
}

// ConnectorStart plans and arms; it programs no output. Plan() runs DML and
// refuses a tuple it cannot carry, ConnectorLinkPrepare writes no pipe, PLL or
// PHY register, and the HPD arming and the published snapshot touch neither. A
// refusal therefore leaves the firmware's own stream on the wire, and the static
// inherited path presents it, as every release up to 0.7.216.63 did ("keeping
// static inherited path"). The adapter start may fail only where nothing can be
// presented: no inherited signal, or no source mode of the POST size.
// InheritedFixed is that static path itself (a scaled firmware tuple, #38 and
// BD-155, or a non-identity inherited transform). Its refusal does mean the
// tuple cannot be described at all, and the start still fails there.
// The one place that decides whether a refused start left the hardware as the
// firmware had it. Three results have to agree, and each is a hardware
// obligation this start took on: the HPD disarm, the caller's own release, and
// the terminal release that ConnectorLinkPrepare performed inside itself. That
// last one is why this is a function: the inner release frees the context and
// clears Device->ConnectorLink, so the caller's release answers SUCCESS for a
// NULL context, and a failed AUX restore (which dpaux.c keeps open on purpose)
// would otherwise be preserved as "nothing happened" (BD-169 review).
BOOLEAN ConnectorRefusalPreserved(const BC250_DEVICE* Device, NTSTATUS Hpd, NTSTATUS Release)
{
    return (NT_SUCCESS(Hpd) && NT_SUCCESS(Release) &&
            NT_SUCCESS(Device->ConnectorLinkCleanup)) ? TRUE : FALSE;
}

BOOLEAN ConnectorRefusalKeepsDisplay(const BC250_DEVICE* Device)
{
    const BC250_MODESET* m=&Device->Modeset;
    // What the start may keep is one pipe: a stable, validated identity pipe
    // whose only missing piece is programmable connector admission.
    // StartPreserved excludes a refusal that wrote and could not restore.
    // The pipe status excludes an unreadable and a moving pipe, and the IDENTITY
    // bit is the whole fetch and ownership contract of Bc250InheritedPipeSupport:
    // the POST-sized viewport at the POST pitch, a linear untiled surface with
    // no DCC and no rotation, pixel format 8, the top MPCC layer with nothing
    // blended under it, OTG1 not master and the MPC output on our pipe. Equal
    // source and target sizes do not establish any of that, so a mode-list
    // entry is not a contract: without the IDENTITY bit this driver cannot
    // promise that its own buffers will scan out, and the start fails (BD-169).
    if (!Device->Connector.StartPreserved) return FALSE;
    if (m->InheritedFixed || !Device->InheritedSignalValid) return FALSE;
    if (!NT_SUCCESS(m->InheritedPipeStatus)) return FALSE;
    if (!(m->InheritedSupport & BC250_SCALING_BIT(BC250_SCALING_IDENTITY))) return FALSE;
    return Bc250ModeListHas(&m->Modes,Device->Post.Width,Device->Post.Height) ? TRUE : FALSE;
}

NTSTATUS ConnectorStart(BC250_DEVICE* Device)
{
    BC250_CONNECTOR* c = &Device->Connector;
    BC250_CONNECTOR_SNAPSHOT* snapshot;
    NTSTATUS status;
    ULONG generation;
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    if (c->Running) return STATUS_DEVICE_BUSY;
    RtlZeroMemory(&c->FirstOpen,sizeof(c->FirstOpen));
    RtlZeroMemory(&c->FirstAttempt,sizeof(c->FirstAttempt));
    RtlZeroMemory(&c->FirstFailure,sizeof(c->FirstFailure));
    RtlZeroMemory(&c->FirstRecovery,sizeof(c->FirstRecovery));
    c->StartKind=c->StartAdapter=c->StartLimit=c->StartTargets=c->StartAccepted=0;
    c->StartPreserved=0;        // earned per return path below (BD-169)
    Device->ConnectorLinkCleanup=STATUS_SUCCESS;     // no earlier attempt's cleanup is this start's
    c->StartStatus=STATUS_NOT_SUPPORTED;
    if (!Device->FullWddm) {
        // Display-only does not own a hardware connector.
        c->StartPreserved=1;
        return c->LastStatus=c->StartStatus=STATUS_SUCCESS;
    }
    if (ConnectorNeedsInherited(Device)) {
        // Route selection and admission only: no discovery transaction and
        // no register write on either outcome.
        c->StartPreserved=1;
        status=ConnectorStartInherited(Device);
        c->LastStatus=c->StartStatus=status;
        if (!NT_SUCCESS(status)) InterlockedIncrement(&c->Refusals);
        return status;
    }
    if (!c->WorkItem) c->WorkItem = IoAllocateWorkItem(Device->PhysicalDeviceObject);
    if (!c->WorkItem) {
        c->LastStatus=c->StartStatus=STATUS_INSUFFICIENT_RESOURCES;c->StartPreserved=1;
        GuardLog("connector: start work-item allocation failed");return c->StartStatus;
    }
    generation = (ULONG)InterlockedIncrement(&c->Generation);
    snapshot = AllocateSnapshot(generation);
    if (!snapshot) {
        c->LastStatus=c->StartStatus=STATUS_INSUFFICIENT_RESOURCES;c->StartPreserved=1;
        GuardLog("connector: start snapshot allocation failed");return c->StartStatus;
    }
    snapshot->Sink.Kind = Device->Modeset.DpcdValid ? BC250_SINK_DP :
        (Device->Modeset.Info.Hdmi ? BC250_SINK_HDMI : BC250_SINK_DVI);
    snapshot->Sink.Info = Device->Modeset.Info;
    snapshot->Sink.MaxPixelClockKhz = 300000ul;
    snapshot->Sink.Adapter = Device->Modeset.Adapter;
    snapshot->Sink.EdidLength = Device->Modeset.EdidLength;
    RtlCopyMemory(snapshot->Sink.Edid, Device->Modeset.Edid, sizeof(snapshot->Sink.Edid));
    RtlCopyMemory(snapshot->Sink.Dpcd, Device->Modeset.Dpcd, sizeof(snapshot->Sink.Dpcd));
    {
        ULONG limit = Device->Modeset.DpcdValid ? Bc250DpMaxPixelClock(snapshot->Sink.Dpcd) :
            Device->Modeset.Adapter.max_tmds_khz;
        if (limit < snapshot->Sink.MaxPixelClockKhz) snapshot->Sink.MaxPixelClockKhz = limit;
    }
    snapshot->Sink.TargetCount = Bc250EdidTargetList(&snapshot->Sink.Info,
        snapshot->Sink.MaxPixelClockKhz, 4096ul, 2160ul, snapshot->Sink.Targets, BC250_EDID_MAX_MODES);
    c->StartKind=(ULONG)snapshot->Sink.Kind;c->StartAdapter=(ULONG)snapshot->Sink.Adapter.type;
    c->StartLimit=snapshot->Sink.MaxPixelClockKhz;c->StartTargets=snapshot->Sink.TargetCount;
    InterlockedExchange(&c->Running, 1);
    ConnectorOperationAcquire(Device);
    status = ConnectorLinkPrepare(Device, &snapshot->Sink, generation);
    if (NT_SUCCESS(status)) c->StartAccepted=snapshot->Sink.TargetCount;
    if (NT_SUCCESS(status) && !snapshot->Sink.TargetCount) status = STATUS_NOT_SUPPORTED;
    if (NT_SUCCESS(status)) status = SynchronizeHpd(Device, TRUE);
    if (NT_SUCCESS(status) && !Publish(Device, snapshot)) status = STATUS_RETRY;
    if (NT_SUCCESS(status)) {
        snapshot = NULL;
        InterlockedExchange(&c->Enabled, 1);
    } else {
        // The planning refusal itself writes nothing (connector_link.c: Plan
        // reads clocks and runs DML), but this path disarms HPD and releases
        // the link context, and the DP branch of the prepare may have opened
        // AUX. The static inherited path may only be told that it still owns
        // a firmware display when those restores succeeded; otherwise the
        // hardware state is unknown and the start must fail (BD-169).
        NTSTATUS hpd = SynchronizeHpd(Device, FALSE);
        NTSTATUS release;
        InterlockedExchange(&c->Enabled, 0);
        InterlockedExchange(&c->Running, 0);
        release = ConnectorLinkRelease(Device);
        if (ConnectorRefusalPreserved(Device, hpd, release)) c->StartPreserved=1;
        else GuardLog("connector: start refusal left HPD %08lX release %08lX prepare %08lX; no static fallback",
                      (ULONG)hpd,(ULONG)release,(ULONG)Device->ConnectorLinkCleanup);
        InterlockedIncrement(&c->Refusals);
    }
    c->LastStatus = c->StartStatus = status;
    ConnectorOperationRelease(Device);
    ConnectorSnapshotRelease(snapshot);
    GuardLog("connector: start generation %lu status %08lX inherited stream preserved %lu",
        generation, (ULONG)status, c->StartPreserved);
    return status;
}

static NTSTATUS ConnectorDrain(BC250_DEVICE* Device, BOOLEAN Suspend)
{
    BC250_CONNECTOR* c = &Device->Connector;
    KIRQL irql;
    NTSTATUS status = STATUS_SUCCESS;
    NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
    KeAcquireSpinLock(&c->Lock, &irql);
    InterlockedExchange(&c->Running, 0);
    if (!Suspend) InterlockedExchange(&c->Enabled, 0);
    InterlockedExchange(&c->Suspended, Suspend);
    InterlockedIncrement(&c->Generation);
    KeCancelTimer(&c->Debounce);
    KeReleaseSpinLock(&c->Lock, irql);
    if (c->Hpd.Saved)
        status = Device->Dxgk.DxgkCbSynchronizeExecution ? SynchronizeHpd(Device, FALSE) :
            STATUS_DEVICE_NOT_READY;
    // Timer DPC and the adapter's already queued DPC cannot queue new work now.
    KeFlushQueuedDpcs();
    (void)KeWaitForSingleObject(&c->Idle, Executive, KernelMode, FALSE, NULL);
    KeAcquireSpinLock(&c->Lock, &irql);
    InterlockedExchange(&c->Pending, 0);
    InterlockedExchange(&c->RxPending, 0);
    KeReleaseSpinLock(&c->Lock, irql);
    ConnectorOperationAcquire(Device);
    // Keep the cold-start reason readable after the early ring has wrapped,
    // even when OpenContext failed and no link context survived.
    GuardLog("connector: retained start status %08lX kind %lu adapter %lu limit %lu targets %lu accepted %lu",
        (ULONG)c->StartStatus,c->StartKind,c->StartAdapter,c->StartLimit,c->StartTargets,c->StartAccepted);
    ConnectorLinkLogOpen(Device);
    if (Suspend) {
        NTSTATUS linkStatus=ConnectorLinkSuspend(Device);
        if (NT_SUCCESS(status)) status=linkStatus;
    } else {
        NTSTATUS linkStatus=ConnectorLinkRelease(Device);
        if (NT_SUCCESS(status)) status=linkStatus;
    }
    ConnectorOperationRelease(Device);
    GuardLog("connector: irq %ld rx %ld work %ld detect %ld stale %ld refused %ld notify %ld failed %ld",
        ReadAtomic(&c->Interrupts),ReadAtomic(&c->RxInterrupts),ReadAtomic(&c->Workers),
        ReadAtomic(&c->Discoveries),ReadAtomic(&c->Stale),ReadAtomic(&c->Refusals),
        ReadAtomic(&c->Notifications),ReadAtomic(&c->NotifyFailures));
    GuardLog("connector: stop status %08lX drained, discovery retries %ld", (ULONG)status,
        ReadAtomic(&c->DiscoveryRetries));
    return status;
}

NTSTATUS ConnectorStop(BC250_DEVICE* Device)
{
    return ConnectorDrain(Device,FALSE);
}

NTSTATUS ConnectorSuspend(BC250_DEVICE* Device)
{
    if (!ConnectorIsEnabled(Device)) return STATUS_SUCCESS;
    if (ReadAtomic(&Device->Connector.Suspended)) return STATUS_SUCCESS;
    return ConnectorDrain(Device,TRUE);
}

NTSTATUS ConnectorResumeDisplay(BC250_DEVICE* Device)
{
    BC250_CONNECTOR* c=&Device->Connector;
    BC250_CONNECTOR_SNAPSHOT *old=NULL,*next=NULL;
    BC250_SINK_GUARD guard={Device,DiscoveryCurrent};
    KIRQL irql;
    ULONG generation;
    NTSTATUS status=STATUS_SUCCESS;
    BOOLEAN same=FALSE;
    if (!ConnectorIsEnabled(Device) || !ReadAtomic(&c->Suspended) ||
        ReadAtomic(&Device->RetainedPowerPhase)!=3) return STATUS_INVALID_DEVICE_STATE;
    ConnectorOperationAcquire(Device);
    KeAcquireSpinLock(&c->Lock,&irql);
    old=c->Snapshot;
    if (old) InterlockedIncrement(&old->References);
    KeReleaseSpinLock(&c->Lock,irql);
    generation=(ULONG)InterlockedIncrement(&c->Generation);
    c->OperationGeneration=generation;
    InterlockedExchange(&c->Restoring,1);
    next=AllocateSnapshot(generation);
    if (!next) {status=STATUS_INSUFFICIENT_RESOURCES;goto done;}
    c->DiscoveryIo.StalledUs=0;
    status=Bc250SinkDiscover(&c->DiscoveryIo,&guard,300000ul,4096ul,2160ul,&next->Sink);
    same=NT_SUCCESS(status) && old && old->Sink.Kind!=BC250_SINK_NONE &&
        old->Sink.Kind==next->Sink.Kind && old->Sink.EdidLength==next->Sink.EdidLength &&
        old->Sink.Adapter.type==next->Sink.Adapter.type &&
        old->Sink.Adapter.max_tmds_khz==next->Sink.Adapter.max_tmds_khz &&
        RtlCompareMemory(old->Sink.Dpcd,next->Sink.Dpcd,sizeof(old->Sink.Dpcd))==sizeof(old->Sink.Dpcd) &&
        old->Sink.EdidLength && old->Sink.EdidLength<=sizeof(old->Sink.Edid) &&
        RtlCompareMemory(old->Sink.Edid,next->Sink.Edid,old->Sink.EdidLength)==old->Sink.EdidLength;
    if (same) {
        status=ConnectorLinkResume(Device,generation);
        if (!NT_SUCCESS(status)) goto done;
        // Retain the already admitted target plans, not an unfiltered reparse.
        next->Sink=old->Sink;
        if (!Publish(Device,next)) {status=STATUS_RETRY;goto done;}
        next=NULL;
    } else {
        // Changed/absent/unreadable sink: never replay a cached timing. Worker
        // rediscovers and publishes the Windows notifications after phase 0.
        InterlockedExchange(&c->Pending,1);
        Device->ModeActive=FALSE;
        status=STATUS_SUCCESS;
    }
done:
    InterlockedExchange(&c->Restoring,0);
    InterlockedExchange(&c->Running,0);
    ConnectorSnapshotRelease(next);
    ConnectorSnapshotRelease(old);
    ConnectorOperationRelease(Device);
    GuardLog("connector: resume discovery generation %lu same %u status %08lX",generation,same,(ULONG)status);
    return status;
}

NTSTATUS ConnectorResumeNotifications(BC250_DEVICE* Device)
{
    BC250_CONNECTOR* c=&Device->Connector;
    NTSTATUS status;
    if (!ConnectorIsEnabled(Device) || !ReadAtomic(&c->Suspended) ||
        ReadAtomic(&Device->RetainedPowerPhase)!=0) return STATUS_INVALID_DEVICE_STATE;
    ConnectorOperationAcquire(Device);
    InterlockedExchange(&c->Running,1);
    InterlockedExchange(&c->Suspended,0);
    status=SynchronizeHpd(Device,TRUE);
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&c->Running,0);
        InterlockedExchange(&c->Suspended,1);
    }
    ConnectorOperationRelease(Device);
    if (NT_SUCCESS(status)) ConnectorDpc(Device);
    return status;
}

void ConnectorDestroy(BC250_DEVICE* Device)
{
    BC250_CONNECTOR_SNAPSHOT* old;
    KIRQL irql;
    (void)ConnectorStop(Device);
    KeAcquireSpinLock(&Device->Connector.Lock, &irql);
    old = Device->Connector.Snapshot;
    Device->Connector.Snapshot = NULL;
    KeReleaseSpinLock(&Device->Connector.Lock, irql);
    ConnectorSnapshotRelease(old);
    if (Device->Connector.WorkItem) {
        IoFreeWorkItem(Device->Connector.WorkItem);
        Device->Connector.WorkItem = NULL;
    }
}
