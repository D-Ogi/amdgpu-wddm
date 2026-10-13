// Discovery is passive and generation-guarded. Windows callbacks run without the
// operation mutex: IndicateChildStatus can cause another VidPN transaction.
#include "bc250kmd.h"
#include "connector.h"
#include "../shim/bc250_dp_train.h"

#define CONNECTOR_TAG 'cC2B'
#define CONNECTOR_DEBOUNCE_100NS (-500000LL) // one 50 ms settling interval
#define CONNECTOR_DISCOVERY_US 250000ul

static LONG ReadAtomic(volatile LONG* Value)
{
    return InterlockedCompareExchange(Value, 0, 0);
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
    status = ConnectorLinkDisable(device, generation);
    GuardLog("connector: disable generation %lu status %08lX", generation, (ULONG)status);
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
        c->DiscoveryIo.StalledUs = 0;
        InterlockedIncrement(&c->Discoveries);
        status = Bc250SinkDiscover(&c->DiscoveryIo, &guard, 300000ul, 4096ul, 2160ul, &next->Sink);
        GuardLog("connector: discovery generation %lu kind %u phase %u edid %lu targets %lu status %08lX cleanup %08lX",
            generation, next->Sink.Kind, next->Sink.Phase, next->Sink.EdidLength,
            next->Sink.TargetCount, (ULONG)status, (ULONG)next->Sink.CleanupStatus);
        if (!NT_SUCCESS(status)) goto done;
        status = ConnectorLinkPrepare(device, &next->Sink, generation);
        if (!NT_SUCCESS(status)) goto done;
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
    c->StartStatus=STATUS_NOT_SUPPORTED;
    if (!Device->FullWddm) {
        // Display-only does not own a hardware connector.
        return c->LastStatus=c->StartStatus=STATUS_SUCCESS;
    }
    if (ConnectorNeedsInherited(Device)) {
        status=ConnectorStartInherited(Device);
        c->LastStatus=c->StartStatus=status;
        if (!NT_SUCCESS(status)) InterlockedIncrement(&c->Refusals);
        return status;
    }
    if (!c->WorkItem) c->WorkItem = IoAllocateWorkItem(Device->PhysicalDeviceObject);
    if (!c->WorkItem) {
        c->LastStatus=c->StartStatus=STATUS_INSUFFICIENT_RESOURCES;
        GuardLog("connector: start work-item allocation failed");return c->StartStatus;
    }
    generation = (ULONG)InterlockedIncrement(&c->Generation);
    snapshot = AllocateSnapshot(generation);
    if (!snapshot) {
        c->LastStatus=c->StartStatus=STATUS_INSUFFICIENT_RESOURCES;
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
        (void)SynchronizeHpd(Device, FALSE);
        InterlockedExchange(&c->Enabled, 0);
        InterlockedExchange(&c->Running, 0);
        (void)ConnectorLinkRelease(Device);
        InterlockedIncrement(&c->Refusals);
    }
    c->LastStatus = c->StartStatus = status;
    ConnectorOperationRelease(Device);
    ConnectorSnapshotRelease(snapshot);
    GuardLog("connector: start generation %lu status %08lX inherited stream preserved", generation, (ULONG)status);
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
    GuardLog("connector: stop status %08lX drained", (ULONG)status);
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
