// PROVENANCE: DMA_DATA packet order follows Mesa radv_cp_dma.c (MIT).
// Full-range GPU readback after explicit eviction; residency status alone is not relocation proof.
#define BC250_RESIDENCY_HELPERS_ONLY
#include "residency-probe.c"
#include "gpu-copy-packets.h"

static BOOL CreateGpuContext(PROBE* p)
{
    struct bc250_umd_context_private blob={0};
    D3DKMT_CREATECONTEXTVIRTUAL c={0};
    blob.magic=BC250_UMD_CONTEXT_MAGIC;blob.version=BC250_UMD_CONTEXT_VERSION;
    blob.size=sizeof(blob);blob.ip_type=AMDGPU_HW_IP_GFX;
    c.hDevice=p->hDevice;c.ClientHint=D3DKMT_CLIENTHINT_VULKAN;
    c.pPrivateDriverData=&blob;c.PrivateDriverDataSize=sizeof(blob);
    if(!NT_SUCCESS(Report("CreateContextVirtual BC2C",D3DKMTCreateContextVirtual(&c))))return FALSE;
    p->hContext=c.hContext;
    p->WaitEvent=CreateEvent(NULL,FALSE,FALSE,NULL);
    return p->WaitEvent!=NULL && StepCreateFence(p);
}

// GPU readback below validates the preserved source in full. Do not also read
// the entire write-combined CPU mapping here; pressure only needs dirty pages.
static BOOL FillGpuPattern(PROBE* p,BUFFER* b,UINT32 seed)
{
    UINT64 i,words=b->Size/sizeof(UINT32);
    UINT32* data;
    if(!LockBuffer(p,b))return FALSE;
    data=(UINT32*)b->Locked;
    for(i=0;i<words;i++)data[i]=Pattern(i,seed);
    MemoryBarrier();
    printf("CPU_FILL bytes=%llu seed=%u; validation uses GPU readback\n",b->Size,seed);
    return UnlockBuffer(p,b);
}

static BOOL BulkMakeResident(PROBE* p,BUFFER* b)
{
    ULONGLONG start=GetTickCount64();
    DWORD previous=p->Opt.FenceTimeoutMs;
    BOOL ok;
    // Tool deadline for an aggregate1GiB operation; KMD hardware watchdog unchanged.
    p->Opt.FenceTimeoutMs=60000;
    ok=StepMakeResident(p,b);
    p->Opt.FenceTimeoutMs=previous;
    printf("BULK_RESIDENT bytes=%llu elapsed_ms=%llu success=%u\n",b->Size,GetTickCount64()-start,ok);
    return ok;
}

static BOOL GpuReadback(PROBE* p,BUFFER* dst,UINT64* sequence)
{
    UINT64 offset;
    for(offset=0;offset<p->Data.Size;offset+=dst->Size) {
        UINT32 bytes=(UINT32)((p->Data.Size-offset<dst->Size)?p->Data.Size-offset:dst->Size);
        UINT32* dw;UINT64 i,bad=0,src=p->Data.MappedVa+offset;
        struct bc250_umd_submit_private blob={0};
        D3DKMT_SUBMITCOMMAND submit={0};
        // A distinct sentinel prevents a stale successful readback satisfying this test.
        if(!LockBuffer(p,dst))return FALSE;
        memset(dst->Locked,0xA5,(size_t)dst->Size);
        MemoryBarrier();if(!UnlockBuffer(p,dst))return FALSE;
        if(!LockBuffer(p,&p->Command))return FALSE;
        dw=(UINT32*)p->Command.Locked;
        // One <=1MiB copy, direct memory addresses (no L2), ME completion before fence.
        // Matches Mesa radv_cs_emit_cp_dma without CP_DMA_USE_L2, with CP_DMA_SYNC.
        dw[0]=PACKET3(PACKET3_DMA_DATA,5);
        dw[1]=(UINT32)PACKET3_DMA_DATA_CP_SYNC;
        dw[2]=(UINT32)src;dw[3]=(UINT32)(src>>32);
        dw[4]=(UINT32)dst->MappedVa;dw[5]=(UINT32)(dst->MappedVa>>32);
        dw[6]=bytes;dw[7]=BC250_CP_NOP;
        MemoryBarrier();if(!UnlockBuffer(p,&p->Command))return FALSE;
        blob.magic=BC250_UMD_SUBMIT_MAGIC;blob.version=BC250_UMD_SUBMIT_VERSION;
        blob.size=(UINT32)(offsetof(struct bc250_umd_submit_private,ib)+sizeof(blob.ib[0]));
        blob.ip_type=AMDGPU_HW_IP_GFX;blob.num_ibs=1;
        blob.ib[0].va_start=p->Command.MappedVa;blob.ib[0].ib_bytes=32;
        blob.ib[0].ip_type=AMDGPU_HW_IP_GFX;
        submit.Commands=p->Command.MappedVa;submit.CommandLength=32;
        submit.BroadcastContextCount=1;submit.BroadcastContext[0]=p->hContext;
        submit.pPrivateDriverData=&blob;submit.PrivateDriverDataSize=blob.size;
        if(!NT_SUCCESS(Report("Submit GPU readback",D3DKMTSubmitCommand(&submit))))return FALSE;
        ++*sequence;
        if(!StepSignalFence(p,*sequence)||!StepWaitFence(p,*sequence))return FALSE;
        if(!LockBuffer(p,dst))return FALSE;
        for(i=0;i<bytes/4;i++) {
            UINT32 got=((volatile UINT32*)dst->Locked)[i];
            UINT32 expected=Pattern(offset/4+i,0xBC250u);
            if(got!=expected){if(bad<4)printf("GPU_MISMATCH offset=%llu got=%08X expected=%08X\n",offset+i*4,got,expected);++bad;}
        }
        if(!UnlockBuffer(p,dst)||bad)return FALSE;
    }
    printf("GPU_READBACK bytes=%llu all_words_match=1 fence=%llu\n",p->Data.Size,*sequence);
    return TRUE;
}

int main(int argc,char**argv)
{
    PROBE p={0};BUFFER dst={0},pressure={0};BOOL ok=FALSE;
    UINT64 bytes=65536,sequence=0;UINT heap=AMDGPU_GEM_DOMAIN_VRAM,cycle;
    if(argc>1&&!strcmp(argv[1],"--help")){puts("gpu-residency-probe [bytes<=1GiB] [vram|gtt]; 3 cycles, full GPU readback, 300s watchdog");return 0;}
    if(argc>1&&(!ParseU64(argv[1],&bytes)||!bytes||bytes>(1ull<<30)||(bytes&4095)))return 2;
    if(argc>2){if(!strcmp(argv[2],"gtt"))heap=AMDGPU_GEM_DOMAIN_GTT;else if(strcmp(argv[2],"vram"))return 2;}
    setvbuf(stdout,NULL,_IONBF,0);if(!StartWatchdog(300000))return 2;
    p.Opt.Match="bc250";p.Opt.FenceTimeoutMs=5000;
    p.Data.Name="preserved";p.Data.Size=bytes;
    p.Command.Name="gpu-ib";p.Command.Size=65536;
    dst.Name="gpu-readback";dst.Size=bytes<(1ull<<20)?bytes:(1ull<<20);
    pressure.Name="pressure";pressure.Size=bytes;
    printf("GPU_RESIDENCY bytes=%llu heap=%u\n",bytes,heap);
    if(!StepFindAdapter(&p)||!StepOpenAdapter(&p)||!StepQueryCaps(&p)||!StepCreateDevice(&p)||!StepCreatePagingQueue(&p))goto done;
    if(!CreateUmdBuffer(&p,&p.Data,heap)||!CreateUmdBuffer(&p,&p.Command,AMDGPU_GEM_DOMAIN_GTT)||!CreateUmdBuffer(&p,&dst,AMDGPU_GEM_DOMAIN_GTT)||!CreateGpuContext(&p))goto done;
    if(!FillGpuPattern(&p,&p.Data,0xBC250u)||!GpuReadback(&p,&dst,&sequence))goto done;
    for(cycle=1;cycle<=3;cycle++) {
        D3DKMT_EVICT evict={0};
        printf("CYCLE %u BEGIN\n",cycle);
        evict.hDevice=p.hDevice;evict.NumAllocations=1;evict.AllocationList=&p.Data.hAllocation;
        if(!NT_SUCCESS(Report("Evict",D3DKMTEvict(&evict))))goto done;
        if(!CreateUmdBuffer(&p,&pressure,heap)||!FillGpuPattern(&p,&pressure,cycle))goto done;
        // Create competing demand before testing residency departure.
        if(!Residency(&p,&p.Data,"after-pressure",TRUE))goto done;
        TeardownBuffer(&p,&pressure);
        if(!BulkMakeResident(&p,&p.Data)||!Residency(&p,&p.Data,"after-resident",FALSE)||!GpuReadback(&p,&dst,&sequence))goto done;
        printf("CYCLE %u PASS\n",cycle);
    }
    ok=TRUE;
done:
    TeardownBuffer(&p,&pressure);TeardownBuffer(&p,&dst);Teardown(&p);SetEvent(g_Done);
    printf("GPU_RESIDENCY_RESULT %s; physical relocation requires paging trace\n",ok?"PASS":"FAIL");
    return ok?0:1;
}
