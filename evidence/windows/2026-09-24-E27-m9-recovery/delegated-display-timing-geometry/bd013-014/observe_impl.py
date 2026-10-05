from pathlib import Path
p=Path('bc250-win/driver/kmd/dcn.c');s=p.read_text();s=s.replace('#include "regs.generated.h"','#include "regs.generated.h"\n#include "display_timing_snapshot.h"',1)
anchor='void DcnEscape('
new='''void DcnObserve(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DCN_OBSERVE* Data,
    _In_ BOOLEAN Admin, _In_ ULONG EscapeFlags)
{
    static const ULONG offsets[] = {
        BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,
        BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
        BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,
        BC250_REG_DMU_OTG0_OTG_STATUS_POSITION,
        BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,
        BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL,
        BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL,
        BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT
    };
    ULONG* outputs[] = { &Data->PrimaryAddressLow, &Data->PrimaryAddressHigh,
        &Data->EarliestInUseLow, &Data->EarliestInUseHigh, &Data->FlipControl,
        &Data->SurfacePitch, &Data->OtgStatusPosition, &Data->OtgGlobalControl0,
        &Data->OtgBlankControl, &Data->OtgDoubleBufferControl, &Data->OtgFrameCount };
    ULONG requestedAbi=Data->AbiVersion, timing[TimingCount]={0}, i;
    NTSTATUS status=STATUS_SUCCESS, readStatus;
    C_ASSERT(sizeof(BC250_ESCAPE_DCN_OBSERVE)==128);
    C_ASSERT(RTL_NUMBER_OF(offsets)==RTL_NUMBER_OF(outputs));
    C_ASSERT(RTL_NUMBER_OF(offsets)+TimingCount==BC250_DCN_OBSERVE_REG_COUNT);
    RtlZeroMemory(Data,sizeof(*Data));
    Data->Magic=BC250_ESCAPE_MAGIC; Data->Command=BC250_ESCAPE_OBSERVE_DCN;
    Data->Version=BC250_KMD_VERSION; Data->AbiVersion=BC250_DCN_OBSERVE_ABI;
    Data->RegisterCount=BC250_DCN_OBSERVE_REG_COUNT;
    Data->Status=BC250_ESCAPE_STATUS_REFUSED;
    if (!Admin) {
        Data->Status=BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus=(ULONG)STATUS_ACCESS_DENIED; return;
    }
    // HardwareAccess requests Level Two exclusion of stop/MMIO unmap. No
    // NoAdapterSynchronization bypass: this reader has no separate lifetime join.
    if (EscapeFlags!=1u || requestedAbi!=BC250_DCN_OBSERVE_ABI) {
        Data->NtStatus=(ULONG)STATUS_INVALID_PARAMETER; return;
    }
    if (Device->Mmio==NULL) { Data->NtStatus=(ULONG)STATUS_DEVICE_NOT_READY; return; }
    Data->SequenceBefore=(ULONG)InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0);
    for (i=0;i<RTL_NUMBER_OF(offsets);i++) {
        ULONG value=0;
        readStatus=MmioDcnRead(Device,offsets[i],&value);
        if (NT_SUCCESS(readStatus)) { *outputs[i]=value; Data->ValidMask|=1u<<i; }
        else if (NT_SUCCESS(status)) status=readStatus;
    }
    readStatus=DisplayTimingSnapshot(Device,timing);
    if (NT_SUCCESS(readStatus)) {
        Data->TimingControl=timing[TimingControl]; Data->TimingHTotal=timing[TimingHTotal];
        Data->TimingVTotal=timing[TimingVTotal]; Data->TimingHBlank=timing[TimingHBlank];
        Data->TimingVBlank=timing[TimingVBlank]; Data->TimingPixelControl=timing[TimingPixelControl];
        Data->TimingPhase=timing[TimingPhase]; Data->TimingModulo=timing[TimingModulo];
        Data->TimingInterlace=timing[TimingInterlace]; Data->TimingVTotalControl=timing[TimingVTotalControl];
        Data->TimingReference=timing[TimingReference];
        Data->ValidMask|=BC250_DCN_OBSERVE_TIMING_MASK;
    } else if (NT_SUCCESS(status)) status=readStatus;
    Data->SequenceAfter=(ULONG)InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0);
    Data->NtStatus=(ULONG)status;
    Data->Status=NT_SUCCESS(status)?BC250_ESCAPE_STATUS_DONE:BC250_ESCAPE_STATUS_REFUSED;
}

'''
assert anchor in s;s=s.replace(anchor,new+anchor,1);p.write_text(s)
