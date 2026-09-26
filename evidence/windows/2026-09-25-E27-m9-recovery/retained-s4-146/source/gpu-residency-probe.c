// PROVENANCE: DMA_DATA packet order follows Mesa radv_cp_dma.c (MIT).
// Full-range GPU readback after explicit eviction; residency status alone is not relocation proof.
#define BC250_RESIDENCY_HELPERS_ONLY
#include "residency-probe.c"
#include "gpu-copy-packets.h"

// A sampled whole-allocation residency witness, distinct from eviction cycling.
// Contracts: local ref/ddi-display/d3dkmthk.md, QUERYALLOCATIONRESIDENCY and
// QUERYVIDEOMEMORYINFO (WDK26100). Budgets are accounting, not residency proof.
static BOOL g_ResidentOnly;
static UINT g_Heap;
static DWORD g_BulkTimeoutMs=60000;
static UINT g_ResidencySamples;
static BUFFER g_Set[12];
static UINT g_SetCount;
static UINT64 g_SetBytes,g_PatternWordBase;

typedef struct {
    UINT64 Bytes;
    UINT Heap;
    BOOL ResidentOnly;
    const char* ReadyPath;
    const char* ReleasePath;
    DWORD GateTimeoutMs;
} GPU_OPTIONS;

static BOOL ParseGpuOptions(int argc,char** argv,GPU_OPTIONS* o)
{
    int i;UINT64 timeout;
    ZeroMemory(o,sizeof(*o));o->Bytes=65536;o->Heap=AMDGPU_GEM_DOMAIN_VRAM;
    if(argc>1 && !ParseU64(argv[1],&o->Bytes))return FALSE;
    if(argc>2){if(!strcmp(argv[2],"gtt"))o->Heap=AMDGPU_GEM_DOMAIN_GTT;else if(strcmp(argv[2],"vram"))return FALSE;}
    for(i=3;i<argc;i++) {
        if(!strcmp(argv[i],"--resident-only") && !o->ResidentOnly)o->ResidentOnly=TRUE;
        else if(!strcmp(argv[i],"--resume-gate") && !o->ReadyPath && argc-i>=4) {
            o->ReadyPath=argv[++i];o->ReleasePath=argv[++i];
            if(!*o->ReadyPath || !*o->ReleasePath || !_stricmp(o->ReadyPath,o->ReleasePath) ||
               !ParseU64(argv[++i],&timeout) || !timeout || timeout>1800000)return FALSE;
            o->GateTimeoutMs=(DWORD)timeout;
        } else return FALSE;
    }
    return o->Bytes && !(o->Bytes&4095) &&
        o->Bytes<=(o->ResidentOnly?(12ull<<30):(1ull<<30)) && !(o->ResidentOnly&&o->ReadyPath);
}

static void ResumeIdentity(PROBE* p,BUFFER* dst,const char* phase,UINT64 sequence)
{
    printf("RESUME_ID phase=%s pid=%lu adapter=%08X device=%08X context=%08X paging_queue=%08X paging_fence=%08X fence=%08X data=%08X data_va=%016llX command=%08X command_va=%016llX readback=%08X readback_va=%016llX sequence=%llu\n",
        phase,GetCurrentProcessId(),p->hAdapter,p->hDevice,p->hContext,p->hPagingQueue,p->hPagingFenceObject,p->hFence,
        p->Data.hAllocation,p->Data.MappedVa,p->Command.hAllocation,p->Command.MappedVa,dst->hAllocation,dst->MappedVa,sequence);
}

// No KMT call, CPU mapping or outstanding GPU submission while this gate waits.
// A unique ready path and a release containing this PID prevent stale release.
// The coordinator proves actual S4 separately; this gate alone proves no sleep.
static BOOL WaitResumeGate(PROBE* p,BUFFER* dst,UINT64 sequence,const GPU_OPTIONS* o)
{
    HANDLE file;DWORD bytes,error;char payload[128];int length;ULONGLONG start=GetTickCount64();
    DWORD pid=GetCurrentProcessId();
    if(GetFileAttributesA(o->ReleasePath)!=INVALID_FILE_ATTRIBUTES) {
        puts("RESUME_GATE FAIL stale release path");return FALSE;
    }
    ResumeIdentity(p,dst,"ready",sequence);fflush(stdout);
    file=CreateFileA(o->ReadyPath,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE){printf("RESUME_GATE FAIL create ready error=%lu\n",GetLastError());return FALSE;}
    length=snprintf(payload,sizeof(payload),"pid=%lu\nfirst_full_readback=PASS\nsequence=%llu\ntimeout_ms=%lu\n",pid,sequence,o->GateTimeoutMs);
    if(length<0 || length>=(int)sizeof(payload) || !WriteFile(file,payload,(DWORD)length,&bytes,NULL) ||
       bytes!=(DWORD)length || !FlushFileBuffers(file)) {
        error=GetLastError();CloseHandle(file);printf("RESUME_GATE FAIL write ready error=%lu\n",error);return FALSE;
    }
    CloseHandle(file);
    printf("RESUME_GATE READY pid=%lu timeout_ms=%lu\n",pid,o->GateTimeoutMs);
    while(GetTickCount64()-start<o->GateTimeoutMs) {
        file=CreateFileA(o->ReleasePath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
        if(file!=INVALID_HANDLE_VALUE) {
            UINT64 releasePid;char* end;
            if(!ReadFile(file,payload,sizeof(payload)-1,&bytes,NULL)){CloseHandle(file);return FALSE;}
            CloseHandle(file);payload[bytes]=0;
            end=payload+strlen(payload);while(end>payload && (end[-1]=='\r'||end[-1]=='\n'))*--end=0;
            if(!ParseU64(payload,&releasePid) || releasePid!=pid) {
                puts("RESUME_GATE FAIL release PID mismatch");return FALSE;
            }
            printf("RESUME_GATE RELEASED pid=%lu elapsed_ms=%llu\n",pid,GetTickCount64()-start);
            ResumeIdentity(p,dst,"released",sequence);return TRUE;
        }
        error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND && error!=ERROR_SHARING_VIOLATION) {
            printf("RESUME_GATE FAIL release open error=%lu\n",error);return FALSE;
        }
        Sleep(100);
    }
    printf("RESUME_GATE TIMEOUT pid=%lu timeout_ms=%lu\n",pid,o->GateTimeoutMs);return FALSE;
}

static BOOL ResidentSnapshot(PROBE* p,const char* tag,UINT64 offset)
{
    D3DKMT_ALLOCATIONRESIDENCYSTATUS residency[12]={0};
    D3DKMT_HANDLE handles[12]={0};
    D3DKMT_QUERYALLOCATIONRESIDENCY q={0};
    UINT group,i;UINT64 gpu=0,shared=0,nonresident=0;
    for(i=0;i<g_SetCount;i++)handles[i]=g_Set[i].hAllocation;
    q.hDevice=p->hDevice;q.phAllocationList=handles;
    q.AllocationCount=g_SetCount;q.pResidencyStatus=residency;
    if(!NT_SUCCESS(Report("Query complete allocation set",D3DKMTQueryAllocationResidency(&q))))return FALSE;
    ++g_ResidencySamples;
    for(i=0;i<g_SetCount;i++) {
        printf("RESIDENT_ALLOCATION sample=%u index=%u bytes=%llu status=%u\n",
               g_ResidencySamples,i,g_Set[i].Size,(unsigned)residency[i]);
        if(residency[i]==D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINGPUMEMORY)gpu+=g_Set[i].Size;
        else if(residency[i]==D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINSHAREDMEMORY)shared+=g_Set[i].Size;
        else nonresident+=g_Set[i].Size;
    }
    printf("RESIDENT_SET tag=%s offset=%llu bytes=%llu allocations=%u gpu=%llu shared=%llu nonresident=%llu sample=%u\n",
           tag,offset+g_PatternWordBase*4,g_SetBytes,g_SetCount,gpu,shared,nonresident,g_ResidencySamples);
    for(group=0;group<2;group++) {
        D3DKMT_QUERYVIDEOMEMORYINFO m={0};
        m.hAdapter=p->hAdapter;
        m.MemorySegmentGroup=group?D3DKMT_MEMORY_SEGMENT_GROUP_NON_LOCAL:D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL;
        if(!NT_SUCCESS(Report("Query video memory budget",D3DKMTQueryVideoMemoryInfo(&m))))return FALSE;
        printf("MEMORY_BUDGET group=%s budget=%llu usage=%llu reservation=%llu available=%llu\n",
               group?"nonlocal":"local",m.Budget,m.CurrentUsage,m.CurrentReservation,m.AvailableForReservation);
    }
    // A VRAM request must not pass on shared-memory residency.
    if((g_Heap==AMDGPU_GEM_DOMAIN_VRAM?gpu:shared)!=g_SetBytes || nonresident) {
        puts("FAIL: complete set is not resident in the requested memory class");
        return FALSE;
    }
    return TRUE;
}

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
    for(i=0;i<words;i++)data[i]=Pattern(g_PatternWordBase+i,seed);
    MemoryBarrier();
    printf("CPU_FILL bytes=%llu seed=%u; validation uses GPU readback\n",b->Size,seed);
    return UnlockBuffer(p,b);
}

static BOOL BulkMakeResident(PROBE* p,BUFFER* b)
{
    ULONGLONG start=GetTickCount64();
    DWORD previous=p->Opt.FenceTimeoutMs;
    BOOL ok;
    // Aggregate tool deadline scales with the requested bytes; KMD watchdog unchanged.
    p->Opt.FenceTimeoutMs=g_BulkTimeoutMs;
    ok=StepMakeResident(p,b);
    p->Opt.FenceTimeoutMs=previous;
    printf("BULK_RESIDENT bytes=%llu elapsed_ms=%llu success=%u\n",b->Size,GetTickCount64()-start,ok);
    return ok;
}

static BOOL BulkCreateBuffer(PROBE* p,BUFFER* b,UINT heap)
{
    ULONGLONG start=GetTickCount64();
    DWORD previous=p->Opt.FenceTimeoutMs;
    BOOL ok;
    // VA mapping may wait behind eviction of an earlier large allocation.
    p->Opt.FenceTimeoutMs=g_BulkTimeoutMs;
    ok=CreateUmdBuffer(p,b,heap);
    p->Opt.FenceTimeoutMs=previous;
    printf("BULK_CREATE name=%s bytes=%llu elapsed_ms=%llu success=%u\n",b->Name,b->Size,GetTickCount64()-start,ok);
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
        if(g_ResidentOnly && !(offset % (64ull<<20)) && !ResidentSnapshot(p,"during-readback",offset))return FALSE;
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
            UINT32 expected=Pattern(g_PatternWordBase+offset/4+i,0xBC250u);
            if(got!=expected){if(bad<4)printf("GPU_MISMATCH offset=%llu got=%08X expected=%08X\n",offset+i*4,got,expected);++bad;}
        }
        if(!UnlockBuffer(p,dst)||bad)return FALSE;
    }
    if(g_ResidentOnly && !ResidentSnapshot(p,"after-readback",p->Data.Size))return FALSE;
    printf("GPU_READBACK bytes=%llu all_words_match=1 fence=%llu word_base=%llu\n",p->Data.Size,*sequence,g_PatternWordBase);
    return TRUE;
}

static BOOL RunResidentSet(PROBE* p,BUFFER* dst,UINT64 bytes,UINT heap,UINT64* sequence)
{
    UINT i;UINT64 remaining=bytes;BOOL ok=FALSE;
    g_SetBytes=bytes;
    // Retain the complete set, including every MakeResident reference, before
    // filling or reading any member. No eviction or pressure allocation here.
    while(remaining) {
        BUFFER* b=&g_Set[g_SetCount++];
        b->Name="resident-set";b->Size=remaining>(1ull<<30)?(1ull<<30):remaining;
        if(!BulkCreateBuffer(p,b,heap))goto done;
        remaining-=b->Size;
    }
    if(!BulkCreateBuffer(p,&p->Command,AMDGPU_GEM_DOMAIN_GTT)||
       !BulkCreateBuffer(p,dst,AMDGPU_GEM_DOMAIN_GTT)||!CreateGpuContext(p))goto done;
    if(!ResidentSnapshot(p,"all-created",0))goto done;
    g_PatternWordBase=0;
    for(i=0;i<g_SetCount;i++) {
        // Distinct global word indices detect accidental aliasing between members.
        if(!FillGpuPattern(p,&g_Set[i],0xBC250u))goto done;
        g_PatternWordBase+=g_Set[i].Size/4;
    }
    g_PatternWordBase=0;
    for(i=0;i<g_SetCount;i++) {
        p->Data=g_Set[i];
        ok=GpuReadback(p,dst,sequence);
        g_Set[i]=p->Data;
        ZeroMemory(&p->Data,sizeof(p->Data)); // ownership stays in g_Set
        if(!ok)goto done;
        g_PatternWordBase+=g_Set[i].Size/4;
    }
    ok=TRUE;
done:
    for(i=0;i<g_SetCount;i++)TeardownBuffer(p,&g_Set[i]);
    return ok;
}

#ifndef BC250_GPU_PROBE_NO_MAIN
int main(int argc,char**argv)
{
    PROBE p={0};BUFFER dst={0},pressure={0};BOOL ok=FALSE;GPU_OPTIONS opt;
    UINT64 bytes=65536,sequence=0;UINT heap=AMDGPU_GEM_DOMAIN_VRAM,cycle;
    if(argc>1&&!strcmp(argv[1],"--help")){
        puts("gpu-residency-probe [bytes] [vram|gtt] [--resident-only | --resume-gate READY RELEASE TIMEOUT_MS]");
        puts("Resume gate: <=1GiB, timeout 1..1800000ms; READY must be new, RELEASE absent and later contain this PID in ASCII. Two full GPU readbacks in one process, no eviction cycle or recreation. External evidence must establish S4.");
        puts("Default: <=1GiB, 3 eviction cycles, 300s watchdog.");
        puts("Resident-only: <=12GiB in <=1GiB members retained together, no pressure allocation; full GPU readback and complete-set residency/budget snapshots every64MiB. No continuous-residency claim.");
        return 0;
    }
    if(!ParseGpuOptions(argc,argv,&opt))return 2;
    bytes=opt.Bytes;heap=opt.Heap;g_ResidentOnly=opt.ResidentOnly;
    g_Heap=heap;
    if(g_ResidentOnly)g_BulkTimeoutMs=60000+(DWORD)((bytes+((1ull<<30)-1))>>30)*60000;
    setvbuf(stdout,NULL,_IONBF,0);
    if(!StartWatchdog((g_ResidentOnly?300000+g_BulkTimeoutMs:300000)+opt.GateTimeoutMs))return 2;
    p.Opt.Match="bc250";p.Opt.FenceTimeoutMs=5000;
    p.Data.Name="preserved";p.Data.Size=bytes;
    p.Command.Name="gpu-ib";p.Command.Size=65536;
    dst.Name="gpu-readback";dst.Size=bytes<(1ull<<20)?bytes:(1ull<<20);
    pressure.Name="pressure";pressure.Size=bytes;
    printf("GPU_RESIDENCY bytes=%llu heap=%u mode=%s bulk_timeout_ms=%lu\n",bytes,heap,g_ResidentOnly?"resident-only":(opt.ReadyPath?"retained-resume":"eviction-cycles"),g_BulkTimeoutMs);
    if(!StepFindAdapter(&p)||!StepOpenAdapter(&p)||!StepQueryCaps(&p)||!StepCreateDevice(&p)||!StepCreatePagingQueue(&p))goto done;
    if(g_ResidentOnly){ok=RunResidentSet(&p,&dst,bytes,heap,&sequence);goto done;}
    if(!BulkCreateBuffer(&p,&p.Data,heap)||!BulkCreateBuffer(&p,&p.Command,AMDGPU_GEM_DOMAIN_GTT)||!BulkCreateBuffer(&p,&dst,AMDGPU_GEM_DOMAIN_GTT)||!CreateGpuContext(&p))goto done;
    if(!FillGpuPattern(&p,&p.Data,0xBC250u)||!GpuReadback(&p,&dst,&sequence))goto done;
    if(opt.ReadyPath) {
        if(!WaitResumeGate(&p,&dst,sequence,&opt))goto done;
        // Reuse the same context, allocation handles, VA mappings and fence.
        // Do not refill the source: only the readback destination gets its sentinel.
        ok=GpuReadback(&p,&dst,&sequence);
        ResumeIdentity(&p,&dst,ok?"verified":"failed",sequence);
        goto done;
    }
    for(cycle=1;cycle<=3;cycle++) {
        D3DKMT_EVICT evict={0};
        printf("CYCLE %u BEGIN\n",cycle);
        evict.hDevice=p.hDevice;evict.NumAllocations=1;evict.AllocationList=&p.Data.hAllocation;
        if(!NT_SUCCESS(Report("Evict",D3DKMTEvict(&evict))))goto done;
        if(!BulkCreateBuffer(&p,&pressure,heap)||!FillGpuPattern(&p,&pressure,cycle))goto done;
        // Create competing demand before testing residency departure.
        if(!Residency(&p,&p.Data,"after-pressure",TRUE))goto done;
        TeardownBuffer(&p,&pressure);
        if(!BulkMakeResident(&p,&p.Data)||!Residency(&p,&p.Data,"after-resident",FALSE)||!GpuReadback(&p,&dst,&sequence))goto done;
        printf("CYCLE %u PASS\n",cycle);
    }
    ok=TRUE;
done:
    if(opt.ReadyPath)printf("GPU_RETAINED_RESUME_RESULT %s pid=%lu bytes=%llu final_sequence=%llu; external S4 evidence required\n",ok?"PASS":"FAIL",GetCurrentProcessId(),bytes,sequence);
    TeardownBuffer(&p,&pressure);TeardownBuffer(&p,&dst);Teardown(&p);SetEvent(g_Done);
    if(g_ResidentOnly)printf("GPU_RESIDENT_SET_RESULT %s bytes=%llu samples=%u snapshots_only=1\n",ok?"PASS":"FAIL",bytes,g_ResidencySamples);
    printf("GPU_RESIDENCY_RESULT %s; physical relocation requires paging trace\n",ok?"PASS":"FAIL");
    return ok?0:1;
}

#endif
