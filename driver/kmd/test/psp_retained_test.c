// Actual PSP load, retained suspend/resume and teardown with synthetic firmware.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include "bc250kmd_escape.h"
#include "../contract/bc250_umd_firmware.h"
typedef long NTSTATUS,LONG;
typedef unsigned long ULONG;
typedef unsigned char* PUCHAR;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef size_t SIZE_T;
typedef int BOOLEAN;
typedef struct {LONGLONG QuadPart;} LARGE_INTEGER;
typedef unsigned u32;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define PAGE_SIZE 4096
#define POOL_FLAG_NON_PAGED 1
#define BC250_PSP_TAG 123
#define BC250_PSP_PAGES_LENGTH (3*PAGE_SIZE)
#define BC250_PSP_STAGING_LENGTH 0x200000
#define BC250_PSP_TMR_BELOW 0x800000
#define BC250_PSP_STAGING_BELOW 0x400000
#define BC250_PSP_PAGES_BELOW 0x19000
#define BC250_FILE_COUNT 8
#define BC250_FW_COUNT 10
#define PSP_TMR_SIZE(n) 0x400000
#define GFX_CMD_ID_LOAD_IP_FW 6
#define GFX_CMD_ID_SETUP_TMR 5
#define STATUS_SUCCESS 0L
#define STATUS_DEVICE_NOT_READY (-1L)
#define STATUS_INVALID_DEVICE_STATE (-2L)
#define STATUS_INVALID_PARAMETER (-3L)
#define STATUS_DEVICE_HARDWARE_ERROR (-4L)
#define STATUS_INSUFFICIENT_RESOURCES (-5L)
#define STATUS_INVALID_IMAGE_FORMAT (-6L)
#define STATUS_BUFFER_OVERFLOW (-7L)
#define STATUS_DEVICE_DATA_ERROR (-8L)
#define STATUS_IO_DEVICE_ERROR (-9L)
#define NT_SUCCESS(s) ((s)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(p,s,n) memcpy(p,s,n)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define le32_to_cpu(v) (v)
#define GuardLog(...) ((void)0)
#define GfxTraceRlcState(...) ((void)0)
enum bc250_fw_file { FILE_ZERO };
enum bc250_fw_id {BC250_FW_CP_ME,BC250_FW_CP_PFP,BC250_FW_CP_CE,BC250_FW_CP_MEC1,BC250_FW_CP_MEC2,BC250_FW_RLC_G,BC250_FW_SDMA0};
enum psp_gfx_fw_type { TYPE_ZERO };
struct gfx_firmware_header_v1_0 {struct {unsigned ucode_version;} header;unsigned ucode_feature_version;};
struct psp_gfx_resp {unsigned fw_addr_hi,fw_addr_lo;};
struct amdgpu_device {void* backend;};
struct bc250_psp_inputs {void *ring_mem,*cmd_buf,*fence_buf;ULONGLONG ring_mc,cmd_buf_mc,fence_buf_mc,tmr_mc;};
struct bc250_psp {struct {struct {void* ring_mem;} km_ring;void *cmd_buf_mem,*fence_buf;} psp;int ring_created;};
typedef struct _BC250_DEVICE BC250_DEVICE;
typedef struct {BC250_DEVICE* Device;NTSTATUS Fault;ULONG WriteCount,FaultOffset,MaxWrites;BC250_ESCAPE_WRITE* Writes;} BC250_SEQUENCE;
struct _BC250_DEVICE {void* Psp;int GartLock,GpuStopUnconfirmed,MmioPspEnabled,FullWddm,GfxStopQuiet,PspStopQuiet;ULONGLONG VramMcBase,VramLength;LARGE_INTEGER VramPhysical;};
/* ACTUAL_TYPES */
static unsigned checks,failures,fileReads,allocs,frees,maps,unmaps,ringCreates,ringStops,tmrLoads,tmrStops,fwLoads;
static int irq,gfxSuspended=1,model_gartEnabled=1,ringFail,tmrStopFail,allowFiles=1;
static unsigned char pages[BC250_PSP_PAGES_LENGTH],model_staging[BC250_PSP_STAGING_LENGTH];
static struct amdgpu_device model_adev;
static ULONGLONG stagingMc;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static int KeGetCurrentIrql(void){return irq;}
static LONG InterlockedIncrement(volatile LONG*p){return ++*p;}
static LONG InterlockedDecrement(volatile LONG*p){return --*p;}
static void* ExAllocatePool2(int flags,SIZE_T n,int tag){(void)tag;CHECK(flags==POOL_FLAG_NON_PAGED);allocs++;return calloc(1,n);}
static void ExFreePoolWithTag(void*p,int tag){(void)tag;if(p){frees++;free(p);}}
static void ExAcquireFastMutex(int*p){CHECK(!*p);*p=1;}
static void ExReleaseFastMutex(int*p){CHECK(*p==1);*p=0;}
static size_t RtlCompareMemory(void*a,const void*b,size_t n){return memcmp(a,b,n)?0:n;}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER*f){static LONGLONG n=0;LARGE_INTEGER r;r.QuadPart=++n;if(f)f->QuadPart=1000000;return r;}
static void SequenceBegin(BC250_SEQUENCE*s,BC250_DEVICE*d,int plan,BC250_ESCAPE_WRITE*w,ULONG max){(void)plan;s->Device=d;s->Fault=0;s->Writes=w;s->MaxWrites=max;s->WriteCount=0;}
static NTSTATUS CheckWindow(const BC250_DEVICE*d){(void)d;return 0;}
static BOOLEAN GfxIsActive(const BC250_DEVICE*d){(void)d;return !gfxSuspended;}
static BOOLEAN GfxPowerIsSuspended(const BC250_DEVICE*d){CHECK(d->GartLock==1);return gfxSuspended;}
static NTSTATUS GartDevice(BC250_DEVICE*d,struct amdgpu_device**a,BOOLEAN*enabled){CHECK(d->GartLock==1);*a=&model_adev;*enabled=model_gartEnabled;return 0;}
static PUCHAR MapVram(const BC250_DEVICE*d,ULONGLONG below,SIZE_T n){(void)n;maps++;if(below==BC250_PSP_STAGING_BELOW){stagingMc=d->VramMcBase+d->VramLength-below;return model_staging;}return pages;}
static void MmUnmapIoSpace(void*p,SIZE_T n){(void)p;(void)n;unmaps++;}
static enum bc250_fw_file bc250_fw_file_of(enum bc250_fw_id id){return (enum bc250_fw_file)((unsigned)id%8);}
static int bc250_fw_locate(enum bc250_fw_id id,void*data,ULONG n,u32*offset,u32*size,enum psp_gfx_fw_type*type){if(!data||n!=24)return -1;*offset=8;*size=16;*type=(enum psp_gfx_fw_type)id;return 0;}
static int ReadFiles(BC250_PSP_FILES*f,ULONG*failed){unsigned i;CHECK(allowFiles);fileReads++;memset(f,0,sizeof(*f));*failed=0;for(i=0;i<8;i++){unsigned*j;f->Data[i]=ExAllocatePool2(1,24,123);f->Size[i]=24;j=(unsigned*)f->Data[i];j[0]=100+i;j[1]=200+i;memset(f->Data[i]+8,(int)(0x40+i),16);}return 0;}
static int bc250_psp_setup(struct amdgpu_device*a,struct bc250_psp*p,const struct bc250_psp_inputs*i){(void)a;p->psp.km_ring.ring_mem=i->ring_mem;p->psp.cmd_buf_mem=i->cmd_buf;p->psp.fence_buf=i->fence_buf;p->ring_created=0;return 0;}
static int bc250_psp_ring_create(struct bc250_psp*p){CHECK(p->psp.km_ring.ring_mem==pages);ringCreates++;p->ring_created=1;return ringFail;}
static int bc250_psp_tmr_load(struct bc250_psp*p){CHECK(p->ring_created);tmrLoads++;return 0;}
static int bc250_psp_load_ip_fw(struct bc250_psp*p,enum psp_gfx_fw_type type,ULONGLONG mc,ULONG n,struct psp_gfx_resp*r){unsigned j;CHECK(p->ring_created && mc>=stagingMc && n==16);for(j=0;j<n;j++)CHECK(model_staging[mc-stagingMc+j]==(unsigned char)(0x40+((unsigned)type%8)));memset(r,0,sizeof(*r));fwLoads++;return 0;}
static u32 bc250_psp_last_status(const struct bc250_psp*p){(void)p;return 0;}
static int bc250_psp_tmr_unload(struct bc250_psp*p){CHECK(p->ring_created);tmrStops++;return tmrStopFail;}
static int bc250_psp_ring_stop(struct bc250_psp*p){ringStops++;p->ring_created=0;return 0;}
NTSTATUS PspInitializePrepared(BC250_DEVICE*,const BC250_PSP_FIRMWARE*,BC250_ESCAPE_PSP*);
/* ACTUAL_FUNCTIONS */
int main(void)
{
    BC250_DEVICE d={0};BC250_PSP* p;BC250_PSP_FIRMWARE* prepared=NULL;BC250_ESCAPE_PSP report;struct bc250_umd_firmware fw,original;void* identity;unsigned before,freeBefore;
    d.Psp=ExAllocatePool2(1,sizeof(*p),123);p=d.Psp;identity=p;d.MmioPspEnabled=1;d.FullWddm=1;d.VramMcBase=0xF400000000ull;d.VramLength=0x200000000ull;d.GfxStopQuiet=1;
    CHECK(PspPrepareFirmware(&d,&report,&prepared)==0 && prepared && prepared->References==1);
    CHECK(PspInitializePrepared(&d,prepared,&report)==0 && p->Loaded && report.CommandsDone==11);
    CHECK(p->Retained==prepared && prepared->References==2);
    if(!p->Retained){PspReleaseFirmware(prepared);PspStop(&d);printf("retained PSP negative control: %u checks, %u failures\n",checks,failures);return 1;}
    PspReleaseFirmware(prepared);prepared=NULL;
    CHECK(p->Retained && p->Retained->References==1);CHECK(PspReadFirmware(&d,&original)==0 && original.me_version==100);
    allowFiles=0;before=fileReads;freeBefore=frees;
    CHECK(PspSetPowerRetained(&d,FALSE,&report)==0 && d.Psp==identity && PspPowerIsSuspended(&d));
    CHECK(!p->Loaded && !p->RingUp && !p->TmrUp && p->Pages==pages && frees==freeBefore);
    CHECK(tmrStops==1 && ringStops==1 && PspReadFirmware(&d,&fw)==0 && !memcmp(&fw,&original,sizeof(fw)));
    memset(pages,0xCD,sizeof(pages));memset(model_staging,0xEF,sizeof(model_staging));
    CHECK(PspSetPowerRetained(&d,TRUE,&report)==0 && d.Psp==identity && p->Loaded && !PspPowerIsSuspended(&d));
    CHECK(fileReads==before && ringCreates==2 && tmrLoads==2 && fwLoads==20 && p->Pages==pages && p->Retained->References==1);
    CHECK(PspReadFirmware(&d,&fw)==0 && !memcmp(&fw,&original,sizeof(fw)));
    // No load against running consumers; state and retained bytes remain owned.
    gfxSuspended=0;before=ringStops;CHECK(PspSetPowerRetained(&d,FALSE,&report)==STATUS_INVALID_DEVICE_STATE && ringStops==before);gfxSuspended=1;
    CHECK(PspSetPowerRetained(&d,FALSE,&report)==0);before=ringStops;
    CHECK(PspSetPowerRetained(&d,FALSE,&report)==0 && ringStops==before); // duplicate suspend
    ringFail=-1;CHECK(PspSetPowerRetained(&d,TRUE,&report)!=0 && !p->Loaded && p->RingUp && !PspPowerIsSuspended(&d));ringFail=0;
    CHECK(PspReadFirmware(&d,&fw)==0 && !memcmp(&fw,&original,sizeof(fw))); // cached identity, not readiness
    CHECK(PspSetPowerRetained(&d,FALSE,&report)==0 && PspPowerIsSuspended(&d));
    CHECK(PspSetPowerRetained(&d,TRUE,&report)==0);
    tmrStopFail=-1;CHECK(PspSetPowerRetained(&d,FALSE,&report)!=0 && p->TmrUp && !p->RingUp && !PspPowerIsSuspended(&d));tmrStopFail=0;
    // Test cleanup models explicit platform recovery, not an automatic retry.
    p->TmrUp=FALSE;PspStop(&d);CHECK(!d.Psp && frees==allocs);
    printf("retained PSP actual-source: %u checks, %u failures; file reads %u\n",checks,failures,fileReads);
    return failures?1:0;
}
