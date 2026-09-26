#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifndef _In_
#define _In_
#define _Inout_
#endif
#define false 0
typedef int BOOLEAN,NTSTATUS;
typedef unsigned long ULONG;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE -1
#define STATUS_DEVICE_NOT_READY -2
#define STATUS_DEVICE_BUSY -3
#define STATUS_IO_DEVICE_ERROR -4
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_EINVAL -22
#define BC250_GFX_STAGE_CP 6
#define BC250_GFX_STAGE_INTERRUPTS 8
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define GuardLog(...) ((void)0)
typedef struct {NTSTATUS Fault;} BC250_SEQUENCE;
struct mem {void*cpu;unsigned size;};
struct amdgpu_ring {uint64_t wptr,wptr_old;int count_dw;unsigned contents[8];};
struct amdgpu_device {void*backend;struct {struct mem wb_mem;struct {struct amdgpu_ring ring;}kiq[1];struct amdgpu_ring compute_ring[8],gfx_ring[1];unsigned num_compute_rings,num_gfx_rings;}gfx;struct {struct mem wb_mem;struct {struct amdgpu_ring ring;}instance[2];int num_instances;}sdma;};
typedef struct {BC250_SEQUENCE Sequence;int SetUp,Failed,SubmitFailed,PagingSubmitFailed,PowerSuspended;unsigned StagesDone;int SubmitInFlight,PagingSubmitInFlight;unsigned RingOwes[4];int FencePage,SdmaFencePage,PagingReady;uint64_t VmidRoot[16];unsigned FenceSeq,SubmitSeq,PagingSubmitSeq;unsigned FenceData[8];} BC250_GFX;
typedef struct {BC250_GFX*Gfx;int IhQuiet,GfxTlbBootstrap,GpuStopUnconfirmed,MmioGfxEnabled,GfxStopPrepared;int GfxPagingLock,GartLock;}BC250_DEVICE;
typedef void (*bc250_gfx_checkpoint_fn)(const char*);
static unsigned checks,failures,closed,opened,sdmaHalt,gfxHalt,resets,rlcStop,stages,bootstrap,clears;
static int irql,critical,pagingLock,gartLock,modelEnabled,pspLoaded,ihActive,modelQuiet,haltError,resetError,failStage,failBootstrap;
static struct amdgpu_device modelAdev;
static BC250_DEVICE*device;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static int KeGetCurrentIrql(void){return irql;}
static void KeEnterCriticalRegion(void){CHECK(!critical);critical=1;}
static void KeLeaveCriticalRegion(void){CHECK(critical&&!pagingLock&&!gartLock);critical=0;}
static void ExAcquirePushLockExclusive(int*p){(void)p;CHECK(critical&&!pagingLock);pagingLock=1;}
static void ExReleasePushLockExclusive(int*p){(void)p;CHECK(!gartLock&&pagingLock);pagingLock=0;}
static void ExAcquireFastMutex(int*p){(void)p;CHECK(pagingLock&&!gartLock);gartLock=1;}
static void ExReleaseFastMutex(int*p){(void)p;CHECK(gartLock);gartLock=0;}
static NTSTATUS GartDevice(BC250_DEVICE*d,struct amdgpu_device**a,BOOLEAN*e){CHECK(d==device&&gartLock);*a=&modelAdev;*e=modelEnabled;return 0;}
static int PspIsLoaded(BC250_DEVICE*d){(void)d;return pspLoaded;}
static int IhIsActive(BC250_DEVICE*d){(void)d;return ihActive;}
static void GfxAccessClose(BC250_DEVICE*d){(void)d;CHECK(gartLock&&pagingLock);closed++;}
static void GfxAccessOpen(BC250_DEVICE*d){(void)d;CHECK(stages==8&&!failStage&&!failBootstrap);opened++;}
static void SequenceBegin(BC250_SEQUENCE*s,BC250_DEVICE*d,int p,void*w,int n){CHECK(d==device&&!p&&!w&&!n);s->Fault=0;}
static void GpuMemBeginSequence(BC250_DEVICE*d,void*w,int n){CHECK(d==device&&!w&&!n);}
static int bc250_sdma_hw_fini(struct amdgpu_device*a){CHECK(a==&modelAdev&&closed);sdmaHalt++;return haltError;}
static int bc250_gfx_hw_fini_keep_rlc(struct amdgpu_device*a){CHECK(a==&modelAdev&&sdmaHalt);gfxHalt++;return 0;}
static int bc250_nbio_enable_doorbell_selfring_aperture(struct amdgpu_device*a,int on){CHECK(a==&modelAdev&&!on&&gfxHalt);return 0;}
static void GrbmSelectDefault(BC250_DEVICE*d){CHECK(d==device&&gfxHalt);}
static int EnginesHalted(BC250_DEVICE*d){CHECK(d==device&&gfxHalt);return modelQuiet;}
static int bc250_sdma_reset_for_reload(struct amdgpu_device*a){CHECK(a==&modelAdev&&gfxHalt&&modelQuiet);resets++;return resetError;}
static void bc250_gfx_rlc_stop(struct amdgpu_device*a){CHECK(a==&modelAdev&&gfxHalt);rlcStop++;}
static void KeMemoryBarrier(void){}
static void amdgpu_ring_clear_ring(struct amdgpu_ring*r){CHECK(!r->wptr&&!r->wptr_old&&!r->count_dw);memset(r->contents,0,sizeof(r->contents));clears++;}
static int stage_run(struct amdgpu_device*a){unsigned i;CHECK(a==&modelAdev&&clears==12&&!device->Gfx->PowerSuspended);stages++;if(stages>=6)CHECK(bootstrap==1&&!device->GfxTlbBootstrap);for(i=0;i<16;i++)CHECK(!device->Gfx->VmidRoot[i]);return (int)stages==failStage?-1:0;}
static struct {int(*Run)(struct amdgpu_device*);}g_Stages[9]={{stage_run},{stage_run},{stage_run},{stage_run},{stage_run},{stage_run},{stage_run},{stage_run},{stage_run}};
static int GpuMemCompleteGfxBootstrap(struct amdgpu_device*a){CHECK(a==&modelAdev&&stages==5);bootstrap++;if(failBootstrap)return -1;device->GfxTlbBootstrap=0;return 0;}
/* ACTUAL_SOURCE */
static void setup(BC250_DEVICE*d,BC250_GFX*g,unsigned*wb){unsigned i;
 memset(d,0,sizeof(*d));memset(g,0,sizeof(*g));memset(&modelAdev,0,sizeof(modelAdev));
 device=d;d->Gfx=g;d->IhQuiet=d->MmioGfxEnabled=1;g->SetUp=g->FencePage=g->SdmaFencePage=g->PagingReady=1;g->StagesDone=8;
 g->FenceSeq=42;g->SubmitSeq=43;g->PagingSubmitSeq=44;g->FenceData[0]=43;g->FenceData[1]=44;
 for(i=0;i<16;i++)g->VmidRoot[i]=0xabc000+i;
 modelAdev.backend=d;modelAdev.gfx.wb_mem.cpu=wb;modelAdev.gfx.wb_mem.size=16;modelAdev.sdma.wb_mem.cpu=wb+4;modelAdev.sdma.wb_mem.size=16;
 memset(wb,0xaa,32);modelAdev.gfx.num_compute_rings=8;modelAdev.gfx.num_gfx_rings=1;modelAdev.sdma.num_instances=2;
 closed=opened=sdmaHalt=gfxHalt=resets=rlcStop=stages=bootstrap=clears=0;
 irql=critical=pagingLock=gartLock=haltError=resetError=failStage=failBootstrap=0;modelEnabled=pspLoaded=ihActive=modelQuiet=1;
}
int main(void){BC250_DEVICE d;BC250_GFX g;unsigned wb[8];void*oldWb;
 setup(&d,&g,wb);oldWb=modelAdev.gfx.wb_mem.cpu;
 CHECK(!GfxPowerIsSuspended(&d));CHECK(GfxSetPowerRetained(&d,FALSE)==0&&GfxPowerIsSuspended(&d));
 CHECK(g.SetUp&&g.StagesDone==8&&closed==1&&!opened&&resets==1&&rlcStop==1&&modelAdev.backend==&d);
 CHECK(GfxSetPowerRetained(&d,FALSE)==0&&sdmaHalt==1);
 CHECK(GfxSetPowerRetained(&d,TRUE)==STATUS_DEVICE_NOT_READY&&GfxPowerIsSuspended(&d)&&!stages);
 d.GfxTlbBootstrap=1;CHECK(GfxSetPowerRetained(&d,TRUE)==0&&!GfxPowerIsSuspended(&d)&&opened==1&&stages==8&&bootstrap==1);
 CHECK(g.SetUp&&g.FenceSeq==42&&g.SubmitSeq==43&&g.PagingSubmitSeq==44&&g.FenceData[0]==43&&g.FenceData[1]==44);
 CHECK(modelAdev.gfx.wb_mem.cpu==oldWb&&modelAdev.backend==&d&&!wb[0]&&!wb[7]);
 CHECK(GfxSetPowerRetained(&d,TRUE)==STATUS_INVALID_DEVICE_STATE);
 setup(&d,&g,wb);g.SubmitInFlight=1;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_DEVICE_BUSY&&!closed&&!sdmaHalt);
 setup(&d,&g,wb);g.PagingSubmitInFlight=1;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_DEVICE_BUSY&&!closed);
 setup(&d,&g,wb);g.RingOwes[2]=7;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_DEVICE_BUSY&&!closed);
 setup(&d,&g,wb);d.IhQuiet=0;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_DEVICE_NOT_READY&&!closed);
 setup(&d,&g,wb);haltError=-1;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_IO_DEVICE_ERROR&&!GfxPowerIsSuspended(&d)&&g.Failed&&d.GpuStopUnconfirmed&&!resets&&rlcStop);
 setup(&d,&g,wb);modelQuiet=0;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_IO_DEVICE_ERROR&&!GfxPowerIsSuspended(&d));
 setup(&d,&g,wb);resetError=-1;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_IO_DEVICE_ERROR&&!GfxPowerIsSuspended(&d));
 setup(&d,&g,wb);CHECK(GfxSetPowerRetained(&d,FALSE)==0);d.GfxTlbBootstrap=1;pspLoaded=0;CHECK(GfxSetPowerRetained(&d,TRUE)==STATUS_DEVICE_NOT_READY&&GfxPowerIsSuspended(&d));
 pspLoaded=1;ihActive=0;CHECK(GfxSetPowerRetained(&d,TRUE)==STATUS_DEVICE_NOT_READY&&!stages);ihActive=1;failBootstrap=1;
 CHECK(GfxSetPowerRetained(&d,TRUE)==STATUS_IO_DEVICE_ERROR&&stages==5&&!opened&&g.Failed&&!GfxPowerIsSuspended(&d)&&d.GfxTlbBootstrap);
 setup(&d,&g,wb);CHECK(GfxSetPowerRetained(&d,FALSE)==0);d.GfxTlbBootstrap=1;failStage=7;CHECK(GfxSetPowerRetained(&d,TRUE)==STATUS_IO_DEVICE_ERROR&&stages==7&&!opened&&g.Failed&&!GfxPowerIsSuspended(&d));
 setup(&d,&g,wb);irql=1;CHECK(GfxSetPowerRetained(&d,FALSE)==STATUS_INVALID_DEVICE_STATE&&!closed);
 printf("gfx retained actual-source: %u checks, %u failures\n",checks,failures);return failures?1:0;
}

