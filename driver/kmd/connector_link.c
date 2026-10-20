// One passive owner for DCN201's physical pipe and connector. Discovery publishes
// immutable capabilities; only a pinned VidPN commit changes timing. No ATOM or
// DML work runs while a spin lock or FAST_MUTEX raises IRQL.
#include "bc250kmd.h"
#include "atom_firmware.h"
#include "hdmi_link.h"
#include "dcn_translate.h"
#include "connector_surface.h"
#include "plane_format.h"
#include "dpaux.h"
#include "../shim/bc250_link_atom.h"
#include "../shim/bc250_link_regs.h"
#include "../shim/bc250_hdmi_link.h"
#include "../shim/bc250_dcn201_pipe.h"
#include "../shim/bc250_optc.h"
#include "../shim/bc250_dp_train.h"
#include "../shim/bc250_dp_phy.h"
#include "../shim/bc250_dp_stream.h"
#include "../shim/dpcd_constants.inc"

#define LINK_TAG 'kL2B'
#define LINK_WORK_TAG 'wL2B'
#define LINK_DEADLINE_US 2000000ull
#define LINK_CLEANUP_US 300000ull

typedef struct LINK_MODE {
    BC250_TARGET_TIMING Timing;
    BC250_DML201_PLAN Dml;
    BC250_SCALER_PLAN Scaler;
    BC250_DCN201_CLOCKS Clocks;
    ULONG Protocol, Pitch, Lanes, Rate;
    BOOLEAN Valid, Inherited;
} LINK_MODE;

typedef struct CONNECTOR_LINK {
    BC250_DEVICE* Device;
    PKTHREAD Owner;
    ULONG Generation, Protocol, PixelKhz, Lanes, Rate;
    unsigned CrystalKhz;
    ULONG Attempts, Completed, Refused, Phase, Step, AtomValid;
    ULONG IoOffset, IoAccess;
    NTSTATUS IoStatus;
    BC250_SCALER_RESULT ScalerResult;
    BC250_CONNECTOR_TARGET_DIAG Target;
    BC250_CONNECTOR_FAILURE_DIAG Failure;
    BC250_CONNECTOR_FAILURE_DIAG TrainingCleanup;
    BC250_CONNECTOR_ATOM_WRITE_DIAG AtomWriteDiag;
    BOOLEAN RecoveryVisible, RecoveryModeActive;
    ULONGLONG Deadline;
    BOOLEAN Cleanup, Active, Inherited, Suspended, NeedsDisable, ClockSession;
    BOOLEAN Keep;   // BD-195: KeepLog breadcrumbs for this operation (GuardKeepActive at Begin)
    ULONG KeptSteps;  // the steps of this operation already on the disk, one bit per BC250_CONNECTOR_STEP
    BC250_ATOM_FIRMWARE Firmware;
    BC250_LINK_ATOM Atom;
    BC250_ATOM_PLAN AtomPlan;
    BC250_DISP_IO Io;
    BC250_AUX_SESSION Aux;
    BC250_HDMI_LINK Hdmi;
    BC250_DP_TRAIN_RESULT Training;
    LINK_MODE Current, Next, Previous;
    D3DKMDT_VIDEO_SIGNAL_INFO Signal;
    UCHAR Caps[16];
    void *AtomWorkspace, *DmlWorkspace;
} CONNECTOR_LINK;

// A boundary records the original error before any cleanup can change Last.
#define LINK_CALL(c,step,expression) do { LinkStep(c,step,FALSE); \
    status=(expression);if(!NT_SUCCESS(status)) {SaveFailure(c,status);return status;} } while(0)
#define LINK_ATOM(c,step,expression) do { BC250_ATOM_STATUS atomStatus; \
    LinkStep(c,step,TRUE);atomStatus=(expression);(c)->Atom.Last.Status=atomStatus; \
    if(atomStatus) {SaveFailure(c,STATUS_IO_DEVICE_ERROR);return STATUS_IO_DEVICE_ERROR;} } while(0)

static ULONG Protocol(const BC250_SINK* Sink);
static ULONGLONG LinkNow(void) { return KeQueryInterruptTime()/10ull; }
static int LinkOwned(void* Context)
{
    CONNECTOR_LINK* c=Context;
    return c->Owner==KeGetCurrentThread() && KeGetCurrentIrql()==PASSIVE_LEVEL &&
        c->Device->Mmio && c->Device->DcnWriteEnabled && LinkNow()<=c->Deadline &&
        (c->Cleanup || ConnectorGenerationCurrent(c->Device,c->Generation));
}
static void Begin(CONNECTOR_LINK* c,ULONG Generation,BOOLEAN Cleanup)
{
    c->Owner=KeGetCurrentThread();c->Generation=Generation;c->Cleanup=Cleanup;
    c->Deadline=LinkNow()+(Cleanup?LINK_CLEANUP_US:LINK_DEADLINE_US);
    c->Io.StalledUs=0;c->Io.BudgetUs=(ULONG)(Cleanup?LINK_CLEANUP_US:LINK_DEADLINE_US);
    c->Keep=GuardKeepActive();c->KeptSteps=0;
}
static void End(CONNECTOR_LINK* c) { c->Owner=NULL;c->Cleanup=FALSE; }
static NTSTATUS ClockSessionEnter(CONNECTOR_LINK* c)
{
    ULONGLONG now=LinkNow(),remaining;NTSTATUS status;
    if(!LinkOwned(c) || now>=c->Deadline)return STATUS_DEVICE_NOT_READY;
    remaining=c->Deadline-now;
    status=PspSessionAcquire(c->Device,(ULONG)remaining);
    if(status!=STATUS_SUCCESS)return status==STATUS_TIMEOUT?STATUS_DEVICE_BUSY:status;
    if(!LinkOwned(c) || !ConnectorGenerationCurrent(c->Device,c->Generation) ||
       !c->Device->Psp || !c->Device->MmioPspEnabled || c->Device->GpuStopUnconfirmed) {
        PspSessionRelease(c->Device);return STATUS_DEVICE_NOT_READY;
    }
    c->ClockSession=TRUE;return STATUS_SUCCESS;
}
static void ClockSessionLeave(CONNECTOR_LINK* c)
{
    if(c->ClockSession) {c->ClockSession=FALSE;PspSessionRelease(c->Device);}
}
static void LinkStep(CONNECTOR_LINK* c,ULONG Step,BOOLEAN Atom)
{
    c->Step=Step;c->AtomValid=Atom;c->IoOffset=0;c->IoAccess=0;c->IoStatus=STATUS_SUCCESS;
    RtlZeroMemory(&c->Atom.Last,sizeof(c->Atom.Last));
    RtlZeroMemory(&c->AtomWriteDiag,sizeof(c->AtomWriteDiag));
    // BD-195 breadcrumb: the first entry of each step per operation goes to the disk before the step runs. Once per
    // step, because training alternates TRAIN and PHY and a flush per AUX transfer would eat the deadline.
    if(c->Keep && Step<32u && !(c->KeptSteps&(1ul<<Step))) {
        c->KeptSteps|=1ul<<Step;
        GuardLog("link: keep step %lu phase %lu generation %lu cleanup %u",Step,c->Phase,c->Generation,
            (unsigned)c->Cleanup);
        GuardLogKeep();
    }
}
static void SaveFailure(CONNECTOR_LINK* c,NTSTATUS Status)
{
    BC250_CONNECTOR_FAILURE_DIAG* d=&c->Failure;
    if(d->Recorded)return;
    d->Recorded=1;d->Phase=c->Phase;d->Step=c->Step;d->Status=Status;
    d->AtomValid=c->AtomValid;d->Atom=c->Atom.Last;
    if(c->AtomValid)d->AtomWrite=c->AtomWriteDiag;
    d->IoOffset=c->IoOffset;d->IoStatus=c->IoStatus;d->IoReads=c->Io.Reads;d->IoWrites=c->Io.Writes;
    d->IoAccess=c->IoAccess;
    // A successful earlier scaler step is not the failing stream boundary.
    if(c->Step==BC250_LINK_SCALER) {
        d->LockStatus=c->ScalerResult.LockStatus;d->LockStatusValid=c->ScalerResult.LockStatusValid;
    }
    if(c->Step==BC250_LINK_HDMI_HANDOFF)HdmiLinkGetRestoreFailure(c->Device,&d->HdmiRestore);
    else if(c->Step==BC250_LINK_HDMI_RESTORE)d->HdmiRestore=c->Hdmi.RestoreFailure;
    if(!c->AtomValid && (c->Step==BC250_LINK_STREAM || c->Step==BC250_LINK_HDMI_MUTE)) {
        d->HdmiStream=c->Hdmi.StreamFailure;d->HdmiStage=c->Hdmi.StreamStage;
    }
    d->Target=c->Target;
}
static void RetainFailure(BC250_DEVICE* Device,const BC250_CONNECTOR_FAILURE_DIAG* Failure)
{
    KIRQL irql;
    KeAcquireSpinLock(&Device->Connector.Lock,&irql);
    if(!Device->Connector.FirstFailure.Recorded)Device->Connector.FirstFailure=*Failure;
    KeReleaseSpinLock(&Device->Connector.Lock,irql);
}
static void PreviousTargetRecord(CONNECTOR_LINK* c,BC250_CONNECTOR_TARGET_DIAG* d)
{
    const BC250_TARGET_TIMING* t=&c->Previous.Timing;
    RtlZeroMemory(d,sizeof(*d));
    if(!c->Previous.Valid)return;
    d->Recorded=1;d->Attempt=c->Attempts;d->Generation=c->Generation;d->Attached=1;
    d->Protocol=c->Previous.Protocol;d->Scaling=D3DKMDT_VPPS_IDENTITY;
    d->SourceWidth=d->Width=t->HActive;d->SourceHeight=d->Height=t->VActive;
    d->HTotal=t->HActive+t->HBlank;d->VTotal=t->VActive+t->VBlank;
    d->PixelKhz=t->PixelClockKhz;d->RefreshNumerator=(ULONGLONG)t->PixelClockKhz*1000;
    d->RefreshDenominator=(ULONGLONG)d->HTotal*d->VTotal;
}
static void RetainRecovery(CONNECTOR_LINK* c,const BC250_CONNECTOR_FAILURE_DIAG* Original,
    const BC250_CONNECTOR_TARGET_DIAG* PreviousTarget,BOOLEAN ReplayAttempted,NTSTATUS Status)
{
    BC250_CONNECTOR_RECOVERY_DIAG d={0};KIRQL irql;
    d.Recorded=1;d.Attempt=c->Attempts;d.PreviousValid=c->Previous.Valid;
    d.GenerationCurrent=ConnectorGenerationCurrent(c->Device,c->Generation);
    d.Active=c->Active;d.Status=Status;d.Original=*Original;d.Failure=c->Failure;
    d.PreviousTarget=*PreviousTarget;d.ReplayAttempted=ReplayAttempted;
    KeAcquireSpinLock(&c->Device->Connector.Lock,&irql);
    if(!c->Device->Connector.FirstRecovery.Recorded)c->Device->Connector.FirstRecovery=d;
    KeReleaseSpinLock(&c->Device->Connector.Lock,irql);
}
static void LogTarget(const BC250_CONNECTOR_TARGET_DIAG* d,const char* When)
{
    if(!d->Recorded)return;
    GuardLog("link: target %s try %lu gen %lu attach %lu proto %lu scaling %lu source %lux%lu",
        When,d->Attempt,d->Generation,d->Attached,d->Protocol,d->Scaling,d->SourceWidth,d->SourceHeight);
    GuardLog("link: timing %lux%lu total %lux%lu pixel %lu kHz refresh %llu/%llu Hz",
        d->Width,d->Height,d->HTotal,d->VTotal,d->PixelKhz,d->RefreshNumerator,d->RefreshDenominator);
}
static void LogFailure(const BC250_CONNECTOR_FAILURE_DIAG* d,const char* When)
{
    if(!d->Recorded)return;
    GuardLog("link: failure %s attempt %lu phase %lu step %lu status %08lX atom-valid %lu",
        When,d->Target.Attempt,d->Phase,d->Step,(ULONG)d->Status,d->AtomValid);
    if(d->AtomValid) {
        GuardLog("link: atom status %u table %u offset %u register-dw %X reads %u writes %u",
            (unsigned)d->Atom.Status,d->Atom.Table,d->Atom.Offset,d->Atom.Register,d->Atom.Reads,d->Atom.Writes);
        GuardLog("link: atom instructions %u delay %u us elapsed %llu us",
            d->Atom.Instructions,d->Atom.DelayUs,d->Atom.ElapsedUs);
    }
    GuardLog("link: io offset %lX status %08lX reads %lu writes %lu scaler-lock %08lX valid %lu",
        d->IoOffset,(ULONG)d->IoStatus,d->IoReads,d->IoWrites,d->LockStatus,d->LockStatusValid);
    if(d->HdmiRestore.Valid) {
        GuardLog("link: hdmi-restore op %u register-dw %X wanted %08X readback %08X mask %08X valid %u",
            d->HdmiRestore.Operation,d->HdmiRestore.Register,d->HdmiRestore.Wanted,
            d->HdmiRestore.Readback,d->HdmiRestore.Mask,d->HdmiRestore.ReadbackValid);
    }
    if(d->HdmiStream.Valid) {
        GuardLog("link: hdmi-stream stage %lu op %u register-dw %X wanted %08X readback %08X mask %08X valid %u",
            d->HdmiStage,d->HdmiStream.Operation,d->HdmiStream.Register,d->HdmiStream.Wanted,
            d->HdmiStream.Readback,d->HdmiStream.Mask,d->HdmiStream.ReadbackValid);
    }
    if(d->AtomWrite.Valid) {
        GuardLog("link: atom-write reason %lu allowed %lu reg-dw %lX before %08lX request %08lX effective %08lX",
            d->AtomWrite.Reason,d->AtomWrite.Allowed,d->AtomWrite.Register,
            d->AtomWrite.Before,d->AtomWrite.Requested,d->AtomWrite.Effective);
    }
    if(d->IoAccess)GuardLog("link: atom-access %s register-dw %lX status %08lX",
        d->IoAccess==2?"write":"read",d->IoOffset/4,(ULONG)d->IoStatus);
}
static void TargetRecord(CONNECTOR_LINK* c,const BC250_CONNECTOR_SNAPSHOT* Snapshot,
    const BC250_TARGET_TIMING* t,ULONG Width,ULONG Height,ULONG Scaling)
{
    BC250_CONNECTOR_TARGET_DIAG* d=&c->Target;KIRQL irql;
    RtlZeroMemory(d,sizeof(*d));d->Recorded=1;d->Attempt=c->Attempts;d->Generation=Snapshot->Generation;
    d->Attached=t!=NULL;d->Protocol=Protocol(&Snapshot->Sink);d->Scaling=Scaling;d->SourceWidth=Width;d->SourceHeight=Height;
    if(t) {
        d->Width=t->HActive;d->Height=t->VActive;d->HTotal=t->HActive+t->HBlank;d->VTotal=t->VActive+t->VBlank;
        d->PixelKhz=t->PixelClockKhz;d->RefreshNumerator=(ULONGLONG)t->PixelClockKhz*1000;
        d->RefreshDenominator=(ULONGLONG)d->HTotal*d->VTotal;
    }
    KeAcquireSpinLock(&c->Device->Connector.Lock,&irql);
    if(!c->Device->Connector.FirstAttempt.Recorded)c->Device->Connector.FirstAttempt=*d;
    KeReleaseSpinLock(&c->Device->Connector.Lock,irql);
    LogTarget(d,"request");
}
static NTSTATUS CloseAux(CONNECTOR_LINK* c)
{
    ULONGLONG deadline=c->Deadline;
    ULONG stalled=c->Io.StalledUs,budget=c->Io.BudgetUs;
    BOOLEAN cleanup=c->Cleanup;
    NTSTATUS status;
    // Cancellation revokes transactions with the sink, not restoration of the
    // AUX controller and GPIO state owned by this passive operation.
    c->Cleanup=TRUE;c->Deadline=LinkNow()+LINK_CLEANUP_US;
    c->Io.StalledUs=0;c->Io.BudgetUs=(ULONG)LINK_CLEANUP_US;
    status=Bc250AuxClose(&c->Aux);
    c->Cleanup=cleanup;c->Deadline=deadline;
    c->Io.StalledUs=stalled;c->Io.BudgetUs=budget;
    return status;
}
static int AtomAllowed(void* Context,uint32_t Dword,int Write)
{
    CONNECTOR_LINK* c=Context;
    int allowed=Bc250LinkRegisterAllowed(Dword,Write) ||
        (c->ClockSession && ConnectorGenerationCurrent(c->Device,c->Generation) &&
         Bc250LinkAtomScopedRegister(&c->Atom,Dword,Write));
    if(!allowed && NT_SUCCESS(c->IoStatus)) {
        c->IoOffset=Dword*4u;c->IoAccess=Write?2:1;c->IoStatus=STATUS_ACCESS_DENIED;
    }
    return allowed;
}
static int AtomRead(void* Context,uint32_t Dword,uint32_t* Value)
{
    CONNECTOR_LINK* c=Context;NTSTATUS status=STATUS_SUCCESS;
    if(!LinkOwned(c))status=STATUS_DEVICE_NOT_READY;
    else if(!AtomAllowed(c,Dword,0))status=STATUS_ACCESS_DENIED;
    if(!NT_SUCCESS(status)) {
        if(NT_SUCCESS(c->IoStatus)){c->IoOffset=Dword*4u;c->IoAccess=1;c->IoStatus=status;}
        return -1;
    }
    *Value=READ_REGISTER_ULONG(c->Device->Mmio+Dword);return 0;
}
static int AtomWrite(void* Context,uint32_t Dword,uint32_t Value)
{
    CONNECTOR_LINK* c=Context;uint32_t before=0,effective=Value,reason=0;
    NTSTATUS status=STATUS_SUCCESS;int allowed;
    if(!LinkOwned(c))status=STATUS_DEVICE_NOT_READY;
    else if(!AtomAllowed(c,Dword,1))status=STATUS_ACCESS_DENIED;
    if(!NT_SUCCESS(status)) {
        if(NT_SUCCESS(c->IoStatus)){c->IoOffset=Dword*4u;c->IoAccess=2;c->IoStatus=status;}
        return -1;
    }
    // The clock table can repeat the stopped-pipe disable sequence. Admit only
    // its named control changes; do not grant arbitrary timing/enable writes.
    if(Bc250LinkRegisterWriteNeedsBefore(Dword)) {
        if(AtomRead(c,Dword,&before))return -1;
        allowed=Bc250LinkRegisterPrepareWrite(Dword,before,Value,&effective,&reason);
        // Retain the first normalization, or the refusal that stops this step.
        // A zero effective value in a refused record is not a hardware write.
        if((reason || !allowed) && (!c->AtomWriteDiag.Valid || !allowed)) {
            BC250_CONNECTOR_ATOM_WRITE_DIAG* d=&c->AtomWriteDiag;
            d->Valid=1;d->Allowed=allowed!=0;d->Reason=reason;d->Register=Dword;
            d->Before=before;d->Requested=Value;d->Effective=allowed?effective:0;
        }
        if(!allowed) {
            if(NT_SUCCESS(c->IoStatus)){c->IoOffset=Dword*4u;c->IoAccess=2;c->IoStatus=STATUS_ACCESS_DENIED;}
            return -1;
        }
    }
    WRITE_REGISTER_ULONG(c->Device->Mmio+Dword,effective);return 0;
}
static uint64_t AtomNow(void* Context) { UNREFERENCED_PARAMETER(Context);return LinkNow(); }
static int LinkDelay(void* Context,uint32_t Us)
{
    LARGE_INTEGER delay;
    if(!LinkOwned(Context) || Us>100000u)return -1;
    if(Us<=1000u)KeStallExecutionProcessor(Us);
    else {delay.QuadPart=-(LONGLONG)Us*10;(void)KeDelayExecutionThread(KernelMode,FALSE,&delay);}
    return LinkOwned(Context)?0:-1;
}
static long LinkRead(void* Context,unsigned long Offset,unsigned long* Value)
{
    CONNECTOR_LINK* c=Context;NTSTATUS status=STATUS_SUCCESS;
    if(!LinkOwned(c))status=STATUS_DEVICE_NOT_READY;
    else if(!Bc250DispReadAllowed(Offset))status=STATUS_ACCESS_DENIED;
    else *Value=READ_REGISTER_ULONG(c->Device->Mmio+Offset/4u);
    // A helper can read terminal diagnostics after a failed access. Retain
    // the first failing callback within this boundary, not that later read.
    if(NT_SUCCESS(c->IoStatus)) {c->IoOffset=Offset;c->IoStatus=status;}
    return status;
}
static long LinkWrite(void* Context,unsigned long Offset,unsigned long Value)
{
    CONNECTOR_LINK* c=Context;NTSTATUS status=STATUS_SUCCESS;
    if(!LinkOwned(c))status=STATUS_DEVICE_NOT_READY;
    else if(!Bc250DispWriteAllowed(Offset))status=STATUS_ACCESS_DENIED;
    else WRITE_REGISTER_ULONG(c->Device->Mmio+Offset/4u,Value);
    if(NT_SUCCESS(c->IoStatus)) {c->IoOffset=Offset;c->IoStatus=status;}
    return status;
}
static void LinkStall(void* Context,unsigned long Us)
{ if(LinkOwned(Context) && Us<=2000u)KeStallExecutionProcessor(Us); }
static ULONG Protocol(const BC250_SINK* Sink)
{ return Sink->Kind==BC250_SINK_DP?BC250_LINK_DP:Sink->Kind==BC250_SINK_HDMI?BC250_LINK_HDMI:BC250_LINK_DVI; }
static void SignalFor(const BC250_TARGET_TIMING* t,D3DKMDT_VIDEO_SIGNAL_INFO* s)
{
    ULONG ht=t->HActive+t->HBlank,vt=t->VActive+t->VBlank;
    RtlZeroMemory(s,sizeof(*s));s->PixelRate=(ULONGLONG)t->PixelClockKhz*1000;
    s->HSyncFreq.Numerator=(UINT)s->PixelRate;s->HSyncFreq.Denominator=ht;
    s->VSyncFreq.Numerator=(UINT)s->PixelRate;s->VSyncFreq.Denominator=ht*vt;
    s->ActiveSize.cx=t->HActive;s->ActiveSize.cy=t->VActive;
    s->TotalSize.cx=ht;s->TotalSize.cy=vt;s->VideoStandard=D3DKMDT_VSS_OTHER;
    s->ScanLineOrdering=D3DDDI_VSSLO_PROGRESSIVE;
}

static NTSTATUS Plan(CONNECTOR_LINK* c,const BC250_SINK* Sink,const BC250_TARGET_TIMING* t,LINK_MODE* m)
{
    BC250_DML201_INPUT input={0};XSTATE_SAVE state;NTSTATUS status;int result;
    BC250_OPTC_SYNC sync;
    RtlZeroMemory(m,sizeof(*m));
    if(!Bc250TargetTimingValid(t) || t->PixelClockKhz<25000 || t->PixelClockKhz>Sink->MaxPixelClockKhz ||
       t->PixelClockKhz>300000 || t->HActive>4096 || t->VActive>2160)return STATUS_NOT_SUPPORTED;
    m->Timing=*t;m->Pitch=DcnPrimaryPitch(t->HActive);m->Protocol=Protocol(Sink);
    m->Lanes=m->Protocol==BC250_LINK_DP?(Sink->Dpcd[DP_MAX_LANE_COUNT]&DP_MAX_LANE_COUNT_MASK):4u;
    if(!m->Pitch || (ULONGLONG)m->Pitch*t->VActive>c->Device->RuntimeFramebufferLength)
        return STATUS_GRAPHICS_INVALID_STRIDE;
    status=Bc250Dcn201ReadClocks(&c->Io,c->CrystalKhz,&m->Clocks);if(!NT_SUCCESS(status))return status;
    input.HActive=t->HActive;input.HTotal=t->HActive+t->HBlank;
    input.HFrontPorch=t->HSyncOffset;input.HSyncWidth=t->HSyncWidth;
    input.VActive=t->VActive;input.VTotal=t->VActive+t->VBlank;
    input.VFrontPorch=t->VSyncOffset;input.VSyncWidth=t->VSyncWidth;
    input.PixelClockKhz=t->PixelClockKhz;input.PitchPixels=m->Pitch/4u;
    input.Output=m->Protocol==BC250_LINK_DP?BC250_DML201_DP:BC250_DML201_HDMI;
    input.DpLanes=m->Lanes;input.CursorWidth=0; // WDDM advertises no hardware pointer.
    input.AudioSampleRateKhz=192; // maximum admitted endpoint rate, not a guessed active stream.
    input.DchubRefClockKhz=m->Clocks.DchubRefClockKhz;input.DentistVcoKhz=m->Clocks.DentistVcoKhz;
    status=KeSaveExtendedProcessorState(XSTATE_MASK_LEGACY,&state);if(!NT_SUCCESS(status))return status;
    result=Bc250Dml201Plan(&input,c->DmlWorkspace,Bc250Dml201WorkspaceSize(),&m->Dml);
    KeRestoreExtendedProcessorState(&state);
    if(result || !m->Dml.Valid)return STATUS_NOT_SUPPORTED;
    sync.VReady=m->Dml.VReady;sync.VStartup=m->Dml.VStartup;
    sync.VUpdateOffset=m->Dml.VUpdateOffset;sync.VUpdateWidth=m->Dml.VUpdateWidth;
    if(!Bc250OptcTimingValid(t,&sync))return STATUS_NOT_SUPPORTED;
    status=Bc250ScalerPlan(t->HActive,t->VActive,t->HActive,t->VActive,BC250_SCALING_IDENTITY,&m->Scaler);
    if(NT_SUCCESS(status))m->Valid=TRUE;return status;
}

static void LogOpenDiagnostic(const BC250_CONNECTOR_OPEN_DIAG* d,const char* When)
{
    static const char* const steps[]={"ready","framebuffer","inherited-pipe","signal","context-pool",
        "atom-pool","dml-pool","firmware","identity","atom-validate","reference-clock"};
    if(!d->Recorded)return;
    GuardLog("link: open %s gen %lu step %lu %s status %08lX atom %lu ready %lX",
        When,d->Generation,d->Step,d->Step<RTL_NUMBER_OF(steps)?steps[d->Step]:"invalid",
        (ULONG)d->Status,d->AtomReason,d->Readiness);
    GuardLog("connector-link: open pipe support %lX status %08lX valid %lX bottom %08lX control %08lX",
        d->Support,(ULONG)d->PipeStatus,d->PipeValid,d->Bottom,d->Control);
}

void ConnectorLinkLogOpen(BC250_DEVICE* Device)
{
    BC250_CONNECTOR_OPEN_DIAG open;
    BC250_CONNECTOR_TARGET_DIAG attempt;
    BC250_CONNECTOR_FAILURE_DIAG failure;
    BC250_CONNECTOR_RECOVERY_DIAG recovery;KIRQL irql;
    // This is also an on-demand summary. Copy retained scalar diagnostics
    // coherently, without waiting for a long hardware operation or reading MMIO.
    KeAcquireSpinLock(&Device->Connector.Lock,&irql);
    open=Device->Connector.FirstOpen;attempt=Device->Connector.FirstAttempt;
    failure=Device->Connector.FirstFailure;recovery=Device->Connector.FirstRecovery;
    KeReleaseSpinLock(&Device->Connector.Lock,irql);
    LogOpenDiagnostic(&open,"retained");LogTarget(&attempt,"first-retained");
    LogFailure(&failure,"first-retained");
    if(failure.Recorded)LogTarget(&failure.Target,"failed-retained");
    if(recovery.Recorded) {
        GuardLog("link: recovery retained attempt %lu status %08lX previous %lu generation-current %lu active %lu",
            recovery.Attempt,(ULONG)recovery.Status,recovery.PreviousValid,recovery.GenerationCurrent,recovery.Active);
        LogTarget(&recovery.Original.Target,"recovery-original");
        GuardLog("link: recovery replay-attempted %lu",recovery.ReplayAttempted);
        LogTarget(&recovery.PreviousTarget,"recovery-previous");
        LogFailure(&recovery.Original,"recovery-original");
        LogFailure(&recovery.Failure,"recovery-retained");
    }
}

static NTSTATUS OpenResult(BC250_DEVICE* Device,ULONG Generation,ULONG Step,NTSTATUS Status,BC250_ATOM_STATUS Reason)
{
    BC250_CONNECTOR_OPEN_DIAG d={0};KIRQL irql;
    d.Recorded=1;d.Generation=Generation;d.Step=Step;d.Status=Status;d.AtomReason=(ULONG)Reason;
    d.Readiness=(Device->RuntimeFramebuffer?1ul:0ul)|(Device->Modeset.InheritedSupport?2ul:0ul)|
        (Device->InheritedSignalValid?4ul:0ul);
    d.Support=Device->Modeset.InheritedSupport;d.PipeStatus=Device->Modeset.InheritedPipeStatus;
    d.PipeValid=Device->Modeset.InheritedPipe.Valid;d.Bottom=Device->Modeset.InheritedPipe.MpccBottom;
    d.Control=Device->Modeset.InheritedPipe.MpccControl;
    // Startup owns this record before worker publication. Later hotplug attempts
    // log their own result but cannot erase the first admission before Stop.
    KeAcquireSpinLock(&Device->Connector.Lock,&irql);
    if(!Device->Connector.FirstOpen.Recorded)Device->Connector.FirstOpen=d;
    KeReleaseSpinLock(&Device->Connector.Lock,irql);
    LogOpenDiagnostic(&d,"result");
    return Status;
}

static NTSTATUS OpenContext(BC250_DEVICE* Device,ULONG Generation,CONNECTOR_LINK** Out)
{
    CONNECTOR_LINK* c;NTSTATUS status=STATUS_INSUFFICIENT_RESOURCES;BC250_ATOM_STATUS reason=BC250_ATOM_OK;
    ULONG step=BC250_OPEN_CONTEXT_POOL;
    *Out=NULL;
    if(!Device->RuntimeFramebuffer)
        return OpenResult(Device,Generation,BC250_OPEN_FRAMEBUFFER,STATUS_NOT_SUPPORTED,reason);
    if(!Device->Modeset.InheritedSupport)
        return OpenResult(Device,Generation,BC250_OPEN_INHERITED_PIPE,STATUS_NOT_SUPPORTED,reason);
    if(!Device->InheritedSignalValid)
        return OpenResult(Device,Generation,BC250_OPEN_SIGNAL,STATUS_NOT_SUPPORTED,reason);
    c=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*c),LINK_TAG);
    if(!c)return OpenResult(Device,Generation,step,status,reason);
    c->Device=Device;c->Inherited=TRUE;
    step=BC250_OPEN_ATOM_POOL;
    c->AtomWorkspace=ExAllocatePool2(POOL_FLAG_NON_PAGED,Bc250AtomContextSize(),LINK_WORK_TAG);
    if(!c->AtomWorkspace)goto failed;
    step=BC250_OPEN_DML_POOL;
    c->DmlWorkspace=ExAllocatePool2(POOL_FLAG_NON_PAGED,Bc250Dml201WorkspaceSize(),LINK_WORK_TAG);
    if(!c->DmlWorkspace)goto failed;
    step=BC250_OPEN_FIRMWARE;
    status=AtomFirmwareCapture(Device,&c->Firmware,&reason);
    if(!NT_SUCCESS(status))goto failed;
    step=BC250_OPEN_IDENTITY;
    if(c->Firmware.Identity.VendorId!=0x1002 || c->Firmware.Identity.DeviceId!=0x13fe) {
        status=STATUS_NOT_SUPPORTED;goto failed;
    }
    c->Io.Context=c;c->Io.Read=LinkRead;c->Io.Write=LinkWrite;c->Io.Stall=LinkStall;
    c->Atom.Io.Context=c;c->Atom.Io.Owned=LinkOwned;c->Atom.Io.Allowed=AtomAllowed;
    c->Atom.Io.Read=AtomRead;c->Atom.Io.Write=AtomWrite;c->Atom.Io.DelayUs=LinkDelay;c->Atom.Io.NowUs=AtomNow;
    c->Atom.Image=&c->Firmware.Image;c->Atom.Plan=&c->AtomPlan;
    c->Atom.Workspace=c->AtomWorkspace;c->Atom.WorkspaceSize=Bc250AtomContextSize();
    step=BC250_OPEN_ATOM_VALIDATE;
    reason=Bc250LinkAtomValidate(&c->Atom);
    if(reason) {status=STATUS_NOT_SUPPORTED;goto failed;}
    step=BC250_OPEN_REFERENCE_CLOCK;
    reason=Bc250LinkAtomReferenceClock(&c->Firmware.Image,&c->CrystalKhz);
    if(reason) {status=STATUS_NOT_SUPPORTED;goto failed;}
    Begin(c,Generation,FALSE);*Out=c;
    return OpenResult(Device,Generation,BC250_OPEN_READY,STATUS_SUCCESS,BC250_ATOM_OK);
failed:
    AtomFirmwareRelease(&c->Firmware);
    if(c->AtomWorkspace)ExFreePoolWithTag(c->AtomWorkspace,LINK_WORK_TAG);
    if(c->DmlWorkspace)ExFreePoolWithTag(c->DmlWorkspace,LINK_WORK_TAG);
    ExFreePoolWithTag(c,LINK_TAG);return OpenResult(Device,Generation,step,status,reason);
}

NTSTATUS ConnectorLinkPrepare(BC250_DEVICE* Device,BC250_SINK* Sink,ULONG Generation)
{
    CONNECTOR_LINK* c=Device->ConnectorLink;NTSTATUS status=STATUS_SUCCESS;ULONG i,count=0;
    BOOLEAN created=c==NULL;
    if(!Sink || Sink->Kind==BC250_SINK_NONE || Sink->TargetCount>BC250_EDID_MAX_MODES)
        return STATUS_INVALID_PARAMETER;
    // This attempt owns the terminal-cleanup result from here on: no earlier
    // attempt's restore failure, and no absence of one, is left to be read as
    // this attempt's (BD-169).
    Device->ConnectorLinkCleanup=STATUS_SUCCESS;
    GuardLog("connector-link: sink generation %lu kind %u adapter %u max %lu kHz limit %lu kHz",
        Generation,(unsigned)Sink->Kind,(unsigned)Sink->Adapter.type,
        (ULONG)Sink->Adapter.max_tmds_khz,(ULONG)Sink->MaxPixelClockKhz);
    if(!c) {status=OpenContext(Device,Generation,&c);if(!NT_SUCCESS(status))return status;Device->ConnectorLink=c;}
    Begin(c,Generation,FALSE);
    if(c->Aux.Open) {status=CloseAux(c);if(!NT_SUCCESS(status))goto done;}
    for(i=0;i<Sink->TargetCount;++i) {
        status=Plan(c,Sink,&Sink->Targets[i],&c->Next);
        if(NT_SUCCESS(status))Sink->Targets[count++]=Sink->Targets[i];
        else GuardLog("connector-link: target %lux%lu %lu kHz refused %08lX",Sink->Targets[i].HActive,
            Sink->Targets[i].VActive,Sink->Targets[i].PixelClockKhz,(ULONG)status);
        if(!LinkOwned(c)) {status=STATUS_RETRY;goto done;}
    }
    Sink->TargetCount=count;if(!count) {status=STATUS_NOT_SUPPORTED;goto done;}
    if(created) {
        BC250_TARGET_TIMING inherited;
        // Recoverable timing comes from the live OTG, not an EDID timing with
        // similar dimensions. Reject before mutation if no complete plan exists.
        LinkStep(c,BC250_LINK_INHERITED_PLAN,FALSE);
        status=Bc250OptcReadTiming(&c->Io,(ULONG)(Device->InheritedSignal.PixelRate/1000),&inherited);
        if(!NT_SUCCESS(status))goto done;
        if(inherited.HActive!=Device->InheritedSignal.ActiveSize.cx ||
           inherited.VActive!=Device->InheritedSignal.ActiveSize.cy ||
           inherited.HActive+inherited.HBlank!=Device->InheritedSignal.TotalSize.cx ||
           inherited.VActive+inherited.VBlank!=Device->InheritedSignal.TotalSize.cy ||
           Device->Post.Width!=inherited.HActive || Device->Post.Height!=inherited.VActive ||
           Device->Post.Pitch!=DcnPrimaryPitch(inherited.HActive)) {
            status=STATUS_NOT_SUPPORTED;goto done;
        }
        status=Plan(c,Sink,&inherited,&c->Current);if(!NT_SUCCESS(status))goto done;
        c->Current.Inherited=TRUE;
        c->Protocol=Protocol(Sink);c->PixelKhz=(ULONG)(Device->InheritedSignal.PixelRate/1000);
        c->Lanes=4;c->Signal=Device->InheritedSignal;
        if(c->Protocol==BC250_LINK_DP) {
            UCHAR settings[2]={0};NTSTATUS close;
            status=Bc250AuxOpenForDetection(&c->Aux,&c->Io);
            if(NT_SUCCESS(status))status=Bc250AuxDpcdRead(&c->Aux,DP_LINK_BW_SET,settings,sizeof(settings));
            close=CloseAux(c);if(NT_SUCCESS(status))status=close;
            if(!NT_SUCCESS(status))goto done;
            c->Rate=settings[0];c->Lanes=settings[1]&DP_MAX_LANE_COUNT_MASK;
            if((c->Rate!=6 && c->Rate!=10) || (c->Lanes!=1 && c->Lanes!=2 && c->Lanes!=4)) {
                status=STATUS_NOT_SUPPORTED;goto done;
            }
        }
        c->Current.Rate=c->Rate;c->Current.Lanes=c->Lanes;
        c->Active=TRUE;c->NeedsDisable=TRUE;RtlCopyMemory(c->Caps,Sink->Dpcd,sizeof(c->Caps));
    }
    status=STATUS_SUCCESS;
done:
    GuardLog("connector-link: prepare generation %lu targets %lu status %08lX",Generation,count,(ULONG)status);
    End(c);
    // A retry must re-run cold admission. A half-prepared context must not
    // bypass the inherited recovery contract on the next discovery. The release
    // is terminal: it frees the context and clears Device->ConnectorLink, so the
    // caller's own release can only answer SUCCESS afterwards. Its result is
    // therefore kept on the device, because a failed AUX restore leaves the
    // session open (dpaux.c) and that obligation outlives this context
    // (BD-169 review). The retry contract is unchanged: the release still runs.
    if(created && !NT_SUCCESS(status)) {
        Device->ConnectorLinkCleanup=ConnectorLinkRelease(Device);
        if(!NT_SUCCESS(Device->ConnectorLinkCleanup))
            GuardLog("connector-link: prepare cleanup %08lX outlives this context",
                (ULONG)Device->ConnectorLinkCleanup);
    }
    return status;
}

static int TrainRead(void* Context,uint32_t Address,uint8_t* Data,unsigned Length)
{
    CONNECTOR_LINK* c=Context;NTSTATUS status;
    LinkStep(c,BC250_LINK_TRAIN,FALSE);status=Bc250AuxDpcdRead(&c->Aux,Address,Data,Length);
    if(!NT_SUCCESS(status))SaveFailure(c,status);return NT_SUCCESS(status)?0:-1;
}
static int TrainWrite(void* Context,uint32_t Address,const uint8_t* Data,unsigned Length)
{
    CONNECTOR_LINK* c=Context;ULONG got=0;NTSTATUS status;
    LinkStep(c,BC250_LINK_TRAIN,FALSE);
    status=Bc250AuxTransfer(&c->Aux,BC250_AUX_DP_WRITE,Address,(UCHAR*)Data,Length,&got);
    if(!NT_SUCCESS(status))SaveFailure(c,status);return NT_SUCCESS(status)?0:-1;
}
static int TrainEnable(void* Context,unsigned Rate,unsigned Lanes,int Enhanced)
{
    CONNECTOR_LINK* c=Context;NTSTATUS status;
    c->Rate=Rate;c->Lanes=Lanes;c->Protocol=BC250_LINK_DP;c->NeedsDisable=TRUE;
    LINK_CALL(c,BC250_LINK_PHY,Bc250DpPhyConfigure(&c->Io,Lanes,Enhanced));
    LINK_ATOM(c,BC250_LINK_STREAM,Bc250LinkAtomStream(&c->Atom,BC250_LINK_DP,c->PixelKhz,Lanes,Rate));
    LINK_ATOM(c,BC250_LINK_TX_ENABLE,Bc250LinkAtomTransmitter(&c->Atom,BC250_LINK_TX_ON,BC250_LINK_DP,Rate*27000u,Lanes,0,0));
    LINK_CALL(c,BC250_LINK_ROUTE,Bc250Dcn201Route(&c->Io,BC250_LINK_DP,1));
    return 0;
}
static int TrainPattern(void* Context,unsigned Pattern)
{
    CONNECTOR_LINK* c=Context;NTSTATUS status;
    LINK_CALL(c,BC250_LINK_PHY,Bc250DpPhyPattern(&c->Io,Pattern,(int)c->Training.Enhanced));return 0;
}
static int TrainDrive(void* Context,unsigned Swing,unsigned Pre)
{
    CONNECTOR_LINK* c=Context;
    LINK_ATOM(c,BC250_LINK_PHY,Bc250LinkAtomTransmitter(&c->Atom,BC250_LINK_TX_DRIVE,BC250_LINK_DP,
        c->Rate*27000u,c->Lanes,Swing,Pre));return 0;
}
static int TrainDisable(void* Context)
{
    CONNECTOR_LINK* c=Context;BOOLEAN cleanup=c->Cleanup;int result;
    NTSTATUS status;BC250_ATOM_STATUS atomStatus;
    BC250_CONNECTOR_FAILURE_DIAG original=c->Failure;
    // The train helper invokes this on generation cancellation too. The passive
    // operation still owns MMIO; cleanup may disable the old PHY, never new AUX.
    c->Cleanup=TRUE;
    RtlZeroMemory(&c->Failure,sizeof(c->Failure));
    LinkStep(c,BC250_LINK_PHY,FALSE);
    status=Bc250DpPhyDisableTraining(&c->Io);result=!NT_SUCCESS(status);
    if(result)SaveFailure(c,status);
    LinkStep(c,BC250_LINK_TX_DISABLE,TRUE);
    atomStatus=Bc250LinkAtomTransmitter(&c->Atom,BC250_LINK_TX_OFF,BC250_LINK_DP,c->Rate*27000u,c->Lanes,0,0);
    c->Atom.Last.Status=atomStatus;
    if(atomStatus) {SaveFailure(c,STATUS_IO_DEVICE_ERROR);result=1;}
    if(c->Failure.Recorded)c->TrainingCleanup=c->Failure;
    c->Failure=original;
    c->Cleanup=cleanup;if(!result)c->NeedsDisable=FALSE;return result?-1:0;
}

static NTSTATUS DisableOwned(CONNECTOR_LINK* c)
{
    NTSTATUS status;
    c->Phase=1;
    // Fail at the first boundary. Recovery gets a separate attempt and cannot
    // replace the failing ATOM result with a later successful TX_OFF.
    if(c->Aux.Open) {LINK_CALL(c,BC250_LINK_AUX_CLOSE,CloseAux(c));}
    LINK_CALL(c,BC250_LINK_PLANE_BLANK,Bc250Dcn201PlaneBlank(&c->Io,1));
    InterlockedExchange(&c->Device->RuntimePlaneBlank,1);
    LINK_CALL(c,BC250_LINK_VISIBILITY,DcnSetVisibility(c->Device,FALSE));
    if(c->Protocol==BC250_LINK_DP && c->NeedsDisable) {
        LINK_CALL(c,BC250_LINK_STREAM_BLANK,Bc250DpStreamBlank(&c->Io));
    }
    if(c->Hdmi.Dirty) {
        LINK_CALL(c,BC250_LINK_HDMI_RESTORE,
            Bc250HdmiLinkRestore(&c->Hdmi)?STATUS_DEVICE_DATA_ERROR:STATUS_SUCCESS);
    }
    if(c->NeedsDisable) {
        LINK_ATOM(c,BC250_LINK_TX_DISABLE,Bc250LinkAtomTransmitter(&c->Atom,BC250_LINK_TX_OFF,c->Protocol,
            c->Protocol==BC250_LINK_DP?c->Rate*27000u:c->PixelKhz,c->Lanes,0,0));
        c->NeedsDisable=FALSE;
    }
    c->Active=FALSE;return STATUS_SUCCESS;
}

NTSTATUS ConnectorLinkDisable(BC250_DEVICE* Device,ULONG Generation)
{
    CONNECTOR_LINK* c=Device->ConnectorLink;ULONG primary;NTSTATUS status,finish;KIRQL irql;
    if(!c)return STATUS_DEVICE_NOT_READY;
    DpAudioStop(Device);
    if(!DpAudioQuiesced(Device))return STATUS_DEVICE_NOT_READY;
    // .35's journal belongs to the inherited timing only. Discharge it before
    // this owner changes any clocks; otherwise stop could replay it over HDMI.
    if(!HdmiLinkStop(Device))return STATUS_DEVICE_DATA_ERROR;
    if(!WddmPrimaryExclusiveBegin(Device,BC250_WRITER_LINK_DISABLE,&primary))return STATUS_DEVICE_BUSY;
    // I3 before the flag below: later writers find this bridge current with HUBP blank.
    DcnLinkSurfaceAdopt(Device);
    Begin(c,Generation,TRUE);status=DisableOwned(c);End(c);
    // A detach ends the rollback scope. A later sink, even with the same mode,
    // must never receive the previous monitor's timing as error recovery.
    c->Current.Valid=FALSE;c->Previous.Valid=FALSE;
    InterlockedExchange(&Device->RuntimeDisplayChanged,1);
    InterlockedExchange(&Device->RuntimePostValid,0);Device->SourceVisible=FALSE;Device->ModeActive=FALSE;
    KeAcquireSpinLock(&Device->DpAudio.Lock,&irql);
    Device->DpAudio.PortId=0;Device->DpAudio.OsManufacturer=0;Device->DpAudio.OsProduct=0;
    KeReleaseSpinLock(&Device->DpAudio.Lock,irql);
    finish=WddmPrimaryExclusiveEnd(Device,primary,TRUE);
    if(!NT_SUCCESS(finish))status=finish;
    return status;
}

NTSTATUS ConnectorLinkCheck(BC250_DEVICE* Device,const BC250_SINK* Sink,ULONG Generation)
{
    CONNECTOR_LINK* c=Device->ConnectorLink;UCHAR link[6];ULONG lane;NTSTATUS status,close;
    if(!c || Sink->Kind!=BC250_SINK_DP || !c->Active)return STATUS_RETRY;
    Begin(c,Generation,FALSE);
    if(c->Aux.Open) {status=CloseAux(c);if(!NT_SUCCESS(status)) {End(c);return status;}}
    status=Bc250AuxOpenForDetection(&c->Aux,&c->Io);
    if(NT_SUCCESS(status))status=Bc250AuxDpcdRead(&c->Aux,DP_LANE0_1_STATUS,link,sizeof(link));
    close=CloseAux(c);End(c);
    if(!NT_SUCCESS(close))return close;
    if(!NT_SUCCESS(status))return STATUS_RETRY;
    for(lane=0;lane<c->Lanes;++lane) {
        UCHAR nibble=(UCHAR)((link[lane/2]>>((lane&1u)*4u))&15u);
        if((nibble&(DP_LANE_CR_DONE|DP_LANE_CHANNEL_EQ_DONE|DP_LANE_SYMBOL_LOCKED))!=
            (DP_LANE_CR_DONE|DP_LANE_CHANNEL_EQ_DONE|DP_LANE_SYMBOL_LOCKED))return STATUS_RETRY;
    }
    return (link[2]&DP_INTERLANE_ALIGN_DONE)?STATUS_SUCCESS:STATUS_RETRY;
}

static NTSTATUS Apply(CONNECTOR_LINK* c,LINK_MODE* m,const UCHAR* Caps,const BC250_LINK_SURFACE* Restore)
{
    NTSTATUS status,close;BC250_OPTC_SYNC sync;
    BC250_DCN201_CLOCKS clocks;
    BC250_DP_TRAIN_IO train={c,LinkOwned,TrainRead,TrainWrite,TrainEnable,TrainPattern,TrainDrive,
        TrainDisable,AtomNow,LinkDelay};
    BC250_DEVICE* d=c->Device;
    if(c->Aux.Open)return STATUS_INVALID_DEVICE_STATE;
    c->Phase=2;
    LINK_CALL(c,BC250_LINK_OTG_DISABLE,Bc250OptcDisable(&c->Io));
    LINK_CALL(c,BC250_LINK_CLOCK_READ,Bc250Dcn201ReadClocks(&c->Io,c->CrystalKhz,&clocks));
    if(clocks.DchubRefClockKhz!=m->Clocks.DchubRefClockKhz ||
       clocks.DentistVcoKhz!=m->Clocks.DentistVcoKhz)return STATUS_DEVICE_DATA_ERROR;
    m->Clocks=clocks;
    LINK_CALL(c,BC250_LINK_PIPE,Bc250Dcn201PipeApply(&c->Io,&m->Dml,&m->Clocks));
    c->Phase=3;
    LINK_ATOM(c,BC250_LINK_PIXEL_CLOCK,Bc250LinkAtomClock(&c->Atom,m->Protocol,m->Timing.PixelClockKhz));
    sync.VReady=m->Dml.VReady;sync.VStartup=m->Dml.VStartup;
    sync.VUpdateOffset=m->Dml.VUpdateOffset;sync.VUpdateWidth=m->Dml.VUpdateWidth;
    LINK_CALL(c,BC250_LINK_TIMING,Bc250OptcProgram(&c->Io,&m->Timing,&sync,m->Protocol==BC250_LINK_DP));
    LINK_CALL(c,BC250_LINK_OUTPUT,Bc250Dcn201OutputProgram(&c->Io,&m->Dml));
    LINK_CALL(c,BC250_LINK_SCALER,Bc250ScalerProgramStopped(&c->Io,&m->Scaler,&c->ScalerResult));
    LINK_CALL(c,BC250_LINK_ROUTE,Bc250Dcn201Route(&c->Io,m->Protocol,1));
    c->Protocol=m->Protocol;c->PixelKhz=m->Timing.PixelClockKhz;c->Lanes=m->Lanes;c->Rate=m->Rate;
    c->Phase=4;
    if(m->Protocol==BC250_LINK_DP) {
        LINK_CALL(c,BC250_LINK_STREAM,Bc250DpStreamProgram(&c->Io,&m->Timing));
        LinkStep(c,BC250_LINK_AUX_OPEN,FALSE);
        status=Bc250AuxOpenForDetection(&c->Aux,&c->Io);
        if(!NT_SUCCESS(status))SaveFailure(c,status);
        if(NT_SUCCESS(status) && Bc250DpTrain(&train,Caps,m->Timing.PixelClockKhz,&c->Training)!=BC250_DP_TRAIN_OK)
        {
            status=STATUS_IO_DEVICE_ERROR;
            // Training may already have saved a failed I/O before disabling
            // the PHY. A training timeout has no ATOM failure provenance.
            LinkStep(c,BC250_LINK_TRAIN,FALSE);SaveFailure(c,status);
        }
        if(NT_SUCCESS(status))RtlZeroMemory(&c->Failure,sizeof(c->Failure));
        else LogFailure(&c->TrainingCleanup,"training-cleanup");
        LinkStep(c,BC250_LINK_AUX_CLOSE,FALSE);
        close=CloseAux(c);if(NT_SUCCESS(status))status=close;
        if(!NT_SUCCESS(status)) {SaveFailure(c,status);return status;}
        m->Rate=c->Training.Rate;m->Lanes=c->Training.Lanes;
    } else {
        LINK_ATOM(c,BC250_LINK_STREAM,Bc250LinkAtomStream(&c->Atom,m->Protocol,m->Timing.PixelClockKhz,4,0));
        if(m->Protocol==BC250_LINK_HDMI) {
            LINK_CALL(c,BC250_LINK_STREAM,Bc250HdmiStreamProgram(&c->Hdmi,&c->Atom.Io,
                m->Timing.PixelClockKhz,m->Timing.HActive,m->Timing.VActive,
                m->Timing.HActive+m->Timing.HBlank,m->Timing.VActive+m->Timing.VBlank)?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS);
        }
        c->NeedsDisable=TRUE;
        LINK_ATOM(c,BC250_LINK_TX_ENABLE,Bc250LinkAtomTransmitter(&c->Atom,BC250_LINK_TX_ON,m->Protocol,m->Timing.PixelClockKhz,4,0,0));
        // Firmware may select DVI during transmitter enable: DC reapplies setup.
        LINK_CALL(c,BC250_LINK_ROUTE,Bc250Dcn201Route(&c->Io,m->Protocol,1));
    }
    c->Phase=5;
    LINK_CALL(c,BC250_LINK_OTG_ENABLE,Bc250OptcEnable(&c->Io));
    d->RuntimePost=d->Post;d->RuntimePost.Width=m->Timing.HActive;d->RuntimePost.Height=m->Timing.VActive;
    d->RuntimePost.Pitch=m->Pitch;d->RuntimePost.ColorFormat=D3DDDIFMT_A8R8G8B8;
    d->RuntimePost.PhysicAddress=d->RuntimeFramebufferPhysical;d->RuntimePost.TargetId=BC250_CHILD_UID;
    // The reserved buffer was cleared at allocation. Do not erase old pixels
    // during a transaction that may have to return to this same surface.
    InterlockedExchange(&d->RuntimePostValid,Restore && m->Inherited?0:1);
    InterlockedExchange(&d->RuntimeDisplayChanged,Restore && m->Inherited?0:1);
    if(Restore) {
        LINK_CALL(c,BC250_LINK_SURFACE_APPLY,DcnLinkSurfaceRestore(d,Restore));
    } else {LINK_CALL(c,BC250_LINK_SURFACE_APPLY,DcnRuntimeSurface(d,TRUE));}
    if(m->Protocol==BC250_LINK_DP) {
        LINK_CALL(c,BC250_LINK_UNBLANK,Bc250DpStreamUnblank(&c->Io,m->Timing.PixelClockKhz,m->Rate));
    }
    LINK_CALL(c,BC250_LINK_UNBLANK,Bc250Dcn201OutputBlank(&c->Io,m->Timing.HActive,m->Timing.VActive,
        Restore && !Restore->Visible));
    if(m->Protocol==BC250_LINK_HDMI) {
        LINK_CALL(c,BC250_LINK_HDMI_MUTE,Bc250HdmiStreamMute(&c->Hdmi,0)?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS);
    }
    LINK_CALL(c,BC250_LINK_GENERATION,LinkOwned(c)?STATUS_SUCCESS:STATUS_RETRY);
    c->Phase=6;c->Active=TRUE;c->Inherited=m->Inherited;c->Current=*m;
    RtlCopyMemory(c->Caps,Caps,sizeof(c->Caps));SignalFor(&m->Timing,&c->Signal);
    InterlockedExchange(&d->Modeset.SourceWidth,(LONG)m->Timing.HActive);
    InterlockedExchange(&d->Modeset.SourceHeight,(LONG)m->Timing.VActive);
    d->Modeset.Scaling=BC250_SCALING_IDENTITY;
    d->SourceVisible=Restore?c->RecoveryVisible:TRUE;d->ModeActive=Restore?c->RecoveryModeActive:TRUE;
    GuardLog("connector-link: enabled generation %lu protocol %lu %lux%lu pitch %lu pixel %lu kHz lanes %lu rate %lu",
        c->Generation,m->Protocol,m->Timing.HActive,m->Timing.VActive,m->Pitch,m->Timing.PixelClockKhz,
        m->Lanes,m->Rate);
    return STATUS_SUCCESS;
}

NTSTATUS ConnectorLinkCommit(BC250_DEVICE* Device,const BC250_CONNECTOR_SNAPSHOT* Snapshot,
    const BC250_TARGET_TIMING* Timing,ULONG SourceWidth,ULONG SourceHeight,
    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling)
{
    CONNECTOR_LINK* c;ULONG i,primary=0;NTSTATUS status,finish=STATUS_SUCCESS,rollback=STATUS_NOT_SUPPORTED;KIRQL irql;
    BOOLEAN exclusive=FALSE,changed=FALSE;
    BC250_LINK_SURFACE previousSurface={0};
    BC250_CONNECTOR_FAILURE_DIAG originalFailure={0};
    BC250_CONNECTOR_TARGET_DIAG previousTarget={0};BOOLEAN replayAttempted=FALSE;
    if(!Snapshot || KeGetCurrentIrql()!=PASSIVE_LEVEL)return STATUS_INVALID_PARAMETER;
    ConnectorOperationAcquire(Device);c=Device->ConnectorLink;
    if(!c) {ConnectorOperationRelease(Device);return STATUS_DEVICE_NOT_READY;}
    Begin(c,Snapshot->Generation,FALSE);++c->Attempts;c->Phase=0;
    RtlZeroMemory(&c->Failure,sizeof(c->Failure));RtlZeroMemory(&c->ScalerResult,sizeof(c->ScalerResult));
    RtlZeroMemory(&c->TrainingCleanup,sizeof(c->TrainingCleanup));
    TargetRecord(c,Snapshot,Timing,SourceWidth,SourceHeight,(ULONG)Scaling);
    LinkStep(c,BC250_LINK_GENERATION,FALSE);
    if(!LinkOwned(c)) {status=STATUS_RETRY;goto done;}
    if(Timing) {
        if(SourceWidth!=Timing->HActive || SourceHeight!=Timing->VActive || Scaling!=D3DKMDT_VPPS_IDENTITY) {
            status=STATUS_GRAPHICS_INVALID_VIDPN;goto done;
        }
        for(i=0;i<Snapshot->Sink.TargetCount;++i)if(Bc250TargetTimingEqual(Timing,&Snapshot->Sink.Targets[i]))break;
        if(i==Snapshot->Sink.TargetCount) {status=STATUS_GRAPHICS_INVALID_VIDPN;goto done;}
        if(c->Active && c->Current.Valid && Bc250TargetTimingEqual(Timing,&c->Current.Timing)) {
            status=STATUS_SUCCESS;goto done;
        }
        LinkStep(c,BC250_LINK_PLAN,FALSE);
        status=Plan(c,&Snapshot->Sink,Timing,&c->Next);if(!NT_SUCCESS(status))goto done;
    }
    // An active old link must have a complete recovery plan before mutating it.
    if(c->Active && !c->Current.Valid) {status=STATUS_NOT_SUPPORTED;goto done;}
    if(!WddmPrimaryExclusiveBegin(Device,BC250_WRITER_LINK_COMMIT,&primary)) {status=STATUS_DEVICE_BUSY;goto done;}
    // I3 for the OS detach and a failed recovery, which leave RuntimeDisplayChanged set without an Apply.
    exclusive=TRUE;DcnLinkSurfaceAdopt(Device);
    c->Previous=c->Current;c->Previous.Valid=c->Active && c->Current.Valid;
    c->RecoveryVisible=Device->SourceVisible;c->RecoveryModeActive=Device->ModeActive;
    if(c->Previous.Valid) {
        BC250_LINK_SURFACE bridge={0};
        // Begin already retired the application's surface onto the reserved
        // bridge. Rollback needs its mode and visibility, never an application
        // address that an immediate deposit can release while this writer runs.
        LinkStep(c,BC250_LINK_SURFACE_CAPTURE,FALSE);
        status=DcnLinkSurfaceCapture(Device,c->Previous.Timing.HActive,c->Previous.Timing.VActive,&bridge);
        if(!NT_SUCCESS(status))goto done;
        previousSurface.Physical=(ULONGLONG)Device->RuntimeFramebufferPhysical.QuadPart;
        previousSurface.Pitch=c->Previous.Pitch;
        previousSurface.Width=c->Previous.Timing.HActive;
        previousSurface.Height=c->Previous.Timing.VActive;
        previousSurface.Format=BC250_PLANE_FORMAT_ARGB8888;
        // Hardware visibility and the OS logical SourceVisible can differ at
        // an inherited cold start. Restore each to its own saved value.
        previousSurface.Visible=bridge.Visible;
    }
    LinkStep(c,BC250_LINK_AUDIO,FALSE);
    status=ClockSessionEnter(c);if(!NT_SUCCESS(status))goto done;
    DpAudioStop(Device);
    if(!DpAudioQuiesced(Device)) {status=STATUS_DEVICE_NOT_READY;goto done;}
    // HDMI's inherited journal shares this exact primary transaction. Calling
    // its externally owning wrapper here would wait on our own odd sequence.
    LinkStep(c,BC250_LINK_HDMI_HANDOFF,FALSE);
    changed=TRUE;
    if(!HdmiLinkStopOwned(Device)) {status=STATUS_DEVICE_DATA_ERROR;goto recover;}
    InterlockedExchange(&Device->RuntimeDisplayChanged,1);InterlockedExchange(&Device->RuntimePostValid,0);
    status=DisableOwned(c);
    if(NT_SUCCESS(status) && Timing)status=Apply(c,&c->Next,Snapshot->Sink.Dpcd,NULL);
    else if(NT_SUCCESS(status)) {
        // An OS detach is a fallible transaction too. Do not discard its old
        // plan until hardware disable succeeds. Physical HPD detach uses the
        // separate ConnectorLinkDisable path and cannot restore an absent sink.
        c->Current.Valid=FALSE;c->Previous.Valid=FALSE;
        Device->SourceVisible=FALSE;Device->ModeActive=FALSE;
        KeAcquireSpinLock(&Device->DpAudio.Lock,&irql);
        Device->DpAudio.PortId=0;Device->DpAudio.OsManufacturer=0;Device->DpAudio.OsProduct=0;
        KeReleaseSpinLock(&Device->DpAudio.Lock,irql);
    }
recover:
    if(!NT_SUCCESS(status)) {
        SaveFailure(c,status);
        RetainFailure(Device,&c->Failure);
        LogFailure(&c->Failure,"original");originalFailure=c->Failure;
        PreviousTargetRecord(c,&previousTarget);
        RtlZeroMemory(&c->Failure,sizeof(c->Failure));
        Begin(c,Snapshot->Generation,TRUE);
        // A partial inherited-journal restore still belongs to the old mode.
        // Discharge it before replaying the previous mode, or stop could later
        // restore stale packet/route state over a successfully recovered link.
        LinkStep(c,BC250_LINK_HDMI_HANDOFF,FALSE);
        rollback=HdmiLinkStopOwned(Device)?STATUS_SUCCESS:STATUS_DEVICE_DATA_ERROR;
        if(!NT_SUCCESS(rollback))SaveFailure(c,rollback);
        else rollback=DisableOwned(c);
        if(NT_SUCCESS(rollback) && c->Previous.Valid && ConnectorGenerationCurrent(Device,Snapshot->Generation)) {
            // The original failure retains the failed requested mode. Label
            // callbacks below with the actual prior plan passed to Apply.
            c->Target=previousTarget;replayAttempted=TRUE;
            Begin(c,Snapshot->Generation,FALSE);rollback=Apply(c,&c->Previous,c->Caps,&previousSurface);
        } else if(NT_SUCCESS(rollback)) {
            GuardLog("link: recovery unavailable attempt %lu previous %u generation-current %u",
                c->Attempts,c->Previous.Valid,ConnectorGenerationCurrent(Device,Snapshot->Generation));
            rollback=STATUS_NOT_SUPPORTED;
        }
        LogFailure(&c->Failure,"recovery");
        if(!NT_SUCCESS(rollback)) {
            Begin(c,Snapshot->Generation,TRUE);(void)Bc250Dcn201PlaneBlank(&c->Io,1);
            InterlockedExchange(&Device->RuntimePlaneBlank,1);InterlockedExchange(&Device->RuntimePostValid,0);
            Device->SourceVisible=FALSE;Device->ModeActive=FALSE;c->Active=FALSE;
        }
        RetainRecovery(c,&originalFailure,&previousTarget,replayAttempted,rollback);
        GuardLog("link: attempt %lu original %08lX recovery %08lX active %u",
            c->Attempts,(ULONG)status,(ULONG)rollback,c->Active);
    }
done:
    if(!NT_SUCCESS(status) && !changed) {
        SaveFailure(c,status);
        RetainFailure(Device,&c->Failure);
        LogFailure(&c->Failure,"admission");
    }
    // Rollback restored the reserved bridge. Only End may select the latest
    // application surface, under the interrupt lock that also accepts deposits.
    ClockSessionLeave(c);
    if(exclusive) {
        finish=WddmPrimaryExclusiveEnd(Device,primary,changed && !NT_SUCCESS(rollback));
        if(!NT_SUCCESS(finish))status=finish;
    }
    if(NT_SUCCESS(status))++c->Completed;else ++c->Refused;
    GuardLog("connector-link: commit generation %lu attempts %lu completed %lu refused %lu active %u status %08lX",
        Snapshot->Generation,c->Attempts,c->Completed,c->Refused,c->Active,(ULONG)status);
    End(c);
    if(NT_SUCCESS(finish) && changed && c->Active && (NT_SUCCESS(status)||NT_SUCCESS(rollback)))DpAudioStart(Device);
    ConnectorOperationRelease(Device);return status;
}

NTSTATUS ConnectorLinkCurrentSignal(BC250_DEVICE* Device,D3DKMDT_VIDEO_SIGNAL_INFO* Signal)
{
    CONNECTOR_LINK* c;NTSTATUS status=STATUS_DEVICE_NOT_READY;
    if(!Signal)return STATUS_INVALID_PARAMETER;
    ConnectorOperationAcquire(Device);c=Device->ConnectorLink;
    if(c && c->Active && !c->Suspended && ConnectorGenerationCurrent(Device,c->Generation)) {
        *Signal=c->Signal;status=STATUS_SUCCESS;
    }
    ConnectorOperationRelease(Device);return status;
}

BOOLEAN ConnectorLinkNeedsPowerOn(BC250_DEVICE* Device,BOOLEAN* Active,BOOLEAN* Connected)
{
    CONNECTOR_LINK* c;BOOLEAN needs=FALSE;
    *Active=FALSE;*Connected=ConnectorIsConnected(Device);
    ConnectorOperationAcquire(Device);c=Device->ConnectorLink;
    if(c) {*Active=c->Active;needs=*Connected && !c->Active && !c->Suspended;}
    ConnectorOperationRelease(Device);return needs;
}

NTSTATUS ConnectorLinkSuspend(BC250_DEVICE* Device)
{
    CONNECTOR_LINK* c=Device->ConnectorLink;ULONG generation;NTSTATUS status,finish;
    if(!c)return STATUS_SUCCESS;
    DpAudioStop(Device);
    if(!DpAudioQuiesced(Device))return STATUS_DEVICE_NOT_READY;
    // A detached link is off and its plane blank: nothing to take or restore (BD-195, I6).
    if(!c->Active) {c->Suspended=TRUE;return STATUS_SUCCESS;}
    if(!WddmPrimaryExclusiveBegin(Device,BC250_WRITER_LINK_SUSPEND,&generation))return STATUS_DEVICE_BUSY;
    status=c->Inherited?DcnRestorePostDisplay(Device):DcnRuntimeSurface(Device,FALSE);
    if(NT_SUCCESS(status))status=DcnSetVisibility(Device,FALSE);
    if(NT_SUCCESS(status))Device->SourceVisible=FALSE;
    if(NT_SUCCESS(status))c->Suspended=TRUE;
    finish=WddmPrimaryExclusiveEnd(Device,generation,TRUE);
    return NT_SUCCESS(finish)?status:finish;
}

NTSTATUS ConnectorLinkResume(BC250_DEVICE* Device,ULONG Generation)
{
    CONNECTOR_LINK* c=Device->ConnectorLink;ULONG primary;NTSTATUS status,finish;
    if(!c)return STATUS_DEVICE_NOT_READY;
    if(c->Inherited) {c->Suspended=FALSE;return STATUS_SUCCESS;}
    // An inactive link stays off; the next commit, full or power-on, applies a mode (BD-195, I6).
    if(!c->Active) {c->Suspended=FALSE;return STATUS_SUCCESS;}
    if(!c->Current.Valid)return STATUS_DEVICE_NOT_READY;
    if(!WddmPrimaryExclusiveBegin(Device,BC250_WRITER_LINK_RESUME,&primary))return STATUS_DEVICE_BUSY;
    Begin(c,Generation,FALSE);c->Next=c->Current;
    status=ClockSessionEnter(c);
    if(!NT_SUCCESS(status)) {
        End(c);finish=WddmPrimaryExclusiveEnd(Device,primary,FALSE);
        return NT_SUCCESS(finish)?status:finish;
    }
    InterlockedExchange(&Device->RuntimePostValid,0);
    status=DisableOwned(c);if(NT_SUCCESS(status))status=Apply(c,&c->Next,c->Caps,NULL);
    if(NT_SUCCESS(status)) {
        // WddmResumeRetained requires a blank output. Windows restores source
        // visibility after the engines and callbacks become available again.
        status=DcnSetVisibility(Device,FALSE);Device->SourceVisible=FALSE;
    }
    if(NT_SUCCESS(status))c->Suspended=FALSE;
    else {
        Begin(c,Generation,TRUE);(void)DisableOwned(c);
        c->Active=FALSE;Device->SourceVisible=FALSE;InterlockedExchange(&Device->RuntimePostValid,0);
    }
    ClockSessionLeave(c);End(c);finish=WddmPrimaryExclusiveEnd(Device,primary,TRUE);
    return NT_SUCCESS(finish)?status:finish;
}

NTSTATUS ConnectorLinkRelease(BC250_DEVICE* Device)
{
    CONNECTOR_LINK* c=Device->ConnectorLink;
    NTSTATUS status=STATUS_SUCCESS;
    if(!c)return status;
    // PnP has drained operations. DcnStop adopts the reserved current-mode CPU
    // surface before BAR teardown. Do not restore an old stream over that mode.
    if(c->Aux.Open) {
        Begin(c,c->Generation,TRUE);status=CloseAux(c);End(c);
        GuardLog("connector-link: release AUX restore %08lX",(ULONG)status);
    }
    // This is the last owner before PnP removes MMIO. A failed final restore
    // propagates through StopDevice's post-display status; it is not described
    // as retryable after the journal has been freed.
    Device->ConnectorLink=NULL;AtomFirmwareRelease(&c->Firmware);
    ExFreePoolWithTag(c->AtomWorkspace,LINK_WORK_TAG);ExFreePoolWithTag(c->DmlWorkspace,LINK_WORK_TAG);
    ExFreePoolWithTag(c,LINK_TAG);return status;
}
