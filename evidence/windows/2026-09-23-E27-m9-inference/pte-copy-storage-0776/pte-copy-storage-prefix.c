#include <stdio.h>
#include <string.h>
#define TRUE 1
#define true 1
#define PAGE_SIZE 4096u
#define BC250_MEM_VRAM 1
#define BC250_EINVAL (-22)
typedef unsigned long long u64;
struct bc250_mem { void*cpu;u64 mc;unsigned size; };
struct amdgpu_device {struct {u64 base;} doorbell;};
struct bc250_gfx_inputs {unsigned max_shader_engines,max_sh_per_se,max_cu_per_sh,max_backends_per_se;int async_gfx_ring,pp_gfxoff;};
typedef struct {int PagingGate,SetUp;struct bc250_mem PagingCopyStaging;struct {void*Device;} Sequence;} BC250_GFX;
static unsigned checks,failures,allocs,frees,gfxCalls,sdmaCalls,gfxFrees,sdmaFrees;
static int failGfx,failSdma,failAlloc,badSize,badAlign;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x);}} while(0)
static int bc250_gfx_setup(struct amdgpu_device*a,struct bc250_gfx_inputs*i){(void)a;CHECK(i->max_shader_engines==2 && i->max_cu_per_sh==10);gfxCalls++;return failGfx?-1:0;}
static int bc250_sdma_setup(struct amdgpu_device*a){(void)a;sdmaCalls++;return failSdma?-2:0;}
static void bc250_gfx_teardown(struct amdgpu_device*a){(void)a;gfxFrees++;}
static void bc250_sdma_teardown(struct amdgpu_device*a){(void)a;sdmaFrees++;}
static u64 GpuMemDoorbellBase(void*d){(void)d;return 0xABC000;}
static int bc250_shim_mem_alloc(struct amdgpu_device*a,int domain,unsigned size,unsigned align,struct bc250_mem*m){
 (void)a;allocs++;CHECK(domain==BC250_MEM_VRAM && size==4096 && align==4096);memset(m,0,sizeof(*m));
 if(failAlloc)return -12;
 m->cpu=(void*)1;m->mc=0x500000000ull+(badAlign?8:0);m->size=badSize?4095:4096;return 0;
}
static void bc250_shim_mem_free(struct amdgpu_device*a,struct bc250_mem*m){(void)a;CHECK(m->cpu==(void*)1);frees++;memset(m,0,sizeof(*m));}
