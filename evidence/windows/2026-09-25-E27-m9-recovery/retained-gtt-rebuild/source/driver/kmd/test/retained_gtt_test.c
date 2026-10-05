#include <stdio.h>
#include <string.h>
#include <stdint.h>
typedef unsigned long long ULONGLONG;
typedef unsigned ULONG;
#define TRUE 1
#define FALSE 0
#define PAGE_SIZE 4096u
#define BC250_GPUMEM_MAX 64
#define BC250_GPUMEM_GTT_FIRST 0x400000ull
#define BC250_GPUMEM_GTT_LIMIT 0x4000000ull
#define BC250_GPUMEM_TABLE_LENGTH 0x100000u
#define NT_SUCCESS(s) ((s)>=0)
typedef struct {int Used,Gtt,Bound,TranslationsRetired,Retired;const void*Owner;void*Cpu;ULONGLONG Mc,GartOffset;ULONG Size;} BC250_GPUMEM_ENTRY;
typedef struct {BC250_GPUMEM_ENTRY Entries[64];unsigned VramNext;ULONGLONG GttNext;unsigned VramLive,GttLive;void*Table;int TlbDirty;} BC250_GPUMEM;
typedef struct {int Plan,Fault;} BC250_SEQUENCE;
typedef struct {long long QuadPart;} PHYSICAL_ADDRESS;
struct amdgpu_device {struct {ULONGLONG gart_size,gart_start;}gmc;};
static BC250_GPUMEM memory;
static BC250_SEQUENCE modelSequence;
static ULONGLONG table[BC250_GPUMEM_TABLE_LENGTH/8],expected[BC250_GPUMEM_TABLE_LENGTH/8];
static unsigned checks,failures,binds,unbinds,barriers,failBind;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static BC250_GPUMEM*MemOf(struct amdgpu_device*a,BC250_SEQUENCE**s){(void)a;*s=&modelSequence;return &memory;}
static PHYSICAL_ADDRESS MmGetPhysicalAddress(void*p){PHYSICAL_ADDRESS a={(long long)(uintptr_t)p};return a;}
static void KeMemoryBarrier(void){barriers++;}
// Deliberately simple independent PTE oracle: binder API arguments/ownership,
// not AMD encoding. Actual AMD PTE encoding is tested by the existing shim suite.
static int bc250_gart_unbind(struct amdgpu_device*a,ULONGLONG off,unsigned pages,void*t){
 unsigned i;(void)a;CHECK(memory.TlbDirty && t==table && off==0 && pages==BC250_GPUMEM_GTT_LIMIT/PAGE_SIZE);unbinds++;
 for(i=0;i<pages;i++)table[off/PAGE_SIZE+i]=0;return 0;
}
static int bc250_gart_bind(struct amdgpu_device*a,ULONGLONG off,unsigned pages,const ULONGLONG*dma,void*t){
 (void)a;CHECK(memory.TlbDirty && t==table && pages==1);binds++;if(failBind && binds==failBind)return -22;
 table[off/PAGE_SIZE]=*dma|1;return 0;
}
/* ACTUAL_SOURCE */
static void setup(struct amdgpu_device*a){unsigned i;
 memset(&memory,0,sizeof(memory));memset(&modelSequence,0,sizeof(modelSequence));memory.Table=table;
 a->gmc.gart_size=0x20000000ull;a->gmc.gart_start=0x800000000ull;
 for(i=0;i<sizeof(table)/sizeof(table[0]);i++)table[i]=0xdead000000000000ull+i;
 memcpy(expected,table,sizeof(table));memset(expected,0,(size_t)(BC250_GPUMEM_GTT_LIMIT/PAGE_SIZE*8));
 binds=unbinds=barriers=failBind=0;
 for(i=0;i<6;i++){
 BC250_GPUMEM_ENTRY*e=&memory.Entries[i];e->Used=e->Gtt=e->Bound=1;e->Owner=e;
 e->Cpu=(void*)(uintptr_t)(0x10000000ull+i*0x200000ull);e->Size=(i+1)*PAGE_SIZE;
 e->GartOffset=BC250_GPUMEM_GTT_FIRST+i*0x10000;e->Mc=a->gmc.gart_start+e->GartOffset;
 }
 memory.Entries[2].Retired=1;memory.Entries[3].TranslationsRetired=1;
 memory.Entries[4].Bound=0;memory.Entries[5].Gtt=0;
}
int main(void){struct amdgpu_device a;BC250_GPUMEM before;unsigned i;
 setup(&a);before=memory;
 for(i=0;i<2;i++){unsigned j;BC250_GPUMEM_ENTRY*e=&memory.Entries[i];for(j=0;j<=i;j++)expected[e->GartOffset/PAGE_SIZE+j]=(ULONGLONG)(uintptr_t)e->Cpu+j*PAGE_SIZE+1;}
 CHECK(GpuMemRebuildRetainedGtt(&a)==0 && memory.TlbDirty && binds==3 && unbinds==1 && barriers==1);
 CHECK(!memcmp(table,expected,sizeof(table)));before.TlbDirty=1;CHECK(!memcmp(&memory,&before,sizeof(memory)));
 // A second loss recreates the same mapping identities and bytes.
 memset(table,0xa5,(size_t)(BC250_GPUMEM_GTT_LIMIT/PAGE_SIZE*8));CHECK(GpuMemRebuildRetainedGtt(&a)==0);CHECK(!memcmp(table,expected,sizeof(table)));
 setup(&a);modelSequence.Plan=1;CHECK(GpuMemRebuildRetainedGtt(&a)!=0 && !unbinds&&!binds);
 setup(&a);memory.Entries[1].Size++;CHECK(GpuMemRebuildRetainedGtt(&a)==-22&&!unbinds&&!binds);
 setup(&a);memory.Entries[1].GartOffset=BC250_GPUMEM_GTT_LIMIT;CHECK(GpuMemRebuildRetainedGtt(&a)==-22&&!unbinds);
 setup(&a);memory.Entries[1].Mc++;CHECK(GpuMemRebuildRetainedGtt(&a)==-22&&!unbinds);
 setup(&a);failBind=2;before=memory;CHECK(GpuMemRebuildRetainedGtt(&a)==-22 && memory.TlbDirty&&!barriers);before.TlbDirty=1;CHECK(!memcmp(&memory,&before,sizeof(memory)));
 printf("Retained GTT rebuild: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
