// Fixed-mode lab experiment. No user address, allocation, timer or worker.
#include "bc250kmd.h"
#include "dcn_latch_probe.h"
#include "plane_format.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
typedef struct {
    BC250_DEVICE* Device;
    BC250_LATCH_PROBE_RESULT* Result;
    BC250_DISP_IO Io;
    ULONG Generation, Width, Height, Pitch, PhaseMs;
    ULONGLONG Bytes;
    LONGLONG Start, Frequency;
    BOOLEAN LockOwned, Prepared;
} LATCH_CONTEXT;

static long LatchRead(void* Context, unsigned long Offset, unsigned long* Value)
{ return MmioDcnRead((BC250_DEVICE*)Context,Offset,Value); }

static BOOLEAN LatchTime(LATCH_CONTEXT* C, LONGLONG Start, ULONG Ms)
{
    LONGLONG now=KeQueryPerformanceCounter(NULL).QuadPart;
    return now>=Start && now-Start <= (C->Frequency*Ms)/1000;
}

static NTSTATUS LatchGuard(LATCH_CONTEXT* C)
{
    BC250_DEVICE* d=C->Device;
    if (!C->Prepared || KeGetCurrentIrql()!=PASSIVE_LEVEL || !d->Mmio || !d->VidPnFlipEnabled ||
        InterlockedCompareExchange(&d->RetainedPowerPhase,0,0)!=0 ||
        DisplaySourceWidth(d)!=C->Width || DisplaySourceHeight(d)!=C->Height)
        return STATUS_INVALID_DEVICE_STATE;
    return WddmLatchProbeCheck(d,C->Generation,FALSE);
}

static NTSTATUS LatchSample(LATCH_CONTEXT* C, BC250_LATCH_SAMPLE* Sample)
{
    NTSTATUS status=LatchGuard(C), after;
    if (!NT_SUCCESS(status)) return status;
    ++C->Result->Samples;
    status=(NTSTATUS)Bc250LatchSampleRead(&C->Io,Sample);
    after=LatchGuard(C);
    if (!NT_SUCCESS(after)) return after;
    if (!LatchTime(C,C->Start,500)) return STATUS_IO_TIMEOUT;
    return status;
}

static NTSTATUS LatchWrite(LATCH_CONTEXT* C, ULONG Offset, ULONG Value)
{
    NTSTATUS status=LatchGuard(C);
    if (!NT_SUCCESS(status)) return status;
    ++C->Result->WritesAttempted;
    return MmioDcnWriteEx(C->Device,Offset,Value,TRUE);
}

static NTSTATUS LatchUnlock(LATCH_CONTEXT* C)
{
    NTSTATUS status;
    ULONG value, i;
    if (!C->LockOwned) return STATUS_SUCCESS;
    // A fault can close the logical owner while this probe still owes unlock.
    // The caller's Level Two lifetime exclusion must still hold here. Do not
    // access a power-transitioned device in an attempt to repair its state.
    if (!C->Device->Mmio || InterlockedCompareExchange(&C->Device->RetainedPowerPhase,0,0))
        return STATUS_DEVICE_NOT_READY;
    ++C->Result->WritesAttempted;
    status=MmioDcnWriteEx(C->Device,BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,0,TRUE);
    if (!NT_SUCCESS(status)) return status;
    for (i=0;i<11;++i) {
        status=MmioDcnRead(C->Device,BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,&value);
        if (!NT_SUCCESS(status)) return status;
        if (!(value & (OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                       OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK))) break;
        if (i<10) KeStallExecutionProcessor(1);
    }
    if (i==11) return STATUS_IO_TIMEOUT;
    C->LockOwned=FALSE;
    if (!C->Device->Mmio || InterlockedCompareExchange(&C->Device->RetainedPowerPhase,0,0))
        return STATUS_DEVICE_NOT_READY;
    ++C->Result->WritesAttempted;
    return MmioDcnWriteEx(C->Device,BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG,1,TRUE);
}

static NTSTATUS LatchLock(LATCH_CONTEXT* C)
{
    BC250_LATCH_SAMPLE sample;
    NTSTATUS status=LatchSample(C,&sample);
    ULONG value, i;
    if (!NT_SUCCESS(status)) return status;
    if (!sample.LockClear) return STATUS_DEVICE_BUSY;
    // Only this owner may touch the lock after the clear endpoint. Set the
    // cleanup obligation before attempting a write whose failure is uncertain.
    C->LockOwned=TRUE;
    status=LatchWrite(C,BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,
        OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK);
    if (!NT_SUCCESS(status)) return status;
    for (i=0;i<11;++i) {
        status=MmioDcnRead(C->Device,BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,&value);
        if (!NT_SUCCESS(status)) return status;
        if ((value & (OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                      OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK)) ==
            (OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
             OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK)) return LatchGuard(C);
        if (i<10) KeStallExecutionProcessor(1);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS LatchProgram(LATCH_CONTEXT* C, ULONGLONG Physical)
{
    ULONGLONG card, offset;
    BOOLEAN wrote=FALSE;
    NTSTATUS status=LatchGuard(C);
    if (!NT_SUCCESS(status)) return status;
    if (Physical<(ULONGLONG)C->Device->VramPhysical.QuadPart) return STATUS_INVALID_PARAMETER;
    offset=Physical-(ULONGLONG)C->Device->VramPhysical.QuadPart;
    if (offset>MAXULONGLONG-C->Device->VramMcBase) return STATUS_INVALID_PARAMETER;
    card=C->Device->VramMcBase+offset;
    status=DcnFlipSourceAddress(C->Device,card,C->Pitch,BC250_PLANE_FORMAT_ARGB8888,
        C->Bytes,FALSE,NULL,&wrote);
    if (wrote) ++C->Result->WritesAttempted; // one production transaction, not its MMIO word count
    return status;
}

static NTSTATUS LatchWait(LATCH_CONTEXT* C, ULONGLONG Physical, BOOLEAN Held,
    BOOLEAN Boundary, ULONG Frame, BC250_LATCH_SAMPLE* Sample)
{
    LONGLONG start=KeQueryPerformanceCounter(NULL).QuadPart;
    ULONG polls, transitions=0, limit=C->PhaseMs*50;
    NTSTATUS status;
    for (polls=0;polls<=limit;++polls) {
        status=LatchSample(C,Sample);
        if (!LatchTime(C,start,C->PhaseMs)) return STATUS_IO_TIMEOUT;
        if (NT_SUCCESS(status)) {
            if (Held ? !Sample->LockHeld : !Sample->LockClear) return STATUS_DEVICE_BUSY;
            // The counter changes at VSync start, before VUPDATE. Two distinct
            // observed changes enclose a whole frame. Do not subtract frame
            // words: wrap is allowed and reset is excluded by the owner guard.
            if (Boundary && Sample->Word[Bc250LatchFrameAfter]!=Frame) {
                Frame=Sample->Word[Bc250LatchFrameAfter];
                ++transitions;
            }
            if ((!Physical || Sample->Physical==Physical) &&
                (!Boundary || transitions>=2)) return STATUS_SUCCESS;
        } else if (status!=STATUS_DEVICE_BUSY) return status;
        if (polls<limit) KeStallExecutionProcessor(20);
    }
    return STATUS_IO_TIMEOUT;
}

static NTSTATUS LatchPrepare(LATCH_CONTEXT* C)
{
    BC250_DEVICE* d=C->Device;
    D3DKMDT_VIDEO_SIGNAL_INFO signal;
    ULONGLONG bytes, second, base, vramOffset, frameUs;
    ULONG control, flip, flip2;
    NTSTATUS status;
    C->Width=DisplaySourceWidth(d);C->Height=DisplaySourceHeight(d);
    C->Pitch=InterlockedCompareExchange(&d->RuntimeDisplayChanged,0,0) ? d->RuntimePost.Pitch : d->Post.Pitch;
    if (InterlockedCompareExchange(&d->RuntimeDisplayChanged,0,0) &&
        (!d->RuntimePostValid || C->Width!=d->RuntimePost.Width || C->Height!=d->RuntimePost.Height))
        return STATUS_NOT_SUPPORTED;
    if (!d->Mmio || InterlockedCompareExchange(&d->RetainedPowerPhase,0,0) ||
        !d->RuntimeFramebuffer || !d->VramEnabled || !d->DcnWriteEnabled ||
        !d->VidPnFlipEnabled || d->Modeset.PipeChanged || d->Modeset.InheritedFixed ||
        !C->Width || !C->Height || !C->Pitch || (C->Pitch&127u) ||
        C->Width>C->Pitch/4 || d->VramPhysical.QuadPart<0 ||
        d->RuntimeFramebufferPhysical.QuadPart<=0) return STATUS_NOT_SUPPORTED;
    if (InterlockedCompareExchange(&d->RuntimeDisplayChanged,0,0)) {
        status=ConnectorLinkCurrentSignal(d,&signal);
        if (!NT_SUCCESS(status)) return status;
    } else {
        if (!d->InheritedSignalValid) return STATUS_NOT_SUPPORTED;
        signal=d->InheritedSignal;
    }
    if (signal.ScanLineOrdering!=D3DDDI_VSSLO_PROGRESSIVE ||
        !signal.VSyncFreq.Numerator || !signal.VSyncFreq.Denominator ||
        signal.VSyncFreq.Numerator==D3DKMDT_FREQUENCY_NOTSPECIFIED ||
        signal.VSyncFreq.Denominator==D3DKMDT_FREQUENCY_NOTSPECIFIED)
        return STATUS_NOT_SUPPORTED;
    frameUs=((ULONGLONG)signal.VSyncFreq.Denominator*1000000u+
        signal.VSyncFreq.Numerator-1)/signal.VSyncFreq.Numerator;
    // Experiment admission, not a display capability limit: three observation
    // phases fit the 500 ms total budget at up to 50 ms/frame. Add 2 ms for
    // polling/EARLIEST lag; this is a timeout allowance, not a latch witness.
    if (!frameUs || frameUs>50000) return STATUS_NOT_SUPPORTED;
    C->PhaseMs=(ULONG)((2*frameUs+2000+999)/1000);
    bytes=(ULONGLONG)C->Pitch*C->Height;
    if (bytes>MAXULONGLONG-65535) return STATUS_INVALID_PARAMETER;
    second=(bytes+65535)&~65535ull;
    if (second>d->RuntimeFramebufferLength || bytes>d->RuntimeFramebufferLength-second)
        return STATUS_BUFFER_TOO_SMALL;
    base=(ULONGLONG)d->RuntimeFramebufferPhysical.QuadPart;
    if ((base&65535) || base<(ULONGLONG)d->VramPhysical.QuadPart || base>MAXULONGLONG-second-bytes)
        return STATUS_INVALID_PARAMETER;
    vramOffset=base-(ULONGLONG)d->VramPhysical.QuadPart;
    if (vramOffset>d->VramLength || second+bytes>d->VramLength-vramOffset) return STATUS_INVALID_PARAMETER;
    C->Bytes=bytes;C->Result->AddressA=base;C->Result->AddressB=base+second;
    status=MmioDcnRead(d,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL,&control);
    if (!NT_SUCCESS(status)) return status;
    status=MmioDcnRead(d,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,&flip);
    if (!NT_SUCCESS(status)) return status;
    status=MmioDcnRead(d,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL2,&flip2);
    if (!NT_SUCCESS(status)) return status;
    // The experiment covers the existing uncompressed double-buffered path.
    // Do not modify mode/buffering/compression to force admission.
    if (control & HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK) return STATUS_NOT_SUPPORTED;
    if (flip & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_UPDATE_LOCK_MASK) return STATUS_DEVICE_BUSY;
    if (flip2 & HUBPREQ0_DCSURF_FLIP_CONTROL2__SURFACE_TRIPLE_BUFFER_ENABLE_MASK) return STATUS_NOT_SUPPORTED;
    if (DcnOtgRunning(d)!=1) return STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

long DcnLatchProbe(BC250_DEVICE* Device, unsigned long Arm, BC250_LATCH_PROBE_RESULT* Result)
{
    LATCH_CONTEXT c;
    NTSTATUS status;
    BC250_LATCH_SAMPLE restore;
    LARGE_INTEGER frequency;
    ULONG frame;
    BOOLEAN acquired=FALSE;
    if (!Result) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Result,sizeof(*Result));Result->Arm=Arm;
    Result->Status=Result->RestoreStatus=Result->EndStatus=STATUS_NOT_SUPPORTED;
    if (!Device || Arm>=Bc250LatchArmCount || KeGetCurrentIrql()!=PASSIVE_LEVEL) {
        Result->Status=STATUS_INVALID_PARAMETER;return Result->Status;
    }
    RtlZeroMemory(&c,sizeof(c));c.Device=Device;c.Result=Result;c.Io.Context=Device;c.Io.Read=LatchRead;
    c.Start=KeQueryPerformanceCounter(&frequency).QuadPart;c.Frequency=frequency.QuadPart;
    if (c.Frequency<=0 || c.Frequency>MAXLONGLONG/500) {
        Result->Status=STATUS_INVALID_DEVICE_STATE;return Result->Status;
    }
    status=LatchPrepare(&c);
    if (!NT_SUCCESS(status)) goto done;
    // Refuse a foreign lock/selector before Begin's bridge writes any register.
    status=(NTSTATUS)Bc250LatchSampleRead(&c.Io,&restore);
    if (!NT_SUCCESS(status)) goto done;
    if (!restore.LockClear) {status=STATUS_DEVICE_BUSY;goto done;}
    if (!Device->Mmio || InterlockedCompareExchange(&Device->RetainedPowerPhase,0,0)) {
        status=STATUS_DEVICE_NOT_READY;goto done;
    }
    if (!WddmPrimaryExclusiveBegin(Device,BC250_WRITER_LATCH_PROBE,&c.Generation)) {
        status=STATUS_DEVICE_BUSY;goto done;
    }
    acquired=TRUE;Result->Generation=c.Generation;
    status=WddmWriterResume(Device);
    if (!NT_SUCCESS(status)) goto cleanup;
    // Preflight was not ownership. Revalidate after the bridge has retired.
    status=LatchPrepare(&c);
    if (!NT_SUCCESS(status)) goto cleanup;
    c.Prepared=TRUE;
    status=LatchProgram(&c,Result->AddressA);
    if (!NT_SUCCESS(status)) goto cleanup;
    status=LatchWait(&c,Result->AddressA,FALSE,FALSE,0,&Result->Before);
    if (!NT_SUCCESS(status)) goto cleanup;
    frame=Result->Before.Word[Bc250LatchFrameAfter];
    if (Arm==Bc250LatchLockBeforeProgram) {
        status=LatchLock(&c);
        if (!NT_SUCCESS(status)) goto cleanup;
        status=LatchWrite(&c,BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,(ULONG)(Result->AddressB>>32));
        if (NT_SUCCESS(status)) status=LatchWrite(&c,BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,(ULONG)Result->AddressB);
    } else status=LatchProgram(&c,Result->AddressB);
    if (!NT_SUCCESS(status)) goto cleanup;
    if (Arm==Bc250LatchLockAfterProgram) {
        status=LatchLock(&c);
        if (!NT_SUCCESS(status)) goto cleanup;
    }
    status=LatchSample(&c,&Result->Armed);
    if (!NT_SUCCESS(status)) goto cleanup;
    if (Result->Armed.Word[Bc250LatchFrameAfter]!=frame ||
        (Arm!=Bc250LatchUnlocked && (!Result->Armed.LockHeld || Result->Armed.Physical!=Result->AddressA))) {
        status=STATUS_RETRY;goto cleanup; // missed the discriminating interval
    }
    status=LatchWait(&c,0,Arm!=Bc250LatchUnlocked,TRUE,frame,&Result->Boundary);
    if (!NT_SUCCESS(status)) goto cleanup;
    Result->BoundaryObserved=1;
    Result->UnlockStatus=LatchUnlock(&c);
    if (!NT_SUCCESS(Result->UnlockStatus)) {status=Result->UnlockStatus;goto cleanup;}
    status=LatchWait(&c,Result->AddressB,FALSE,FALSE,0,&Result->AfterUnlock);
cleanup:
    // Begin/Resume/End own their bridge and recovery. If no probe write could
    // have taken effect, do not invent a restore failure or request E5. Keep
    // Restored zero: no restore was performed, and the measurement still failed.
    if (!Result->WritesAttempted) {
        Result->RestoreStatus=STATUS_SUCCESS;
        goto end_owner;
    }
    // Cleanup has its own bounded observation window, but still keeps the
    // owner and Level Two call. A failed experiment must not skip restoration.
    c.Start=KeQueryPerformanceCounter(NULL).QuadPart;
    if (c.LockOwned) Result->UnlockStatus=LatchUnlock(&c);
    Result->RestoreStatus=Result->UnlockStatus;
    if (NT_SUCCESS(Result->RestoreStatus)) Result->RestoreStatus=LatchProgram(&c,Result->AddressA);
    if (NT_SUCCESS(Result->RestoreStatus))
        Result->RestoreStatus=LatchWait(&c,Result->AddressA,FALSE,FALSE,0,&restore);
    Result->Restored=NT_SUCCESS(Result->RestoreStatus)?1u:0u;
    if (!Result->Restored) (void)WddmLatchProbeCheck(Device,c.Generation,TRUE);
end_owner:
    Result->EndStatus=WddmPrimaryExclusiveEnd(Device,c.Generation,FALSE);
done:
    Result->Status=status;
    if (!NT_SUCCESS(status)) return status;
    if (!acquired || !Result->Restored) return Result->RestoreStatus;
    return Result->EndStatus;
}
#endif
