#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "bc250_sdma.h"
#include "bc250_gmc.h"
#include "bc250_gfx.h"
#include "navi10_sdma_pkt_open.h"
#include "gc/gc_10_1_0_offset.h"
typedef unsigned char BOOLEAN;
typedef unsigned long ULONG;
typedef long LONG,NTSTATUS;
typedef int KIRQL;
#define TRUE 1
#define FALSE 0
#define _Inout_
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_INVALID_DEVICE_STATE -2
#define STATUS_DEVICE_NOT_READY -3
#define STATUS_IO_DEVICE_ERROR -4
#define STATUS_IO_TIMEOUT -5
#define STATUS_DEVICE_DATA_ERROR -6
#define NT_SUCCESS(x) ((x)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define KeMemoryBarrier() ((void)0)
#define BC250_GFX_STAGE_INTERRUPTS 8
#define BC250_FENCE_RING_SDMA0 10u
// Reduced host poll budget; production uses the unchanged100000us constant.
#define BC250_FENCE_TIMEOUT_US 8u
#define Executive 0
#define KernelMode 0
#define GuardLog(...) ((void)0)
typedef struct {NTSTATUS Fault;BOOLEAN Plan;} BC250_SEQUENCE;
typedef struct {BC250_SEQUENCE Sequence;BOOLEAN SetUp,PowerSuspended,Failed,PagingReady,PagingGate,SdmaFencePage;LONG SubmitFailed,PagingSubmitFailed,PagingSubmitInFlight,FenceSeq;ULONG StagesDone,PagingSubmitSeq,RingOwes[12],RingOwesSlot[12];struct amdgpu_ring*PagingRing;struct amdgpu_device*PagingDevicePtr;int Sdma0RingLock;} BC250_GFX;
typedef struct _BC250_DEVICE {BC250_GFX*Gfx;void*Wddm;BOOLEAN Started,FullWddm,GfxStopPrepared,GpuStopUnconfirmed,GfxTlbBootstrap,MmioGfxEnabled,GfxAccessClosed;int GfxPagingLock,GartLock,GfxAccessLock,GfxAccessDrained;} BC250_DEVICE;
#include "gfx_recovery.h"
static unsigned checks,failures,mode,resets,commits,undos,waits,delays,reads,modelUsers;
static int critical,pagingHeld,gartHeld,spinHeld,modelIrql,modelEnabled=1,pspLoaded=1,ihActive=1;
static u64 savedRptr,savedWptr;
static BC250_DEVICE*d;static BC250_GFX*g;static struct amdgpu_device adev;
static u32 ring0[64],ring1[64];static u64 slots[16];static u64 originalOtherSlots[6];
static const struct amdgpu_ring_funcs funcs={AMDGPU_RING_TYPE_SDMA,15,0};
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line%u mode%u: %s\n",(unsigned)__LINE__,mode,#x);}}while(0)
static int KeGetCurrentIrql(void){return modelIrql;}
static void KeEnterCriticalRegion(void){CHECK(!critical);critical=1;}
static void KeLeaveCriticalRegion(void){CHECK(critical&&!pagingHeld&&!gartHeld&&!spinHeld);critical=0;}
static void ExAcquirePushLockExclusive(int*p){(void)p;CHECK(critical&&!pagingHeld);pagingHeld=1;}
static void ExReleasePushLockExclusive(int*p){(void)p;CHECK(pagingHeld&&!gartHeld);pagingHeld=0;}
static void ExAcquireFastMutex(int*p){(void)p;CHECK(pagingHeld&&!gartHeld);gartHeld=1;}
static void ExReleaseFastMutex(int*p){(void)p;CHECK(gartHeld&&!spinHeld);gartHeld=0;}
static void KeAcquireSpinLock(int*p,KIRQL*i){CHECK(!spinHeld);*i=0;spinHeld=p==&g->Sdma0RingLock?2:1;}
static void KeReleaseSpinLock(int*p,KIRQL i){(void)p;(void)i;CHECK(spinHeld);spinHeld=0;}
static int KeWaitForSingleObject(int*e,int why,int type,int alert,void*t){(void)e;(void)why;(void)type;(void)alert;(void)t;CHECK(!spinHeld&&pagingHeld&&gartHeld&&d->GfxAccessClosed);modelUsers=0;waits++;return 0;}
static LONG InterlockedIncrement(volatile LONG*p){return ++*p;}
static LONG InterlockedExchange(volatile LONG*p,LONG v){LONG old=*p;*p=v;return old;}
static void KeStallExecutionProcessor(int us){CHECK(us==1&&!spinHeld);delays++;}
static int PspIsLoaded(BC250_DEVICE*x){CHECK(x==d&&gartHeld);return pspLoaded;}
static int IhIsActive(BC250_DEVICE*x){CHECK(x==d&&gartHeld);return ihActive;}
static NTSTATUS GartDevice(BC250_DEVICE*x,struct amdgpu_device**a,BOOLEAN*e){CHECK(x==d&&gartHeld);*a=&adev;*e=(BOOLEAN)modelEnabled;return 0;}
static void SequenceBegin(BC250_SEQUENCE*s,BC250_DEVICE*x,BOOLEAN plan,void*w,int n){CHECK(s==&g->Sequence&&x==d&&!plan&&!w&&!n&&d->GfxAccessClosed&&waits);s->Fault=0;s->Plan=plan;}
int bc250_sdma_reset_retained_instance(struct amdgpu_device*a,u32 instance,struct bc250_sdma_reset_receipt*r)
{
 CHECK(a==&adev&&!instance&&pagingHeld&&gartHeld&&!spinHeld&&d->GfxAccessClosed&&!modelUsers&&a->backend==&g->Sequence);resets++;
 r->instance=0;r->previous_wptr=a->sdma.instance[0].ring.wptr;r->stage=BC250_SDMA_RESET_PROGRAMMED;
 if(mode==1)return BC250_EIO;
 if(mode==2){r->stage=BC250_SDMA_RESET_VERIFY;return 0;}
 memset(ring0,0,sizeof(ring0));a->sdma.instance[0].ring.wptr=0;a->sdma.instance[0].ring.wptr_old=0;savedRptr=savedWptr=0;
 return 0;
}
int amdgpu_ring_alloc(struct amdgpu_ring*r,unsigned n){CHECK(spinHeld==2&&d->GfxAccessClosed&&resets&&r==&adev.sdma.instance[0].ring&&n==14);r->wptr_old=r->wptr;r->count_dw=(int)n;return mode==7?BC250_EINVAL:0;}
void amdgpu_ring_undo(struct amdgpu_ring*r){undos++;r->wptr=r->wptr_old;}
void amdgpu_ring_commit(struct amdgpu_ring*r)
{
 u64 content,fence;u32 pattern,seq;unsigned i;
 CHECK(r==&adev.sdma.instance[0].ring&&spinHeld==2&&r->wptr==14);commits++;
 CHECK(ring0[0]==(SDMA_PKT_HEADER_OP(SDMA_OP_WRITE)|SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR))&&ring0[3]==SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(1));
 content=(u64)ring0[1]|((u64)ring0[2]<<32);fence=(u64)ring0[7]|((u64)ring0[8]<<32);pattern=ring0[4];seq=ring0[9];
 CHECK(content==adev.sdma.fence_mem.mc+48&&fence==adev.sdma.fence_mem.mc+56&&ring0[5]==~pattern&&seq);
 CHECK(ring0[6]==(SDMA_PKT_HEADER_OP(SDMA_OP_FENCE)|SDMA_PKT_FENCE_HEADER_MTYPE(3))&&ring0[10]==ring0[6]);
 CHECK(((u64)ring0[11]|((u64)ring0[12]<<32))==fence+4&&ring0[13]==0);
 CHECK(slots[6]==~((u64)pattern|((u64)(~pattern)<<32))&&slots[7]==~(u64)seq);
 if(mode!=4&&mode!=6)slots[6]=(u64)pattern|((u64)(~pattern)<<32);
 if(mode==3)slots[7]=(u64)(seq-1);else if(mode!=6)slots[7]=seq;
 for(i=14;i<16;i++)ring0[i]=0;r->wptr=16;savedRptr=savedWptr=64;
 if(mode==5)savedRptr=60;
 if(mode==8)g->Sequence.Fault=STATUS_IO_DEVICE_ERROR;
}
u32 bc250_sdma_reg_offset(struct amdgpu_device*a,u32 instance,u32 reg){CHECK(a==&adev&&!instance);return reg;}
u32 bc250_shim_rreg(struct amdgpu_device*a,u32 reg)
{
 CHECK(a==&adev&&gartHeld&&pagingHeld&&!spinHeld&&d->GfxAccessClosed);reads++;
 switch(reg){case mmSDMA0_GFX_RB_RPTR:return (u32)savedRptr;case mmSDMA0_GFX_RB_RPTR_HI:return (u32)(savedRptr>>32);case mmSDMA0_GFX_RB_WPTR:return (u32)savedWptr;case mmSDMA0_GFX_RB_WPTR_HI:return (u32)(savedWptr>>32);default:CHECK(0);return ~0u;}
}
/* ACTUAL_SOURCE */
static void setup(BC250_DEVICE*x,BC250_GFX*y)
{
 unsigned i;memset(x,0,sizeof(*x));memset(y,0,sizeof(*y));memset(&adev,0,sizeof(adev));d=x;g=y;x->Gfx=y;x->FullWddm=1;x->MmioGfxEnabled=1;
 y->SetUp=1;y->PagingReady=1;y->PagingGate=1;y->SdmaFencePage=1;y->StagesDone=8;y->PagingDevicePtr=&adev;y->PagingRing=&adev.sdma.instance[0].ring;y->FenceSeq=91;y->PagingSubmitSeq=90;
 y->PagingSubmitInFlight=1;y->PagingSubmitFailed=1;y->RingOwes[10]=90;y->RingOwesSlot[10]=4;y->RingOwes[11]=73;
 adev.backend=(void*)0x1234;adev.sdma.num_instances=2;adev.sdma.fence_mem.cpu=slots;adev.sdma.fence_mem.mc=0x800000;adev.sdma.fence_mem.size=sizeof(slots);
 for(i=0;i<2;i++){struct amdgpu_ring*r=&adev.sdma.instance[i].ring;r->adev=&adev;r->funcs=&funcs;r->ring=i?ring1:ring0;r->me=i;r->wptr=32;r->wptr_old=31;r->buf_mask=63;r->ptr_mask=~0ull;r->max_dw=32;r->ring_size=256;}
 for(i=0;i<64;i++){ring0[i]=0xfefefefe;ring1[i]=0x12340000+i;}for(i=0;i<16;i++)slots[i]=0x9988776600000000ull+i;
 memcpy(originalOtherSlots,slots,sizeof(originalOtherSlots));resets=commits=undos=waits=delays=reads=0;modelUsers=3;mode=0;modelIrql=critical=pagingHeld=gartHeld=spinHeld=0;modelEnabled=pspLoaded=ihActive=1;
}
int main(void)
{
 BC250_DEVICE device;BC250_GFX gfx;BC250_SDMA0_RECOVERY_REPORT report;NTSTATUS st;unsigned i,m;struct amdgpu_ring sibling;
 setup(&device,&gfx);sibling=adev.sdma.instance[1].ring;
 st=GfxRecoverSdma0Unpublished(&device,&report);
 CHECK(st==0&&report.Ready&&report.Stage==BC250_SDMA0_RECOVERY_READY&&resets==1&&commits==1&&waits==1&&reads==4&&!delays);
 CHECK(!device.GfxAccessClosed&&!gfx.PagingSubmitFailed&&!gfx.PagingSubmitInFlight&&!gfx.RingOwes[10]);
 CHECK(gfx.PagingSubmitSeq==90&&gfx.FenceSeq==92&&report.ProbeSequence==92&&report.ObservedFence==92&&report.ExpectedContent==report.ObservedContent);
 CHECK(report.PriorPagingSeq==90&&report.PriorPagingInFlight==1&&report.PriorPagingFailed==1&&report.PriorRingOwes==90);
 CHECK(!memcmp(&sibling,&adev.sdma.instance[1].ring,sizeof(sibling))&&!memcmp(originalOtherSlots,slots,sizeof(originalOtherSlots))&&gfx.RingOwes[11]==73);
 for(i=0;i<64;i++)CHECK(ring1[i]==0x12340000+i);
 CHECK(adev.backend==(void*)0x1234&&gfx.PagingRing==&adev.sdma.instance[0].ring&&adev.sdma.fence_mem.cpu==slots);
 // Repeated private transaction consumes a distinct sequence and poisons old success.
 st=GfxRecoverSdma0Unpublished(&device,&report);CHECK(st==0&&report.ProbeSequence==93&&report.ObservedFence==93&&resets==2);
 for(m=1;m<=8;m++){
  setup(&device,&gfx);mode=m;st=GfxRecoverSdma0Unpublished(&device,&report);
  CHECK(st<0&&!report.Ready&&device.GfxAccessClosed&&gfx.PagingSubmitFailed&&gfx.PagingSubmitInFlight==1);
  CHECK(adev.backend==(void*)0x1234&&adev.sdma.fence_mem.cpu==slots&&gfx.PagingSubmitSeq==90&&gfx.RingOwes[11]==73);
  if(m==1||m==2)CHECK(commits==0&&gfx.RingOwes[10]==90);
  if(m==3||m==6)CHECK(st==STATUS_IO_TIMEOUT&&!reads); // stale/no fence cannot prove progress
  if(m==4)CHECK(st==STATUS_DEVICE_DATA_ERROR&&!reads); // fence alone is insufficient
  if(m==5)CHECK(st==STATUS_IO_TIMEOUT&&reads==4*BC250_FENCE_TIMEOUT_US); // queue must actually empty
 }
 setup(&device,&gfx);gfx.FenceSeq=-1;CHECK(GfxRecoverSdma0Unpublished(&device,&report)==0&&report.ProbeSequence==1);
 for(m=0;m<7;m++){
  setup(&device,&gfx);if(m==0)device.Started=1;if(m==1)device.Wddm=&gfx;if(m==2)gfx.SubmitFailed=1;if(m==3)device.GfxAccessClosed=1;if(m==4)gfx.Sequence.Fault=-99;if(m==5)modelIrql=2;if(m==6)adev.sdma.fence_mem.size=56;
  CHECK(GfxRecoverSdma0Unpublished(&device,&report)<0&&!resets&&!waits&&!commits&&!report.Ready);
 }
 printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
