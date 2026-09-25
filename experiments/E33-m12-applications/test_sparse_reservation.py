from pathlib import Path
import argparse
args=argparse.ArgumentParser()
args.add_argument('--source',type=Path,required=True)
args.add_argument('--out',type=Path,required=True)
a=args.parse_args();w=a.out.resolve();w.mkdir(parents=True,exist_ok=True)
source=(a.source/'src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c').read_text()
a=source.index('static uint64_t\nradv_wddm2_reserve_va_range');b=source.index('/* Same two-view policy',a)
body=source[a:b]
destroy=source[source.index('static void\nradv_wddm2_bo_destroy'):]
a=destroy.index('   const D3DKMT_FREEGPUVIRTUALADDRESS unmap');b=destroy.index('\n\n   if (!bo->base.is_virtual)',a)
release=destroy[a:b]
prefix=r'''
#include <windows.h>
#include <d3dkmthk.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#define NT_SUCCESS(s) ((s)>=0)
#define MAX2(a,b) ((a)>(b)?(a):(b))
#define align64(a,b) (((a)+(b)-1)&~((uint64_t)(b)-1))
#define CALLOC_STRUCT(t) ((struct t*)calloc(1,sizeof(struct t)))
#define FREE free
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
typedef int VkResult;
enum radeon_bo_flag { RADEON_FLAG_32BIT=1, RADEON_FLAG_REPLAYABLE=2 };
enum radeon_bo_domain { RADEON_DOMAIN_VRAM=1 };
#define RADV_WDDM2_HEAP_START 0x200000000ull
#define RADV_WDDM2_32BIT_HEAP_START 0x100000000ull
#define RADV_WDDM2_REPLAY_HEAP_START (1ull<<45)
struct radeon_winsys {int unused;};
struct radeon_winsys_bo {enum radeon_bo_domain initial_domain;bool is_virtual;uint64_t va,size;};
struct radv_wddm2_winsys {struct radeon_winsys base;D3DKMT_HANDLE adapter_h,paging_queue_h,device_h,paging_fence_h;};
struct radv_wddm2_bo {struct radeon_winsys_bo base;struct radv_wddm2_winsys *ws;enum radeon_bo_flag flags;uint64_t reserved_va,reserved_size;};
static struct radv_wddm2_winsys *radv_wddm2_winsys(struct radeon_winsys *p){return (struct radv_wddm2_winsys*)p;}
static uint64_t chosen, mapped, reserved, reservation_size, released, release_size;
static unsigned phase;
static NTSTATUS mock_ReserveGpuVirtualAddress(D3DDDI_RESERVEGPUVIRTUALADDRESS *p){
 assert(phase++==0);assert(p->hAdapter==11);
 assert(p->Size%65536==0);assert(!p->Reserved0 && !p->Reserved1);
 reserved=p->BaseAddress?p->BaseAddress:chosen;reservation_size=p->Size;
 assert(p->BaseAddress || (reserved>=p->MinimumAddress && reserved+p->Size<=p->MaximumAddress));
 p->VirtualAddress=reserved;return 0;
}
static NTSTATUS mock_MapGpuVirtualAddress(D3DDDI_MAPGPUVIRTUALADDRESS *p){
 assert(phase++==1);assert(p->hPagingQueue==22 && !p->hAllocation && p->Protection.Zero);
 mapped=p->BaseAddress;assert(mapped>=reserved && mapped+p->SizeInPages*4096<=reserved+reservation_size);
 p->PagingFenceValue=77;return 0x103; /* STATUS_PENDING is success; it still requires a wait. */
}
static NTSTATUS mock_WaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *p){
 assert(phase++==2);assert(p->hDevice==33 && p->ObjectCount==1);
 assert(*p->ObjectHandleArray==44 && *p->FenceValueArray==77);return 0;
}
static NTSTATUS mock_FreeGpuVirtualAddress(const D3DKMT_FREEGPUVIRTUALADDRESS *p){
 assert(phase++==3);assert(p->hAdapter==11);released=p->BaseAddress;release_size=p->Size;return 0;
}
#define WDDM2_DISPATCH(call) mock_##call
'''
tests=r'''
static void check_case(uint64_t size,unsigned alignment,uint64_t expected_va,uint64_t expected_reserved_size,uint64_t replay) {
 struct radv_wddm2_winsys ws={0};struct radeon_winsys_bo *base=NULL;
 ws.adapter_h=11;ws.paging_queue_h=22;ws.device_h=33;ws.paging_fence_h=44;phase=0;
 struct radv_wddm2_bo storage={0};struct radv_wddm2_bo *bo=&storage;
 storage.base.size=size;storage.base.is_virtual=true;base=&storage.base;
 base->va=radv_wddm2_reserve_va_range(&ws,bo,size,alignment,
     replay?RADEON_FLAG_REPLAYABLE:0,replay);
 assert(base->va && phase==3);
 assert(base->va==expected_va && mapped==expected_va);
 assert(bo->reserved_va==reserved && bo->reserved_size==expected_reserved_size);
 release_actual(&ws,bo);
 assert(phase==4 && released==reserved && release_size==expected_reserved_size);

}
int main(void){
 chosen=0x200010000ull;
 check_case(65536,65536,0x200010000ull,65536,0);
 check_case(1048576,524288,0x200080000ull,1048576+524288-65536,0);
 check_case(5ull<<30,65536,0x200010000ull,5ull<<30,0);
 check_case(1048576,524288,0x200000080000ull,1048576,0x200000080000ull);
 puts("PASS: 64KiB, interior 512KiB alignment, 5GiB range, exact replay; asynchronous fence precedes return; full reservation released");
 return 0;
}
'''
(w/'reserve-test.c').write_text(prefix+body+'\nstatic void release_actual(struct radv_wddm2_winsys *ws,struct radv_wddm2_bo *bo){NTSTATUS status;\n'+release+'\n(void)status;}\n'+tests,newline='\n')
cmd=r'''@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cl /nologo /TC /W3 /I P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um /I P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared /FoP:\bc-250\scratch\m12\sparse-reserve\reserve-test.obj /FeP:\bc-250\scratch\m12\sparse-reserve\reserve-test.exe P:\bc-250\scratch\m12\sparse-reserve\reserve-test.c
if errorlevel 1 exit /b 1
P:\bc-250\scratch\m12\sparse-reserve\reserve-test.exe
exit /b %errorlevel%
'''
cmd=cmd.replace('P:\\bc-250\\scratch\\m12\\sparse-reserve',str(w))
(w/'run.cmd').write_text(cmd,newline='\n')
print('Extracted production reserve and release call using actual WDK declarations')
