from pathlib import Path
import sys
r=Path(sys.argv[1]);out=Path(sys.argv[2]);source=(r/'driver/kmd/gpumem.c').read_text()
def get(marker):
 a=source.index(marker);b=source.index('{',a);i=b+1;depth=1
 while depth:
  if source[i]=='{':depth+=1
  elif source[i]=='}':depth-=1
  i+=1
 return source[a:i]
prefix=r"""
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define VOID void
#define TRUE 1
#define FALSE 0
#define PAGE_SIZE 4096
#define BC250_GPUMEM_MAX 64
#define NT_SUCCESS(x) ((x)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define GuardLog(...) ((void)0)
typedef unsigned int ULONG; typedef unsigned long long ULONGLONG;
typedef unsigned char BOOLEAN; typedef void* PVOID; typedef unsigned char* PUCHAR;
typedef struct { long long QuadPart; } PHYSICAL_ADDRESS;
typedef struct { int unused; } BC250_ESCAPE_DOORBELL;
typedef struct { void *GpuMem; int GfxTlbBootstrap; } BC250_DEVICE;
typedef struct { BC250_DEVICE *Device; int Plan,Fault,TraceRlcRetirement; } BC250_SEQUENCE;
struct amdgpu_device { void *backend; };
struct bc250_mem { void *cpu; unsigned long long mc; unsigned int size; };
static unsigned checks,failures;
#define CHECK(x) do {checks++; if(!(x)){failures++;printf("FAIL line %d\n",__LINE__);}}while(0)
"""
for tag in ['BC250_GPUMEM_ENTRY','BC250_GPUMEM']:
 prefix+=get('typedef struct _'+tag+' {')+' '+tag+';\n'
prefix+=r"""
static unsigned unbinds,flushes,releases,fail_unbind; static int fail_flush;
static unsigned long long offsets[8]; static unsigned pages_seen[8];
static int bc250_gart_unbind(struct amdgpu_device *a,unsigned long long off,unsigned pages,void *table){
 (void)a; (void)table;offsets[unbinds]=off;pages_seen[unbinds]=pages;unbinds++;
 return fail_unbind==unbinds ? -22 : 0;
}
static int FlushTlb(struct amdgpu_device *a,const BC250_SEQUENCE *seq){
 BC250_GPUMEM *m=seq->Device->GpuMem;(void)a;CHECK(m->Entries[0].Used && m->Entries[1].Used);
 CHECK(m->Entries[0].Bound && m->Entries[1].Bound);CHECK(!m->Entries[0].TranslationsRetired);
 flushes++;return fail_flush ? -62 : 0;
}
static void GfxTraceRlcState(BC250_DEVICE *d,const char *phase){(void)d;(void)phase;}
static void ReleaseEntry(BC250_GPUMEM *m,BC250_GPUMEM_ENTRY *e){(void)m;releases++;memset(e,0,sizeof(*e));}
"""
for name in ['static BC250_GPUMEM* MemOf(', 'int GpuMemRetireGttMappings(', 'void bc250_shim_mem_free(', 'void GpuMemRelease(']:prefix+=get(name)+'\n'
prefix+=r"""
int main(void){
 unsigned scenario;
 for(scenario=0;scenario<5;scenario++){
  BC250_GPUMEM mem={0}; BC250_DEVICE dev={&mem,0}; BC250_SEQUENCE seq={&dev,0,0,0},other={&dev,0,0,0};
  struct amdgpu_device adev={&seq}; BC250_GPUMEM_ENTRY saved[4];struct bc250_mem allocation;unsigned i;
  unbinds=flushes=releases=0;fail_unbind=scenario==1?2:0;fail_flush=scenario==2;
  for(i=0;i<4;i++) {BC250_GPUMEM_ENTRY *x=&mem.Entries[i];x->Used=1;x->Gtt=i!=3;x->Bound=i!=3;
   x->Owner=i==2?&other:&seq;x->Cpu=(void*)(uintptr_t)(i+1);x->Mc=0x100000+i*8192;
   x->GartOffset=0x400000+i*8192;x->Size=(i+1)*4096;}
  memcpy(saved,mem.Entries,sizeof(saved));
  if(scenario==3)seq.Plan=1;if(scenario==4)seq.Fault=-1;
  CHECK(GpuMemRetireGttMappings(&adev)==(scenario==0?0:scenario==1?-22:scenario==2?-62:-5));
  CHECK(releases==0);
  CHECK(!memcmp(&mem.Entries[2],&saved[2],2*sizeof(saved[0])));
  if(scenario==0){
   CHECK(unbinds==2 && flushes==1 && !mem.TlbDirty);
   CHECK(offsets[0]==saved[0].GartOffset && offsets[1]==saved[1].GartOffset);
   CHECK(pages_seen[0]==1 && pages_seen[1]==2);
   for(i=0;i<2;i++){saved[i].TranslationsRetired=1;CHECK(!memcmp(&saved[i],&mem.Entries[i],sizeof(saved[i])));}
   CHECK(GpuMemRetireGttMappings(&adev)==0 && unbinds==2 && flushes==1);
   allocation.cpu=mem.Entries[0].Cpu;allocation.mc=mem.Entries[0].Mc;allocation.size=mem.Entries[0].Size;
   bc250_shim_mem_free(&adev,&allocation);
   CHECK(unbinds==2 && flushes==1 && releases==0 && mem.Entries[0].Retired);
   GpuMemRelease(&dev,&seq,FALSE);CHECK(releases==0 && mem.Entries[0].Used);
   GpuMemRelease(&dev,&seq,TRUE);CHECK(releases==1 && !mem.Entries[0].Used && mem.Entries[1].Used);
  }else {CHECK(!memcmp(mem.Entries,saved,sizeof(saved)));CHECK(mem.TlbDirty==(scenario<3));}
 }
 printf("%u checks, %u failures\n",checks,failures);return failures?1:0;
}
"""
out.write_text(prefix)
