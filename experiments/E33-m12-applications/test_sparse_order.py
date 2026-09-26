from pathlib import Path
import argparse,os
# BC250_ROOT is the workspace root, by default the parent directory of this repository.
ROOT=os.environ.get("BC250_ROOT", str(Path(__file__).resolve().parents[2].parent))
args=argparse.ArgumentParser(description='Source-extracted sparse ordering model; not a GPU acceptance test.')
args.add_argument('--source',type=Path,required=True)
args.add_argument('--out',type=Path,required=True)
a=args.parse_args();w=a.out.resolve();w.mkdir(parents=True,exist_ok=True)
src=(a.source/'src/amd/vulkan/winsys/wddm2/radv_wddm2_bo.c').read_text()
a=src.index('struct radv_wddm2_sparse_group {');b=src.index('static VkResult\nradv_wddm2_bo_virtual_bind',a)
body=src[a:b]
prefix=r'''
#include <windows.h>
#include <d3dkmthk.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define NT_SUCCESS(s) ((s)>=0)
#define VK_SUCCESS 0
#define VK_ERROR_DEVICE_LOST -4
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
typedef int VkResult;
enum amd_ip_type {AMD_IP_GFX=0};
struct vk_sync_wait {uint64_t wait_value;};
struct radv_winsys_submit_info {enum amd_ip_type ip_type;uint32_t queue_index;};
struct radeon_winsys_ctx {int unused;};
struct radeon_winsys {VkResult (*cs_submit)(struct radeon_winsys_ctx*,const struct radv_winsys_submit_info*,uint32_t,const struct vk_sync_wait*,uint32_t,const void*);};
struct util_dynarray {void *data;unsigned size;};
#define util_dynarray_foreach(p,type,it) for(type *it=(p)->data; it<(type*)((char*)(p)->data+(p)->size);it++)
static void util_dynarray_init(struct util_dynarray *p,void *unused){(void)unused;p->data=NULL;p->size=0;}
static void util_dynarray_fini(struct util_dynarray *p){free(p->data);p->data=NULL;p->size=0;}
static void util_dynarray_clear(struct util_dynarray *p){p->size=0;}
static void *util_dynarray_grow_bytes(struct util_dynarray *p,unsigned count,size_t size){
 unsigned old=p->size;p->data=realloc(p->data,old+count*size);assert(p->data);p->size+=(unsigned)(count*size);return (char*)p->data+old;
}
#define util_dynarray_num_elements(p,type) ((p)->size/sizeof(type))
struct vk_wddm2_fence {uint32_t handle;uint64_t wait_value;};
struct radv_wddm2_queue {uint32_t context_h,handle;struct vk_wddm2_fence vm_fence;struct util_dynarray sparse_ops;bool sparse_batch_active;};
struct radv_wddm2_ctx {struct radeon_winsys_ctx base;struct {struct radv_wddm2_queue queue;} per_ip[1];};
#define RADV_WDDM2_PRT_CONTROL_MASK (1ull<<46)
#define RADEON_FLAG_READ_ONLY 1
#ifndef MIN2
#define MIN2(a,b) ((a)<(b)?(a):(b))
#endif
struct radeon_winsys_bo {uint64_t va;uint32_t handle;uint64_t size;};
struct radv_wddm2_winsys {struct radeon_winsys base;uint32_t device_h;struct {struct radeon_winsys_bo *bo;} null_prt;};
struct radv_wddm2_bo {struct radeon_winsys_bo base;unsigned flags;bool emulate_sparse_residency;};
static struct radv_wddm2_winsys *radv_wddm2_winsys(struct radeon_winsys *p){return (void*)p;}
enum {APP_WAIT,BOUNDARY,MAP_WAIT,DRAW,APP_SIGNAL};
struct command {unsigned op;uint64_t value;unsigned a,b;};
static struct command stream[64];static unsigned head,tail;
static uint64_t app_value,vm_value,signal_value;
static unsigned pages[2],low_pages[2],draws,updates,map_count,wait_calls,hw;
static uint64_t map_wait;static unsigned paired;
static D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION pending_ops[8][600];
static unsigned pending_counts[8],pending_head,pending_tail;
static uint64_t pending_fences[8];
static void push(unsigned op,uint64_t value,unsigned a,unsigned b){assert(tail<64);stream[tail++]=(struct command){op,value,a,b};}
static void pump(void){
 bool progress;
 do{
  progress=false;
  if(pending_head<pending_tail && vm_value>=pending_fences[pending_head]){
   for(unsigned i=0;i<pending_counts[pending_head];i++){
    D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION *o=&pending_ops[pending_head][i];
    if(o->OperationType==D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT){
     uint64_t va=o->MapProtect.BaseAddress;
     unsigned page=(unsigned)(((va & ~RADV_WDDM2_PRT_CONTROL_MASK)-0x200000000ull)/65536);
     assert(page<2 && o->MapProtect.SizeInBytes==65536);
     if(paired && !(va & RADV_WDDM2_PRT_CONTROL_MASK)) {
      low_pages[page]=o->MapProtect.hAllocation;
      if(low_pages[page]==303)assert(!o->MapProtect.Protection.Write && !o->MapProtect.AllocationOffsetInBytes);
     } else pages[page]=o->MapProtect.hAllocation;
    }else{
     assert(o->OperationType==D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP && o->Unmap.Protection.Zero);
     unsigned page=(unsigned)(((o->Unmap.BaseAddress & ~RADV_WDDM2_PRT_CONTROL_MASK)-0x200000000ull)/65536);
     assert(page<2 && o->Unmap.SizeInBytes==65536);pages[page]=0;
    }
   }
   vm_value=pending_fences[pending_head]+1;pending_head++;progress=true;
  }
  if(head<tail){
   struct command c=stream[head];
   if(c.op==APP_WAIT && app_value<c.value)continue;
   if(c.op==MAP_WAIT && vm_value<c.value)continue;
   if(c.op==BOUNDARY)vm_value=c.value;
   if(c.op==DRAW){assert(pages[0]==c.a && pages[1]==c.b);
    if(paired){assert(low_pages[0]==(c.a?c.a:303) && low_pages[1]==(c.b?c.b:303));}
    draws++;}
   if(c.op==APP_SIGNAL)signal_value=c.value;
   head++;progress=true;
  }
 }while(progress);
}
static VkResult mock_submit(struct radeon_winsys_ctx *ctx,const struct radv_winsys_submit_info *info,
 uint32_t count,const struct vk_sync_wait *waits,uint32_t signals,const void *signal){
 (void)ctx;(void)signal;assert(info->ip_type==AMD_IP_GFX && info->queue_index==0 && signals==0);
 for(unsigned i=0;i<count;i++){push(APP_WAIT,waits[i].wait_value,0,0);wait_calls++;}
 pump();return VK_SUCCESS;
}
static NTSTATUS boundary_signal(uint32_t handle,uint64_t value){
 assert(handle==44);push(BOUNDARY,value,0,0);pump();return 0;
}
static NTSTATUS completion_wait(uint32_t handle,uint64_t value){
 assert(handle==44);push(MAP_WAIT,value,0,0);pump();return 0;
}
static NTSTATUS mock_SignalSynchronizationObjectFromGpu2(const D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *p){
 assert(!hw && p->BroadcastContextCount==1 && *p->BroadcastContextArray==22 && p->ObjectCount==1);
 return boundary_signal(*p->ObjectHandleArray,*p->MonitoredFenceValueArray);
}
static NTSTATUS mock_SubmitSignalSyncObjectsToHwQueue(const D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE *p){
 assert(hw && p->BroadcastHwQueueCount==1 && *p->BroadcastHwQueueArray==55 && p->ObjectCount==1);
 return boundary_signal(*p->ObjectHandleArray,*p->FenceValueArray);
}
static NTSTATUS mock_UpdateGpuVirtualAddress(const D3DKMT_UPDATEGPUVIRTUALADDRESS *p){
 assert(pending_tail<8 && p->hDevice==33 && p->hContext==22 && p->hFenceObject==44);
 assert(!p->Flags.DoNotWait && p->NumOperations && p->NumOperations<=600);
 /* Model the Windows contract: a call never spans low/high reservations. */
 uint64_t view=p->Operations[0].MapProtect.BaseAddress & RADV_WDDM2_PRT_CONTROL_MASK;
 for(unsigned i=0;i<p->NumOperations;i++)
  assert((p->Operations[i].MapProtect.BaseAddress & RADV_WDDM2_PRT_CONTROL_MASK)==view);
 memcpy(pending_ops[pending_tail],p->Operations,p->NumOperations*sizeof(pending_ops[0][0]));
 pending_counts[pending_tail]=p->NumOperations;pending_fences[pending_tail]=p->FenceValue;
 pending_tail++;updates++;pump();return 0;
}
static NTSTATUS mock_WaitForSynchronizationObjectFromGpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *p){
 assert(!hw && p->hContext==22 && p->ObjectCount==1);
 return completion_wait(*p->ObjectHandleArray,*p->MonitoredFenceValueArray);
}
static NTSTATUS mock_SubmitWaitForSyncObjectsToHwQueue(const D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE *p){
 assert(hw && p->hHwQueue==55 && p->ObjectCount==1);
 return completion_wait(*p->ObjectHandleArray,*p->FenceValueArray);
}
#define WDDM2_DISPATCH(call) mock_##call
'''
tests=r'''
static void run_case(unsigned hardware,unsigned already_signaled,unsigned alias){
 struct radv_wddm2_winsys ws={0};struct radv_wddm2_ctx ctx={0};
 struct radv_wddm2_queue *q=&ctx.per_ip[0].queue;
 struct radv_wddm2_bo parent={0},a={0},b={0};struct radeon_winsys_bo zero={0};
 struct vk_sync_wait waits[1]={{5}};
 ws.base.cs_submit=mock_submit;ws.device_h=33;q->context_h=22;q->handle=hardware?55:0;q->vm_fence.handle=44;
 parent.base.va=0x200000000ull|(alias?RADV_WDDM2_PRT_CONTROL_MASK:0);a.base.handle=101;b.base.handle=202;
 parent.emulate_sparse_residency=alias;paired=alias;zero.handle=303;zero.size=65536;ws.null_prt.bo=&zero;
 head=tail=updates=map_count=draws=wait_calls=pending_head=pending_tail=0;hw=hardware;
 app_value=already_signaled?5:0;vm_value=signal_value=0;pages[0]=low_pages[0]=11;pages[1]=low_pages[1]=22;
 push(DRAW,0,11,22); /* Prior render must see the old mapping. */
 assert(radv_wddm2_virtual_bind_begin(&ws.base,&ctx.base,AMD_IP_GFX)==0);
 assert(radv_wddm2_virtual_bo_map(&ws,&ctx,AMD_IP_GFX,&parent,0,65536,&a,0)==0);
 assert(radv_wddm2_virtual_bo_unmap(&ws,&ctx,AMD_IP_GFX,&parent,65536,65536)==0);
 assert(updates==0);
 assert(radv_wddm2_virtual_bind_end(&ws.base,&ctx.base,AMD_IP_GFX,0,1,waits,true)==0);
 assert(updates==(alias?2:1) && q->vm_fence.wait_value==(alias?3:2) && wait_calls==1);
 push(DRAW,0,101,0);push(APP_SIGNAL,1,0,0);pump();
 if(!already_signaled){assert(draws==1 && !signal_value && pages[0]==11 && pages[1]==22);}
 app_value=5;pump();assert(draws==2 && signal_value==1 && vm_value==(alias?3:2));
 /* Rebind the same VA with an independently delayed dependency. */
 waits[0].wait_value=7;
 assert(radv_wddm2_virtual_bind_begin(&ws.base,&ctx.base,AMD_IP_GFX)==0);
 for(unsigned i=0;i<257;i++)
  assert(radv_wddm2_virtual_bo_map(&ws,&ctx,AMD_IP_GFX,&parent,65536,65536,&b,0)==0);
 assert(radv_wddm2_virtual_bind_end(&ws.base,&ctx.base,AMD_IP_GFX,0,1,waits,true)==0);
 assert(updates==(alias?4:2) && q->vm_fence.wait_value==(alias?6:4) && wait_calls==2);
 push(DRAW,0,101,202);push(APP_SIGNAL,2,0,0);pump();
 assert(signal_value==1 && pages[1]==0);
 app_value=7;pump();assert(draws==3 && signal_value==2 && vm_value==(alias?6:4));
 /* Empty transactions still consume their input dependency exactly once. */
 waits[0].wait_value=9;
 assert(radv_wddm2_virtual_bind_begin(&ws.base,&ctx.base,AMD_IP_GFX)==0);
 assert(radv_wddm2_virtual_bind_end(&ws.base,&ctx.base,AMD_IP_GFX,0,1,waits,true)==0);
 push(APP_SIGNAL,3,0,0);pump();assert(signal_value==2 && updates==(alias?4:2));
 app_value=9;pump();assert(signal_value==3 && wait_calls==3 && q->vm_fence.wait_value==(alias?6:4));
 assert(radv_wddm2_virtual_bind_begin(&ws.base,&ctx.base,AMD_IP_GFX)==0);
 assert(radv_wddm2_virtual_bo_unmap(&ws,&ctx,AMD_IP_GFX,&parent,0,65536)==0);
 assert(radv_wddm2_virtual_bind_end(&ws.base,&ctx.base,AMD_IP_GFX,0,0,NULL,false)==0);
 pump();assert(updates==(alias?4:2) && pages[0]==101 && head==tail && !q->sparse_batch_active && q->sparse_ops.size==0);
 free(q->sparse_ops.data);
}
int main(void){
 for(unsigned alias=0;alias<2;alias++)for(unsigned hwq=0;hwq<2;hwq++)for(unsigned signaled=0;signaled<2;signaled++)run_case(hwq,signaled,alias);
 puts("PASS: paired high/low mappings and shared zero backing; legacy/HW queues, early/delayed application waits, old/new mapping draws, batch257, reservation-specific batches and chained fences, empty/cancelled batches");
 return 0;
}
'''
(w/'order-test.c').write_text(prefix+body+tests,newline='\n')
cmd='@echo off\ncall "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat"\nif errorlevel 1 exit /b 1\nset TEMP=ROOTDIR\\scratch\\tmp\nset TMP=%TEMP%\ncl /nologo /TC /W3 /I ROOTDIR\\toolchain\\nuget\\microsoft.windows.wdk.x64\\c\\Include\\10.0.26100.0\\um /I ROOTDIR\\toolchain\\nuget\\microsoft.windows.wdk.x64\\c\\Include\\10.0.26100.0\\shared /FoOUTDIR\\order-test.obj /FeOUTDIR\\order-test.exe OUTDIR\\order-test.c\nif errorlevel 1 exit /b 1\nOUTDIR\\order-test.exe\nexit /b %errorlevel%\n'
cmd=cmd.replace('ROOTDIR',ROOT).replace('OUTDIR',str(w))
(w/'run.cmd').write_text(cmd,newline='\n')
print('Generated source-extracted ordering test')
