from pathlib import Path
import sys
r=Path(sys.argv[1]);out=Path(sys.argv[2])
def get(source,marker):
 a=source.index(marker);b=source.index('{',a);i=b+1;depth=1
 while depth:
  if source[i]=='{':depth+=1
  elif source[i]=='}':depth-=1
  i+=1
 return source[a:i]
g=(r/'driver/kmd/gfx.c').read_text();m=(r/'driver/kmd/gpumem.c').read_text()
s=r"""
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define VOID void
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(x) ((x)>=0)
static unsigned persisted;
static void GuardLogKeep(void){persisted++;}
static void GuardLog(const char*fmt,...){(void)fmt;}
#define BC250_EINVAL (-22)
#define BC250_GFX_STAGE_CP 6
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE (-2)
#define BC250_GPUMEM_MAX 64
#define AMDGPU_GFXHUB(x) 0
#define AMDGPU_MMHUB0(x) 8
typedef unsigned int u32;typedef unsigned int ULONG;typedef unsigned long long ULONGLONG;
typedef unsigned char BOOLEAN;typedef void* PVOID;typedef unsigned char* PUCHAR;
typedef int NTSTATUS;
typedef void (*bc250_gfx_checkpoint_fn)(const char*);
typedef struct {long long QuadPart;} PHYSICAL_ADDRESS;
typedef struct {int unused;} BC250_ESCAPE_DOORBELL;
typedef struct {void *GpuMem,*Gfx,*Wddm;int GfxTlbBootstrap,Started,FullWddm;} BC250_DEVICE;
typedef struct {BC250_DEVICE *Device;int Fault,Plan,TraceRlcRetirement,TraceBootstrapTlb;} BC250_SEQUENCE;
typedef struct {int SetUp,StagesDone;} BC250_GFX;
struct amdgpu_device {void *backend;};
static unsigned checks,bad,gfx_flushes,mm_flushes,cp_calls,releases;
static int halted=1,rlc_started,fail_flush,fail_stage,fault_flush,in_flush;
#define CHECK(x) do {checks++;if(!(x)){bad++;printf("FAIL line %d\n",__LINE__);}}while(0)
static int EnginesHalted(const BC250_DEVICE*d){(void)d;return halted;}
static void GfxTraceRlcState(const BC250_DEVICE*d,const char*p){(void)d;(void)p;CHECK(!in_flush);}
static int bc250_gmc_flush_gpu_tlb(struct amdgpu_device*a,unsigned vmid,unsigned hub,unsigned type){
 (void)a;(void)vmid;(void)type;CHECK(hub==8);mm_flushes++;return 0;
}
static int bc250_gmc_flush_gpu_tlb_observed(struct amdgpu_device*a,unsigned vmid,unsigned hub,unsigned type,
 void (*observe)(struct amdgpu_device*,const char*,unsigned)){
 BC250_SEQUENCE*q=a->backend;(void)vmid;(void)type;CHECK(hub==0);gfx_flushes++;
 if(q->Device->GfxTlbBootstrap)CHECK(rlc_started);
 in_flush=1;
 if(observe){
  observe(a,"after-invalidate-request",1);
  observe(a,"after-invalidate-request-read",1);
  observe(a,"after-invalidate-ack",1);
 }
 in_flush=0;
 if(fault_flush)q->Fault=-1;
 return fail_flush ? -62:0;
}
static int stage_other(struct amdgpu_device*a){(void)a;return 0;}
static int stage_rlc(struct amdgpu_device*a){(void)a;rlc_started=1;return fail_stage?-22:0;}
static int stage_cp(struct amdgpu_device*a){BC250_SEQUENCE*q=a->backend;CHECK(!q->Device->GfxTlbBootstrap);cp_calls++;return 0;}
static struct {int(*Run)(struct amdgpu_device*);} g_Stages[]={
 {stage_other},{stage_other},{stage_other},{stage_other},{stage_other},{stage_rlc},{stage_cp}};
"""
for tag in ['BC250_GPUMEM_ENTRY','BC250_GPUMEM']:
 s+=get(m,'typedef struct _'+tag+' {')+' '+tag+';\n'
s+='static void ReleaseEntry(BC250_GPUMEM*m,BC250_GPUMEM_ENTRY*e){(void)m;releases++;memset(e,0,sizeof(*e));}\n'
for marker in ['static BC250_GPUMEM* MemOf(', 'static void ObserveRetirementTlb(', 'static int FlushTlb(',
               'int GpuMemCompleteGfxBootstrap(', 'void GpuMemRelease(']:s+=get(m,marker)+'\n'
for marker in ['NTSTATUS GfxBeginTranslationBootstrap(', 'static int RunEngineStage(']:s+=get(g,marker)+'\n'
s+=r"""
static unsigned boundary_count;
static void boundary(const char*p){
 const char* expected[]={"before-rlc-resume","after-rlc-resume","before-gfx-visibility","after-gfx-visibility"};
 CHECK(boundary_count<4);
 if(boundary_count<4)CHECK(strcmp(p,expected[boundary_count])==0);
 if(boundary_count==0)CHECK(!rlc_started && gfx_flushes==0);
 if(boundary_count==1 || boundary_count==2)CHECK(rlc_started && gfx_flushes==0);
 if(boundary_count==3)CHECK(gfx_flushes==1);
 boundary_count++;
}
int main(void){
 unsigned scenario,i,traced;
 for(traced=0;traced<2;traced++)for(scenario=0;scenario<4;scenario++){
  BC250_GPUMEM mem={0};BC250_GFX gfx={0};BC250_DEVICE dev={0};BC250_SEQUENCE seq={0};
  struct amdgpu_device adev={&seq};
  dev.GpuMem=&mem;dev.Gfx=&gfx;dev.FullWddm=1;seq.Device=&dev;
  gfx_flushes=mm_flushes=cp_calls=releases=0;rlc_started=0;boundary_count=0;persisted=0;
  seq.TraceBootstrapTlb=(int)traced;
  fail_flush=scenario==1;fail_stage=scenario==2;fault_flush=scenario==3;
  CHECK(GfxBeginTranslationBootstrap(&dev)==0 && dev.GfxTlbBootstrap);
  for(i=0;i<3;i++)CHECK(FlushTlb(&adev,&seq)==0);
  CHECK(mm_flushes==3 && gfx_flushes==0 && mem.TlbDirty);
  mem.Entries[0].Used=mem.Entries[0].Gtt=mem.Entries[0].Retired=mem.Entries[0].Bound=1;
  mem.Entries[0].Owner=&seq;mem.Entries[0].Size=4096;
  GpuMemRelease(&dev,&seq,TRUE);CHECK(releases==0);
  CHECK(RunEngineStage(&dev,&adev,6,NULL)==-22 && cp_calls==0);
  for(i=1;i<5;i++)CHECK(RunEngineStage(&dev,&adev,i,NULL)==0 && dev.GfxTlbBootstrap);
  CHECK(RunEngineStage(&dev,&adev,5,traced?boundary:NULL)==(scenario==0?0:scenario==1?-62:scenario==2?-22:-5));
  CHECK(boundary_count==(traced?(scenario==2?2u:4u):0u));
  CHECK(persisted==(traced?(scenario==2?0u:scenario==0?10u:8u):0u));
  if(scenario==0){
   CHECK(!dev.GfxTlbBootstrap && !mem.TlbDirty && gfx_flushes==1 && mm_flushes==4);
   CHECK(RunEngineStage(&dev,&adev,6,NULL)==0 && cp_calls==1);
   CHECK(FlushTlb(&adev,&seq)==0 && gfx_flushes==2 && mm_flushes==5);
   GpuMemRelease(&dev,&seq,TRUE);CHECK(releases==1);
  }else{
   CHECK(dev.GfxTlbBootstrap && mem.TlbDirty);
   CHECK(RunEngineStage(&dev,&adev,6,NULL)==-22 && cp_calls==0);
   GpuMemRelease(&dev,&seq,TRUE);CHECK(releases==0);
  }
 }
 printf("%u bootstrap checks, %u failures\n",checks,bad);return bad?1:0;
}
"""
if '--omit-commit' in sys.argv:s=s.replace('result=GpuMemCompleteGfxBootstrap(Adev);','result=0;')
if '--omit-checkpoints' in sys.argv:s=s.replace('    int result;\n    if (Device->GfxTlbBootstrap', '    int result;\n    Checkpoint=NULL;\n    if (Device->GfxTlbBootstrap')
if '--observer-mmio' in sys.argv:s=s.replace('    // Observe only values', '    GfxTraceRlcState(sequence->Device,phase);\n    // Observe only values')
out.write_text(s)
