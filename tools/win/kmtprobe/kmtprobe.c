// kmtprobe - ADR 0008 stage B driven from user mode, through raw D3DKMT calls.
//
// It creates one allocation on our full WDDM miniport, gives it a GPU virtual address we choose, makes it
// resident, locks it and writes a pattern a witness (bc250rd, walking the page tables) can find again in
// physical memory. Nothing here is a context and nothing is submitted: that is stage C (research document
// section 5.3), and the steps below are one function each so that a later --submit can be bolted on without
// rewriting the ones before it.
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
    DWORD HoldSeconds;
    DWORD FenceTimeoutMs;
    DWORD WatchdogMs;
} OPTIONS;

// ---- probe state --------------------------------------------------------------------------------------

typedef struct _PROBE {
    OPTIONS Opt;
    LUID Luid;
    D3DKMT_HANDLE hAdapter;
    D3DKMT_HANDLE hDevice;
    D3DKMT_HANDLE hPagingQueue;
    D3DKMT_HANDLE hPagingFenceObject;
    volatile UINT64* PagingFence;       // CPU-readable value of the paging queue's monitored fence
    D3DKMT_HANDLE hAllocation;
    UINT64 Size;                        // page-rounded
    UINT64 ReservedBase;                // 0 when nothing was reserved
    UINT64 ReservedSize;
    UINT64 MappedVa;                    // 0 when nothing is mapped
    void* Locked;                       // NULL when not locked
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

static BOOL StepCreateAllocation(PROBE* Probe)
{
    BC250_WDDM_ALLOCATION_PRIVATE private;
    D3DDDI_ALLOCATIONINFO2 info;
    D3DKMT_CREATEALLOCATION create;

    // The KMD checks Magic and Size and keeps the rest for DescribeAllocation. Presented as one linear row so
    // that Pitch * Height == Size holds, which is the shape stage A's GetStandardAllocationDriverData builds.
    ZeroMemory(&private, sizeof(private));
    private.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    private.Version = BC250_ALLOCATION_PRIVATE_VERSION;
    private.Width = (ULONG)(Probe->Size / 4);
    private.Height = 1;
    private.Pitch = (ULONG)Probe->Size;
    private.Format = (ULONG)D3DDDIFMT_A8R8G8B8;
    private.Size = Probe->Size;

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
    if (!NT_SUCCESS(Report("D3DKMTCreateAllocation2", D3DKMTCreateAllocation2(&create)))) return FALSE;
    Probe->hAllocation = info.hAllocation;
    Note("hAllocation 0x%08lX, GpuVirtualAddress reported 0x%016llX",
         (unsigned long)Probe->hAllocation, info.GpuVirtualAddress);
    return TRUE;
}

// ---- step 5: GPU virtual address -------------------------------------------------------------------------

static BOOL StepReserveVa(PROBE* Probe)
{
    D3DDDI_RESERVEGPUVIRTUALADDRESS reserve;
    UINT64 size = (Probe->Size + GPU_RESERVE_ALIGN - 1) & ~(GPU_RESERVE_ALIGN - 1);

    // The struct changed meaning between revisions (research section 3.2): zero it, set only hAdapter,
    // BaseAddress, Minimum, Maximum and Size, and never wait on the fence slot of this call.
    ZeroMemory(&reserve, sizeof(reserve));
    reserve.hAdapter = Probe->hAdapter;
    reserve.BaseAddress = Probe->Opt.Va;
    if (Probe->Opt.Va == 0)
    {
        reserve.MinimumAddress = Probe->Opt.VaMin;
        reserve.MaximumAddress = Probe->Opt.VaMax;
    }
    reserve.Size = size;
    reserve.ReservationType = D3DDDIGPUVIRTUALADDRESS_RESERVE_NO_ACCESS;
    Note("reserving 0x%llX bytes at 0x%016llX", size, Probe->Opt.Va);
    if (!NT_SUCCESS(Report("D3DKMTReserveGpuVirtualAddress", D3DKMTReserveGpuVirtualAddress(&reserve))))
        return FALSE;
    Probe->ReservedBase = reserve.VirtualAddress;
    Probe->ReservedSize = size;
    Note("reserved at 0x%016llX (asked 0x%016llX)", Probe->ReservedBase, Probe->Opt.Va);
    return TRUE;
}

static BOOL StepMapVa(PROBE* Probe)
{
    D3DDDI_MAPGPUVIRTUALADDRESS map;
    UINT64 base = Probe->ReservedBase != 0 ? Probe->ReservedBase : Probe->Opt.Va;

    ZeroMemory(&map, sizeof(map));
    map.hPagingQueue = Probe->hPagingQueue;
    map.hAllocation = Probe->hAllocation;
    map.BaseAddress = base;
    if (base == 0)
    {
        // Only consulted when no exact base is named; with a base, Minimum and Maximum are ignored.
        map.MinimumAddress = Probe->Opt.VaMin;
        map.MaximumAddress = Probe->Opt.VaMax;
    }
    map.OffsetInPages = 0;
    map.SizeInPages = Probe->Size / GPU_PAGE_SIZE;
    // Read plus write: read is the absence of NoAccess, there is no separate Read bit.
    map.Protection.Value = 0;
    map.Protection.Write = 1;
    map.DriverProtection = 0;           // stage C sends our PTE bits here; stage B has no page-table code yet

    Note("mapping %llu page(s) of 0x%llX bytes at base 0x%016llX", map.SizeInPages, Probe->Size, base);
    if (!NT_SUCCESS(Report("D3DKMTMapGpuVirtualAddress", D3DKMTMapGpuVirtualAddress(&map)))) return FALSE;
    Probe->MappedVa = map.VirtualAddress;
    Note("VirtualAddress 0x%016llX (asked 0x%016llX, %s), PagingFenceValue %llu", map.VirtualAddress, base,
         (base == 0 || map.VirtualAddress == base) ? "honoured" : "DIFFERENT", map.PagingFenceValue);
    return WaitPagingFence(Probe, map.PagingFenceValue, "MapGpuVirtualAddress");
}

// ---- step 6: residency -----------------------------------------------------------------------------------

static BOOL StepMakeResident(PROBE* Probe)
{
    D3DDDI_MAKERESIDENT resident;
    D3DKMT_HANDLE list[1];
    UINT priorities[1];
    NTSTATUS status;

    list[0] = Probe->hAllocation;
    priorities[0] = D3DDDI_ALLOCATIONPRIORITY_NORMAL;

    ZeroMemory(&resident, sizeof(resident));
    resident.hPagingQueue = Probe->hPagingQueue;
    resident.NumAllocations = 1;
    resident.AllocationList = list;
    resident.PriorityList = priorities;
    status = Report("D3DKMTMakeResident", D3DKMTMakeResident(&resident));
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

static BOOL StepLockAndWrite(PROBE* Probe)
{
    D3DKMT_LOCK2 lock;
    D3DKMT_UNLOCK2 unlock;
    UINT64 words = Probe->Size / sizeof(UINT64);
    UINT64 i;
    volatile UINT64* data;

    ZeroMemory(&lock, sizeof(lock));
    lock.hDevice = Probe->hDevice;
    lock.hAllocation = Probe->hAllocation;
    lock.Flags.Value = 0;
    if (!NT_SUCCESS(Report("D3DKMTLock2", D3DKMTLock2(&lock)))) return FALSE;
    Probe->Locked = lock.pData;
    Note("locked at CPU VA %p", lock.pData);
    if (lock.pData == NULL) return FALSE;

    if (Probe->Opt.NoWrite)
    {
        Note("--no-write: contents left alone");
    }
    else
    {
        data = (volatile UINT64*)lock.pData;
        for (i = 0; i < words; i++) data[i] = PATTERN_TAG | (i & 0xFFFFull);
        Note("wrote %llu qword(s), first 0x%016llX last 0x%016llX", words, data[0], data[words - 1]);
    }

    ZeroMemory(&unlock, sizeof(unlock));
    unlock.hDevice = Probe->hDevice;
    unlock.hAllocation = Probe->hAllocation;
    if (!NT_SUCCESS(Report("D3DKMTUnlock2", D3DKMTUnlock2(&unlock)))) return FALSE;
    Probe->Locked = NULL;
    return TRUE;
}

// ---- step 8: the machine-readable line ---------------------------------------------------------------------

static void PrintResult(const PROBE* Probe)
{
    printf("RESULT va=0x%016llX size=0x%llX pattern=0x%016llX words=%llu luid=%08lX:%08lX alloc=0x%08lX\n",
           Probe->MappedVa, Probe->Size, (UINT64)PATTERN_TAG, Probe->Size / sizeof(UINT64),
           (unsigned long)Probe->Luid.HighPart, (unsigned long)Probe->Luid.LowPart,
           (unsigned long)Probe->hAllocation);
    fflush(stdout);
}

// ---- step 9: teardown, in reverse ---------------------------------------------------------------------------
//
// Called on every path, including the failing ones, and it must survive a half-built probe: every handle is
// checked before it is used, and no failure here stops the next step from being tried.

static void Teardown(PROBE* Probe)
{
    printf("-- teardown\n");
    fflush(stdout);

    if (Probe->Locked != NULL && Probe->hAllocation != 0)
    {
        D3DKMT_UNLOCK2 unlock;

        ZeroMemory(&unlock, sizeof(unlock));
        unlock.hDevice = Probe->hDevice;
        unlock.hAllocation = Probe->hAllocation;
        (void)Report("D3DKMTUnlock2 (teardown)", D3DKMTUnlock2(&unlock));
        Probe->Locked = NULL;
    }
    // A reservation covers the range the map went into, so one free is enough and freeing the mapped range
    // separately would be freeing a part of it twice.
    if (Probe->ReservedBase != 0 || Probe->MappedVa != 0)
    {
        D3DKMT_FREEGPUVIRTUALADDRESS free_va;

        ZeroMemory(&free_va, sizeof(free_va));
        free_va.hAdapter = Probe->hAdapter;
        free_va.BaseAddress = Probe->ReservedBase != 0 ? Probe->ReservedBase : Probe->MappedVa;
        free_va.Size = Probe->ReservedBase != 0 ? Probe->ReservedSize : Probe->Size;
        (void)Report("D3DKMTFreeGpuVirtualAddress", D3DKMTFreeGpuVirtualAddress(&free_va));
        Probe->ReservedBase = 0;
        Probe->MappedVa = 0;
    }
    if (Probe->hAllocation != 0)
    {
        D3DKMT_DESTROYALLOCATION2 destroy;
        D3DKMT_HANDLE list[1];

        list[0] = Probe->hAllocation;
        ZeroMemory(&destroy, sizeof(destroy));
        destroy.hDevice = Probe->hDevice;
        destroy.phAllocationList = list;
        destroy.AllocationCount = 1;
        (void)Report("D3DKMTDestroyAllocation2", D3DKMTDestroyAllocation2(&destroy));
        Probe->hAllocation = 0;
    }
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
}

// ---- the sequence --------------------------------------------------------------------------------------------
//
// One place that names the steps in order. Stage C adds CreateContextVirtual, CreateSynchronizationObject2 and
// SubmitCommand between the lock and the hold, behind a --submit flag; nothing above has to change for that.

static BOOL RunProbe(PROBE* Probe)
{
    if (!StepFindAdapter(Probe)) return FALSE;
    if (!StepOpenAdapter(Probe)) return FALSE;
    if (!StepQueryCaps(Probe)) return FALSE;
    if (!StepCreateDevice(Probe)) return FALSE;
    if (!StepCreatePagingQueue(Probe)) return FALSE;
    if (!StepCreateAllocation(Probe)) return FALSE;
    if (Probe->Opt.Reserve && !StepReserveVa(Probe)) return FALSE;
    if (Probe->Opt.ResidentFirst)
    {
        if (!StepMakeResident(Probe)) return FALSE;
        if (!StepMapVa(Probe)) return FALSE;
    }
    else
    {
        if (!StepMapVa(Probe)) return FALSE;
        if (!StepMakeResident(Probe)) return FALSE;
    }
    if (!StepLockAndWrite(Probe)) return FALSE;
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
    return TRUE;
}

// ---- command line ------------------------------------------------------------------------------------------

static void Usage(void)
{
    printf(
        "kmtprobe - ADR 0008 stage B: one allocation, one GPU VA, one pattern, through raw D3DKMT.\n"
        "Runs on the target against the bc250kmd full WDDM miniport. Nothing is submitted.\n"
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
        "  --hold <s>        keep everything alive for s seconds after the write (default 0)\n"
        "  --fence-timeout <ms>  per-wait paging fence deadline (default 5000)\n"
        "  --timeout <s>     watchdog on the whole process (default 30, raised to cover --hold)\n"
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
    probe.Opt.FenceTimeoutMs = 5000;
    probe.Opt.WatchdogMs = 30000;

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
            if (!ParseU64(argv[++i], &seconds) || seconds == 0 || seconds > 3600) { printf("bad --timeout\n"); return 2; }
            probe.Opt.WatchdogMs = (DWORD)(seconds * 1000);
        }
        else if (strcmp(argv[i], "--reserve") == 0) probe.Opt.Reserve = TRUE;
        else if (strcmp(argv[i], "--resident-first") == 0) probe.Opt.ResidentFirst = TRUE;
        else if (strcmp(argv[i], "--no-write") == 0) probe.Opt.NoWrite = TRUE;
        else { printf("unknown argument %s (try --help)\n", argv[i]); return 2; }
    }

    if (probe.Opt.Size == 0) { printf("--size 0 makes no allocation\n"); return 2; }
    probe.Size = (probe.Opt.Size + GPU_PAGE_SIZE - 1) & ~(GPU_PAGE_SIZE - 1);
    if (probe.Size != probe.Opt.Size) printf("size 0x%llX rounded up to 0x%llX\n", probe.Opt.Size, probe.Size);
    if (probe.Size > 0x40000000ull) { printf("--size above 1 GB refused for a probe\n"); return 2; }
    if (probe.Opt.Va != 0)
    {
        UINT64 alignment = probe.Opt.Reserve ? GPU_RESERVE_ALIGN : GPU_PAGE_SIZE;

        if ((probe.Opt.Va & (alignment - 1)) != 0)
        {
            printf("--va 0x%llX is not aligned to 0x%llX\n", probe.Opt.Va, alignment);
            return 2;
        }
    }
    // The watchdog has to outlive the hold, or a --hold longer than the timeout would always be killed.
    if (probe.Opt.WatchdogMs < probe.Opt.HoldSeconds * 1000 + 15000)
    {
        probe.Opt.WatchdogMs = probe.Opt.HoldSeconds * 1000 + 15000;
        printf("watchdog raised to %lu ms to cover --hold\n", probe.Opt.WatchdogMs);
    }
    if (!StartWatchdog(probe.Opt.WatchdogMs)) { printf("could not start the watchdog\n"); return 2; }
    printf("kmtprobe: size 0x%llX, va 0x%016llX, reserve %s, resident-first %s, hold %lu s, watchdog %lu ms\n",
           probe.Size, probe.Opt.Va, probe.Opt.Reserve ? "yes" : "no",
           probe.Opt.ResidentFirst ? "yes" : "no", probe.Opt.HoldSeconds, probe.Opt.WatchdogMs);
    fflush(stdout);

    ok = RunProbe(&probe);
    Teardown(&probe);
    SetEvent(g_Done);
    printf("kmtprobe: %s\n", ok ? "all steps passed" : "FAILED");
    fflush(stdout);
    return ok ? 0 : 1;
}
