// Private CPU-only mapping diagnostic; no context, command buffer or GPU submit.
#define main unused_kmtprobe_main
#include "kmtprobe-snapshot.c"
#undef main

#define TEST_BYTES (4u * 1024u * 1024u)
#define TEST_WORDS (TEST_BYTES / sizeof(UINT32))
typedef struct { ULONG Magic, Version, Shared, CpuAccessFlags; } RESOURCE_PRIVATE;
C_ASSERT(sizeof(RESOURCE_PRIVATE)==16);

static UINT32 Expected(UINT32 index) { return index * 1664525u + 1013904223u; }
static void Fill(volatile UINT32* data)
{
    UINT32 i;
    for(i=0;i<TEST_WORDS;i++) data[i]=Expected(i);
    MemoryBarrier();
}
static UINT64 Verify(const volatile UINT32* data)
{
    UINT32 i; UINT64 bad=0;
    for(i=0;i<TEST_WORDS;i++) if(data[i]!=Expected(i)) bad++;
    return bad;
}
// Four independent sums avoid making the cached control depend on one add chain.
// Volatile preserves every scalar read. This is not a SIMD memcpy benchmark.
static UINT64 ReadSequential(const volatile UINT32* data)
{
    UINT32 i; UINT64 a=0,b=0,c=0,d=0;
    for(i=0;i<TEST_WORDS;i+=4) {a+=data[i];b+=data[i+1];c+=data[i+2];d+=data[i+3];}
    return a+b+c+d;
}
static UINT64 ExpectedSum(void)
{
    UINT32 i; UINT64 sum=0;
    for(i=0;i<TEST_WORDS;i++) sum+=Expected(i);
    return sum;
}
static BOOL InspectMapping(const char* tag, void* data)
{
    BYTE* at=(BYTE*)data; BYTE* end=at+TEST_BYTES;
    while(at<end) {
        MEMORY_BASIC_INFORMATION m; SIZE_T bytes=VirtualQuery(at,&m,sizeof(m));
        if(bytes!=sizeof(m)) {printf("VIRTUAL_QUERY_FAIL %s error=%lu\n",tag,GetLastError());return FALSE;}
        printf("MAPPING %s address=%p base=%p allocation_base=%p region=%llu state=0x%lX type=0x%lX protect=0x%lX allocation_protect=0x%lX wc=%u nocache=%u\n",
            tag,at,m.BaseAddress,m.AllocationBase,(UINT64)m.RegionSize,m.State,m.Type,m.Protect,m.AllocationProtect,
            !!(m.Protect&PAGE_WRITECOMBINE),!!(m.Protect&PAGE_NOCACHE));
        if(m.State!=MEM_COMMIT || (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
           !(m.Protect&(PAGE_READWRITE|PAGE_EXECUTE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_WRITECOPY)) ||
           (BYTE*)m.BaseAddress+m.RegionSize<=at) return FALSE;
        at=(BYTE*)m.BaseAddress+m.RegionSize;
    }
    return TRUE;
}
static BOOL Measure(const char* tag, void* memory)
{
    LARGE_INTEGER freq,start,end; UINT pass; UINT64 sum,wanted=ExpectedSum(),bad;
    if(!QueryPerformanceFrequency(&freq)||!InspectMapping(tag,memory))return FALSE;
    QueryPerformanceCounter(&start); Fill((volatile UINT32*)memory); QueryPerformanceCounter(&end);
    printf("WRITE %s bytes=%u qpc_start=%lld qpc_end=%lld frequency=%lld ms=%.6f\n",tag,TEST_BYTES,start.QuadPart,end.QuadPart,freq.QuadPart,1000.0*(end.QuadPart-start.QuadPart)/freq.QuadPart);
    bad=Verify((const volatile UINT32*)memory);
    printf("ORACLE %s words=%llu mismatches=%llu\n",tag,(UINT64)TEST_WORDS,bad);
    if(bad)return FALSE;
    for(pass=0;pass<3;pass++) {
        QueryPerformanceCounter(&start); sum=ReadSequential((const volatile UINT32*)memory); QueryPerformanceCounter(&end);
        printf("READ %s pass=%u bytes=%u qpc_start=%lld qpc_end=%lld frequency=%lld ms=%.6f MiBps=%.3f sum=%llu expected=%llu\n",
            tag,pass,TEST_BYTES,start.QuadPart,end.QuadPart,freq.QuadPart,1000.0*(end.QuadPart-start.QuadPart)/freq.QuadPart,
            4.0*freq.QuadPart/(end.QuadPart-start.QuadPart),sum,wanted);
        if(sum!=wanted)return FALSE;
    }
    return TRUE;
}
static UINT ResourceBlob(UINT version, RESOURCE_PRIVATE* blob)
{
    ZeroMemory(blob,sizeof(*blob));blob->Magic=0x52363245;blob->Version=version;blob->Shared=1;
    if(version==1)return 12;
    if(version==2){blob->CpuAccessFlags=2;return 16;}
    return 0;
}
static BOOL CreateSurface(PROBE* p, UINT version, D3DKMT_HANDLE* resource)
{
    BC250_WDDM_ALLOCATION_PRIVATE surface={0}; RESOURCE_PRIVATE blob;
    D3DDDI_ALLOCATIONINFO2 info={0}; D3DKMT_CREATEALLOCATION create={0};
    surface.Magic=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;surface.Version=1;
    surface.Width=1024;surface.Height=1024;surface.Pitch=4096;surface.Format=D3DDDIFMT_A8R8G8B8;surface.Size=TEST_BYTES;
    info.pPrivateDriverData=&surface;info.PrivateDriverDataSize=sizeof(surface);
    create.hDevice=p->hDevice;create.pPrivateDriverData=&blob;create.PrivateDriverDataSize=ResourceBlob(version,&blob);
    create.NumAllocations=1;create.pAllocationInfo2=&info;
    create.Flags.CreateResource=1;create.Flags.CreateShared=1;create.Flags.NtSecuritySharing=1;
    // Do not set CreateCached/CreateWriteCombined: these flags are kernel-only.
    printf("CREATE E26R version=%u bytes=%u shared=%lu cpu_access=%lu primary=%u surface=%ux%u pitch=%u size=%llu\n",
        version,create.PrivateDriverDataSize,blob.Shared,blob.CpuAccessFlags,info.Flags.Primary,surface.Width,surface.Height,surface.Pitch,surface.Size);
    if(!NT_SUCCESS(Report("CreateAllocation2 shared",D3DKMTCreateAllocation2(&create))))return FALSE;
    *resource=create.hResource;p->Data.hAllocation=info.hAllocation;
    printf("IDENTITY pid=%lu device=%08X resource=%08X allocation=%08X\n",GetCurrentProcessId(),p->hDevice,*resource,p->Data.hAllocation);
    return *resource!=0 && p->Data.hAllocation!=0;
}
static BOOL Cleanup(PROBE* p,D3DKMT_HANDLE resource)
{
    BOOL ok=TRUE;
    if(p->Data.Locked)ok=UnlockBuffer(p,&p->Data)&&ok;
    if(p->Data.MappedVa) {
        D3DKMT_FREEGPUVIRTUALADDRESS f={0};f.hAdapter=p->hAdapter;f.BaseAddress=p->Data.MappedVa;f.Size=p->Data.Size;
        ok=NT_SUCCESS(Report("FreeGpuVirtualAddress",D3DKMTFreeGpuVirtualAddress(&f)))&&ok;
    }
    if(resource||p->Data.hAllocation) {
        D3DKMT_DESTROYALLOCATION2 d={0};d.hDevice=p->hDevice;d.hResource=resource;
        if(!resource){d.phAllocationList=&p->Data.hAllocation;d.AllocationCount=1;}
        ok=NT_SUCCESS(Report("DestroyAllocation2/resource",D3DKMTDestroyAllocation2(&d)))&&ok;
    }
    if(p->hPagingQueue){D3DDDI_DESTROYPAGINGQUEUE d={0};d.hPagingQueue=p->hPagingQueue;ok=NT_SUCCESS(Report("DestroyPagingQueue",D3DKMTDestroyPagingQueue(&d)))&&ok;}
    if(p->hDevice){D3DKMT_DESTROYDEVICE d={0};d.hDevice=p->hDevice;ok=NT_SUCCESS(Report("DestroyDevice",D3DKMTDestroyDevice(&d)))&&ok;}
    if(p->hAdapter){D3DKMT_CLOSEADAPTER d={0};d.hAdapter=p->hAdapter;ok=NT_SUCCESS(Report("CloseAdapter",D3DKMTCloseAdapter(&d)))&&ok;}
    return ok;
}
static int SelfTest(void)
{
    UINT32* memory=(UINT32*)malloc(TEST_BYTES); RESOURCE_PRIVATE blob; BOOL ok;
    if(!memory)return 1;
    Fill(memory);ok=Verify(memory)==0 && ReadSequential(memory)==ExpectedSum();
    memory[0]^=1;memory[TEST_WORDS/2]^=1;memory[TEST_WORDS-1]^=1;
    ok=ok && Verify(memory)==3;
    ok=ok && ResourceBlob(1,&blob)==12 && blob.Version==1 && blob.Shared==1 && blob.CpuAccessFlags==0;
    ok=ok && ResourceBlob(2,&blob)==16 && blob.Version==2 && blob.Shared==1 && blob.CpuAccessFlags==2;
    ok=ok && ResourceBlob(3,&blob)==0;
    free(memory);printf("SELFTEST %s full4MiB oracle, three corruptions, sequential checksum, ABI v1/v2; no KMT calls\n",ok?"PASS":"FAIL");return ok?0:1;
}
int main(int argc,char** argv)
{
    PROBE p={0};D3DKMT_HANDLE resource=0;UINT version;BOOL ok;void* control;
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc==2&&!strcmp(argv[1],"--help")){puts("shared-map-probe --run v1|v2 : target only; fixed4MiB,3reads,30s watchdog\nshared-map-probe --selftest : host-safe memory/ABI oracle only");return 0;}
    if(argc==2&&!strcmp(argv[1],"--selftest"))return SelfTest();
    if(argc!=3||strcmp(argv[1],"--run")||(strcmp(argv[2],"v1")&&strcmp(argv[2],"v2"))){puts("Invalid arguments; use --help. No KMT calls made.");return 2;}
    version=!strcmp(argv[2],"v1")?1:2;
    p.Opt.Match="bc250";p.Opt.FenceTimeoutMs=5000;p.Data.Name="shared-map";p.Data.Size=TEST_BYTES;
    if(!StartWatchdog(30000))return 2;
    puts("VirtualQuery describes VM attributes, NOT effective PAT/MTRR. No GPU submission. Paging may occur.");
    control=VirtualAlloc(NULL,TEST_BYTES,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    ok=control && Measure("cached-control-before",control);
    if(ok)ok=StepFindAdapter(&p)&&StepOpenAdapter(&p)&&StepCreateDevice(&p)&&StepCreatePagingQueue(&p)&&
        CreateSurface(&p,version,&resource)&&StepMapVa(&p,&p.Data)&&StepMakeResident(&p,&p.Data)&&
        LockBuffer(&p,&p.Data)&&Measure("shared-Lock2",p.Data.Locked)&&UnlockBuffer(&p,&p.Data);
    ok=Cleanup(&p,resource)&&ok;
    if(control){if(ok)ok=Measure("cached-control-after",control);ok=VirtualFree(control,0,MEM_RELEASE)&&ok;}
    SetEvent(g_Done);printf("SHARED_MAP_RESULT %s version=%u\n",ok?"PASS":"FAIL",version);return ok?0:1;
}
