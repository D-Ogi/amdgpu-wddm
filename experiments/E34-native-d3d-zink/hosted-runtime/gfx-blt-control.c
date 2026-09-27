/* Native positive control for the exact production row-copy builder.
 * No KMD change, Vulkan ICD, window or registry switch is needed. */
#define BC250_GPU_PROBE_NO_MAIN
#include "../../E27-m9-inference/gpu-residency-probe.c"
#include "../../../driver/kmd/gfx_blt.h"
#include "../../../driver/kmd/gfx_copy.h"

static BOOL g_CopyPending;
static BOOL SubmitCopy(PROBE* p,UINT dwords,UINT64* sequence)
{
    struct bc250_umd_submit_private blob={0};D3DKMT_SUBMITCOMMAND submit={0};
    blob.magic=BC250_UMD_SUBMIT_MAGIC;blob.version=BC250_UMD_SUBMIT_VERSION;
    blob.size=(UINT32)(offsetof(struct bc250_umd_submit_private,ib)+sizeof(blob.ib[0]));
    blob.ip_type=AMDGPU_HW_IP_GFX;blob.num_ibs=1;
    blob.ib[0].va_start=p->Command.MappedVa;blob.ib[0].ib_bytes=dwords*4;
    blob.ib[0].ip_type=AMDGPU_HW_IP_GFX;
    submit.Commands=p->Command.MappedVa;submit.CommandLength=dwords*4;
    submit.BroadcastContextCount=1;submit.BroadcastContext[0]=p->hContext;
    submit.pPrivateDriverData=&blob;submit.PrivateDriverDataSize=blob.size;
    if(!NT_SUCCESS(Report("GFX copy SubmitCommand",D3DKMTSubmitCommand(&submit))))return FALSE;
    g_CopyPending=TRUE;
    if(!StepSignalFence(p,++*sequence)||!StepWaitFence(p,*sequence))return FALSE;
    g_CopyPending=FALSE;return TRUE;
}
static BOOL Fill(PROBE* p,BUFFER* b,BOOL pattern)
{
    UINT64 i;
    if(!LockBuffer(p,b))return FALSE;
    for(i=0;i<b->Size/4;i++)((UINT32*)b->Locked)[i]=pattern?Pattern(i,0xBC250u):0xa5a5a5a5u;
    MemoryBarrier();return UnlockBuffer(p,b);
}
static BOOL RunCase(PROBE* p,UINT sourceHeap,UINT destinationHeap,UINT width,UINT height,UINT capacity,UINT64* sequence)
{
    BUFFER dst={0},readback={0};BC250_BLIT_SURFACE s,d;
    BC250_BLIT_RECT sr,dr;BC250_BLIT_PLAN plan;BC250_BLIT_CURSOR cursor={0};
    BC250_GFX_BLIT_RESULT result;BOOL ok=FALSE;
    UINT written,padded,i,packets=0;UINT64 at,bad=0;
    p->Data.Name="copy-source";dst.Name="copy-destination";readback.Name="copy-readback";
    s.Width=width+4;s.Height=height+3;s.Pitch=(width+8)*4;s.Format=Bc250BltBgra8;
    d.Width=width+6;d.Height=height+5;d.Pitch=(width+12)*4;d.Format=Bc250BltBgra8;
    s.Bytes=((UINT64)s.Pitch*s.Height+65535)&~65535ull;
    d.Bytes=((UINT64)d.Pitch*d.Height+65535)&~65535ull;
    p->Data.Size=s.Bytes;dst.Size=readback.Size=d.Bytes;
    sr.Left=2;sr.Top=1;sr.Right=(int)width+2;sr.Bottom=(int)height+1;
    dr.Left=4;dr.Top=3;dr.Right=(int)width+4;dr.Bottom=(int)height+3;
    if(Bc250PlanBlt(&s,&d,&sr,&dr,NULL,&plan)!=Bc250BltCopy)return FALSE;
    Note("COPY_CASE source_heap=%u destination_heap=%u width=%u height=%u capacity=%u",sourceHeap,destinationHeap,width,height,capacity);
    if(!CreateUmdBuffer(p,&p->Data,sourceHeap)||!CreateUmdBuffer(p,&dst,destinationHeap)||
       !CreateUmdBuffer(p,&readback,AMDGPU_GEM_DOMAIN_GTT)||!Fill(p,&p->Data,TRUE)||
       !Fill(p,&dst,FALSE)||!Fill(p,&readback,FALSE))goto done;
    do {
        UINT32* dw;
        if(!LockBuffer(p,&p->Command))goto done;
        dw=(UINT32*)p->Command.Locked;
        result=Bc250EmitGfxBlt(&plan,p->Data.MappedVa,dst.MappedVa,&cursor,&cursor,dw,capacity,&written);
        if((result!=Bc250GfxBltDone && result!=Bc250GfxBltMore)||!written){UnlockBuffer(p,&p->Command);goto done;}
        padded=(written+7u)&~7u;
        for(i=written;i<padded;i++)dw[i]=BC250_CP_NOP;
        MemoryBarrier();if(!UnlockBuffer(p,&p->Command)||!SubmitCopy(p,padded,sequence))goto done;
        packets+=written/7u;
    }while(result==Bc250GfxBltMore);
    /* Independent readback uses the previously exercised E27 direct-memory DMA
     * layout, without the new row builder or L2 selectors. The preceding hardware
     * completion/fence path must make the copied destination visible to it. */
    for(at=0;at<dst.Size;at+=(1u<<20)){
        UINT32* dw;UINT bytes=(UINT)((dst.Size-at>(1u<<20))?(1u<<20):dst.Size-at);
        UINT64 from=dst.MappedVa+at,to=readback.MappedVa+at;
        if(!LockBuffer(p,&p->Command))goto done;
        dw=(UINT32*)p->Command.Locked;
        dw[0]=PACKET3(PACKET3_DMA_DATA,5);dw[1]=(UINT32)PACKET3_DMA_DATA_CP_SYNC;
        dw[2]=(UINT32)from;dw[3]=(UINT32)(from>>32);dw[4]=(UINT32)to;dw[5]=(UINT32)(to>>32);
        dw[6]=bytes;dw[7]=BC250_CP_NOP;
        MemoryBarrier();if(!UnlockBuffer(p,&p->Command)||!SubmitCopy(p,8,sequence))goto done;
    }
    if(!LockBuffer(p,&readback))goto done;
    for(at=0;at<readback.Size/4;at++){
        UINT64 y=at/(d.Pitch/4),x=at%(d.Pitch/4);UINT32 expected=0xa5a5a5a5u;
        UINT32 got=((volatile UINT32*)readback.Locked)[at];
        if(y>=3 && y<height+3u && x>=4 && x<width+4u)
            expected=Pattern((y-3u+1u)*(s.Pitch/4)+(x-4u+2u),0xBC250u);
        if(got!=expected){if(bad<4)printf("COPY_MISMATCH word=%llu got=%08x expected=%08x\n",at,got,expected);bad++;}
    }
    if(!UnlockBuffer(p,&readback))goto done;
    printf("COPY_RESULT %s bytes=%llu pixels=%llu packets=%u mismatches=%llu fence=%llu\n",bad?"FAIL":"PASS",readback.Size,(UINT64)width*height,packets,bad,*sequence);
    ok=bad==0;
done:
    if(g_CopyPending){puts("FAIL unresolved GPU work: defer allocation teardown to process/device cleanup");SetEvent(g_Done);ExitProcess(1);}
    TeardownBuffer(p,&readback);TeardownBuffer(p,&dst);TeardownBuffer(p,&p->Data);return ok;
}
int main(int argc,char** argv)
{
    PROBE p={0};BOOL ok=FALSE;UINT64 sequence=0;
    if(argc!=2 || strcmp(argv[1],"--run")){
        puts("gfx-blt-control --run: five bounded native GFX row-copy controls, hardware required; no display or ICD change");
        return argc==1 || (argc==2 && !strcmp(argv[1],"--help"))?0:2;
    }
    setvbuf(stdout,NULL,_IONBF,0);
    if(!StartWatchdog(180000))return 2;
    p.Opt.Match="bc250";p.Opt.FenceTimeoutMs=5000;
    p.Command.Name="copy-ib";p.Command.Size=65536;
    if(!StepFindAdapter(&p)||!StepOpenAdapter(&p)||!StepQueryCaps(&p)||!StepCreateDevice(&p)||
       !StepCreatePagingQueue(&p)||!CreateUmdBuffer(&p,&p.Command,AMDGPU_GEM_DOMAIN_GTT)||!CreateGpuContext(&p))goto done;
    if(!RunCase(&p,AMDGPU_GEM_DOMAIN_GTT,AMDGPU_GEM_DOMAIN_GTT,5,5,7,&sequence)||
       !RunCase(&p,AMDGPU_GEM_DOMAIN_GTT,AMDGPU_GEM_DOMAIN_VRAM,5,5,14,&sequence)||
       !RunCase(&p,AMDGPU_GEM_DOMAIN_VRAM,AMDGPU_GEM_DOMAIN_VRAM,5,5,35,&sequence)||
       !RunCase(&p,AMDGPU_GEM_DOMAIN_VRAM,AMDGPU_GEM_DOMAIN_GTT,5,5,35,&sequence)||
       !RunCase(&p,AMDGPU_GEM_DOMAIN_GTT,AMDGPU_GEM_DOMAIN_VRAM,1920,1200,16384,&sequence))goto done;
    ok=TRUE;
done:
    Teardown(&p);SetEvent(g_Done);
    printf("GFX_BLT_CONTROL %s final_fence=%llu\n",ok?"PASS":"FAIL",sequence);
    return ok?0:1;
}
