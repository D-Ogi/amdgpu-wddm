/* DCN201 inherited-link takeover at the serialized PnP start/D0 boundary.
 * WDDM's primary transaction excludes flips; no FAST_MUTEX or spin lock is
 * held while ATOM runs. Stop/D3 restores before BAR5 or power is released. */
#include "bc250kmd.h"
#include "hdmi_link.h"
#include "dpaudio_seq.h"
#include "atom_firmware.h"
#include "../shim/bc250_hdmi_link.h"
#include "dcn_2_0_1_sh_mask.h"

typedef struct HDMI_LINK_CONTEXT {
    BC250_DEVICE *Device;
    PKTHREAD Owner;
    BC250_HDMI_LINK Link;
} HDMI_LINK_CONTEXT;

static int Owned(void *Context)
{
    HDMI_LINK_CONTEXT *c=Context;
    return c->Owner==KeGetCurrentThread() && KeGetCurrentIrql()==PASSIVE_LEVEL &&
        c->Device->Mmio!=NULL && c->Device->DcnWriteEnabled;
}
static int Allowed(void *Context,uint32_t Dword,int Write)
{ UNREFERENCED_PARAMETER(Context);return Bc250HdmiLinkAllowed(Dword,Write); }
static int Read(void *Context,uint32_t Dword,uint32_t *Value)
{
    HDMI_LINK_CONTEXT *c=Context;
    if (!Owned(c) || !Allowed(c,Dword,0)) return -1;
    *Value=READ_REGISTER_ULONG((PULONG)&c->Device->Mmio[Dword]);return 0;
}
static int Write(void *Context,uint32_t Dword,uint32_t Value)
{
    HDMI_LINK_CONTEXT *c=Context;
    if (!Owned(c) || !Allowed(c,Dword,1)) return -1;
    WRITE_REGISTER_ULONG((PULONG)&c->Device->Mmio[Dword],Value);return 0;
}
static uint64_t NowUs(void *Context)
{ UNREFERENCED_PARAMETER(Context);return KeQueryInterruptTime()/10u; }
static int Delay(void *Context,uint32_t Us)
{
    if (!Owned(Context) || Us>2000) return -1;
    KeStallExecutionProcessor(Us);return 0;
}
static void Audio(BC250_DEVICE *Device,BOOLEAN Resume)
{ if (Resume) DpAudioResume(Device);else DpAudioStart(Device); }

static int Predict(HDMI_LINK_CONTEXT *Context,const BC250_ATOM_CALLBACKS *Io,
    const BC250_ATOM_IMAGE *Image,BC250_ATOM_PLAN *Plan,void *Workspace)
{
    BC250_DEVICE *d=Context->Device;
    BC250_HDMI_SNAPSHOT *snapshot=NULL;
    BC250_HDMI_PREFLIGHT *prediction=NULL;
    void *shadow=NULL;size_t shadowBytes=Bc250HdmiLinkShadowSize();
    uint64_t started=NowUs(Context),elapsed;unsigned i;int result=-1;

    // The snapshot and evaluator are pool-backed, not a kernel-stack MMIO model.
    snapshot=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*snapshot),'LsAB');
    prediction=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*prediction),'LdAB');
    shadow=ExAllocatePool2(POOL_FLAG_NON_PAGED,shadowBytes,'LmAB');
    if (!snapshot || !prediction || !shadow) {
        GuardLog("hdmilink: preflight refused: allocation; no hardware writes");goto done;
    }
    result=Bc250HdmiLinkSnapshot(Io,snapshot);
    for (i=0;i<snapshot->Count && i<BC250_HDMI_SNAPSHOT_REGS;++i) {
        const BC250_HDMI_SNAPSHOT_ENTRY *entry=&snapshot->Entries[i];
        if (!entry->Status)
            GuardLog("hdmilink: inherited DW 0x%X value 0x%08X",entry->Register,entry->Value);
        else
            GuardLog("hdmilink: inherited DW 0x%X unreadable status %d",entry->Register,entry->Status);
    }
    if (result || !snapshot->Complete) {
        GuardLog("hdmilink: preflight refused: incomplete snapshot; no hardware writes");result=-1;goto done;
    }
    // No hardware callbacks enter this evaluator. Its success predicts one
    // sampled path; live Start still checks ownership, writes and readback.
    result=Bc250HdmiLinkPreflight(snapshot,Image,Plan,Workspace,Bc250AtomContextSize(),
        shadow,shadowBytes,(uint32_t)(d->InheritedSignal.PixelRate/1000),
        d->InheritedSignal.ActiveSize.cx,d->InheritedSignal.ActiveSize.cy,
        d->InheritedSignal.TotalSize.cx,d->InheritedSignal.TotalSize.cy,prediction);
    for (i=0;i<prediction->Count && i<BC250_HDMI_PREFLIGHT_REFUSALS;++i) {
        const BC250_HDMI_REFUSAL *refusal=&prediction->Refusals[i];
        GuardLog("hdmilink: preflight I/O op %u DW 0x%X wanted 0x%08X readback 0x%08X mask 0x%08X",
            refusal->Operation,refusal->Register,refusal->Wanted,refusal->Before,refusal->Mask);
    }
    if (prediction->Fatal || !prediction->Complete)
        GuardLog("hdmilink: preflight fatal op %u DW 0x%X wanted 0x%08X readback 0x%08X mask 0x%08X",
            prediction->Last.Operation,prediction->Last.Register,prediction->Last.Wanted,
            prediction->Last.Before,prediction->Last.Mask);
    elapsed=NowUs(Context)-started;
    GuardLog("hdmilink: preflight rc %d complete %u fatal %u refusals %u events %u overflow %u wall_us %llu",
        result,prediction->Complete,prediction->Fatal,prediction->Count,prediction->Events,prediction->Overflow,elapsed);
    GuardLog("hdmilink: model reads %u writes %u delay_us %u time_us %llu atom %u table %u offset %u",
        prediction->Reads,prediction->Writes,prediction->DelayUs,prediction->ElapsedUs,
        (unsigned)prediction->Atom.Status,prediction->Atom.Table,prediction->Atom.Offset);
    if (!Owned(Context) || elapsed>500000 || !prediction->Complete || prediction->Fatal ||
        prediction->Count || prediction->Overflow) result=-1;
    if (result) GuardLog("hdmilink: preflight refused; no hardware writes");
done:
    if (shadow) ExFreePoolWithTag(shadow,'LmAB');
    if (prediction) ExFreePoolWithTag(prediction,'LdAB');
    if (snapshot) ExFreePoolWithTag(snapshot,'LsAB');
    return result;
}

void HdmiLinkStartAudio(BC250_DEVICE *Device,BOOLEAN Resume)
{
    HDMI_LINK_CONTEXT *c=NULL;
    BC250_ATOM_FIRMWARE firmware={0}; BC250_ATOM_STATUS reason=BC250_ATOM_OK;
    BC250_ATOM_PLAN *plan=NULL;void *workspace=NULL;
    BC250_DPAUDIO_SINK sink; BC250_ATOM_CALLBACKS io;
    ULONG generation=0;BOOLEAN exclusive=FALSE,predictionRefused=FALSE;NTSTATUS status;int result=-1,restore=0;
    const BC250_EDID_INFO *edid;

    NT_ASSERT(KeGetCurrentIrql()==PASSIVE_LEVEL);
    // Scope: the ASIC whose DCN201 tables and powered DIG0 route are covered by
    // this shim, no audio switch override and no inherited DP or non-HDMI sink.
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL || !Device->InheritedTmds || !Device->InheritedSignalValid ||
        !Device->Mmio || !Device->DcnWriteEnabled || Device->HdmiLinkContext ||
        GuardReadSetting(L"EnableHdmiTakeover",1)!=1 || GuardReadSetting(L"EnableDpAudio",1)!=1 ||
        GuardReadSetting(L"EnableDpAudioEndpoint",1)!=1 || GuardReadSetting(L"EnableDpAudioStream",1)!=1) {
        Audio(Device,Resume);return;
    }
    edid=ModesetEdidForAudio(Device);
    if (!Bc250HdmiAudioSinkFromEdid(edid,&sink) || Device->InheritedSignal.PixelRate<25000000 ||
        Device->InheritedSignal.PixelRate>300000000 || !Device->Modeset.Adapter.max_tmds_khz ||
        Device->InheritedSignal.PixelRate/1000>Device->Modeset.Adapter.max_tmds_khz) {
        GuardLog("hdmilink: refused: HDMI CTA, inherited clock or identified adapter limit");
        Audio(Device,Resume);return;
    }
    status=AtomFirmwareCapture(Device,&firmware,&reason);
    if (!NT_SUCCESS(status) || firmware.Identity.VendorId!=0x1002 || firmware.Identity.DeviceId!=0x13fe) {
        GuardLog("hdmilink: refused: VFCT identity/layout status 0x%08lX reason %u",status,(unsigned)reason);
        goto done;
    }
    c=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*c),'LhAB');
    plan=ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*plan),'LpAB');
    workspace=ExAllocatePool2(POOL_FLAG_NON_PAGED,Bc250AtomContextSize(),'LwAB');
    if (!c || !plan || !workspace) { GuardLog("hdmilink: refused: workspace allocation");goto done; }
    c->Device=Device;c->Owner=KeGetCurrentThread();
    io.Context=c;io.Owned=Owned;io.Allowed=Allowed;io.Read=Read;io.Write=Write;io.NowUs=NowUs;io.DelayUs=Delay;
    if (!WddmPrimaryExclusiveBegin(Device,BC250_WRITER_HDMI_START,&generation)) {
        GuardLog("hdmilink: refused: primary busy");goto done;
    }
    exclusive=TRUE;
    if (Predict(c,&io,&firmware.Image,plan,workspace)) { predictionRefused=TRUE;goto done; }
    result=Bc250HdmiLinkStart(&c->Link,&io,&firmware.Image,plan,workspace,Bc250AtomContextSize(),
        (uint32_t)(Device->InheritedSignal.PixelRate/1000),Device->InheritedSignal.ActiveSize.cx,
        Device->InheritedSignal.ActiveSize.cy,Device->InheritedSignal.TotalSize.cx,Device->InheritedSignal.TotalSize.cy);
    if (!result) {
        status=DisplayVerifyInheritedTiming(Device);
        GuardLog("hdmilink: unchanged timing remeasure 0x%08lX",status);
        if (!NT_SUCCESS(status)) result=-1;
    }
    if (result) {
        restore=Bc250HdmiLinkRestore(&c->Link);
        if (restore) Device->HdmiLinkContext=c; // retain the obligation through StopDevice
        GuardLog("hdmilink: refused: atom %u table %u offset %u register DW 0x%X; initial rollback %s",
            (unsigned)c->Link.Atom.Status,c->Link.Atom.Table,c->Link.Atom.Offset,c->Link.FailureRegister,
            restore?"FAILED":"verified");
        GuardLog("hdmilink: I/O op %u wanted 0x%08X readback 0x%08X mask 0x%08X (1 read, 2 access, 3 write, 4 verify, 5 wait, 6 unknown field)",
            c->Link.FailureOperation,c->Link.FailureWanted,c->Link.FailureReadback,c->Link.FailureMask);
        goto done;
    }
    Device->HdmiLinkContext=c;
    GuardLog("hdmilink: HDMI DIG0, timing unchanged %lux%lu clock %llu Hz; %u saved registers",
        Device->InheritedSignal.ActiveSize.cx,Device->InheritedSignal.ActiveSize.cy,
        Device->InheritedSignal.PixelRate,c->Link.Count);
done:
    if (exclusive) WddmPrimaryExclusiveEnd(Device,generation,FALSE);
    if (workspace) ExFreePoolWithTag(workspace,'LwAB');
    if (plan) ExFreePoolWithTag(plan,'LpAB');
    AtomFirmwareRelease(&firmware);
    if (c && Device->HdmiLinkContext!=c) ExFreePoolWithTag(c,'LhAB');
    if (!restore && !predictionRefused) Audio(Device,Resume);
    if (Device->HdmiLinkContext) {
        c=Device->HdmiLinkContext;
        c->Owner=NULL;
        if (Device->DpAudio.State!=BC250_DPAUDIO_STATE_ENABLED) {
            DpAudioStop(Device);
            HdmiLinkStop(Device);
        } else GuardLog("hdmilink: audio enabled; AVI and current timing retained; restore owed at stop/D3");
    }
    GuardLog("hdmilink: start final state %s",
        !Device->HdmiLinkContext?"inherited route retained; no restore owed":
        (((HDMI_LINK_CONTEXT*)Device->HdmiLinkContext)->Link.Active?
         "HDMI active; restore owed":"rollback incomplete; restore owed"));
}

BOOLEAN HdmiLinkStopOwned(BC250_DEVICE *Device)
{
    HDMI_LINK_CONTEXT *c=Device->HdmiLinkContext;
    int restore;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) {
        GuardLog("hdmilink: restore deferred: owner unavailable");return FALSE;
    }
    if (!c) return TRUE;
    // The caller already excludes primary flips. Do not nest that transaction.
    c->Owner=KeGetCurrentThread();
    restore=Bc250HdmiLinkRestore(&c->Link);
    c->Owner=NULL;
    GuardLog("hdmilink: inherited DVI restore %s; mismatch DW 0x%X",
        restore?"FAILED":"verified",c->Link.RestoreRegister);
    if (!restore) { Device->HdmiLinkContext=NULL;ExFreePoolWithTag(c,'LhAB'); }
    return restore==0;
}

void HdmiLinkGetRestoreFailure(BC250_DEVICE *Device, BC250_HDMI_RESTORE_DETAIL* Out)
{
    HDMI_LINK_CONTEXT* c;
    if (!Out) return;
    RtlZeroMemory(Out,sizeof(*Out));
    c=Device->HdmiLinkContext;
    if (c) *Out=c->Link.RestoreFailure;
}

BOOLEAN HdmiLinkStop(BC250_DEVICE *Device)
{
    ULONG generation;BOOLEAN restored;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return FALSE;
    if (!Device->HdmiLinkContext) return TRUE;
    if (!WddmPrimaryExclusiveBegin(Device,BC250_WRITER_HDMI_STOP,&generation)) {
        GuardLog("hdmilink: restore deferred: primary busy");return FALSE;
    }
    restored=HdmiLinkStopOwned(Device);
    WddmPrimaryExclusiveEnd(Device,generation,FALSE);
    return restored;
}

NTSTATUS HdmiLinkRelease(BC250_DEVICE *Device)
{
    // StopDevice calls again after WddmStop has drained flips, before BAR5 is
    // unmapped. A final failed restore is reported as a failed display handoff;
    // the saved software state must not leak past device removal.
    if (HdmiLinkStop(Device)) return STATUS_SUCCESS;
    if (Device->HdmiLinkContext) {
        ExFreePoolWithTag(Device->HdmiLinkContext,'LhAB');
        Device->HdmiLinkContext=NULL;
    }
    return STATUS_DEVICE_DATA_ERROR;
}
