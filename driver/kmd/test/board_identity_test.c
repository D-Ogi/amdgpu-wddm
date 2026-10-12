/* Compile the actual probe implementation with a mocked HAL boundary. No hardware calls. */
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "board_identity.h"
#include "bc250kmd_escape.h"
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xc00000bbL)
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
typedef struct {
    NTSTATUS (*DxgkCbReadDeviceSpace)(void*, ULONG, void*, ULONG, ULONG, ULONG*);
    void* DeviceHandle;
} MOCK_DXGK;
typedef struct {
    MOCK_DXGK Dxgk;
    volatile LONG64 UmaActiveBytes;
    volatile LONG RetainedPowerPhase;
    volatile LONG BoardMemoryProviderId;
    ULONG BoardMemoryReason;
    ULONG BoardMemoryBiosId;
    unsigned char BoardMemoryMachineId[16];
} BC250_DEVICE;
typedef struct BC250_BOARD_MEMORY_PROVIDER {
    void (*Query)(BC250_DEVICE*, BC250_ESCAPE_BOARD_MEMORY*);
    NTSTATUS (*Set)(BC250_DEVICE*, const BC250_ESCAPE_BOARD_MEMORY*);
    NTSTATUS (*Restore)(BC250_DEVICE*, const BC250_ESCAPE_BOARD_MEMORY*);
    void (*Probe)(BC250_DEVICE*, BC250_ESCAPE_BOARD_MEMORY_PROBE*, BOOLEAN, ULONG);
} BC250_BOARD_MEMORY_PROVIDER;
const BC250_BOARD_MEMORY_PROVIDER* BoardMemoryProvider(BC250_DEVICE* Device);
void BoardMemoryIdentityClear(BC250_DEVICE* Device);
void BoardMemoryIdentityCapture(BC250_DEVICE* Device);
void AblBoardMemoryProbeRequest(BC250_DEVICE*, BC250_ESCAPE_BOARD_MEMORY_PROBE*, BOOLEAN, ULONG);

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

#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xc0000023L)
#define POOL_FLAG_NON_PAGED 1
#define DXGK_WHICHSPACE_CONFIG 0
static unsigned char pciImage[64], firmwareImage[1024];
static ULONG pciCount=64, firmwareSize, declaredSize, returnedSize;
static NTSTATUS pciStatus, auxStatus, readStatus, sizeStatus;
static unsigned pciCalls, auxCalls, allocations, frees;
static int allocFails;
static NTSTATUS read_pci(void* handle, ULONG space, void* buffer, ULONG offset, ULONG size, ULONG* count)
{
    CHECK(handle==(void*)1 && space==DXGK_WHICHSPACE_CONFIG && offset==0 && size==64);
    ++pciCalls;memcpy(buffer,pciImage,64);*count=pciCount;return pciStatus;
}
static NTSTATUS AuxKlibInitialize(void) { return auxStatus; }
static NTSTATUS AuxKlibGetSystemFirmwareTable(ULONG provider, ULONG id, void* data, ULONG capacity, ULONG* bytes)
{
    CHECK(provider=='RSMB' && id==0);++auxCalls;
    if(!data){CHECK(capacity==0);*bytes=declaredSize;return sizeStatus;}
    CHECK(capacity==declaredSize);memcpy(data,firmwareImage,firmwareSize<capacity?firmwareSize:capacity);
    *bytes=returnedSize;return readStatus;
}
static void* ExAllocatePool2(ULONG flags, size_t bytes, ULONG tag)
{
    CHECK(flags==POOL_FLAG_NON_PAGED && tag=='ImAB');
    if(allocFails)return NULL;
    ++allocations;return malloc(bytes);
}
static void ExFreePoolWithTag(void* data, ULONG tag)
{ CHECK(tag=='ImAB');++frees;free(data); }
/* ACTUAL_IDENTITY */
/* ACTUAL_CAPTURE */
/* Write provider is outside this older boundary fixture's scope. */
static void BoardMemoryWriteQuery(BC250_DEVICE* d, BC250_ESCAPE_BOARD_MEMORY* q) { (void)d; (void)q; }
static NTSTATUS AblBoardMemorySet(BC250_DEVICE* d, const BC250_ESCAPE_BOARD_MEMORY* q) { (void)d; (void)q; return STATUS_NOT_SUPPORTED; }
static NTSTATUS AblBoardMemoryRestore(BC250_DEVICE* d, const BC250_ESCAPE_BOARD_MEMORY* q) { (void)d; (void)q; return STATUS_NOT_SUPPORTED; }
/* ACTUAL_UMA */
/* ACTUAL_PROBE */
static BC250_ESCAPE_BOARD_MEMORY_PROBE request(void)
{
    BC250_ESCAPE_BOARD_MEMORY_PROBE q={0};
    q.Magic=BC250_ESCAPE_MAGIC;q.Command=BC250_ESCAPE_RUN_BOARD_MEMORY;
    q.Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;q.AbiVersion=BC250_BOARD_MEMORY_ABI;q.Op=BC250_BOARD_MEMORY_OP_PROBE;
    return q;
}
static void reset(void) { unsigned i;calls=0;irql=0;for(i=0;i<6;++i)counts[i]=i<2?28:1; }

static void append_table(unsigned type, unsigned length, const char* strings, size_t count)
{
    unsigned char* h=firmwareImage+firmwareSize;
    memset(h,0,length);h[0]=(unsigned char)type;h[1]=(unsigned char)length;
    if(type==0){h[4]=1;h[5]=2;h[8]=3;}
    if(type==2){h[4]=1;h[5]=2;}
    firmwareSize+=length;memcpy(firmwareImage+firmwareSize,strings,count);firmwareSize+=(ULONG)count;
}
static void fixture(const char* version, const char* date)
{
    char strings[128];size_t n=0,k;
    memset(pciImage,0,sizeof(pciImage));pciImage[0]=2;pciImage[1]=0x10;
    pciImage[2]=0xfe;pciImage[3]=0x13;pciImage[44]=0x22;pciImage[45]=0x10;
    memset(firmwareImage,0,sizeof(firmwareImage));firmwareSize=8;
    k=strlen("American Megatrends Inc.")+1;memcpy(strings+n,"American Megatrends Inc.",k);n+=k;
    k=strlen(version)+1;memcpy(strings+n,version,k);n+=k;
    k=strlen(date)+1;memcpy(strings+n,date,k);n+=k;strings[n++]=0;
    append_table(0,9,strings,n);
    append_table(2,6,"ASRock\0AMD BC-250\0",sizeof("ASRock\0AMD BC-250\0"));
    append_table(127,4,"\0",2);
    {ULONG len=firmwareSize-8;memcpy(firmwareImage+4,&len,4);}
    pciCount=64;declaredSize=returnedSize=firmwareSize;
    pciStatus=auxStatus=readStatus=sizeStatus=0;pciCalls=auxCalls=allocations=frees=0;allocFails=0;irql=0;
}
static void capture_check(BC250_DEVICE* d, unsigned selected)
{
    BC250_ESCAPE_BOARD_MEMORY_PROBE q=request();D3DDDI_ESCAPEFLAGS flags={0};
    BC250_ESCAPE_BOARD_MEMORY query={0};
    flags.HardwareAccess=1;calls=0;
    /* A failed new capture must erase an earlier successful start. */
    d->BoardMemoryProviderId=BC250_BOARD_MEMORY_PROVIDER_BC250_ABL;
    BoardMemoryIdentityCapture(d);
    CHECK((unsigned)d->BoardMemoryProviderId==selected);
    CHECK(allocations==frees);
    BoardMemoryProbeRequest(d,&q,TRUE,flags.Value);
    CHECK(calls==(selected?6u:0u));
    CHECK(selected ? q.Flags==BC250_BOARD_MEMORY_SUPPORTED : q.Flags==0);
    query.Magic=BC250_ESCAPE_MAGIC;query.Command=BC250_ESCAPE_RUN_BOARD_MEMORY;
    query.Status=BC250_ESCAPE_STATUS_UNKNOWN_COMMAND;query.AbiVersion=BC250_BOARD_MEMORY_ABI;
    flags.Value=0;flags.NoAdapterSynchronization=1;
    BoardMemoryRequest(d,&query,FALSE,flags.Value);
    CHECK(query.Status==BC250_ESCAPE_STATUS_DONE && query.NtStatus==0);
    CHECK(query.ProviderId==selected);
    CHECK(selected ? query.ActiveBytes==(8192ull<<20) : query.ActiveBytes==0);
}
int main(void)
{
    BC250_DEVICE d={0};unsigned i;ULONG full;
    d.Dxgk.DxgkCbReadDeviceSpace=read_pci;d.Dxgk.DeviceHandle=(void*)1;d.UmaActiveBytes=8192ll<<20;
    reset();fixture("P2.00","11/09/2021");capture_check(&d,1);
    fixture("P3.00","12/09/2021");capture_check(&d,1);
    fixture("P5.00","05/03/2022");capture_check(&d,1);
    BoardMemoryIdentityClear(&d);CHECK(d.BoardMemoryProviderId==0);
    for(i=0;i<64;++i){fixture("P3.00","12/09/2021");pciCount=i;capture_check(&d,0);CHECK(auxCalls==0);}
    {static const unsigned offsets[]={0,2,14,44,46};for(i=0;i<5;++i){fixture("P3.00","12/09/2021");pciImage[offsets[i]]^=1;capture_check(&d,0);CHECK(auxCalls==0);}}
    fixture("P3.00","12/09/2021");pciImage[14]=0x80;capture_check(&d,1);
    fixture("P9.00","12/09/2021");capture_check(&d,0);
    fixture("P3.00","12/09/2022");capture_check(&d,0);
    fixture("P3.00","12/09/2021");full=firmwareSize;
    for(i=0;i<full;++i){fixture("P3.00","12/09/2021");firmwareSize=declaredSize=returnedSize=i;capture_check(&d,0);}
    /* Mutate each identifying string byte, including indexes and terminators. */
    fixture("P3.00","12/09/2021");full=firmwareSize;
    for(i=17;i<full-6;++i){
        /* Type 2 handle bytes are not board identity. */
        if(i==62 || i==63)continue;
        fixture("P3.00","12/09/2021");firmwareImage[i]^=0x40;capture_check(&d,0);}
    fixture("P3.00","12/09/2021");firmwareImage[8]=1;capture_check(&d,0);
    fixture("P3.00","12/09/2021");firmwareImage[9]=3;capture_check(&d,0);
    fixture("P3.00","12/09/2021");firmwareImage[9]=255;capture_check(&d,0);
    /* Duplicated type 0 or type 2 is ambiguous even if identical. */
    for(i=0;i<2;++i){ULONG first=8,end=17;
        fixture("P3.00","12/09/2021");while(firmwareImage[end] || firmwareImage[end+1])++end;end+=2;
        if(i){first=end;end=full-6;}
        memmove(firmwareImage+end+(end-first),firmwareImage+end,full-end);
        memcpy(firmwareImage+end,firmwareImage+first,end-first);
        firmwareSize=declaredSize=returnedSize=full+end-first;
        {ULONG n=firmwareSize-8;memcpy(firmwareImage+4,&n,4);}capture_check(&d,0);
    }
    fixture("P3.00","12/09/2021");pciStatus=STATUS_ACCESS_DENIED;capture_check(&d,0);
    fixture("P3.00","12/09/2021");auxStatus=STATUS_ACCESS_DENIED;capture_check(&d,0);
    fixture("P3.00","12/09/2021");sizeStatus=STATUS_ACCESS_DENIED;capture_check(&d,0);
    fixture("P3.00","12/09/2021");readStatus=STATUS_ACCESS_DENIED;capture_check(&d,0);
    fixture("P3.00","12/09/2021");returnedSize--;capture_check(&d,0);
    fixture("P3.00","12/09/2021");returnedSize++;capture_check(&d,0);
    fixture("P3.00","12/09/2021");declaredSize=1024*1024+1;capture_check(&d,0);
    fixture("P3.00","12/09/2021");allocFails=1;capture_check(&d,0);
    fixture("P3.00","12/09/2021");irql=2;capture_check(&d,0);CHECK(pciCalls==0);
    fixture("P3.00","12/09/2021");d.Dxgk.DxgkCbReadDeviceSpace=NULL;capture_check(&d,0);CHECK(pciCalls==0);
    printf("Board provider: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
