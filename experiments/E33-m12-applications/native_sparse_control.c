// Laboratory probe. The --holes run bugchecked on KMD147 (M483).
// Default runs bound content only; no Vulkan capability acceptance is implied.

#define main kmtprobe_original_main
#include "../../tools/win/kmtprobe/kmtprobe.c"
#undef main
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#define VK_SUCCESS 0
#define VK_ERROR_DEVICE_LOST -4
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
typedef int VkResult;
enum amd_ip_type {AMD_IP_GFX=0};
struct vk_sync_wait {uint64_t wait_value;D3DKMT_HANDLE handle;};
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
struct radv_wddm2_winsys {struct radeon_winsys base;uint32_t device_h,adapter_h,paging_queue_h,paging_fence_h;struct {struct radeon_winsys_bo *bo;} null_prt;};
struct radv_wddm2_bo {struct radeon_winsys_bo base;unsigned flags;bool emulate_sparse_residency;uint64_t sparse_high_va;};
static struct radv_wddm2_winsys *radv_wddm2_winsys(struct radeon_winsys *p){return (void*)p;}
static NTSTATUS NativeStatus(const char *name,NTSTATUS status){return Report(name,status);}
#define WDDM2_DISPATCH(call) NativeStatus(#call,D3DKMT##call)
static VkResult NativeWaits(struct radeon_winsys_ctx *_ctx,const struct radv_winsys_submit_info *submit,
                           uint32_t count,const struct vk_sync_wait *waits,uint32_t signal_count,const void *signals)
{
    struct radv_wddm2_ctx *ctx=(void*)_ctx;
    if(!count)return VK_SUCCESS;
    assert(count==1 && signal_count==0);
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait={0};
    wait.hContext=ctx->per_ip[submit->ip_type].queue.context_h;
    wait.ObjectCount=1;wait.ObjectHandleArray=&waits[0].handle;wait.MonitoredFenceValueArray=&waits[0].wait_value;
    return NT_SUCCESS(Report("Application GPU wait",D3DKMTWaitForSynchronizationObjectFromGpu(&wait)))?VK_SUCCESS:VK_ERROR_DEVICE_LOST;
}
static BOOL CpuRelease(D3DKMT_HANDLE device,D3DKMT_HANDLE handle)
{
    UINT64 value=1;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU signal={0};
    signal.hDevice=device;signal.ObjectCount=1;signal.ObjectHandleArray=&handle;signal.FenceValueArray=&value;
    return NT_SUCCESS(Report("Application CPU signal",D3DKMTSignalSynchronizationObjectFromCpu(&signal)));
}
#include "radv_sparse_helpers.inc"

/* PM4 source: Mesa ac_emit_cp_copy_data; field values from imported AMD nvd.h (MIT). */
#undef PACKET_TYPE3
#undef PACKET3
#undef PACKET3_NOP
#undef PACKET3_SET_UCONFIG_REG
#undef PACKET3_SET_UCONFIG_REG_START
#undef PACKET3_SET_UCONFIG_REG_END
#include "../../driver/amdgpu-import/nvd.h"
#include <emmintrin.h>

/* Mesa radv_cp_dma.c uses DMA_DATA for bulk copies; COPY_DATA is a
 * different access path and its PRT behavior is not implied by CP DMA support. */
static BOOL g_UseDma;static UINT64 g_ContentFence;
static BOOL CopyContent(PROBE *p,PROBE *completion,BUFFER *ib,UINT64 source,
                        UINT64 destination,UINT64 sequence)
{
    sequence=++g_ContentFence;
    UINT32 *dw;
    D3DKMT_SUBMITCOMMAND submit={0};
    if(!LockBuffer(p,ib))return FALSE;
    dw=(UINT32*)ib->Locked;
    if(g_UseDma) {
        /* Mesa radv_cs_emit_cp_dma, GFX10, CP_DMA_SYNC | CP_DMA_USE_L2.
         * Seven dwords plus packet2 padding. nvd.h defines L2 selector3. */
        dw[0]=PACKET3(PACKET3_DMA_DATA,5);
        dw[1]=(UINT32)PACKET3_DMA_DATA_CP_SYNC |
              PACKET3_DMA_DATA_SRC_SEL(3u) | PACKET3_DMA_DATA_DST_SEL(3u);
        dw[2]=(UINT32)source;dw[3]=(UINT32)(source>>32);
        dw[4]=(UINT32)destination;dw[5]=(UINT32)(destination>>32);
        dw[6]=8;dw[7]=BC250_CP_NOP;
    } else {
    /* Six COPY_DATA dwords plus two packet2 NOPs, matching GFX IB alignment. */
    dw[0]=PACKET3(PACKET3_COPY_DATA,4);
    dw[1]=PACKET3_COPY_DATA__SRC_SEL(PACKET3_COPY_DATA__SRC_SEL__TC_L2_OBSOLETE) |
          PACKET3_COPY_DATA__DST_SEL(PACKET3_COPY_DATA__DST_SEL__TC_L2_OBSOLETE) |
          PACKET3_COPY_DATA__COUNT_SEL(PACKET3_COPY_DATA__COUNT_SEL__64_BITS_OF_DATA) |
          PACKET3_COPY_DATA__WR_CONFIRM(PACKET3_COPY_DATA__WR_CONFIRM__WAIT_FOR_CONFIRMATION);
    dw[2]=(UINT32)source;dw[3]=(UINT32)(source>>32);
    dw[4]=(UINT32)destination;dw[5]=(UINT32)(destination>>32);
    dw[6]=BC250_CP_NOP;dw[7]=BC250_CP_NOP;
    }
    _mm_sfence();
    if(!UnlockBuffer(p,ib))return FALSE;
    submit.Commands=ib->MappedVa;submit.CommandLength=32;
    submit.BroadcastContextCount=1;submit.BroadcastContext[0]=p->hContext;
    if(!NT_SUCCESS(Report(g_UseDma?"Content DMA_DATA SubmitCommand":"Content COPY_DATA SubmitCommand",D3DKMTSubmitCommand(&submit)))||
       !StepSignalFence(completion,sequence)||!StepWaitFence(completion,sequence))return FALSE;
    return TRUE;
}

static BOOL ReadContent(PROBE *p,PROBE *completion,BUFFER *ib,UINT64 source,
                        UINT64 expected,UINT64 sequence,const char *label)
{
    UINT64 destination=p->Data.MappedVa+131072+sequence*8,observed;
    if(!CopyContent(p,completion,ib,source,destination,sequence))return FALSE;
    if(!LockBuffer(p,&p->Data))return FALSE;
    observed=*(volatile UINT64*)((char*)p->Data.Locked+131072+sequence*8);
    Note("CONTENT %s observed=0x%016llX expected=0x%016llX %s",
         label,observed,expected,observed==expected?"PASS":"FAIL");
    if(!UnlockBuffer(p,&p->Data))return FALSE;
    return observed==expected;
}

int main(int argc,char **argv)
{
    BOOL holes=FALSE,writeHole=FALSE,highVa=FALSE,paired=FALSE;
    int arg;
    for(arg=1;arg<argc;arg++) {
        if(strcmp(argv[arg],"--holes")==0)holes=TRUE;
        else if(strcmp(argv[arg],"--dma")==0)g_UseDma=TRUE;
        else if(strcmp(argv[arg],"--write-hole")==0)writeHole=TRUE;
        else if(strcmp(argv[arg],"--high")==0)highVa=TRUE;
        else if(strcmp(argv[arg],"--paired")==0)paired=TRUE;
        else {puts("usage: native-sparse-control [--holes] [--dma] [--write-hole] [--high] [--paired]");return 2;}
    }
    if(writeHole && (!holes || !g_UseDma)){puts("--write-hole requires --holes --dma");return 2;}
    printf("Access path: %s\n",g_UseDma?"CP DMA_DATA":"CP COPY_DATA");
    PROBE p={0},completion={0};BUFFER ib={0},zeroBacking={0};
    BOOL ok=FALSE,released=FALSE;
    const UINT64 a=0x13579BDF2468ACE0ull,b=0xFEDCBA9876543210ull;
    struct radv_wddm2_winsys ws={0};struct radv_wddm2_ctx ctx={0};
    struct radv_wddm2_queue *q=&ctx.per_ip[AMD_IP_GFX].queue;
    struct radv_wddm2_bo parent={0},physical={0};struct radeon_winsys_bo zeroBo={0};
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 input={0};
    D3DKMT_CREATECONTEXTVIRTUAL context={0};
    struct vk_sync_wait app_wait={0};D3DDDI_MAPGPUVIRTUALADDRESS zero={0};
    p.Opt.Match="bc250";p.Opt.FenceTimeoutMs=5000;
    p.Data.Name="physical-control";p.Data.Size=196608;p.Data.RequestedVa=0x200000000ull;
    ib.Name="copy-ib";ib.Size=65536;ib.RequestedVa=0x200040000ull;
    p.Command.Name="virtual-control";p.Command.Size=65536;p.Command.RequestedVa=0x200060000ull;
    if(highVa && !paired)p.Command.RequestedVa|=1ull<<46;
    setvbuf(stdout,NULL,_IONBF,0);if(!StartWatchdog(30000))return 2;
    if(!StepFindAdapter(&p)||!StepOpenAdapter(&p)||!StepCreateDevice(&p)||!StepCreatePagingQueue(&p)||
       !StepCreateAllocation(&p,&p.Data)||!StepMakeResident(&p,&p.Data)||!StepReserveVa(&p,&p.Data)||
       !StepMapVa(&p,&p.Data)||!StepReserveVa(&p,&p.Command)||
       !StepCreateAllocation(&p,&ib)||!StepMakeResident(&p,&ib)||!StepReserveVa(&p,&ib)||!StepMapVa(&p,&ib))goto done;
    if(!LockBuffer(&p,&p.Data))goto done;
    memset(p.Data.Locked,0xAC,(size_t)p.Data.Size);
    *(volatile UINT64*)p.Data.Locked=a;
    *(volatile UINT64*)((char*)p.Data.Locked+65536)=b;
    _mm_sfence();if(!UnlockBuffer(&p,&p.Data))goto done;
    zero.hPagingQueue=p.hPagingQueue;zero.BaseAddress=p.Command.ReservedBase;zero.SizeInPages=16;zero.Protection.Zero=1;
    if(!NT_SUCCESS(Report("Initial zero mapping",D3DKMTMapGpuVirtualAddress(&zero)))||
       !WaitPagingFence(&p,zero.PagingFenceValue,"initial zero"))goto done;
    ws.base.cs_submit=NativeWaits;ws.device_h=p.hDevice;ws.adapter_h=p.hAdapter;
    ws.paging_queue_h=p.hPagingQueue;ws.paging_fence_h=p.hPagingFenceObject;
    parent.base.va=p.Command.ReservedBase;parent.base.size=p.Command.Size;
    parent.emulate_sparse_residency=paired;
    if(paired) {
        zeroBacking.Name="real-zero";zeroBacking.Size=65536;zeroBacking.RequestedVa=0x200080000ull;
        if(!StepCreateAllocation(&p,&zeroBacking)||!StepMakeResident(&p,&zeroBacking)||
           !StepReserveVa(&p,&zeroBacking)||!StepMapVa(&p,&zeroBacking)||!LockBuffer(&p,&zeroBacking))goto done;
        memset(zeroBacking.Locked,0,(size_t)zeroBacking.Size);_mm_sfence();
        if(!UnlockBuffer(&p,&zeroBacking))goto done;
        zeroBo.handle=zeroBacking.hAllocation;zeroBo.size=zeroBacking.Size;ws.null_prt.bo=&zeroBo;
        if(!radv_wddm2_init_sparse_alias(&ws,&parent))goto done;
        printf("PAIRED high=0x%llX low=0x%llX\n",parent.base.va,p.Command.ReservedBase);
    }
    context.hDevice=p.hDevice;context.NodeOrdinal=0;context.ClientHint=D3DKMT_CLIENTHINT_VULKAN;
    if(!NT_SUCCESS(Report("CreateContextVirtual (normal TDR policy)",D3DKMTCreateContextVirtual(&context))))goto done;
    p.hContext=context.hContext;if(!StepCreateFence(&p))goto done;
    p.WaitEvent=CreateEventW(NULL,FALSE,FALSE,NULL);if(!p.WaitEvent)goto done;
    completion.hDevice=p.hDevice;completion.hContext=p.hContext;completion.WaitEvent=p.WaitEvent;completion.Opt.FenceTimeoutMs=5000;
    if(!StepCreateFence(&completion))goto done;
    if(!ReadContent(&p,&completion,&ib,p.Data.MappedVa,a,1,"physical-A")||
       !ReadContent(&p,&completion,&ib,p.Data.MappedVa+65536,b,2,"physical-B"))goto done;
    if(holes && !ReadContent(&p,&completion,&ib,parent.base.va,0,3,"initial-zero"))goto done;
    if(paired && holes && !ReadContent(&p,&completion,&ib,p.Command.ReservedBase,0,11,"low-initial-zero"))goto done;
    input.hDevice=p.hDevice;input.Info.Type=D3DDDI_MONITORED_FENCE;input.Info.MonitoredFence.EngineAffinity=1;
    if(!NT_SUCCESS(Report("Create application fence",D3DKMTCreateSynchronizationObject2(&input))))goto done;
    ws.base.cs_submit=NativeWaits;ws.device_h=p.hDevice;q->context_h=p.hContext;q->vm_fence.handle=p.hFence;
    physical.base.va=p.Data.MappedVa;physical.base.handle=p.Data.hAllocation;
    app_wait.handle=input.hSyncObject;app_wait.wait_value=1;
    if(radv_wddm2_virtual_bind_begin(&ws.base,&ctx.base,AMD_IP_GFX)||
       radv_wddm2_virtual_bo_map(&ws,&ctx,AMD_IP_GFX,&parent,0,65536,&physical,0)||
       radv_wddm2_virtual_bind_end(&ws.base,&ctx.base,AMD_IP_GFX,0,1,&app_wait,true))goto done;
    Sleep(20);printf("DELAYED_WAIT vm_fence=%llu expected=0\n",*p.FenceCpuVa);
    if(*p.FenceCpuVa!=0||!CpuRelease(p.hDevice,input.hSyncObject))goto done;
    released=TRUE;if(!StepWaitFence(&p,q->vm_fence.wait_value))goto done;
    if(!ReadContent(&p,&completion,&ib,parent.base.va,a,4,"alias-A"))goto done;
    if(paired && !ReadContent(&p,&completion,&ib,p.Command.ReservedBase,a,12,"low-alias-A"))goto done;
    if(radv_wddm2_virtual_bind_begin(&ws.base,&ctx.base,AMD_IP_GFX)||
       radv_wddm2_virtual_bo_map(&ws,&ctx,AMD_IP_GFX,&parent,0,65536,&physical,65536)||
       radv_wddm2_virtual_bind_end(&ws.base,&ctx.base,AMD_IP_GFX,0,0,NULL,true)||
       !StepWaitFence(&p,q->vm_fence.wait_value))goto done;
    if(!ReadContent(&p,&completion,&ib,parent.base.va,b,5,"rebind-B"))goto done;
    if(paired && !ReadContent(&p,&completion,&ib,p.Command.ReservedBase,b,13,"low-rebind-B"))goto done;
    if(radv_wddm2_virtual_bind_begin(&ws.base,&ctx.base,AMD_IP_GFX)||
       radv_wddm2_virtual_bo_unmap(&ws,&ctx,AMD_IP_GFX,&parent,0,65536)||
       radv_wddm2_virtual_bind_end(&ws.base,&ctx.base,AMD_IP_GFX,0,0,NULL,true)||
       !StepWaitFence(&p,q->vm_fence.wait_value))goto done;
    if(holes && !ReadContent(&p,&completion,&ib,parent.base.va,0,6,"unmapped-zero"))goto done;
    if(paired && holes && !ReadContent(&p,&completion,&ib,p.Command.ReservedBase,0,14,"low-unmapped-zero"))goto done;
    if(writeHole) {
        if(!CopyContent(&p,&completion,&ib,p.Data.MappedVa,parent.base.va,7) ||
           !ReadContent(&p,&completion,&ib,parent.base.va,0,8,"zero-after-write") ||
           !ReadContent(&p,&completion,&ib,p.Data.MappedVa,a,9,"physical-A-after-write") ||
           !ReadContent(&p,&completion,&ib,p.Data.MappedVa+65536,b,10,"physical-B-after-write"))goto done;
        if(paired && !ReadContent(&p,&completion,&ib,p.Command.ReservedBase,0,15,"low-zero-after-write"))goto done;
        puts("WRITE_DISCARD PASS; hole stays zero and bound controls unchanged");
    }
    puts(holes ? "ZERO_READ PASS; shader semantics untested" : "BOUND_READ PASS; no reads from holes");
    ok=TRUE;
done:
    if(input.hSyncObject&&!released)CpuRelease(p.hDevice,input.hSyncObject);
    if(input.hSyncObject){D3DKMT_DESTROYSYNCHRONIZATIONOBJECT d={0};d.hSyncObject=input.hSyncObject;Report("Destroy application fence",D3DKMTDestroySynchronizationObject(&d));}
    if(completion.hFence){D3DKMT_DESTROYSYNCHRONIZATIONOBJECT d={0};d.hSyncObject=completion.hFence;Report("Destroy copy fence",D3DKMTDestroySynchronizationObject(&d));}
    if(parent.sparse_high_va){D3DKMT_FREEGPUVIRTUALADDRESS f={0};f.hAdapter=p.hAdapter;f.BaseAddress=parent.sparse_high_va;f.Size=parent.base.size;Report("Free high alias",D3DKMTFreeGpuVirtualAddress(&f));}
    TeardownBuffer(&p,&zeroBacking);TeardownBuffer(&p,&ib);Teardown(&p);free(q->sparse_ops.data);SetEvent(g_Done);
    puts(ok?"RESULT PASS":"RESULT FAIL");return ok?0:1;
}
