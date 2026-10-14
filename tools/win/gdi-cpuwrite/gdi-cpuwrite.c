// What the processor pays to reach the memory this driver gives a CPU-visible surface. Trial 5 of
// docs\testing\gdi-hw-accel-lab.md: the kernel GDI fallback and every staging copy of the CDD run on the
// processor, so the design page may not call that memory "shared DRAM" without a number beside it.
//
// Three memory classes, one process, one run:
//   sysmem  ordinary committed pages (VirtualAlloc). The baseline, write-back cached.
//   type0   a 32-byte LB7A block with no GDI trailer: the surface class of the compositor's own primaries.
//           WddmGdiAllocationPolicy(0, 0, 0) gives CpuVisible 1, Aperture 0, Cached 0, AccessedPhysically 1.
//   type2   a 48-byte LB7A block with the GDI1 trailer and Type 2, the standard GDI staging CPU-visible
//           surface. WddmGdiAllocationPolicy(2, ...) gives CpuVisible 1, Aperture 1, Cached 1.
// The policy line of each class is printed as an expectation, not a measurement: it is transcribed from
// driver\kmd\gdi_private.h, and build.ps1 fails when the magic numbers of that header move.
//
// Why not the OS's own GDI surface. D3DKMT_CREATESTANDARDALLOCATION of WDK 10.0.26100 holds one union
// member, ExistingHeapData (ref\ddi-display\d3dkmthk.md:21498). A user-mode process therefore cannot ask
// dxgkrnl for a D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE surface at all. The LB7A route reaches the same
// kernel decision from the other side: CreateAllocation runs Bc250Lb7aAdmit on this block and places it
// with the same WddmGdiAllocationPolicy call the CDD's staging surface takes (driver\kmd\gdi_admission.h).
//
// Three kinds of access per class, and every one of them is a kind the driver itself performs:
//   store64  a loop of 8-byte stores, like the kernel fallback's fill.
//   memcpy   a copy from system memory, like the kernel fallback's blt (RtlCopyMemory).
//   read64   a loop of 8-byte loads, like every blt that reads its destination.
//
// Bounded by construction: --mib megabytes (16 by default), --passes passes (3), and a watchdog that ends
// the process at --budget-ms (120000). Nothing is written to disk, no window is opened, no setting is
// changed, and every allocation is destroyed on the way out.
//
//   gdi-cpuwrite.exe                       the three classes, three passes each
//   gdi-cpuwrite.exe --classes sys,type2   two of them
//   gdi-cpuwrite.exe --help

#define WIN32_LEAN_AND_MEAN
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <winternl.h>       // NTSTATUS; d3dkmthk.h needs it and does not include it
#include <d3dkmthk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- what the KMD accepts, from driver\kmd\gdi_private.h ----------------------------------------------
//
// build.ps1 compares both magic numbers against that header: a copy that drifts is a build failure.
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul    // "LB7A"
#define BC250_GDI_PRIVATE_MAGIC 0x31494447ul                // "GDI1"

typedef struct _BC250_WDDM_ALLOCATION_PRIVATE {
    ULONG Magic;
    ULONG Version;
    ULONG Width;
    ULONG Height;
    ULONG Pitch;
    ULONG Format;                       // D3DDDIFORMAT
    ULONGLONG Size;
} BC250_WDDM_ALLOCATION_PRIVATE;

typedef struct _BC250_GDI_PRIVATE {
    BC250_WDDM_ALLOCATION_PRIVATE Surface;
    ULONG Magic;
    ULONG Type;
    ULONG Flags;
    ULONG Reserved;
} BC250_GDI_PRIVATE;

C_ASSERT(sizeof(BC250_WDDM_ALLOCATION_PRIVATE) == 32);
C_ASSERT(sizeof(BC250_GDI_PRIVATE) == 48);

#define BC250_ALLOCATION_PRIVATE_VERSION 1u
#define BC250_FMT_A8R8G8B8 21ul         // D3DDDIFMT_A8R8G8B8
#define GPU_PAGE_SIZE 4096ull
// The fork's winsys maps its allocations in this range and lets VidMm choose the address.
#define VA_MIN 0x0000000200000000ull
#define VA_MAX (1ull << 45)
// A staging surface's row is Width * 4 rounded up to 4 bytes (DcnStagingLayout), and the type-0 geometry
// check wants no more than that. One width keeps both layouts exact: 1024 pixels is a 4096-byte row.
#define SURFACE_WIDTH 1024ul
#define SURFACE_PITCH (SURFACE_WIDTH * 4ul)

#define PASSES_MAX 16u
#define CLASS_SYS 0
#define CLASS_TYPE0 1
#define CLASS_TYPE2 2
#define CLASS_COUNT 3
#define KIND_STORE 0
#define KIND_MEMCPY 1
#define KIND_READ 2
#define KIND_COUNT 3

static const char* const g_ClassNames[CLASS_COUNT] = { "sysmem", "type0", "type2" };
static const char* const g_KindNames[KIND_COUNT] = { "store64", "memcpy", "read64" };
// CpuVisible, Aperture, Cached, AccessedPhysically, as WddmGdiAllocationPolicy computes them for the type
// each class asks for. sysmem is not that function's business: it is ordinary cached memory.
static const char* const g_PolicyNames[CLASS_COUNT] = {
    "cpu_visible=1 aperture=0 cached=1 physical=0 (committed pages, not a driver allocation)",
    "cpu_visible=1 aperture=0 cached=0 physical=1",
    "cpu_visible=1 aperture=1 cached=1 physical=0"
};

typedef struct _OPTIONS {
    ULONG Mib;
    ULONG Passes;
    ULONG BudgetMs;
    int Class[CLASS_COUNT];
    char Match[64];
    BOOL HaveLuid;
    LUID Luid;
} OPTIONS;

typedef struct _RUN {
    OPTIONS Opt;
    LUID Luid;
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hPagingQueue;
    volatile UINT64* PagingFence;
    double Mbs[CLASS_COUNT][KIND_COUNT];        // the median of the passes, 0 when the class did not run
} RUN;

static LARGE_INTEGER g_Freq;

static const char* StatusName(NTSTATUS Status)
{
    switch (Status)
    {
    case STATUS_SUCCESS: return "STATUS_SUCCESS";
    case STATUS_PENDING: return "STATUS_PENDING";
    case STATUS_INVALID_PARAMETER: return "STATUS_INVALID_PARAMETER";
    case STATUS_NO_MEMORY: return "STATUS_NO_MEMORY";
    case STATUS_INSUFFICIENT_RESOURCES: return "STATUS_INSUFFICIENT_RESOURCES";
    case STATUS_GRAPHICS_NO_VIDEO_MEMORY: return "STATUS_GRAPHICS_NO_VIDEO_MEMORY";
    default: return "";
    }
}

static NTSTATUS Report(const char* Call, NTSTATUS Status)
{
    const char* name = StatusName(Status);

    if (name[0] != '\0') printf("  %-44s %s\n", Call, name);
    else printf("  %-44s 0x%08lX\n", Call, (unsigned long)Status);
    fflush(stdout);
    return Status;
}

static void Note(const char* Format, ...)
{
    va_list args;

    printf("  ");
    va_start(args, Format);
    vprintf(Format, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

static DWORD WINAPI WatchdogThread(LPVOID Parameter)
{
    DWORD ms = (DWORD)(ULONG_PTR)Parameter;

    Sleep(ms);
    printf("WATCHDOG the run passed its %lu ms budget\n", (unsigned long)ms);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 124);
    return 0;
}

// ---- the three kinds of access ------------------------------------------------------------------------
//
// Each one returns the seconds it took. The results are printed, so no loop can be removed: the store and
// the copy are read back word by word in Checksum, and the read's accumulator is printed with the rate.

static double Seconds(LARGE_INTEGER From, LARGE_INTEGER To)
{
    return (double)(To.QuadPart - From.QuadPart) / (double)g_Freq.QuadPart;
}

static double StorePass(void* Memory, SIZE_T Bytes, UINT64 Value)
{
    UINT64* p = (UINT64*)Memory;
    SIZE_T words = Bytes / sizeof(UINT64);
    SIZE_T i;
    LARGE_INTEGER t0, t1;

    QueryPerformanceCounter(&t0);
    for (i = 0; i < words; i++) p[i] = Value;
    QueryPerformanceCounter(&t1);
    return Seconds(t0, t1);
}

static double CopyPass(void* Memory, const void* Source, SIZE_T Bytes)
{
    LARGE_INTEGER t0, t1;

    QueryPerformanceCounter(&t0);
    memcpy(Memory, Source, Bytes);
    QueryPerformanceCounter(&t1);
    return Seconds(t0, t1);
}

static double ReadPass(const void* Memory, SIZE_T Bytes, UINT64* Sum)
{
    const UINT64* p = (const UINT64*)Memory;
    SIZE_T words = Bytes / sizeof(UINT64);
    SIZE_T i;
    UINT64 sum = 0;
    LARGE_INTEGER t0, t1;

    QueryPerformanceCounter(&t0);
    for (i = 0; i < words; i++) sum += p[i];
    QueryPerformanceCounter(&t1);
    *Sum = sum;
    return Seconds(t0, t1);
}

// The witness that the stores and the copy arrived: one word of every page, added up. A sum and not an
// exclusive-or, because the pass writes one value everywhere and an even number of equal words cancels.
static UINT64 Checksum(const void* Memory, SIZE_T Bytes)
{
    const UINT64* p = (const UINT64*)Memory;
    SIZE_T stride = (SIZE_T)(GPU_PAGE_SIZE / sizeof(UINT64));
    SIZE_T words = Bytes / sizeof(UINT64);
    SIZE_T i;
    UINT64 x = 0;

    for (i = 0; i < words; i += stride) x += p[i];
    return x;
}

static int CompareDouble(const void* A, const void* B)
{
    double a = *(const double*)A, b = *(const double*)B;

    return a < b ? -1 : (a > b ? 1 : 0);
}

// One class, three kinds, -Passes passes of each. Memory is already mapped for the processor.
static void Measure(RUN* Run, int Class, void* Memory, SIZE_T Bytes, const void* Source)
{
    double rates[PASSES_MAX];
    double mb = (double)Bytes / (1024.0 * 1024.0);
    ULONG pass;
    int kind;

    for (kind = 0; kind < KIND_COUNT; kind++)
    {
        for (pass = 0; pass < Run->Opt.Passes; pass++)
        {
            UINT64 sum = 0;
            double seconds;

            if (kind == KIND_STORE) seconds = StorePass(Memory, Bytes, 0x1122334455667788ull + pass);
            else if (kind == KIND_MEMCPY) seconds = CopyPass(Memory, Source, Bytes);
            else seconds = ReadPass(Memory, Bytes, &sum);
            rates[pass] = seconds > 0.0 ? mb / seconds : 0.0;
            // witness is read after the pass and outside its timing: one word of every page, so that a pass
            // which wrote nothing cannot look like a fast one.
            printf("CPUWRITE class=%s kind=%s pass=%lu bytes=%llu ms=%.2f mbs=%.1f sum=0x%016llX witness=0x%016llX\n",
                   g_ClassNames[Class], g_KindNames[kind], (unsigned long)(pass + 1), (unsigned long long)Bytes,
                   seconds * 1000.0, rates[pass], (unsigned long long)sum,
                   (unsigned long long)Checksum(Memory, Bytes));
            fflush(stdout);
        }
        qsort(rates, Run->Opt.Passes, sizeof(rates[0]), CompareDouble);
        Run->Mbs[Class][kind] = rates[Run->Opt.Passes / 2];
        printf("CPUWRITE class=%s kind=%s median_mbs=%.1f passes=%lu\n", g_ClassNames[Class], g_KindNames[kind],
               Run->Mbs[Class][kind], (unsigned long)Run->Opt.Passes);
        fflush(stdout);
    }
    printf("CPUWRITE class=%s checksum=0x%016llX\n", g_ClassNames[Class],
           (unsigned long long)Checksum(Memory, Bytes));
    fflush(stdout);
}

// ---- the adapter and the device -----------------------------------------------------------------------

static BOOL WideContainsAscii(const WCHAR* Haystack, const char* Needle)
{
    size_t n = strlen(Needle);
    size_t i, j;

    if (n == 0) return FALSE;
    for (i = 0; Haystack[i] != L'\0'; i++)
    {
        for (j = 0; j < n; j++)
        {
            WCHAR a = Haystack[i + j];
            WCHAR b = (WCHAR)Needle[j];

            if (a >= L'A' && a <= L'Z') a = (WCHAR)(a - L'A' + L'a');
            if (b >= L'A' && b <= L'Z') b = (WCHAR)(b - L'A' + L'a');
            if (a != b) break;
        }
        if (j == n) return TRUE;
    }
    return FALSE;
}

static NTSTATUS QueryAdapter(D3DKMT_HANDLE hAdapter, KMTQUERYADAPTERINFOTYPE Type, void* Buffer, UINT Size)
{
    D3DKMT_QUERYADAPTERINFO query;

    ZeroMemory(&query, sizeof(query));
    query.hAdapter = hAdapter;
    query.Type = Type;
    query.pPrivateDriverData = Buffer;
    query.PrivateDriverDataSize = Size;
    return D3DKMTQueryAdapterInfo(&query);
}

static BOOL FindAdapter(RUN* Run)
{
    D3DKMT_ENUMADAPTERS2 enumerate;
    D3DKMT_ADAPTERINFO* adapters;
    ULONG count, i;
    BOOL found = FALSE;

    ZeroMemory(&enumerate, sizeof(enumerate));
    if (!NT_SUCCESS(Report("D3DKMTEnumAdapters2 (count)", D3DKMTEnumAdapters2(&enumerate)))) return FALSE;
    count = enumerate.NumAdapters;
    if (count == 0) return FALSE;
    adapters = (D3DKMT_ADAPTERINFO*)calloc(count, sizeof(*adapters));
    if (adapters == NULL) return FALSE;
    enumerate.NumAdapters = count;
    enumerate.pAdapters = adapters;
    if (!NT_SUCCESS(Report("D3DKMTEnumAdapters2", D3DKMTEnumAdapters2(&enumerate)))) { free(adapters); return FALSE; }
    count = enumerate.NumAdapters;
    for (i = 0; i < count; i++)
    {
        D3DKMT_ADAPTERREGISTRYINFO registry;
        D3DKMT_UMDFILENAMEINFO umd;
        D3DKMT_CLOSEADAPTER close;
        BOOL match;

        ZeroMemory(&registry, sizeof(registry));
        ZeroMemory(&umd, sizeof(umd));
        umd.Version = KMTUMDVERSION_DX11;
        (void)QueryAdapter(adapters[i].hAdapter, KMTQAITYPE_ADAPTERREGISTRYINFO, &registry, sizeof(registry));
        (void)QueryAdapter(adapters[i].hAdapter, KMTQAITYPE_UMDRIVERNAME, &umd, sizeof(umd));
        Note("adapter %lu: \"%ls\" chip \"%ls\" umd \"%ls\"", (unsigned long)i, registry.AdapterString,
             registry.ChipType, umd.UmdFileName);
        if (Run->Opt.HaveLuid)
            match = adapters[i].AdapterLuid.LowPart == Run->Opt.Luid.LowPart &&
                    adapters[i].AdapterLuid.HighPart == Run->Opt.Luid.HighPart;
        else
            match = WideContainsAscii(registry.AdapterString, Run->Opt.Match) ||
                    WideContainsAscii(registry.ChipType, Run->Opt.Match) ||
                    WideContainsAscii(umd.UmdFileName, Run->Opt.Match);
        if (match && !found) { Run->Luid = adapters[i].AdapterLuid; found = TRUE; Note("  ^ selected"); }
        ZeroMemory(&close, sizeof(close));
        close.hAdapter = adapters[i].hAdapter;
        (void)D3DKMTCloseAdapter(&close);
    }
    free(adapters);
    if (!found) printf("FAILED no adapter matched \"%s\"\n", Run->Opt.Match);
    return found;
}

static BOOL OpenDevice(RUN* Run)
{
    D3DKMT_OPENADAPTERFROMLUID open;
    D3DKMT_CREATEDEVICE device;
    D3DKMT_CREATEPAGINGQUEUE queue;

    ZeroMemory(&open, sizeof(open));
    open.AdapterLuid = Run->Luid;
    if (!NT_SUCCESS(Report("D3DKMTOpenAdapterFromLuid", D3DKMTOpenAdapterFromLuid(&open)))) return FALSE;
    Run->hAdapter = open.hAdapter;
    ZeroMemory(&device, sizeof(device));
    device.hAdapter = Run->hAdapter;
    if (!NT_SUCCESS(Report("D3DKMTCreateDevice", D3DKMTCreateDevice(&device)))) return FALSE;
    Run->hDevice = device.hDevice;
    ZeroMemory(&queue, sizeof(queue));
    queue.hDevice = Run->hDevice;
    queue.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    queue.PhysicalAdapterIndex = 0;
    if (!NT_SUCCESS(Report("D3DKMTCreatePagingQueue", D3DKMTCreatePagingQueue(&queue)))) return FALSE;
    Run->hPagingQueue = queue.hPagingQueue;
    Run->PagingFence = (volatile UINT64*)queue.FenceValueCPUVirtualAddress;
    return TRUE;
}

static BOOL WaitPagingFence(RUN* Run, UINT64 Value, const char* What)
{
    ULONGLONG deadline = GetTickCount64() + 5000;

    if (Value == 0) return TRUE;
    if (Run->PagingFence == NULL) { Note("paging fence for %s is %llu with no CPU mapping", What, Value); return FALSE; }
    for (;;)
    {
        UINT64 seen = *Run->PagingFence;

        if (seen >= Value) return TRUE;
        if (GetTickCount64() >= deadline) { Note("TIMEOUT on the %s paging fence: %llu, wanted %llu", What, seen, Value); return FALSE; }
        Sleep(1);
    }
}

// A GPU virtual address and residency, exactly as a surface of this class gets them in service. The
// measurement is a CPU one, but a surface nobody mapped is not the surface the question is about.
static BOOL MapAndMakeResident(RUN* Run, D3DKMT_HANDLE hAllocation, UINT64 Size)
{
    D3DDDI_MAPGPUVIRTUALADDRESS map;
    D3DDDI_MAKERESIDENT resident;
    D3DKMT_HANDLE list[1];
    UINT priorities[1];
    NTSTATUS status;

    ZeroMemory(&map, sizeof(map));
    map.hPagingQueue = Run->hPagingQueue;
    map.hAllocation = hAllocation;
    map.MinimumAddress = VA_MIN;
    map.MaximumAddress = VA_MAX;
    map.SizeInPages = (Size + GPU_PAGE_SIZE - 1) / GPU_PAGE_SIZE;
    map.Protection.Write = 1;
    if (!NT_SUCCESS(Report("D3DKMTMapGpuVirtualAddress", D3DKMTMapGpuVirtualAddress(&map)))) return FALSE;
    if (!WaitPagingFence(Run, map.PagingFenceValue, "map")) return FALSE;

    list[0] = hAllocation;
    priorities[0] = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    ZeroMemory(&resident, sizeof(resident));
    resident.hPagingQueue = Run->hPagingQueue;
    resident.NumAllocations = 1;
    resident.AllocationList = list;
    resident.PriorityList = priorities;
    status = Report("D3DKMTMakeResident", D3DKMTMakeResident(&resident));
    if (status != STATUS_PENDING && !NT_SUCCESS(status)) return FALSE;
    return WaitPagingFence(Run, resident.PagingFenceValue, "makeresident");
}

// One allocation of the class, locked for the processor, measured, unlocked and destroyed.
static BOOL MeasureAllocation(RUN* Run, int Class, SIZE_T Bytes, const void* Source)
{
    BC250_GDI_PRIVATE block;
    D3DDDI_ALLOCATIONINFO2 info;
    D3DKMT_CREATEALLOCATION create;
    D3DKMT_DESTROYALLOCATION2 destroy;
    D3DKMT_LOCK2 lock;
    D3DKMT_UNLOCK2 unlock;
    D3DKMT_HANDLE list[1];
    ULONG height = (ULONG)(Bytes / SURFACE_PITCH);
    BOOL ok = FALSE;

    ZeroMemory(&block, sizeof(block));
    block.Surface.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    block.Surface.Version = BC250_ALLOCATION_PRIVATE_VERSION;
    block.Surface.Width = SURFACE_WIDTH;
    block.Surface.Height = height;
    block.Surface.Pitch = SURFACE_PITCH;
    block.Surface.Format = BC250_FMT_A8R8G8B8;
    block.Surface.Size = (ULONGLONG)Bytes;
    block.Magic = BC250_GDI_PRIVATE_MAGIC;
    block.Type = 2;
    ZeroMemory(&info, sizeof(info));
    info.pPrivateDriverData = &block;
    // A type-0 surface is the 32-byte block alone. The trailer is what names a standard GDI type, and the
    // kernel refuses any size between the two (WddmGdiPrivate).
    info.PrivateDriverDataSize = Class == CLASS_TYPE2 ? (UINT)sizeof(block) : (UINT)sizeof(block.Surface);
    ZeroMemory(&create, sizeof(create));
    create.hDevice = Run->hDevice;
    create.NumAllocations = 1;
    create.pAllocationInfo2 = &info;
    Note("%s: %lux%lu pitch %lu format A8R8G8B8 size %llu, private %u bytes", g_ClassNames[Class],
         SURFACE_WIDTH, (unsigned long)height, SURFACE_PITCH, (unsigned long long)Bytes,
         (unsigned)info.PrivateDriverDataSize);
    if (!NT_SUCCESS(Report("D3DKMTCreateAllocation2", D3DKMTCreateAllocation2(&create)))) return FALSE;
    if (!MapAndMakeResident(Run, info.hAllocation, (UINT64)Bytes)) goto out;

    ZeroMemory(&lock, sizeof(lock));
    lock.hDevice = Run->hDevice;
    lock.hAllocation = info.hAllocation;
    if (!NT_SUCCESS(Report("D3DKMTLock2", D3DKMTLock2(&lock))) || lock.pData == NULL) goto out;
    Note("%s: locked, page aligned %s", g_ClassNames[Class],
         ((ULONG_PTR)lock.pData & (ULONG_PTR)(GPU_PAGE_SIZE - 1)) == 0 ? "yes" : "no");
    Measure(Run, Class, lock.pData, Bytes, Source);
    ZeroMemory(&unlock, sizeof(unlock));
    unlock.hDevice = Run->hDevice;
    unlock.hAllocation = info.hAllocation;
    (void)Report("D3DKMTUnlock2", D3DKMTUnlock2(&unlock));
    ok = TRUE;

out:
    list[0] = info.hAllocation;
    ZeroMemory(&destroy, sizeof(destroy));
    destroy.hDevice = Run->hDevice;
    destroy.phAllocationList = list;
    destroy.AllocationCount = 1;
    (void)Report("D3DKMTDestroyAllocation2", D3DKMTDestroyAllocation2(&destroy));
    return ok;
}

// ---- options ------------------------------------------------------------------------------------------

static void Usage(void)
{
    printf("gdi-cpuwrite: the processor's rate into the memory classes of a CPU-visible surface\n");
    printf("  --mib <n>        megabytes per class (default 16, 1..256)\n");
    printf("  --passes <n>     passes of each kind (default 3, 1..%u, odd for a median)\n", PASSES_MAX);
    printf("  --budget-ms <n>  the watchdog (default 120000)\n");
    printf("  --classes <list> sys, type0, type2, comma separated (default all three)\n");
    printf("  --match <text>   adapter name, chip or user-mode driver file (default amdgpu)\n");
    printf("  --luid <hi:lo>   the adapter by LUID instead\n");
    printf("  --help\n");
}

static BOOL ParseClasses(OPTIONS* Opt, const char* List)
{
    const char* p = List;
    int i;

    for (i = 0; i < CLASS_COUNT; i++) Opt->Class[i] = 0;
    while (*p != '\0')
    {
        size_t n = strcspn(p, ",");

        if (n == 3 && _strnicmp(p, "sys", 3) == 0) Opt->Class[CLASS_SYS] = 1;
        else if (n == 6 && _strnicmp(p, "sysmem", 6) == 0) Opt->Class[CLASS_SYS] = 1;
        else if (n == 5 && _strnicmp(p, "type0", 5) == 0) Opt->Class[CLASS_TYPE0] = 1;
        else if (n == 5 && _strnicmp(p, "type2", 5) == 0) Opt->Class[CLASS_TYPE2] = 1;
        else { printf("FAILED unknown class \"%.*s\"\n", (int)n, p); return FALSE; }
        p += n;
        if (*p == ',') p++;
    }
    return Opt->Class[CLASS_SYS] || Opt->Class[CLASS_TYPE0] || Opt->Class[CLASS_TYPE2];
}

int main(int argc, char** argv)
{
    RUN run;
    OPTIONS* opt;
    void* source;
    SIZE_T bytes;
    int i, kind, any = 0;

    ZeroMemory(&run, sizeof(run));
    opt = &run.Opt;
    opt->Mib = 16;
    opt->Passes = 3;
    opt->BudgetMs = 120000;
    for (i = 0; i < CLASS_COUNT; i++) opt->Class[i] = 1;
    strcpy_s(opt->Match, sizeof(opt->Match), "amdgpu");
    for (i = 1; i < argc; i++)
    {
        const char* a = argv[i];
        const char* v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { Usage(); return 0; }
        else if (strcmp(a, "--mib") == 0 && v) { opt->Mib = (ULONG)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--passes") == 0 && v) { opt->Passes = (ULONG)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--budget-ms") == 0 && v) { opt->BudgetMs = (ULONG)strtoul(v, NULL, 10); i++; }
        else if (strcmp(a, "--classes") == 0 && v) { if (!ParseClasses(opt, v)) return 2; i++; }
        else if (strcmp(a, "--match") == 0 && v) { strcpy_s(opt->Match, sizeof(opt->Match), v); i++; }
        else if (strcmp(a, "--luid") == 0 && v)
        {
            unsigned long hi = 0, lo = 0;

            if (sscanf_s(v, "%lx:%lx", &hi, &lo) != 2) { printf("FAILED --luid wants hi:lo in hex\n"); return 2; }
            opt->Luid.HighPart = (LONG)hi;
            opt->Luid.LowPart = (DWORD)lo;
            opt->HaveLuid = TRUE;
            i++;
        }
        else { printf("FAILED unknown option \"%s\"\n", a); Usage(); return 2; }
    }
    if (opt->Mib < 1 || opt->Mib > 256) { printf("FAILED --mib is 1..256\n"); return 2; }
    if (opt->Passes < 1 || opt->Passes > PASSES_MAX) { printf("FAILED --passes is 1..%u\n", PASSES_MAX); return 2; }
    bytes = (SIZE_T)opt->Mib << 20;
    QueryPerformanceFrequency(&g_Freq);
    if (g_Freq.QuadPart == 0) { printf("FAILED no performance counter\n"); return 3; }
    if (CreateThread(NULL, 0, WatchdogThread, (LPVOID)(ULONG_PTR)opt->BudgetMs, 0, NULL) == NULL)
    {
        printf("FAILED the watchdog thread could not start\n");
        return 3;
    }
    printf("GDI-CPUWRITE %lu MiB, %lu pass(es), budget %lu ms\n", (unsigned long)opt->Mib,
           (unsigned long)opt->Passes, (unsigned long)opt->BudgetMs);
    for (i = 0; i < CLASS_COUNT; i++)
        if (opt->Class[i]) printf("POLICY class=%s expect %s\n", g_ClassNames[i], g_PolicyNames[i]);
    fflush(stdout);

    // The copy's source is ordinary memory, filled once: every class is copied from the same pages.
    source = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (source == NULL) { printf("FAILED VirtualAlloc of the source failed\n"); return 3; }
    memset(source, 0x5A, bytes);

    if (opt->Class[CLASS_SYS])
    {
        void* memory = VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        if (memory == NULL) printf("FAILED VirtualAlloc of the baseline failed\n");
        else
        {
            // First touch is a page fault, not a memory rate: the baseline is committed before it is timed.
            memset(memory, 0, bytes);
            Measure(&run, CLASS_SYS, memory, bytes, source);
            VirtualFree(memory, 0, MEM_RELEASE);
            any = 1;
        }
    }
    if (opt->Class[CLASS_TYPE0] || opt->Class[CLASS_TYPE2])
    {
        if (FindAdapter(&run) && OpenDevice(&run))
        {
            if (opt->Class[CLASS_TYPE0] && MeasureAllocation(&run, CLASS_TYPE0, bytes, source)) any = 1;
            if (opt->Class[CLASS_TYPE2] && MeasureAllocation(&run, CLASS_TYPE2, bytes, source)) any = 1;
        }
        if (run.hDevice)
        {
            D3DKMT_DESTROYDEVICE close;

            ZeroMemory(&close, sizeof(close));
            close.hDevice = run.hDevice;
            (void)Report("D3DKMTDestroyDevice", D3DKMTDestroyDevice(&close));
        }
        if (run.hAdapter)
        {
            D3DKMT_CLOSEADAPTER close;

            ZeroMemory(&close, sizeof(close));
            close.hAdapter = run.hAdapter;
            (void)D3DKMTCloseAdapter(&close);
        }
    }
    VirtualFree(source, 0, MEM_RELEASE);

    // One line a reader can quote: what each class costs against ordinary memory. A class that did not run
    // has no ratio, and says so rather than printing a zero that looks like an answer.
    for (i = 0; i < CLASS_COUNT; i++)
    {
        if (i == CLASS_SYS || run.Mbs[i][0] == 0.0) continue;
        printf("PENALTY class=%s", g_ClassNames[i]);
        for (kind = 0; kind < KIND_COUNT; kind++)
        {
            double base = run.Mbs[CLASS_SYS][kind];

            if (base > 0.0) printf(" %s=%.3fx", g_KindNames[kind], run.Mbs[i][kind] / base);
            else printf(" %s=no-baseline", g_KindNames[kind]);
        }
        printf("\n");
    }
    printf("%s\n", any ? "DONE" : "FAILED nothing was measured");
    fflush(stdout);
    return any ? 0 : 1;
}
