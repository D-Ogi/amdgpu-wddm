/* Compile the actual probe implementation with a mocked HAL boundary. No hardware calls. */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "bc250kmd_escape.h"
#define STATUS_SUCCESS ((NTSTATUS)0)
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#endif
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022L)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_DATA_ERROR ((NTSTATUS)0xc000003eL)
#define PASSIVE_LEVEL 0
#define Cmos 0
static unsigned checks, failures, calls;
static ULONG counts[6];
static int irql;
static void check(int value, unsigned line, const char* text)
{ ++checks; if (!value) { ++failures; printf("FAIL CHECK line %u: %s\n",line,text); } }
#define CHECK(x) check(!!(x),__LINE__,#x)
typedef struct { volatile LONG64 UmaActiveBytes; volatile LONG RetainedPowerPhase; } BC250_DEVICE;
static int KeGetCurrentIrql(void) { return irql; }
#define RtlCompareMemory mock_compare_memory
static SIZE_T mock_compare_memory(const void* a, const void* b, SIZE_T n)
{
    SIZE_T i; const unsigned char* x=a; const unsigned char* y=b;
    for(i=0;i<n && x[i]==y[i];++i) {}
    return i;
}
static ULONG HalGetBusDataByOffset(int type, ULONG bus, ULONG slot, void* out, ULONG offset, ULONG length)
{
    static const ULONG buses[6]={1,1,0,0,0,0};
    static const ULONG slots[6]={0x90,0,0,2,4,0xd};
    static const ULONG offsets[6]={0,0x90,0,0,0,0};
    ULONG index=calls++;
    CHECK(index<6);
    if(index>=6) return 0;
    CHECK(type==Cmos && bus==buses[index] && slot==slots[index] && offset==offsets[index]);
    CHECK(length==(index<2 ? 28u:1u));
    /* Dirty even the unreturned tail: the probe must not expose it after short returns. */
    memset(out,0x80+index,length);
    return counts[index];
}
/* ACTUAL_PROBE */
static BC250_ESCAPE_UMA_PROBE request(void)
{
    BC250_ESCAPE_UMA_PROBE q={0};
    q.Magic=BC250_ESCAPE_MAGIC;q.Command=BC250_ESCAPE_RUN_UMA;
    q.Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;q.AbiVersion=BC250_UMA_ABI;q.Op=BC250_UMA_OP_PROBE;
    return q;
}
static void reset(void) { unsigned i;calls=0;irql=0;for(i=0;i<6;++i)counts[i]=i<2?28:1; }
int main(void)
{
    BC250_DEVICE device={8192ll<<20,0};
    BC250_ESCAPE_UMA_PROBE q;
    D3DDDI_ESCAPEFLAGS flags={0};
    unsigned i,j,field;
    flags.HardwareAccess=1;
    CHECK(sizeof(q)==128 && offsetof(BC250_ESCAPE_UMA_PROBE,SlotBlock)==40 &&
          offsetof(BC250_ESCAPE_UMA_PROBE,OffsetBlock)==68 &&
          offsetof(BC250_ESCAPE_UMA_PROBE,RtcCount)==96 &&
          offsetof(BC250_ESCAPE_UMA_PROBE,RtcValue)==112);
    reset();q=request();UmaProbeRequest(&device,&q,TRUE,flags.Value);
    CHECK(calls==6 && q.NtStatus==0 && q.Status==BC250_ESCAPE_STATUS_DONE);
    CHECK(q.Flags==0 && q.Reason==BC250_UMA_REASON_READ);
    CHECK(q.SlotCount==28 && q.OffsetCount==28 && q.SlotBlock[27]==0x80 && q.OffsetBlock[27]==0x81);
    for(i=0;i<4;++i)CHECK(q.RtcCount[i]==1 && q.RtcValue[i]==0x82+i);
    /* Every input byte is checked, including reserved padding and output-only slots. */
    for(i=0;i<sizeof(q);++i) {
        reset();q=request();((unsigned char*)&q)[i]^=1;
        UmaProbeRequest(&device,&q,TRUE,flags.Value);
        CHECK(!calls && q.NtStatus==(ULONG)STATUS_INVALID_PARAMETER);
    }
    reset();q=request();UmaProbeRequest(&device,&q,FALSE,flags.Value);
    CHECK(!calls && q.Status==BC250_ESCAPE_STATUS_NOT_ADMIN);
    for(i=0;i<32;++i) {
        reset();q=request();UmaProbeRequest(&device,&q,TRUE,flags.Value^(1u<<i));
        CHECK(!calls && q.NtStatus==(ULONG)STATUS_INVALID_PARAMETER);
    }
    reset();q=request();irql=1;UmaProbeRequest(&device,&q,TRUE,flags.Value);
    CHECK(!calls && q.NtStatus==(ULONG)STATUS_INVALID_DEVICE_STATE);
    reset();q=request();device.UmaActiveBytes=0;UmaProbeRequest(&device,&q,TRUE,flags.Value);
    CHECK(!calls && q.NtStatus==(ULONG)STATUS_INVALID_DEVICE_STATE);device.UmaActiveBytes=8192ll<<20;
    for(i=1;i<=4;++i) {
        reset();q=request();device.RetainedPowerPhase=(LONG)i;UmaProbeRequest(&device,&q,TRUE,flags.Value);
        CHECK(!calls && q.NtStatus==(ULONG)STATUS_INVALID_DEVICE_STATE);
    }
    device.RetainedPowerPhase=0;
    for(field=0;field<6;++field)for(i=0;i<=(field<2?29u:2u);++i) {
        unsigned char* bytes;ULONG returned,length=field<2?28:1;
        reset();counts[field]=i;q=request();UmaProbeRequest(&device,&q,TRUE,flags.Value);
        CHECK(calls==6 && q.NtStatus==(i>length?(ULONG)STATUS_DATA_ERROR:0));
        CHECK(q.Status==(i>length?BC250_ESCAPE_STATUS_REFUSED:BC250_ESCAPE_STATUS_DONE));
        bytes=field==0?q.SlotBlock:field==1?q.OffsetBlock:&q.RtcValue[field-2];
        returned=field==0?q.SlotCount:field==1?q.OffsetCount:q.RtcCount[field-2];
        CHECK(returned==i);
        for(j=0;j<length;++j)CHECK(bytes[j]==(i>length||j>=i?0:0x80+field));
    }
    printf("UMA HAL probe: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
