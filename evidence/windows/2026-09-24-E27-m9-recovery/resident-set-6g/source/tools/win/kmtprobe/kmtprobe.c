// kmtprobe - ADR 0008 stage B driven from user mode, through raw D3DKMT calls.
//
// It creates one allocation on our full WDDM miniport, gives it a GPU virtual address we choose, makes it
// resident, locks it and writes a pattern a witness (bc250rd, walking the page tables) can find again in
// physical memory. With --submit it continues into stage C (research document section 5.3): a virtual-
// addressing context, a monitored fence, a second allocation holding PM4 NOPs, one SubmitCommand and a
// bounded wait for the fence to reach 1. Every step is one function, and stage B is untouched by stage C.
//
// It runs ON THE TARGET, elevated, in session 0. On the development PC it only answers --help.
//
// Every call prints its name and its NTSTATUS. No wait is unbounded: each paging-fence wait has its own
// deadline and the whole process has a watchdog that kills it. A failure anywhere unwinds what was built,
// in reverse, and leaves a non-zero exit code behind.
//
// "Mierz siły na zamiary" - measure your strength against your intentions. Here the intention is one 64 KB
// buffer at an address we picked ourselves; the strength is a miniport that has never seen a page table.

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

// ---- what the KMD accepts -----------------------------------------------------------------------------
//
// driver/kmd/wddm.c, Bc250WddmCreateAllocation: an allocation whose per-allocation private blob is not
// exactly this structure is refused with STATUS_INVALID_PARAMETER. Magic and Size are checked; Size is what
// the KMD rounds to pages and reports back as DXGK_ALLOCATIONINFO::Size. Keep the two definitions identical.
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul    // "LB7A"

typedef struct _BC250_WDDM_ALLOCATION_PRIVATE {
    ULONG Magic;
    ULONG Version;
    ULONG Width;
    ULONG Height;
    ULONG Pitch;
    ULONG Format;                       // D3DDDIFORMAT
    ULONGLONG Size;
} BC250_WDDM_ALLOCATION_PRIVATE;

C_ASSERT(sizeof(BC250_WDDM_ALLOCATION_PRIVATE) == 32);

#define BC250_ALLOCATION_PRIVATE_VERSION 1u

// The GPU page size the KMD's declared VA layout is built on (BC250_WDDM_PAGE_SHIFT), and the unit
// D3DDDI_MAPGPUVIRTUALADDRESS counts offsets and sizes in either way.
#define GPU_PAGE_SIZE 4096ull
// D3DKMTReserveGpuVirtualAddress wants a 64 KB-aligned base (research section 3.2, constraints table).
#define GPU_RESERVE_ALIGN 0x10000ull

// The witness looks for this in memory. High six bytes "BC250B", low two bytes the word index.
#define PATTERN_TAG 0x4243323530420000ull

// ---- PM4, as the shim already writes it ---------------------------------------------------------------
//
// Verbatim from driver/amdgpu-import/nvd.h:33 (PACKET_TYPE3), :48 (PACKET3) and :55 (PACKET3_NOP). The dword
// count is the one gfx_v10_0.c:9505 uses - amdgpu_ring_write(ring, PACKET3(PACKET3_NOP, num_nop - 2)) - so a
// NOP that occupies `total` dwords carries n = total - 2: one header plus n + 1 body dwords, and the CP
// ignores the body. The single-dword pad is the PACKET2 NOP of driver/shim/bc250_gfx.c:68 (BC250_CP_NOP),
// what all three GFX10 ring types pad with.
#define PACKET_TYPE3 3u
#define PACKET3(op, n)  (((PACKET_TYPE3) << 30) | (((op) & 0xFFu) << 8) | ((((UINT32)(n)) & 0x3FFFu) << 16))
#define PACKET3_NOP 0x10u
#define BC250_CP_NOP 0xFFFF1000u
// bc250_gfx.c:66: "All three CP ring types pad to 8 dwords". The command stream is rounded to that.
#define PM4_DWORD_ALIGN 8u

// ---- the scratch write, as the driver's own ring and IB tests write it --------------------------------
//
// driver/shim/bc250_gfx.c:1707, bc250_gfx_ib_ring_test_build(), which is bc250_gfx_ring_test() (:1323) moved
// into an indirect buffer:
//
//     ib[0] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
//     ib[1] = scratch - PACKET3_SET_UCONFIG_REG_START;    // scratch = SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0)
//     ib[2] = 0xDEADBEEF;
//
// Three dwords, and the only thing --scratch changes is the value in ib[2]. The two packet constants are
// nvd.h:535-537; the register is driver/kmd/regs.generated.h:4, which gen_regs.py generated from the
// vendored AMD headers through tools/regcalc (regcalc lookup mmSCRATCH_REG0: mm=0x2040 seg1=0xA000 ->
// BAR5+0x30100). Repo rules 1 and 2: nothing here is typed from memory, and build.ps1 checks all four
// against their sources. SOC15_REG_OFFSET is a dword index into BAR5, hence the division by four.
#define PACKET3_SET_UCONFIG_REG 0x79u
#define PACKET3_SET_UCONFIG_REG_START 0x0000c000u
#define PACKET3_SET_UCONFIG_REG_END 0x0000c400u
#define BC250_REG_GC_SCRATCH_REG0 0x30100ul
#define SCRATCH_REG0_DWORD ((UINT32)(BC250_REG_GC_SCRATCH_REG0 / 4ul))
#define SCRATCH_REG0_UCONFIG (SCRATCH_REG0_DWORD - PACKET3_SET_UCONFIG_REG_START)
#define PM4_SCRATCH_DWORDS 3u

// A register outside the UCONFIG window cannot be written by this packet at all, and the offset in ib[1]
// would silently address something else. Better a build failure than a run that writes somewhere unknown.
C_ASSERT(SCRATCH_REG0_DWORD >= PACKET3_SET_UCONFIG_REG_START);
C_ASSERT(SCRATCH_REG0_DWORD < PACKET3_SET_UCONFIG_REG_END);

// ---- options ------------------------------------------------------------------------------------------

typedef struct _OPTIONS {
    BOOL HaveLuid;
    LUID Luid;
    const char* Match;                  // substring of the adapter string / UMD name, case-insensitive
    UINT64 Size;                        // allocation size in bytes, rounded up to a GPU page
    UINT64 Va;                          // requested base GPU virtual address, 0 = let VidMm choose
    UINT64 VaMin;                       // only consulted when Va == 0
    UINT64 VaMax;
    BOOL Reserve;                       // D3DKMTReserveGpuVirtualAddress before the map
    BOOL ResidentFirst;                 // MakeResident before MapGpuVirtualAddress
    BOOL NoWrite;                       // lock, but leave the contents alone
    BOOL Submit;                        // stage C: context, monitored fence, one submission of NOPs
    BOOL HaveScratch;                   // H4: lead the stream with a SET_UCONFIG_REG write to SCRATCH_REG0
    UINT32 Scratch;                     // the value that write carries
    UINT64 CommandVa;                   // where the command buffer is mapped
    UINT32 Nops;                        // dwords of PM4 in the command buffer
    DWORD HoldSeconds;
    DWORD FenceTimeoutMs;
    DWORD WatchdogMs;
} OPTIONS;

// ---- probe state --------------------------------------------------------------------------------------

// One allocation with its GPU VA. The data buffer and the command buffer differ only in what goes into them,
// so every memory step below takes one of these and the transcript says which.
typedef struct _BUFFER {
    const char* Name;                   // "data" or "cmd", printed next to every call
    UINT64 Size;                        // page-rounded
    UINT64 RequestedVa;                 // what we ask for, 0 = let VidMm choose
    D3DKMT_HANDLE hAllocation;
    UINT64 ReservedBase;                // 0 when nothing was reserved
    UINT64 ReservedSize;
    UINT64 MappedVa;                    // 0 when nothing is mapped
    void* Locked;                       // NULL when not locked
} BUFFER;

typedef enum _SUBMIT_RESULT {
    SubmitNotAttempted = 0,
    SubmitFenceReached,                 // the monitored fence reached the value we asked for
    SubmitFenceTimeout,                 // every call succeeded, the fence did not move in time
    SubmitFailed                        // a call failed
} SUBMIT_RESULT;

typedef struct _PROBE {
    OPTIONS Opt;
    LUID Luid;
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hPagingQueue;
    D3DKMT_HANDLE hPagingFenceObject;
    volatile UINT64* PagingFence;       // CPU-readable value of the paging queue's monitored fence
    BUFFER Data;
    BUFFER Command;

    // stage C
    D3DKMT_HANDLE hContext;
    D3DKMT_HANDLE hFence;
    volatile UINT64* FenceCpuVa;        // read-only mapping of the monitored fence
    UINT64 FenceGpuVa;                  // where the GPU would write it
    HANDLE WaitEvent;                   // handed to WaitForSynchronizationObjectFromCpu, never NULL
    UINT32 CommandLength;               // bytes of PM4 actually submitted
    SUBMIT_RESULT Submit;
    DWORD SubmitElapsedMs;
} PROBE;

// ---- NTSTATUS names -----------------------------------------------------------------------------------

#define STATUS_ENTRY(x) { (x), #x }

static const struct { NTSTATUS Status; const char* Name; } g_StatusNames[] = {
    STATUS_ENTRY(STATUS_SUCCESS),
    STATUS_ENTRY(STATUS_PENDING),
    STATUS_ENTRY(STATUS_INVALID_PARAMETER),
    STATUS_ENTRY(STATUS_INVALID_HANDLE),
    STATUS_ENTRY(STATUS_NO_MEMORY),
    STATUS_ENTRY(STATUS_BUFFER_TOO_SMALL),
    STATUS_ENTRY(STATUS_INSUFFICIENT_RESOURCES),
    STATUS_ENTRY(STATUS_NOT_SUPPORTED),
    STATUS_ENTRY(STATUS_NOT_IMPLEMENTED),
    STATUS_ENTRY(STATUS_ACCESS_DENIED),
    STATUS_ENTRY(STATUS_DEVICE_REMOVED),
    STATUS_ENTRY(STATUS_GRAPHICS_INVALID_DRIVER_MODEL),
    STATUS_ENTRY(STATUS_GRAPHICS_NO_VIDEO_MEMORY),
    STATUS_ENTRY(STATUS_GRAPHICS_CANT_LOCK_MEMORY),
    STATUS_ENTRY(STATUS_GRAPHICS_ALLOCATION_BUSY),
    STATUS_ENTRY(STATUS_GRAPHICS_ALLOCATION_INVALID),
    STATUS_ENTRY(STATUS_GRAPHICS_INVALID_ALLOCATION_USAGE),
    STATUS_ENTRY(STATUS_GRAPHICS_GPU_EXCEPTION_ON_DEVICE),
};

static const char* StatusName(NTSTATUS Status)
{
    size_t i;

    for (i = 0; i < ARRAYSIZE(g_StatusNames); i++)
        if (g_StatusNames[i].Status == Status) return g_StatusNames[i].Name;
    return "";
}

// Every D3DKMT call in this file goes through here, so that the transcript has one shape.
static NTSTATUS Report(const char* Call, NTSTATUS Status)
{
    const char* name = StatusName(Status);

    printf("%-34s 0x%08lX%s%s\n", Call, (unsigned long)Status, name[0] ? "  " : "", name);
    fflush(stdout);
    return Status;
}

// The same, for a call that acts on one buffer: "D3DKMTLock2 [cmd]".
static NTSTATUS ReportOn(const char* Call, const BUFFER* Buffer, NTSTATUS Status)
{
    char label[64];

    sprintf_s(label, sizeof(label), "%s [%s]", Call, Buffer->Name);
    return Report(label, Status);
}

static void Note(const char* Format, ...)
{
    va_list args;

    va_start(args, Format);
    printf("    ");
    vprintf(Format, args);
    printf("\n");
    va_end(args);
    fflush(stdout);
}

// ---- the watchdog -------------------------------------------------------------------------------------
//
// Nothing below may hang the machine, but a D3DKMT call that never returns would hang the SSH session that
// started us, and the owner would have to look at the target. The watchdog is the promise that the process
// ends by itself. Exit code 4 means it fired.

static HANDLE g_Done;

static DWORD WINAPI WatchdogThread(LPVOID Parameter)
{
    DWORD ms = (DWORD)(ULONG_PTR)Parameter;

    if (WaitForSingleObject(g_Done, ms) == WAIT_TIMEOUT)
    {
        printf("WATCHDOG fired after %lu ms, terminating\n", ms);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 4);
    }
    return 0;
}

static BOOL StartWatchdog(DWORD Milliseconds)
{
    HANDLE thread;

    g_Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_Done == NULL) return FALSE;
    thread = CreateThread(NULL, 0, WatchdogThread, (LPVOID)(ULONG_PTR)Milliseconds, 0, NULL);
    if (thread == NULL) return FALSE;
    CloseHandle(thread);
    return TRUE;
}

// ---- paging fence -------------------------------------------------------------------------------------
//
// "The user-mode driver must ensure that this fence is retired ... before allowing the GPU to access the
// mapped range" (research section 3.2). A zero fence value means the operation is already done. The poll is
// the zero-syscall form; the deadline is what keeps a KMD that never completes a paging operation - which is
// exactly what stage A's inert BuildPagingBuffer might look like - from turning into a hung tool.
static BOOL WaitPagingFence(PROBE* Probe, UINT64 Value, const char* What)
{
    ULONGLONG deadline = GetTickCount64() + Probe->Opt.FenceTimeoutMs;
    UINT64 seen;

    if (Value == 0)
    {
        Note("paging fence for %s is 0, already retired", What);
        return TRUE;
    }
    if (Probe->PagingFence == NULL)
    {
        Note("paging fence for %s is %llu but there is no CPU mapping to poll", What, Value);
        return FALSE;
    }
    for (;;)
    {
        seen = *Probe->PagingFence;
        if (seen >= Value)
        {
            Note("paging fence for %s reached %llu (waited for %llu)", What, seen, Value);
            return TRUE;
        }
        if (GetTickCount64() >= deadline)
        {
            Note("TIMEOUT waiting for the %s paging fence: %llu, wanted %llu, after %lu ms", What, seen,
                 Value, Probe->Opt.FenceTimeoutMs);
            return FALSE;
        }
        Sleep(1);
    }
}

// ---- step 1: find the adapter -------------------------------------------------------------------------

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

// One KMTQAITYPE query, on an adapter handle we already hold. Failures are printed, not fatal: an adapter
// that refuses a query is still worth listing.
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

static BOOL StepFindAdapter(PROBE* Probe)
{
    D3DKMT_ENUMADAPTERS2 enumerate;
    D3DKMT_ADAPTERINFO* adapters = NULL;
    NTSTATUS status;
    ULONG count = 0;
    ULONG i;
    BOOL found = FALSE;

    ZeroMemory(&enumerate, sizeof(enumerate));
    status = Report("D3DKMTEnumAdapters2 (count)", D3DKMTEnumAdapters2(&enumerate));
    if (!NT_SUCCESS(status)) return FALSE;
    count = enumerate.NumAdapters;
    Note("%lu adapter(s)", count);
    if (count == 0) return FALSE;

    adapters = (D3DKMT_ADAPTERINFO*)calloc(count, sizeof(*adapters));
    if (adapters == NULL) return FALSE;
    enumerate.NumAdapters = count;
    enumerate.pAdapters = adapters;
    status = Report("D3DKMTEnumAdapters2", D3DKMTEnumAdapters2(&enumerate));
    if (!NT_SUCCESS(status))
    {
        free(adapters);
        return FALSE;
    }
    count = enumerate.NumAdapters;

    for (i = 0; i < count; i++)
    {
        D3DKMT_ADAPTERREGISTRYINFO registry;
        D3DKMT_UMDFILENAMEINFO umd;
        BOOL match;

        ZeroMemory(&registry, sizeof(registry));
        ZeroMemory(&umd, sizeof(umd));
        umd.Version = KMTUMDVERSION_DX11;

        printf("  adapter %lu: LUID %08lX:%08lX  NumOfSources %lu\n", i,
               (unsigned long)adapters[i].AdapterLuid.HighPart,
               (unsigned long)adapters[i].AdapterLuid.LowPart, adapters[i].NumOfSources);
        if (NT_SUCCESS(QueryAdapter(adapters[i].hAdapter, KMTQAITYPE_ADAPTERREGISTRYINFO, &registry,
                                    sizeof(registry))))
            printf("    AdapterString \"%ls\"  ChipType \"%ls\"\n", registry.AdapterString, registry.ChipType);
        if (NT_SUCCESS(QueryAdapter(adapters[i].hAdapter, KMTQAITYPE_UMDRIVERNAME, &umd, sizeof(umd))))
            printf("    UmdFileName   \"%ls\"\n", umd.UmdFileName);

        if (Probe->Opt.HaveLuid)
            match = (adapters[i].AdapterLuid.LowPart == Probe->Opt.Luid.LowPart &&
                     adapters[i].AdapterLuid.HighPart == Probe->Opt.Luid.HighPart);
        else
            match = WideContainsAscii(registry.AdapterString, Probe->Opt.Match) ||
                    WideContainsAscii(registry.ChipType, Probe->Opt.Match) ||
                    WideContainsAscii(umd.UmdFileName, Probe->Opt.Match);
        if (match && !found)
        {
            Probe->Luid = adapters[i].AdapterLuid;
            found = TRUE;
            printf("    ^ selected\n");
        }
        // EnumAdapters2 hands out open adapter handles. They are not the handle we work with (the task is to
        // go through OpenAdapterFromLuid), and leaking them would leak kernel objects for the process.
        {
            D3DKMT_CLOSEADAPTER close;

            ZeroMemory(&close, sizeof(close));
            close.hAdapter = adapters[i].hAdapter;
            (void)D3DKMTCloseAdapter(&close);
        }
    }
    free(adapters);
    fflush(stdout);

    if (!found)
    {
        printf("no adapter matched %s%s\n", Probe->Opt.HaveLuid ? "the LUID given with --luid" : "--match ",
               Probe->Opt.HaveLuid ? "" : Probe->Opt.Match);
        return FALSE;
    }
    return TRUE;
}

static BOOL StepOpenAdapter(PROBE* Probe)
{
    D3DKMT_OPENADAPTERFROMLUID open;

    ZeroMemory(&open, sizeof(open));
    open.AdapterLuid = Probe->Luid;
    if (!NT_SUCCESS(Report("D3DKMTOpenAdapterFromLuid", D3DKMTOpenAdapterFromLuid(&open)))) return FALSE;
    Probe->hAdapter = open.hAdapter;
    Note("hAdapter 0x%08lX for LUID %08lX:%08lX", (unsigned long)Probe->hAdapter,
         (unsigned long)Probe->Luid.HighPart, (unsigned long)Probe->Luid.LowPart);
    return TRUE;
}

// ---- step 2: the caps -----------------------------------------------------------------------------------

static BOOL StepQueryCaps(PROBE* Probe)
{
    D3DKMT_QUERY_GPUMMU_CAPS gpummu;
    D3DKMT_NODEMETADATA node;
    D3DKMT_DRIVERVERSION version = (D3DKMT_DRIVERVERSION)0;
    D3DKMT_WDDM_2_0_CAPS caps20;

    ZeroMemory(&gpummu, sizeof(gpummu));
    gpummu.PhysicalAdapterIndex = 0;
    if (NT_SUCCESS(Report("QueryAdapterInfo GPUMMU_CAPS",
                          QueryAdapter(Probe->hAdapter, KMTQAITYPE_QUERY_GPUMMU_CAPS, &gpummu, sizeof(gpummu)))))
        Note("VirtualAddressBitCount %u, flags 0x%08X (ReadOnly %u, NoExecute %u, CacheCoherent %u)",
             gpummu.Caps.VirtualAddressBitCount, gpummu.Caps.Flags.Value,
             gpummu.Caps.Flags.ReadOnlyMemorySupported, gpummu.Caps.Flags.NoExecuteMemorySupported,
             gpummu.Caps.Flags.CacheCoherentMemorySupported);

    // High word physical adapter index, low word node ordinal. Node 0 is our only node (DXGK_ENGINE_TYPE_3D).
    ZeroMemory(&node, sizeof(node));
    node.NodeOrdinalAndAdapterIndex = 0;
    if (NT_SUCCESS(Report("QueryAdapterInfo NODEMETADATA 0",
                          QueryAdapter(Probe->hAdapter, KMTQAITYPE_NODEMETADATA, &node, sizeof(node)))))
        Note("node 0: EngineType %u \"%ls\" GpuMmu %u IoMmu %u flags 0x%08X",
             (unsigned)node.NodeData.EngineType, node.NodeData.FriendlyName, node.NodeData.GpuMmuSupported,
             node.NodeData.IoMmuSupported, node.NodeData.Flags.Value);

    if (NT_SUCCESS(Report("QueryAdapterInfo DRIVERVERSION",
                          QueryAdapter(Probe->hAdapter, KMTQAITYPE_DRIVERVERSION, &version, sizeof(version)))))
        Note("KMT_DRIVERVERSION %u (2000 = WDDM 2.0)", (unsigned)version);

    // Documented as "reserved for system use", queried anyway because what dxgkrnl reports back about our own
    // caps is evidence. GpuMmu per node comes from NODEMETADATA above, not from here.
    ZeroMemory(&caps20, sizeof(caps20));
    if (NT_SUCCESS(Report("QueryAdapterInfo WDDM_2_0_CAPS",
                          QueryAdapter(Probe->hAdapter, KMTQAITYPE_WDDM_2_0_CAPS, &caps20, sizeof(caps20)))))
        Note("WDDM_2_0_CAPS 0x%08X (GpuMmuSupported %u, IoMmuSupported %u)", caps20.Value,
             caps20.GpuMmuSupported, caps20.IoMmuSupported);

    // None of these is fatal: the point of the probe is to see how far the miniport gets.
    return TRUE;
}

// ---- step 3: device and paging queue ---------------------------------------------------------------------

static BOOL StepCreateDevice(PROBE* Probe)
{
    D3DKMT_CREATEDEVICE create;

    ZeroMemory(&create, sizeof(create));
    create.hAdapter = Probe->hAdapter;
    if (!NT_SUCCESS(Report("D3DKMTCreateDevice", D3DKMTCreateDevice(&create)))) return FALSE;
    Probe->hDevice = create.hDevice;
    Note("hDevice 0x%08lX", (unsigned long)Probe->hDevice);
    return TRUE;
}

static BOOL StepCreatePagingQueue(PROBE* Probe)
{
    D3DKMT_CREATEPAGINGQUEUE create;

    ZeroMemory(&create, sizeof(create));
    create.hDevice = Probe->hDevice;
    create.Priority = D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    create.PhysicalAdapterIndex = 0;
    if (!NT_SUCCESS(Report("D3DKMTCreatePagingQueue", D3DKMTCreatePagingQueue(&create)))) return FALSE;
    Probe->hPagingQueue = create.hPagingQueue;
    Probe->hPagingFenceObject = create.hSyncObject;
    Probe->PagingFence = (volatile UINT64*)create.FenceValueCPUVirtualAddress;
    Note("hPagingQueue 0x%08lX hSyncObject 0x%08lX fence CPU VA %p, current value %llu",
         (unsigned long)Probe->hPagingQueue, (unsigned long)Probe->hPagingFenceObject,
         (void*)Probe->PagingFence, Probe->PagingFence ? *Probe->PagingFence : 0ull);
    return TRUE;
}

// ---- step 4: the allocation ------------------------------------------------------------------------------

static BOOL StepCreateAllocation(PROBE* Probe, BUFFER* Buffer)
{
    BC250_WDDM_ALLOCATION_PRIVATE private;
    D3DDDI_ALLOCATIONINFO2 info;
    D3DKMT_CREATEALLOCATION create;

    // The KMD checks Magic and Size and keeps the rest for DescribeAllocation. Presented as one linear row so
    // that Pitch * Height == Size holds, which is the shape stage A's GetStandardAllocationDriverData builds.
    ZeroMemory(&private, sizeof(private));
    private.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    private.Version = BC250_ALLOCATION_PRIVATE_VERSION;
    private.Width = (ULONG)(Buffer->Size / 4);
    private.Height = 1;
    private.Pitch = (ULONG)Buffer->Size;
    private.Format = (ULONG)D3DDDIFMT_A8R8G8B8;
    private.Size = Buffer->Size;

    ZeroMemory(&info, sizeof(info));
    info.pPrivateDriverData = &private;
    info.PrivateDriverDataSize = (UINT)sizeof(private);

    ZeroMemory(&create, sizeof(create));
    create.hDevice = Probe->hDevice;
    // No resource: hResource stays 0 and Flags.CreateResource stays clear, so the resource-level blob the KMD
    // ignores today is not sent either.
    create.NumAllocations = 1;
    create.pAllocationInfo2 = &info;

    Note("private blob %u bytes, magic 0x%08lX, Size %llu, %lux%lu pitch %lu format %lu",
         (unsigned)sizeof(private), (unsigned long)private.Magic, private.Size,
         (unsigned long)private.Width, (unsigned long)private.Height, (unsigned long)private.Pitch,
         (unsigned long)private.Format);
    if (!NT_SUCCESS(ReportOn("D3DKMTCreateAllocation2", Buffer, D3DKMTCreateAllocation2(&create)))) return FALSE;
    Buffer->hAllocation = info.hAllocation;
    Note("hAllocation 0x%08lX, GpuVirtualAddress reported 0x%016llX",
         (unsigned long)Buffer->hAllocation, info.GpuVirtualAddress);
    return TRUE;
}

// ---- step 5: GPU virtual address -------------------------------------------------------------------------

static BOOL StepReserveVa(PROBE* Probe, BUFFER* Buffer)
{
    D3DDDI_RESERVEGPUVIRTUALADDRESS reserve;
    UINT64 size = (Buffer->Size + GPU_RESERVE_ALIGN - 1) & ~(GPU_RESERVE_ALIGN - 1);

    // The struct changed meaning between revisions (research section 3.2): zero it, set only hAdapter,
    // BaseAddress, Minimum, Maximum and Size, and never wait on the fence slot of this call.
    ZeroMemory(&reserve, sizeof(reserve));
    reserve.hAdapter = Probe->hAdapter;
    reserve.BaseAddress = Buffer->RequestedVa;
    if (Buffer->RequestedVa == 0)
    {
        reserve.MinimumAddress = Probe->Opt.VaMin;
        reserve.MaximumAddress = Probe->Opt.VaMax;
    }
    reserve.Size = size;
    reserve.ReservationType = D3DDDIGPUVIRTUALADDRESS_RESERVE_NO_ACCESS;
    Note("reserving 0x%llX bytes at 0x%016llX", size, Buffer->RequestedVa);
    if (!NT_SUCCESS(ReportOn("D3DKMTReserveGpuVirtualAddress", Buffer, D3DKMTReserveGpuVirtualAddress(&reserve))))
        return FALSE;
    Buffer->ReservedBase = reserve.VirtualAddress;
    Buffer->ReservedSize = size;
    Note("reserved at 0x%016llX (asked 0x%016llX)", Buffer->ReservedBase, Buffer->RequestedVa);
    return TRUE;
}

static BOOL StepMapVa(PROBE* Probe, BUFFER* Buffer)
{
    D3DDDI_MAPGPUVIRTUALADDRESS map;
    UINT64 base = Buffer->ReservedBase != 0 ? Buffer->ReservedBase : Buffer->RequestedVa;

    ZeroMemory(&map, sizeof(map));
    map.hPagingQueue = Probe->hPagingQueue;
    map.hAllocation = Buffer->hAllocation;
    map.BaseAddress = base;
    if (base == 0)
    {
        // Only consulted when no exact base is named; with a base, Minimum and Maximum are ignored.
        map.MinimumAddress = Probe->Opt.VaMin;
        map.MaximumAddress = Probe->Opt.VaMax;
    }
    map.OffsetInPages = 0;
    map.SizeInPages = Buffer->Size / GPU_PAGE_SIZE;
    // Read plus write: read is the absence of NoAccess, there is no separate Read bit.
    map.Protection.Value = 0;
    map.Protection.Write = 1;
    map.DriverProtection = 0;           // the per-PTE hook, once BuildPagingBuffer translates DXGK_PTE

    Note("mapping %llu page(s) of 0x%llX bytes at base 0x%016llX", map.SizeInPages, Buffer->Size, base);
    if (!NT_SUCCESS(ReportOn("D3DKMTMapGpuVirtualAddress", Buffer, D3DKMTMapGpuVirtualAddress(&map)))) return FALSE;
    Buffer->MappedVa = map.VirtualAddress;
    Note("VirtualAddress 0x%016llX (asked 0x%016llX, %s), PagingFenceValue %llu", map.VirtualAddress, base,
         (base == 0 || map.VirtualAddress == base) ? "honoured" : "DIFFERENT", map.PagingFenceValue);
    return WaitPagingFence(Probe, map.PagingFenceValue, "MapGpuVirtualAddress");
}

// ---- step 6: residency -----------------------------------------------------------------------------------

static BOOL StepMakeResident(PROBE* Probe, BUFFER* Buffer)
{
    D3DDDI_MAKERESIDENT resident;
    D3DKMT_HANDLE list[1];
    UINT priorities[1];
    NTSTATUS status;

    list[0] = Buffer->hAllocation;
    priorities[0] = D3DDDI_ALLOCATIONPRIORITY_NORMAL;

    ZeroMemory(&resident, sizeof(resident));
    resident.hPagingQueue = Probe->hPagingQueue;
    resident.NumAllocations = 1;
    resident.AllocationList = list;
    resident.PriorityList = priorities;
    status = ReportOn("D3DKMTMakeResident", Buffer, D3DKMTMakeResident(&resident));
    // NumAllocations is in and out: partial success is legal and is not an error by itself.
    Note("NumAllocations back %u, PagingFenceValue %llu, NumBytesToTrim %llu", resident.NumAllocations,
         resident.PagingFenceValue, resident.NumBytesToTrim);
    if (status == STATUS_PENDING) Note("STATUS_PENDING: the fence below is the completion");
    else if (!NT_SUCCESS(status)) return FALSE;
    if (resident.NumAllocations != 1)
    {
        Note("only %u of 1 allocation(s) made resident", resident.NumAllocations);
        return FALSE;
    }
    return WaitPagingFence(Probe, resident.PagingFenceValue, "MakeResident");
}

// ---- step 7: lock, write the pattern, unlock ---------------------------------------------------------------

static BOOL LockBuffer(PROBE* Probe, BUFFER* Buffer)
{
    D3DKMT_LOCK2 lock;

    ZeroMemory(&lock, sizeof(lock));
    lock.hDevice = Probe->hDevice;
    lock.hAllocation = Buffer->hAllocation;
    lock.Flags.Value = 0;               // WDDM 2.0's Lock2 flags are entirely reserved; there is nothing to say
    if (!NT_SUCCESS(ReportOn("D3DKMTLock2", Buffer, D3DKMTLock2(&lock)))) return FALSE;
    Buffer->Locked = lock.pData;
    Note("locked at CPU VA %p", lock.pData);
    return lock.pData != NULL;
}

static BOOL UnlockBuffer(PROBE* Probe, BUFFER* Buffer)
{
    D3DKMT_UNLOCK2 unlock;

    ZeroMemory(&unlock, sizeof(unlock));
    unlock.hDevice = Probe->hDevice;
    unlock.hAllocation = Buffer->hAllocation;
    if (!NT_SUCCESS(ReportOn("D3DKMTUnlock2", Buffer, D3DKMTUnlock2(&unlock)))) return FALSE;
    Buffer->Locked = NULL;
    return TRUE;
}

static BOOL StepLockAndWrite(PROBE* Probe, BUFFER* Buffer)
{
    UINT64 words = Buffer->Size / sizeof(UINT64);
    UINT64 i;
    volatile UINT64* data;

    if (!LockBuffer(Probe, Buffer)) return FALSE;
    if (Probe->Opt.NoWrite)
    {
        Note("--no-write: contents left alone");
    }
    else
    {
        data = (volatile UINT64*)Buffer->Locked;
        for (i = 0; i < words; i++) data[i] = PATTERN_TAG | (i & 0xFFFFull);
        Note("wrote %llu qword(s), first 0x%016llX last 0x%016llX", words, data[0], data[words - 1]);
    }
    return UnlockBuffer(Probe, Buffer);
}

// ---- stage C: a context, a monitored fence, one submission of NOPs -------------------------------------------
//
// Research section 5.3, steps 6 to 15, minus the RELEASE_MEM: this first version puts nothing but NOPs in the
// command buffer, so the only thing that can move the monitored fence is dxgkrnl's own signal path through the
// miniport. That is the measurement - whether a submission on a GpuMmu context reaches the fence at all.

static BOOL StepCreateContext(PROBE* Probe)
{
    D3DKMT_CREATECONTEXTVIRTUAL create;

    ZeroMemory(&create, sizeof(create));
    create.hDevice = Probe->hDevice;
    create.NodeOrdinal = 0;             // our only node, DXGK_ENGINE_TYPE_3D
    create.EngineAffinity = 0;
    // DisableGpuTimeout suppresses TDR on this context. ADR 0008 point 7: until a reset exists, timeouts are
    // avoided rather than recovered, and a TDR here costs the owner a trip to the machine.
    create.Flags.DisableGpuTimeout = 1;
    create.ClientHint = D3DKMT_CLIENTHINT_VULKAN;
    create.pPrivateDriverData = NULL;
    create.PrivateDriverDataSize = 0;
    Note("CreateContextVirtual: node 0, flags 0x%08X, ClientHint %u", create.Flags.Value,
         (unsigned)create.ClientHint);
    if (!NT_SUCCESS(Report("D3DKMTCreateContextVirtual", D3DKMTCreateContextVirtual(&create)))) return FALSE;
    Probe->hContext = create.hContext;
    Note("hContext 0x%08lX", (unsigned long)Probe->hContext);
    return TRUE;
}

static BOOL StepCreateFence(PROBE* Probe)
{
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create;

    ZeroMemory(&create, sizeof(create));
    create.hDevice = Probe->hDevice;
    create.Info.Type = D3DDDI_MONITORED_FENCE;
    create.Info.Flags.Value = 0;
    create.Info.MonitoredFence.InitialFenceValue = 0;
    create.Info.MonitoredFence.EngineAffinity = 0;
    if (!NT_SUCCESS(Report("D3DKMTCreateSynchronizationObject2", D3DKMTCreateSynchronizationObject2(&create))))
        return FALSE;
    Probe->hFence = create.hSyncObject;
    Probe->FenceCpuVa = (volatile UINT64*)create.Info.MonitoredFence.FenceValueCPUVirtualAddress;
    Probe->FenceGpuVa = create.Info.MonitoredFence.FenceValueGPUVirtualAddress;
    Note("hSyncObject 0x%08lX, fence CPU VA %p, fence GPU VA 0x%016llX, initial value %llu",
         (unsigned long)Probe->hFence, (void*)Probe->FenceCpuVa, Probe->FenceGpuVa,
         Probe->FenceCpuVa != NULL ? *Probe->FenceCpuVa : 0ull);
    if (Probe->FenceCpuVa == NULL)
    {
        Note("no CPU mapping of the fence: nothing could read whether it moved");
        return FALSE;
    }
    return TRUE;
}

// With --scratch: the three dwords of the driver's own IB ring test first, then NOP padding. Without it:
// one type-3 NOP covering the whole stream, the way gfx_v10_0_ring_insert_nop writes one. Either way the
// rest of the page is PACKET2 pad, so that a CP which ever ran past CommandLength would still find no-ops
// rather than a type-0 register write to register 0.
static BOOL StepFillCommandBuffer(PROBE* Probe, BUFFER* Buffer)
{
    UINT32* dwords;
    UINT32 total = Probe->Opt.Nops;
    UINT32 page = (UINT32)(Buffer->Size / sizeof(UINT32));
    UINT32 at = 0;
    UINT32 rest;
    UINT32 i;

    if (!LockBuffer(Probe, Buffer)) return FALSE;
    dwords = (UINT32*)Buffer->Locked;

    if (Probe->Opt.HaveScratch)
    {
        dwords[0] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
        dwords[1] = SCRATCH_REG0_UCONFIG;
        dwords[2] = Probe->Opt.Scratch;
        at = PM4_SCRATCH_DWORDS;
    }
    // Whatever is left is one type-3 NOP, or a single PACKET2 when there is no room for a header and a body.
    rest = total - at;
    if (rest >= 2)
    {
        dwords[at] = PACKET3(PACKET3_NOP, rest - 2u);
        for (i = at + 1; i < total; i++) dwords[i] = 0;         // the NOP body, ignored by the CP
    }
    else if (rest == 1)
    {
        dwords[at] = BC250_CP_NOP;
    }
    for (i = total; i < page; i++) dwords[i] = BC250_CP_NOP;    // PACKET2 pad over the rest of the page
    Probe->CommandLength = total * (UINT32)sizeof(UINT32);

    // The dwords that will be fetched, in full: this is the evidence that the CP saw what we think it saw.
    for (i = 0; i < total && i < 8u; i++)
        Note("PM4[%lu] = 0x%08lX%s", (unsigned long)i, (unsigned long)dwords[i],
             (Probe->Opt.HaveScratch && i == 0) ? "  PACKET3(SET_UCONFIG_REG, 1)" :
             (Probe->Opt.HaveScratch && i == 1) ? "  SCRATCH_REG0 - SET_UCONFIG_REG_START" :
             (Probe->Opt.HaveScratch && i == 2) ? "  the --scratch value" :
             (i == at) ? "  PACKET3(NOP, ...)" : "  NOP body");
    if (total > 8u) Note("PM4[%lu..%lu] = 0x00000000 (NOP body)", 8ul, (unsigned long)(total - 1u));
    Note("%lu dword(s) = %lu bytes submitted, page padded with PACKET2 0x%08lX", (unsigned long)total,
         (unsigned long)Probe->CommandLength, (unsigned long)BC250_CP_NOP);
    if (Probe->Opt.HaveScratch)
        Note("SCRATCH_REG0 is BAR5+0x%05lX, dword 0x%04lX, UCONFIG index 0x%04lX",
             BC250_REG_GC_SCRATCH_REG0, (unsigned long)SCRATCH_REG0_DWORD, (unsigned long)SCRATCH_REG0_UCONFIG);
    return UnlockBuffer(Probe, Buffer);
}

static BOOL StepSubmit(PROBE* Probe)
{
    D3DKMT_SUBMITCOMMAND submit;

    ZeroMemory(&submit, sizeof(submit));
    submit.Commands = Probe->Command.MappedVa;      // a GPU virtual address, not a pointer
    submit.CommandLength = Probe->CommandLength;    // bytes
    submit.BroadcastContextCount = 1;               // there is no hContext member; this array is the target
    submit.BroadcastContext[0] = Probe->hContext;
    submit.NumPrimaries = 0;                        // offscreen work, nothing is being scanned out
    submit.NumHistoryBuffers = 0;
    submit.HistoryBufferArray = NULL;
    submit.pPrivateDriverData = NULL;
    submit.PrivateDriverDataSize = 0;
    Note("submitting 0x%lX byte(s) at GPU VA 0x%016llX on context 0x%08lX",
         (unsigned long)submit.CommandLength, submit.Commands, (unsigned long)Probe->hContext);
    return NT_SUCCESS(Report("D3DKMTSubmitCommand", D3DKMTSubmitCommand(&submit)));
}

static BOOL StepSignalFence(PROBE* Probe, UINT64 Value)
{
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU signal;
    D3DKMT_HANDLE objects[1];
    UINT64 values[1];

    objects[0] = Probe->hFence;
    values[0] = Value;
    ZeroMemory(&signal, sizeof(signal));
    signal.hContext = Probe->hContext;
    signal.ObjectCount = 1;
    signal.ObjectHandleArray = objects;
    signal.MonitoredFenceValueArray = values;
    Note("asking the context to signal the monitored fence to %llu", Value);
    return NT_SUCCESS(Report("D3DKMTSignalSynchronizationObjectFromGpu",
                             D3DKMTSignalSynchronizationObjectFromGpu(&signal)));
}

// The wait is given an event, never NULL: with NULL the call does not return until the condition is met, and
// this tool may not contain a wait that can outlive the run. The event wait carries the timeout; the fence's
// CPU mapping is read afterwards as the independent answer to "did it actually move".
static BOOL StepWaitFence(PROBE* Probe, UINT64 Value)
{
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait;
    D3DKMT_HANDLE objects[1];
    UINT64 values[1];
    ULONGLONG started = GetTickCount64();
    DWORD waited;
    UINT64 seen;

    objects[0] = Probe->hFence;
    values[0] = Value;
    ZeroMemory(&wait, sizeof(wait));
    wait.hDevice = Probe->hDevice;
    wait.ObjectCount = 1;
    wait.ObjectHandleArray = objects;
    wait.FenceValueArray = values;
    wait.hAsyncEvent = Probe->WaitEvent;
    wait.Flags.Value = 0;
    if (!NT_SUCCESS(Report("D3DKMTWaitForSynchronizationObjectFromCpu",
                           D3DKMTWaitForSynchronizationObjectFromCpu(&wait))))
        return FALSE;

    waited = WaitForSingleObject(Probe->WaitEvent, Probe->Opt.FenceTimeoutMs);
    Probe->SubmitElapsedMs = (DWORD)(GetTickCount64() - started);
    seen = *Probe->FenceCpuVa;
    if (waited == WAIT_OBJECT_0 && seen >= Value)
    {
        Note("the monitored fence reached %llu after %lu ms", seen, Probe->SubmitElapsedMs);
        return TRUE;
    }
    Note("the monitored fence is %llu, wanted %llu, after %lu ms (%s)", seen, Value,
         Probe->SubmitElapsedMs, waited == WAIT_TIMEOUT ? "event timed out" :
         waited == WAIT_OBJECT_0 ? "event signalled but the value did not move" : "wait failed");
    return FALSE;
}

static void StepStageC(PROBE* Probe)
{
    Probe->Submit = SubmitFailed;       // anything that returns early below has failed
    if (!StepCreateContext(Probe)) return;
    if (!StepCreateFence(Probe)) return;
    if (!StepCreateAllocation(Probe, &Probe->Command)) return;
    if (Probe->Opt.Reserve && !StepReserveVa(Probe, &Probe->Command)) return;
    if (!StepMapVa(Probe, &Probe->Command)) return;
    if (!StepMakeResident(Probe, &Probe->Command)) return;
    if (!StepFillCommandBuffer(Probe, &Probe->Command)) return;
    if (!StepSubmit(Probe)) return;
    if (!StepSignalFence(Probe, 1)) return;
    // From here the calls have all succeeded: a fence that does not move is a timeout, not a failure, and it
    // is the interesting outcome rather than an error in the tool.
    Probe->Submit = StepWaitFence(Probe, 1) ? SubmitFenceReached : SubmitFenceTimeout;
}

static const char* SubmitWord(SUBMIT_RESULT Result)
{
    switch (Result)
    {
    case SubmitFenceReached: return "ok";
    case SubmitFenceTimeout: return "timeout";
    case SubmitFailed:       return "failed";
    default:                 return "skipped";
    }
}

// ---- step 8: the machine-readable line ---------------------------------------------------------------------

static void PrintResult(const PROBE* Probe)
{
    printf("RESULT va=0x%016llX size=0x%llX pattern=0x%016llX words=%llu luid=%08lX:%08lX alloc=0x%08lX",
           Probe->Data.MappedVa, Probe->Data.Size, (UINT64)PATTERN_TAG, Probe->Data.Size / sizeof(UINT64),
           (unsigned long)Probe->Luid.HighPart, (unsigned long)Probe->Luid.LowPart,
           (unsigned long)Probe->Data.hAllocation);
    // The stage B fields above keep their shape and order; stage C only appends.
    if (Probe->Opt.Submit)
        printf(" cmdva=0x%016llX cmdlen=0x%lX fencegpuva=0x%016llX fence=%llu submit=%s ms=%lu",
               Probe->Command.MappedVa, (unsigned long)Probe->CommandLength, Probe->FenceGpuVa,
               Probe->FenceCpuVa != NULL ? *Probe->FenceCpuVa : 0ull, SubmitWord(Probe->Submit),
               Probe->SubmitElapsedMs);
    printf("\n");
    // A line of its own, because nothing in this process can read the register back: the comparison happens
    // in the run script, from the values printed here.
    if (Probe->Opt.HaveScratch)
        printf("SCRATCH asked=0x%08lX reg=0x%05lX uconfig=0x%04lX submit=%s\n",
               (unsigned long)Probe->Opt.Scratch, BC250_REG_GC_SCRATCH_REG0,
               (unsigned long)SCRATCH_REG0_UCONFIG, SubmitWord(Probe->Submit));
    fflush(stdout);
}

// ---- step 9: teardown, in reverse ---------------------------------------------------------------------------
//
// Called on every path, including the failing ones, and it must survive a half-built probe: every handle is
// checked before it is used, and no failure here stops the next step from being tried.

static void TeardownBuffer(PROBE* Probe, BUFFER* Buffer)
{
    if (Buffer->Locked != NULL && Buffer->hAllocation != 0)
    {
        D3DKMT_UNLOCK2 unlock;

        ZeroMemory(&unlock, sizeof(unlock));
        unlock.hDevice = Probe->hDevice;
        unlock.hAllocation = Buffer->hAllocation;
        (void)ReportOn("D3DKMTUnlock2 (teardown)", Buffer, D3DKMTUnlock2(&unlock));
        Buffer->Locked = NULL;
    }
    // A reservation covers the range the map went into, so one free is enough and freeing the mapped range
    // separately would be freeing a part of it twice.
    if (Buffer->ReservedBase != 0 || Buffer->MappedVa != 0)
    {
        D3DKMT_FREEGPUVIRTUALADDRESS free_va;

        ZeroMemory(&free_va, sizeof(free_va));
        free_va.hAdapter = Probe->hAdapter;
        free_va.BaseAddress = Buffer->ReservedBase != 0 ? Buffer->ReservedBase : Buffer->MappedVa;
        free_va.Size = Buffer->ReservedBase != 0 ? Buffer->ReservedSize : Buffer->Size;
        (void)ReportOn("D3DKMTFreeGpuVirtualAddress", Buffer, D3DKMTFreeGpuVirtualAddress(&free_va));
        Buffer->ReservedBase = 0;
        Buffer->MappedVa = 0;
    }
    if (Buffer->hAllocation != 0)
    {
        D3DKMT_DESTROYALLOCATION2 destroy;
        D3DKMT_HANDLE list[1];

        list[0] = Buffer->hAllocation;
        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hDevice = Probe->hDevice;
        destroy.phAllocationList = list;
        destroy.AllocationCount = 1;
        (void)ReportOn("D3DKMTDestroyAllocation2", Buffer, D3DKMTDestroyAllocation2(&destroy));
        Buffer->hAllocation = 0;
    }
}

static void Teardown(PROBE* Probe)
{
    printf("-- teardown\n");
    fflush(stdout);

    // The fence first: it is what a still-outstanding WaitForSynchronizationObjectFromCpu is waiting on, and
    // the context next, because it is the thing that was asked to signal it.
    if (Probe->hFence != 0)
    {
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hSyncObject = Probe->hFence;
        (void)Report("D3DKMTDestroySynchronizationObject", D3DKMTDestroySynchronizationObject(&destroy));
        Probe->hFence = 0;
        Probe->FenceCpuVa = NULL;
    }
    if (Probe->hContext != 0)
    {
        D3DKMT_DESTROYCONTEXT destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hContext = Probe->hContext;
        (void)Report("D3DKMTDestroyContext", D3DKMTDestroyContext(&destroy));
        Probe->hContext = 0;
    }
    TeardownBuffer(Probe, &Probe->Command);
    TeardownBuffer(Probe, &Probe->Data);
    if (Probe->hPagingQueue != 0)
    {
        D3DDDI_DESTROYPAGINGQUEUE destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hPagingQueue = Probe->hPagingQueue;
        (void)Report("D3DKMTDestroyPagingQueue", D3DKMTDestroyPagingQueue(&destroy));
        Probe->hPagingQueue = 0;
        Probe->PagingFence = NULL;
    }
    if (Probe->hDevice != 0)
    {
        D3DKMT_DESTROYDEVICE destroy;

        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hDevice = Probe->hDevice;
        (void)Report("D3DKMTDestroyDevice", D3DKMTDestroyDevice(&destroy));
        Probe->hDevice = 0;
    }
    if (Probe->hAdapter != 0)
    {
        D3DKMT_CLOSEADAPTER close;

        ZeroMemory(&close, sizeof(close));
        close.hAdapter = Probe->hAdapter;
        (void)Report("D3DKMTCloseAdapter", D3DKMTCloseAdapter(&close));
        Probe->hAdapter = 0;
    }
    // Last, and only now: an unsatisfied wait held a reference to this event until the objects above went away.
    if (Probe->WaitEvent != NULL)
    {
        CloseHandle(Probe->WaitEvent);
        Probe->WaitEvent = NULL;
    }
}

// ---- the sequence --------------------------------------------------------------------------------------------
//
// One place that names the steps in order. Stage C (--submit) hangs off the end of stage B and reuses every
// memory step for its command buffer; nothing in stage B changes when it is off.

static BOOL RunProbe(PROBE* Probe)
{
    if (!StepFindAdapter(Probe)) return FALSE;
    if (!StepOpenAdapter(Probe)) return FALSE;
    if (!StepQueryCaps(Probe)) return FALSE;
    if (!StepCreateDevice(Probe)) return FALSE;
    if (!StepCreatePagingQueue(Probe)) return FALSE;
    if (!StepCreateAllocation(Probe, &Probe->Data)) return FALSE;
    if (Probe->Opt.Reserve && !StepReserveVa(Probe, &Probe->Data)) return FALSE;
    if (Probe->Opt.ResidentFirst)
    {
        if (!StepMakeResident(Probe, &Probe->Data)) return FALSE;
        if (!StepMapVa(Probe, &Probe->Data)) return FALSE;
    }
    else
    {
        if (!StepMapVa(Probe, &Probe->Data)) return FALSE;
        if (!StepMakeResident(Probe, &Probe->Data)) return FALSE;
    }
    if (!StepLockAndWrite(Probe, &Probe->Data)) return FALSE;
    if (Probe->Opt.Submit) StepStageC(Probe);
    PrintResult(Probe);

    if (Probe->Opt.HoldSeconds > 0)
    {
        DWORD left = Probe->Opt.HoldSeconds;

        printf("-- holding for %lu s, the witness may read now\n", left);
        fflush(stdout);
        while (left > 0)
        {
            Sleep(1000);
            left--;
        }
    }
    // A fence that never moved is the finding, not a broken tool: the run still passes and the RESULT line
    // carries submit=timeout. Only a stage C call that failed outright makes the exit code non-zero, and the
    // hold above happens either way, because the data buffer is worth witnessing in both cases.
    return Probe->Submit != SubmitFailed;
}

// ---- command line ------------------------------------------------------------------------------------------

static void Usage(void)
{
    printf(
        "kmtprobe - ADR 0008 stage B: one allocation, one GPU VA, one pattern, through raw D3DKMT.\n"
        "With --submit, stage C as well: a context, a monitored fence and one command buffer of PM4 NOPs.\n"
        "Runs on the target against the bc250kmd full WDDM miniport.\n"
        "\n"
        "  --match <text>    substring of the adapter string / chip type / UMD name (default \"bc250\")\n"
        "  --luid H:L        pick the adapter by LUID in hex instead, HighPart:LowPart\n"
        "  --size <bytes>    allocation size, rounded up to 4 KB (default 0x10000; 0x, K, M suffixes ok)\n"
        "  --va <address>    GPU virtual address to ask for (default 0x10000000, 0 = let VidMm choose)\n"
        "  --vamin <a>       minimum GPU VA, only used with --va 0\n"
        "  --vamax <a>       maximum GPU VA, only used with --va 0\n"
        "  --reserve         D3DKMTReserveGpuVirtualAddress the range first (64 KB-aligned base)\n"
        "  --resident-first  D3DKMTMakeResident before the map instead of after it\n"
        "  --no-write        lock, but do not write the pattern\n"
        "  --submit          stage C: context, monitored fence, one command buffer of PM4 NOPs, submit it\n"
        "  --cmdva <address> where the command buffer is mapped (default 0x20000000)\n"
        "  --nops <dwords>   PM4 dwords in that command buffer (default 16, at least 2, rounded up to 8)\n"
        "  --scratch <hex32> with --submit: lead the stream with the driver's own IB ring test packet,\n"
        "                    SET_UCONFIG_REG writing this value to GC.SCRATCH_REG0 (BAR5+0x30100).\n"
        "                    Read it back on the target with: bc250kmd_cli read 30100\n"
        "  --hold <s>        keep everything alive for s seconds after the write (default 0)\n"
        "  --fence-timeout <ms>  paging fence and monitored fence deadline (default 5000)\n"
        "  --timeout <s>     watchdog on the whole process, in SECONDS (default 30, raised to cover --hold)\n"
        "  --help\n"
        "\n"
        "Exit: 0 all steps passed, 1 a step failed, 2 bad arguments, 4 the watchdog fired.\n");
}

// Accepts decimal, 0x hex, and a K / M / G suffix. Returns FALSE on anything it does not understand.
static BOOL ParseU64(const char* Text, UINT64* Value)
{
    UINT64 result = 0;
    int base = 10;
    const char* p = Text;

    if (p == NULL || *p == '\0') return FALSE;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        base = 16;
        p += 2;
        if (*p == '\0') return FALSE;
    }
    for (; *p != '\0'; p++)
    {
        int digit;

        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (base == 16 && *p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (base == 16 && *p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else break;
        result = result * (UINT64)base + (UINT64)digit;
    }
    if (*p == 'K' || *p == 'k') { result *= 1024ull; p++; }
    else if (*p == 'M' || *p == 'm') { result *= 1024ull * 1024ull; p++; }
    else if (*p == 'G' || *p == 'g') { result *= 1024ull * 1024ull * 1024ull; p++; }
    if (*p != '\0') return FALSE;
    *Value = result;
    return TRUE;
}

// A register value: hexadecimal whether or not it carries 0x, the way every register value in this project
// is written, and every character has to be a hex digit.
static BOOL ParseHex32(const char* Text, UINT32* Value)
{
    const char* p = Text;
    UINT64 result = 0;
    int digits = 0;

    if (p == NULL) return FALSE;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    for (; *p != '\0'; p++, digits++)
    {
        int digit;

        if (*p >= '0' && *p <= '9') digit = *p - '0';
        else if (*p >= 'a' && *p <= 'f') digit = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') digit = *p - 'A' + 10;
        else return FALSE;
        result = result * 16ull + (UINT64)digit;
        if (result > 0xFFFFFFFFull) return FALSE;
    }
    if (digits == 0) return FALSE;
    *Value = (UINT32)result;
    return TRUE;
}

// HIGH:LOW, both hexadecimal without 0x, exactly the way the adapter listing above prints them.
static BOOL ParseLuid(const char* Text, LUID* Luid)
{
    const char* colon = strchr(Text, ':');
    char* end = NULL;
    unsigned long long high, low;

    if (colon == NULL || colon == Text || colon[1] == '\0') return FALSE;
    high = strtoull(Text, &end, 16);
    if (end != colon || high > 0xFFFFFFFFull) return FALSE;
    low = strtoull(colon + 1, &end, 16);
    if (end == colon + 1 || *end != '\0' || low > 0xFFFFFFFFull) return FALSE;
    Luid->HighPart = (LONG)(UINT32)high;
    Luid->LowPart = (DWORD)(UINT32)low;
    return TRUE;
}

#define NEED_VALUE(i, argc) do { if ((i) + 1 >= (argc)) { printf("%s needs a value\n", argv[i]); return 2; } } while (0)

int main(int argc, char** argv)
{
    PROBE probe;
    int i;
    BOOL ok;

    ZeroMemory(&probe, sizeof(probe));
    probe.Opt.Match = "bc250";
    probe.Opt.Size = 0x10000;
    probe.Opt.Va = 0x10000000;
    probe.Opt.CommandVa = 0x20000000;
    probe.Opt.Nops = 16;
    probe.Opt.FenceTimeoutMs = 5000;
    probe.Opt.WatchdogMs = 30000;
    probe.Data.Name = "data";
    probe.Command.Name = "cmd";

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { Usage(); return 0; }
        else if (strcmp(argv[i], "--match") == 0) { NEED_VALUE(i, argc); probe.Opt.Match = argv[++i]; }
        else if (strcmp(argv[i], "--luid") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseLuid(argv[++i], &probe.Opt.Luid)) { printf("bad --luid, want HIGH:LOW in hex\n"); return 2; }
            probe.Opt.HaveLuid = TRUE;
        }
        else if (strcmp(argv[i], "--size") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &probe.Opt.Size)) { printf("bad --size\n"); return 2; }
        }
        else if (strcmp(argv[i], "--va") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &probe.Opt.Va)) { printf("bad --va\n"); return 2; }
        }
        else if (strcmp(argv[i], "--vamin") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &probe.Opt.VaMin)) { printf("bad --vamin\n"); return 2; }
        }
        else if (strcmp(argv[i], "--vamax") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &probe.Opt.VaMax)) { printf("bad --vamax\n"); return 2; }
        }
        else if (strcmp(argv[i], "--hold") == 0)
        {
            UINT64 seconds;

            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &seconds) || seconds > 3600) { printf("bad --hold\n"); return 2; }
            probe.Opt.HoldSeconds = (DWORD)seconds;
        }
        else if (strcmp(argv[i], "--fence-timeout") == 0)
        {
            UINT64 ms;

            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &ms) || ms == 0 || ms > 600000) { printf("bad --fence-timeout\n"); return 2; }
            probe.Opt.FenceTimeoutMs = (DWORD)ms;
        }
        else if (strcmp(argv[i], "--timeout") == 0)
        {
            UINT64 seconds;

            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &seconds) || seconds == 0 || seconds > 3600)
            {
                printf("bad --timeout: it is in SECONDS, 1 to 3600 (--fence-timeout is the one in ms)\n");
                return 2;
            }
            probe.Opt.WatchdogMs = (DWORD)(seconds * 1000);
        }
        else if (strcmp(argv[i], "--cmdva") == 0)
        {
            NEED_VALUE(i, argc);
            if (!ParseU64(argv[++i], &probe.Opt.CommandVa)) { printf("bad --cmdva\n"); return 2; }
        }
        else if (strcmp(argv[i], "--nops") == 0)
        {
            UINT64 dwords;

            NEED_VALUE(i, argc);
            // A type-3 NOP is a header plus at least one body dword, and the command buffer is one page.
            if (!ParseU64(argv[++i], &dwords) || dwords < 2 || dwords > GPU_PAGE_SIZE / 4)
            {
                printf("bad --nops, want 2 to %llu dwords\n", GPU_PAGE_SIZE / 4);
                return 2;
            }
            probe.Opt.Nops = (UINT32)dwords;
        }
        else if (strcmp(argv[i], "--scratch") == 0)
        {
            UINT32 value;

            NEED_VALUE(i, argc);
            if (!ParseHex32(argv[++i], &value)) { printf("bad --scratch, want a 32-bit hex value\n"); return 2; }
            probe.Opt.Scratch = value;
            probe.Opt.HaveScratch = TRUE;
        }
        else if (strcmp(argv[i], "--submit") == 0) probe.Opt.Submit = TRUE;
        else if (strcmp(argv[i], "--reserve") == 0) probe.Opt.Reserve = TRUE;
        else if (strcmp(argv[i], "--resident-first") == 0) probe.Opt.ResidentFirst = TRUE;
        else if (strcmp(argv[i], "--no-write") == 0) probe.Opt.NoWrite = TRUE;
        else { printf("unknown argument %s (try --help)\n", argv[i]); return 2; }
    }

    if (probe.Opt.Size == 0) { printf("--size 0 makes no allocation\n"); return 2; }
    probe.Data.Size = (probe.Opt.Size + GPU_PAGE_SIZE - 1) & ~(GPU_PAGE_SIZE - 1);
    probe.Data.RequestedVa = probe.Opt.Va;
    if (probe.Data.Size != probe.Opt.Size)
        printf("size 0x%llX rounded up to 0x%llX\n", probe.Opt.Size, probe.Data.Size);
    if (probe.Data.Size > 0x40000000ull) { printf("--size above 1 GB refused for a probe\n"); return 2; }
    // The command buffer is one page, whatever --nops says; only the submitted length changes.
    probe.Command.Size = GPU_PAGE_SIZE;
    probe.Command.RequestedVa = probe.Opt.CommandVa;
    probe.Opt.Nops = (probe.Opt.Nops + PM4_DWORD_ALIGN - 1u) & ~(PM4_DWORD_ALIGN - 1u);
    if (probe.Opt.Nops > GPU_PAGE_SIZE / 4) probe.Opt.Nops = (UINT32)(GPU_PAGE_SIZE / 4);
    if (probe.Opt.HaveScratch && !probe.Opt.Submit)
    {
        printf("--scratch needs --submit: there is nothing to execute the packet otherwise\n");
        return 2;
    }
    if (probe.Opt.HaveScratch && probe.Opt.Nops < PM4_SCRATCH_DWORDS)
    {
        printf("--scratch needs at least %lu dwords, --nops gives %lu\n", (unsigned long)PM4_SCRATCH_DWORDS,
               (unsigned long)probe.Opt.Nops);
        return 2;
    }
    {
        UINT64 alignment = probe.Opt.Reserve ? GPU_RESERVE_ALIGN : GPU_PAGE_SIZE;

        if (probe.Opt.Va != 0 && (probe.Opt.Va & (alignment - 1)) != 0)
        {
            printf("--va 0x%llX is not aligned to 0x%llX\n", probe.Opt.Va, alignment);
            return 2;
        }
        if (probe.Opt.Submit && probe.Opt.CommandVa != 0 && (probe.Opt.CommandVa & (alignment - 1)) != 0)
        {
            printf("--cmdva 0x%llX is not aligned to 0x%llX\n", probe.Opt.CommandVa, alignment);
            return 2;
        }
        // Two ranges in one per-process VA space: they may not overlap, and with --reserve each is rounded up
        // to 64 KB before it is reserved.
        if (probe.Opt.Submit && probe.Opt.Va != 0 && probe.Opt.CommandVa != 0)
        {
            UINT64 data_span = (probe.Data.Size + alignment - 1) & ~(alignment - 1);
            UINT64 cmd_span = (probe.Command.Size + alignment - 1) & ~(alignment - 1);

            if (probe.Opt.Va < probe.Opt.CommandVa + cmd_span && probe.Opt.CommandVa < probe.Opt.Va + data_span)
            {
                printf("--va 0x%llX and --cmdva 0x%llX overlap\n", probe.Opt.Va, probe.Opt.CommandVa);
                return 2;
            }
        }
    }
    if (probe.Opt.Submit)
    {
        // Auto-reset, unsignalled: the wait below is the only thing that ever sets it.
        probe.WaitEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (probe.WaitEvent == NULL) { printf("could not create the wait event\n"); return 2; }
    }
    // The watchdog has to outlive the hold and the fence waits, or a long --hold would always be killed.
    {
        DWORD needed = probe.Opt.HoldSeconds * 1000 + probe.Opt.FenceTimeoutMs * 3 + 15000;

        if (probe.Opt.WatchdogMs < needed)
        {
            probe.Opt.WatchdogMs = needed;
            printf("watchdog raised to %lu ms to cover --hold and the fence waits\n", probe.Opt.WatchdogMs);
        }
    }
    if (!StartWatchdog(probe.Opt.WatchdogMs)) { printf("could not start the watchdog\n"); return 2; }
    printf("kmtprobe: size 0x%llX, va 0x%016llX, reserve %s, resident-first %s, hold %lu s, watchdog %lu ms\n",
           probe.Data.Size, probe.Opt.Va, probe.Opt.Reserve ? "yes" : "no",
           probe.Opt.ResidentFirst ? "yes" : "no", probe.Opt.HoldSeconds, probe.Opt.WatchdogMs);
    if (probe.Opt.Submit)
        printf("kmtprobe: --submit, command buffer 0x%llX bytes at 0x%016llX, %lu PM4 dword(s)\n",
               probe.Command.Size, probe.Opt.CommandVa, (unsigned long)probe.Opt.Nops);
    if (probe.Opt.HaveScratch)
        printf("kmtprobe: --scratch 0x%08lX into GC.SCRATCH_REG0 (BAR5+0x%05lX); read it back with"
               " \"bc250kmd_cli read %05lX\"\n", (unsigned long)probe.Opt.Scratch, BC250_REG_GC_SCRATCH_REG0,
               BC250_REG_GC_SCRATCH_REG0);
    fflush(stdout);

    ok = RunProbe(&probe);
    Teardown(&probe);
    SetEvent(g_Done);
    printf("kmtprobe: %s\n", ok ? "all steps passed" : "FAILED");
    fflush(stdout);
    return ok ? 0 : 1;
}
