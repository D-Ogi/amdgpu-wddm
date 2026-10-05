// E27 residency control. Reuses the bounded KMT probe helpers; does not submit GPU work.
// A passing CPU readback alone is not evidence of GPU paging: collect KMD paging counters too.
#define main kmtprobe_original_main
#include "../../tools/win/kmtprobe/kmtprobe.c"
#undef main
#pragma warning(push)
#pragma warning(disable:4201) // imported Linux UAPI anonymous unions
#include "../../driver/contract/bc250_umd_submit.h"
#pragma warning(pop)

static BOOL CreateUmdBuffer(PROBE* p, BUFFER* b, UINT heap)
{
    struct bc250_umd_alloc_private blob = {0};
    D3DDDI_ALLOCATIONINFO2 info = {0};
    D3DKMT_CREATEALLOCATION create = {0};
    C_ASSERT(sizeof(blob) == 192);
    blob.magic = BC250_UMD_ALLOC_MAGIC;
    blob.version = BC250_UMD_ALLOC_VERSION;
    blob.size = sizeof(blob);
    blob.alloc_size = b->Size;
    blob.phys_alignment = 4096;
    blob.preferred_heap = heap;
    blob.va_size = b->Size;
    info.pPrivateDriverData = &blob;
    info.PrivateDriverDataSize = sizeof(blob);
    create.hDevice = p->hDevice;
    create.NumAllocations = 1;
    create.pAllocationInfo2 = &info;
    create.Flags.NonSecure = 1;
    if (!NT_SUCCESS(ReportOn("CreateAllocation2 BC2A", b, D3DKMTCreateAllocation2(&create)))) return FALSE;
    b->hAllocation = info.hAllocation;
    return StepMapVa(p,b) && StepMakeResident(p,b);
}
static BOOL Residency(PROBE* p, BUFFER* b, const char* tag, BOOL requireEvicted)
{
    D3DKMT_ALLOCATIONRESIDENCYSTATUS residency = 0;
    D3DKMT_QUERYALLOCATIONRESIDENCY q = {0};
    ULONGLONG deadline = GetTickCount64() + 5000;
    q.hDevice=p->hDevice;q.phAllocationList=&b->hAllocation;q.AllocationCount=1;q.pResidencyStatus=&residency;
    // Evict queues removal; it is not a synchronous page-out fence.
    // Shared-memory residency can follow removal from GPU-memory residency.
    // Neither status 2 nor 3 proves a hardware copy occurred, especially on UMA.
    // Contract: ref/ddi-display/d3dkmthk.md, D3DKMTEvict and residency enum.
    do {
        if (!NT_SUCCESS(ReportOn("QueryAllocationResidency",b,D3DKMTQueryAllocationResidency(&q))))return FALSE;
        if (!requireEvicted || residency != D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINGPUMEMORY)break;
        Sleep(50);
    } while (GetTickCount64() < deadline);
    printf("RESIDENCY %s value=%u\n",tag,(unsigned)residency);
    if (requireEvicted && residency != D3DKMT_ALLOCATIONRESIDENCYSTATUS_NOTRESIDENT &&
        residency != D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINSHAREDMEMORY) {
        printf("INCONCLUSIVE: no departure from GPU-memory residency within 5 seconds\n");return FALSE;
    }
    if (!requireEvicted && residency != D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINGPUMEMORY &&
        residency != D3DKMT_ALLOCATIONRESIDENCYSTATUS_RESIDENTINSHAREDMEMORY) {
        printf("FAIL: residency was not witnessed\n");return FALSE;
    }
    return TRUE;
}
static UINT32 Pattern(UINT64 i, UINT32 seed) {return (UINT32)i * 1664525u + 1013904223u + seed;}
static BOOL PatternBuffer(PROBE* p, BUFFER* b, UINT32 seed, BOOL write)
{
    UINT64 i,words=b->Size/sizeof(UINT32),bad=0,hash=14695981039346656037ull;
    volatile UINT32* data;
    if(!LockBuffer(p,b))return FALSE;
    data=(volatile UINT32*)b->Locked;
    if(write)for(i=0;i<words;i++)data[i]=Pattern(i,seed);
    MemoryBarrier();
    for(i=0;i<words;i++) {
        UINT32 value=data[i];
        if(value!=Pattern(i,seed)) {if(bad<4)printf("MISMATCH %s word=%llu got=%08X expected=%08X\n",b->Name,i,value,Pattern(i,seed));bad++;}
        hash=(hash^value)*1099511628211ull;
    }
    printf("PATTERN %s bytes=%llu write=%u mismatches=%llu word_hash=%016llX\n",b->Name,b->Size,write,bad,hash);
    return UnlockBuffer(p,b) && bad==0;
}
static BOOL RunResidency(PROBE* p, UINT heap)
{
    UINT cycle;
    if(!StepFindAdapter(p)||!StepOpenAdapter(p)||!StepQueryCaps(p)||!StepCreateDevice(p)||!StepCreatePagingQueue(p))return FALSE;
    if(!CreateUmdBuffer(p,&p->Data,heap)||!PatternBuffer(p,&p->Data,0xBC250u,TRUE)||!Residency(p,&p->Data,"initial",FALSE))return FALSE;
    for(cycle=1;cycle<=3;cycle++) {
        D3DKMT_EVICT evict={0};
        evict.hDevice=p->hDevice;evict.NumAllocations=1;evict.AllocationList=&p->Data.hAllocation;
        printf("CYCLE %u BEGIN\n",cycle);
        if(!NT_SUCCESS(Report("Evict",D3DKMTEvict(&evict)))||!Residency(p,&p->Data,"after-evict",TRUE))return FALSE;
        // Dirty an equally large allocation after Evict, then release it.
        // This encourages reuse but does not by itself prove physical relocation.
        if(!CreateUmdBuffer(p,&p->Command,heap)||!PatternBuffer(p,&p->Command,cycle,TRUE))return FALSE;
        TeardownBuffer(p,&p->Command);
        if(!StepMakeResident(p,&p->Data)||!Residency(p,&p->Data,"after-resident",FALSE)||!PatternBuffer(p,&p->Data,0xBC250u,FALSE))return FALSE;
        printf("CYCLE %u PASS\n",cycle);
    }
    return TRUE;
}

#ifndef BC250_RESIDENCY_HELPERS_ONLY
int main(int argc,char**argv)
{
    PROBE p; BOOL ok; UINT heap=AMDGPU_GEM_DOMAIN_VRAM; UINT64 bytes=1ull<<30;
    if(argc>1 && strcmp(argv[1],"--help")==0) {puts("residency-probe [bytes<=1G] [vram|gtt]; target only;120s watchdog;3cycles");return 0;}
    if(argc>1 && (!ParseU64(argv[1],&bytes)||!bytes||bytes>(1ull<<30)||(bytes&4095)))return 2;
    if(argc>2) {if(!strcmp(argv[2],"gtt"))heap=AMDGPU_GEM_DOMAIN_GTT;else if(strcmp(argv[2],"vram"))return 2;}
    ZeroMemory(&p,sizeof(p));p.Opt.Match="bc250";p.Opt.FenceTimeoutMs=5000;
    p.Data.Name="preserved";p.Data.Size=bytes;p.Data.RequestedVa=0;
    p.Command.Name="pressure";p.Command.Size=bytes;p.Command.RequestedVa=0;
    setvbuf(stdout,NULL,_IONBF,0);
    if(!StartWatchdog(120000))return 2;
    printf("RESIDENCY PROBE bytes=%llu heap=%u cycles=3\n",bytes,heap);
    ok=RunResidency(&p,heap);Teardown(&p);SetEvent(g_Done);
    printf("RESIDENCY_RESULT %s (CPU readback; GPU paging requires separate evidence)\n",ok?"PASS":"FAIL");
    return ok?0:1;
}

#endif
