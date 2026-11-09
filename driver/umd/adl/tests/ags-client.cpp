// A small AMD AGS 6.3 client for the open ADL subset: host tests and the lab kit use the same program.
//
//   ags-client.exe info [--no-gpu-info] [--out <file>]
//       agsInitialize with counting allocation callbacks, the full AGSGPUInfo, agsDeInitialize, and the callback
//       allocations left after it (must be 0). Exit 0 when agsInitialize returns AGS_SUCCESS (0).
//   ags-client.exe expect <code> [--out <file>]
//       Exit 0 when agsInitialize returns <code> (negative controls: 6 without an ADL library, 5 without one of
//       the nine mandatory exports).
//   ags-client.exe loop <n> [--out <file>]
//       n rounds of agsInitialize/agsDeInitialize, with and without GPU information in turn. Fails on a nonzero
//       result, on a callback allocation left after a round, and on a leak: live process heap bytes (HeapWalk) or
//       handles that grow in a sustained way after the warm-up rounds (see LeakVerdict; BD-328). One-time growth
//       (a lazy cache of the loader or of a system library, a thread pool worker that another thread's timing
//       keeps alive) is not a leak and does not fail.
//       Self-test mutants of the leak check (never in a release run): --selftest-leak <bytes> [--selftest-period <k>]
//       leaks <bytes> of the process heap every k-th round (default 1); --selftest-steps <m> --selftest-step-bytes
//       <bytes> makes m one-time allocations at spread rounds; --selftest-handle-leak leaks one event handle a round.
//   ags-client.exe <mode> --inject ...
//       Before AGS loads, load the ADL library of this directory and give it the test snapshot of
//       test-snapshot.h through Bc250AdlTest_SetSnapshot (test builds only): two AMD GPUs, three displays.
//       This drives the AGS parsers over a nonempty adapter list on a host without an AMD GPU.
//   ags-client.exe dx12 [--any-adapter] [--out <file>]
//       agsDriverExtensionsDX12_CreateDevice on the DXGI adapter of the first AGS device (with --any-adapter: the
//       first hardware adapter, for a host without an AMD GPU), one clear of a 64x64 render target, a copy to a
//       readback buffer, a check of every pixel, then agsDriverExtensionsDX12_DestroyDevice and agsDeInitialize.
//
// amd_ags_x64.dll (amd_ags_x86.dll in a 32-bit build) is loaded from the directory of this program; AGS then loads
// atiadlxx.dll (or atiadlxy.dll) by name.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amd_ags.h"
#include "test-snapshot.h"
#include "ags-kmt-fixture.h"

namespace {

FILE* g_out;

void Say(const char* fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    fflush(stdout);
    if (g_out) {
        fputs(line, g_out);
        fflush(g_out);
    }
}

LONG g_live;
LONG g_total;
bool g_guard_allocations;
bool g_expect_injected;

constexpr bool GuardLayout(size_t bytes, size_t* aligned, size_t* pages)
{
    const size_t maximum = (size_t)-1;
    if (bytes == 0) bytes = 1;
    if (bytes > maximum - 15) return false;
    *aligned = (bytes + 15) & ~(size_t)15;
    if (*aligned > maximum - 4095) return false;
    *pages = (*aligned + 4095) & ~(size_t)4095;
    return *pages <= maximum - 4096;
}

constexpr bool CheckGuardLayout(size_t bytes, bool valid, size_t aligned = 0, size_t pages = 0)
{
    size_t actual_aligned = 0, actual_pages = 0;
    bool actual = GuardLayout(bytes, &actual_aligned, &actual_pages);
    return actual == valid && (!actual || (actual_aligned == aligned && actual_pages == pages));
}
// Compile in both architectures: rounding and the final guard-page addition must not wrap size_t.
static_assert(CheckGuardLayout(0, true, 16, 4096));
static_assert(CheckGuardLayout(1, true, 16, 4096));
static_assert(CheckGuardLayout(15, true, 16, 4096));
static_assert(CheckGuardLayout(16, true, 16, 4096));
static_assert(CheckGuardLayout(17, true, 32, 4096));
static_assert(CheckGuardLayout(4096, true, 4096, 4096));
static_assert(CheckGuardLayout(4097, true, 4112, 8192));
static_assert(CheckGuardLayout((size_t)-1 - 8191, true, (size_t)-1 - 8191, (size_t)-1 - 8191));
static_assert(CheckGuardLayout((size_t)-1 - 8190, false));
static_assert(CheckGuardLayout((size_t)-1 - 4095, false));
static_assert(CheckGuardLayout((size_t)-1 - 15, false));
static_assert(CheckGuardLayout((size_t)-1 - 14, false));
static_assert(CheckGuardLayout((size_t)-1, false));

void* __stdcall AgsAlloc(size_t bytes)
{
    void* p = nullptr;
    if (g_guard_allocations) {
        // Keep the ABI's 16-byte alignment, followed by at most 15 bytes of padding
        // and a no-access page. VirtualQuery lets free find the allocation base.
        size_t aligned = 0, pages = 0;
        if (!GuardLayout(bytes, &aligned, &pages)) return nullptr;
        BYTE* base = (BYTE*)VirtualAlloc(nullptr, pages + 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD old;
        if (base && VirtualProtect(base + pages, 4096, PAGE_NOACCESS, &old)) p = base + pages - aligned;
        else if (base) VirtualFree(base, 0, MEM_RELEASE);
    } else p = malloc(bytes ? bytes : 1);
    if (p) {
        InterlockedIncrement(&g_live);
        InterlockedIncrement(&g_total);
    }
    return p;
}

void __stdcall AgsFree(void* p)
{
    if (p) {
        if (g_guard_allocations) {
            MEMORY_BASIC_INFORMATION memory = {};
            if (!VirtualQuery(p, &memory, sizeof(memory)) || !VirtualFree(memory.AllocationBase, 0, MEM_RELEASE)) abort();
        } else free(p);
        InterlockedDecrement(&g_live);
    }
}

typedef AGSReturnCode (*PFN_Init)(int, const AGSConfiguration*, AGSContext**, AGSGPUInfo*);
typedef AGSReturnCode (*PFN_DeInit)(AGSContext*);
typedef int (*PFN_Version)();
typedef AGSDriverVersionResult (*PFN_Check)(const char*, unsigned int);
typedef AGSReturnCode (*PFN_Dx12Create)(AGSContext*, const AGSDX12DeviceCreationParams*, const AGSDX12ExtensionParams*,
                                        AGSDX12ReturnedParams*);
typedef AGSReturnCode (*PFN_Dx12Destroy)(AGSContext*, ID3D12Device*, unsigned int*);

struct Ags {
    HMODULE dll;
    PFN_Init init;
    PFN_DeInit deinit;
    PFN_Version version;
    PFN_Check check;
    PFN_Dx12Create create;
    PFN_Dx12Destroy destroy;
};

#if defined(_WIN64)
const char kAgsName[] = "amd_ags_x64.dll";
#else
const char kAgsName[] = "amd_ags_x86.dll";
#endif

bool LoadAgs(Ags* a)
{
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (n == 0 || !slash || (size_t)(slash - path) + sizeof(kAgsName) + 1 > sizeof(path)) {
        return false;
    }
    strcpy_s(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), kAgsName);
    a->dll = LoadLibraryA(path);
    if (!a->dll) {
        Say("FAIL cannot load %s (error %lu)\n", path, GetLastError());
        return false;
    }
    a->init = (PFN_Init)(void*)GetProcAddress(a->dll, "agsInitialize");
    a->deinit = (PFN_DeInit)(void*)GetProcAddress(a->dll, "agsDeInitialize");
    a->version = (PFN_Version)(void*)GetProcAddress(a->dll, "agsGetVersionNumber");
    a->check = (PFN_Check)(void*)GetProcAddress(a->dll, "agsCheckDriverVersion");
    a->create = (PFN_Dx12Create)(void*)GetProcAddress(a->dll, "agsDriverExtensionsDX12_CreateDevice");
    a->destroy = (PFN_Dx12Destroy)(void*)GetProcAddress(a->dll, "agsDriverExtensionsDX12_DestroyDevice");
    if (!a->init || !a->deinit || !a->version || !a->check || !a->create || !a->destroy) {
        Say("FAIL %s lacks an AGS 6.3 export\n", path);
        return false;
    }
    int v = a->version();
    Say("ags: %s version %d.%d.%d\n", path, (v >> 22) & 0x3ff, (v >> 12) & 0x3ff, v & 0xfff);
    return true;
}

// The ADL library that AGS loaded, if any, with its path.
void SayAdlModule(const char* when)
{
    const char* names[] = {"atiadlxx.dll", "atiadlxy.dll"};
    bool any = false;
    for (const char* name : names) {
        HMODULE m = GetModuleHandleA(name);
        if (m) {
            char path[MAX_PATH] = {};
            GetModuleFileNameA(m, path, MAX_PATH);
            Say("adl module %s: %s\n", when, path);
            any = true;
        }
    }
    if (!any) {
        Say("adl module %s: none loaded\n", when);
    }
}

const char* AsicName(AGSAsicFamily f)
{
    switch (f) {
    case AGSAsicFamily_Unknown: return "Unknown";
    case AGSAsicFamily_PreGCN: return "PreGCN";
    case AGSAsicFamily_GCN1: return "GCN1";
    case AGSAsicFamily_GCN2: return "GCN2";
    case AGSAsicFamily_GCN3: return "GCN3";
    case AGSAsicFamily_GCN4: return "GCN4";
    case AGSAsicFamily_Vega: return "Vega";
    case AGSAsicFamily_RDNA: return "RDNA";
    case AGSAsicFamily_RDNA2: return "RDNA2";
    case AGSAsicFamily_RDNA3: return "RDNA3";
    case AGSAsicFamily_RDNA4: return "RDNA4";
    default: return "?";
    }
}

void SayGpuInfo(const Ags& a, const AGSGPUInfo& info)
{
    Say("gpuinfo: driverVersion \"%s\" radeonSoftwareVersion \"%s\" numDevices %d\n",
        info.driverVersion ? info.driverVersion : "(null)",
        info.radeonSoftwareVersion ? info.radeonSoftwareVersion : "(null)", info.numDevices);
    if (info.radeonSoftwareVersion) {
        Say("gpuinfo: agsCheckDriverVersion(radeonSoftwareVersion, 23.1.1) = %d (0 ok, 1 older, 2 undefined)\n",
            (int)a.check(info.radeonSoftwareVersion, AGS_MAKE_VERSION(23, 1, 1)));
    }
    for (int i = 0; i < info.numDevices; i++) {
        const AGSDeviceInfo& d = info.devices[i];
        Say("device %d: \"%s\" asicFamily %s isAPU %u isPrimaryDevice %u isExternal %u\n", i,
            d.adapterString ? d.adapterString : "(null)", AsicName(d.asicFamily), d.isAPU, d.isPrimaryDevice,
            d.isExternal);
        Say("device %d: vendorId 0x%04X deviceId 0x%04X revisionId 0x%02X adlAdapterIndex %d\n", i, d.vendorId,
            d.deviceId, d.revisionId, d.adlAdapterIndex);
        Say("device %d: numCUs %d numWGPs %d numROPs %d coreClock %d memoryClock %d memoryBandwidth %d teraFlops %.3f\n",
            i, d.numCUs, d.numWGPs, d.numROPs, d.coreClock, d.memoryClock, d.memoryBandwidth, d.teraFlops);
        Say("device %d: localMemoryInBytes %llu sharedMemoryInBytes %llu\n", i, d.localMemoryInBytes,
            d.sharedMemoryInBytes);
        Say("device %d: eyefinityEnabled %d grid %dx%d resolution %dx%d bezel %d numDisplays %d\n", i,
            d.eyefinityEnabled, d.eyefinityGridWidth, d.eyefinityGridHeight, d.eyefinityResolutionX,
            d.eyefinityResolutionY, d.eyefinityBezelCompensated, d.numDisplays);
        for (int k = 0; k < d.numDisplays; k++) {
            const AGSDisplayInfo& s = d.displays[k];
            Say("device %d display %d: \"%s\" on %s primary %u HDR10 %u dolbyVision %u freesync %u freesyncHDR %u "
                "eyefinityInGroup %u\n",
                i, k, s.name, s.displayDeviceName, s.isPrimaryDisplay, s.HDR10, s.dolbyVision, s.freesync,
                s.freesyncHDR, s.eyefinityInGroup);
            Say("device %d display %d: current %dx%d at %d,%d visible %dx%d at %d,%d %.3f Hz max %dx%d %.3f Hz "
                "adlAdapterIndex %d\n",
                i, k, s.currentResolution.width, s.currentResolution.height, s.currentResolution.offsetX,
                s.currentResolution.offsetY, s.visibleResolution.width, s.visibleResolution.height,
                s.visibleResolution.offsetX, s.visibleResolution.offsetY, s.currentRefreshRate, s.maxResolutionX,
                s.maxResolutionY, s.maxRefreshRate, s.adlAdapterIndex);
        }
    }
}

AGSConfiguration Config()
{
    AGSConfiguration c = {};
    c.allocCallback = AgsAlloc;
    c.freeCallback = AgsFree;
    return c;
}

int RunInfo(const Ags& a, bool gpu_info)
{
    AGSConfiguration config = Config();
    AGSContext* ctx = nullptr;
    AGSGPUInfo info = {};
    AGSReturnCode rc = a.init(AGS_CURRENT_VERSION, &config, &ctx, gpu_info ? &info : nullptr);
    Say("agsInitialize(gpu info %s) = %d\n", gpu_info ? "yes" : "no", (int)rc);
    SayAdlModule("after agsInitialize");
    if (rc != AGS_SUCCESS) {
        return 1;
    }
    if (gpu_info) {
        SayGpuInfo(a, info);
    }
    bool contents = !gpu_info || !g_expect_injected ||
        (info.numDevices == 2 && info.devices && info.devices[0].vendorId == 0x1002 &&
         info.devices[0].deviceId == 0x13fe && info.devices[0].numDisplays == 3 &&
         info.devices[0].localMemoryInBytes == (8ull << 30) && info.devices[1].vendorId == 0x1002);
    if (!contents) Say("FAIL injected AMD GPU information differs from the fixture\n");
    Say("callback allocations live before agsDeInitialize: %ld\n", g_live);
    AGSReturnCode d = a.deinit(ctx);
    Say("agsDeInitialize = %d, callback allocations live after: %ld (of %ld)\n", (int)d, g_live, g_total);
    SayAdlModule("after agsDeInitialize");
    return (contents && d == AGS_SUCCESS && g_live == 0) ? 0 : 1;
}

int RunExpect(const Ags& a, int expected)
{
    AGSConfiguration config = Config();
    AGSContext* ctx = nullptr;
    AGSGPUInfo info = {};
    AGSReturnCode rc = a.init(AGS_CURRENT_VERSION, &config, &ctx, &info);
    Say("agsInitialize = %d, expected %d, context %s, callback allocations live %ld\n", (int)rc, expected,
        ctx ? "set" : "null", g_live);
    if (rc == AGS_SUCCESS && ctx) {
        a.deinit(ctx);
    }
    return ((int)rc == expected && g_live == 0) ? 0 : 1;
}

struct Usage {
    SIZE_T private_bytes;
    SIZE_T heap_busy_bytes;  // busy blocks of every process heap: the allocations that are live now
    DWORD handles;
};

Usage Measure()
{
    Usage u = {};
    PROCESS_MEMORY_COUNTERS_EX pm = {};
    pm.cb = sizeof(pm);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm));
    u.private_bytes = pm.PrivateUsage;
    GetProcessHandleCount(GetCurrentProcess(), &u.handles);
    HANDLE heaps[64];
    DWORD n = GetProcessHeaps(64, heaps);
    for (DWORD h = 0; h < n && h < 64; h++) {
        if (!HeapLock(heaps[h])) {
            continue;
        }
        PROCESS_HEAP_ENTRY e = {};
        while (HeapWalk(heaps[h], &e)) {
            if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
                u.heap_busy_bytes += e.cbData;
            }
        }
        HeapUnlock(heaps[h]);
    }
    return u;
}

// Self-test mutants of the leak check (BD-328): the gate runs them and expects the verdicts below.
struct LoopSelfTest {
    size_t leak_bytes = 0;  // leaked every `period`-th round: must FAIL
    int period = 1;
    int steps = 0;          // one-time allocations at spread rounds: must PASS
    size_t step_bytes = 0;
    bool handle_leak = false;  // one event handle a round: must FAIL
};
LoopSelfTest g_selftest;

// The leak verdict over the samples after the warm-up (one sample after every round).
//
// A leak is growth that goes on: a block lost every round (or every few rounds) raises the floor of the live bytes in
// every part of the run. One-time growth raises it once. The samples are cut into kSegments equal parts; the floor of
// a part is its minimum, so a transient allocation of another thread at the moment of a sample does not count. The
// verdict is a leak when the floors rise strictly from each part to the next and every part has at least
// kGrowthsPerPart rounds that grew (a steady leak grows again and again in each part), or when the whole growth passes
// a hard bound (a large leak needs no trend). Detection limit: any steady leak of at least one byte whose period is at
// most a part divided by kGrowthsPerPart (9 rounds for 100 rounds) shows in every part. A false leak needs two
// growths in each of the kSegments parts and a raised floor in each of the later ones; a loaded host gave at most
// three one-time growths in 100 rounds (BD-328: 2026-11-06, and 20 runs under 2 x CPU load on 2026-11-09). The
// floors alone were not enough: three one-time growths of the self-test and one of the host raised all four floors.
// The old rule (the last sample minus the sample after round 1, above 4 KiB) failed on one growth of 4141 bytes.
constexpr int kSegments = 5;
constexpr int kGrowthsPerPart = 2;

struct Trend {
    long long floor[kSegments];
    int growths[kSegments];  // rounds of the part whose sample is above the sample before
    long long total;         // last sample - first sample
    bool rising;             // floors strictly rise through every part, and every part grew kGrowthsPerPart times
};

Trend Floors(const long long* y, int n)
{
    Trend t = {};
    t.total = y[n - 1] - y[0];
    t.rising = true;
    for (int k = 0; k < kSegments; k++) {
        int b = n * k / kSegments, e = n * (k + 1) / kSegments;
        long long m = y[b];
        for (int i = b + 1; i < e; i++) m = y[i] < m ? y[i] : m;
        t.floor[k] = m;
        for (int i = b > 0 ? b : 1; i < e; i++) t.growths[k] += y[i] > y[i - 1];
        if (k > 0 && !(t.floor[k] > t.floor[k - 1])) t.rising = false;
        if (t.growths[k] < kGrowthsPerPart) t.rising = false;
    }
    return t;
}

int RunLoop(const Ags& a, int rounds)
{
    AGSConfiguration config = Config();
    int failures = 0;
    // Warm-up: the first tenth of the rounds (at least one) fills the caches of the CRT, the loader and the system
    // libraries; the samples start after it.
    int warm = rounds / 10 > 0 ? rounds / 10 : 1;
    int n = rounds - warm;
    if (n < 2 * kSegments) {
        Say("loop: %d rounds leave %d samples after %d warm-up rounds; at least %d are needed\n", rounds, n, warm,
            2 * kSegments);
        return 1;
    }
    // The samples live outside the process heaps, so that storing them changes nothing that is measured.
    long long* heap = (long long*)VirtualAlloc(nullptr, sizeof(long long) * 3 * (size_t)n, MEM_RESERVE | MEM_COMMIT,
                                               PAGE_READWRITE);
    if (!heap) {
        Say("loop: no memory for %d samples\n", n);
        return 1;
    }
    long long* priv = heap + n;
    long long* handles = priv + n;
    int step = rounds >= 100 ? rounds / 10 : rounds;
    for (int i = 0; i < rounds; i++) {
        AGSContext* ctx = nullptr;
        AGSGPUInfo info = {};
        bool gpu_info = (i % 2) == 0;
        AGSReturnCode rc = a.init(AGS_CURRENT_VERSION, &config, &ctx, gpu_info ? &info : nullptr);
        AGSReturnCode d = rc == AGS_SUCCESS ? a.deinit(ctx) : AGS_FAILURE;
        if (rc != AGS_SUCCESS || d != AGS_SUCCESS || g_live != 0) {
            Say("round %d: agsInitialize %d agsDeInitialize %d live %ld\n", i, (int)rc, (int)d, g_live);
            failures++;
        }
        if (g_selftest.leak_bytes && (i % g_selftest.period) == 0) {
            HeapAlloc(GetProcessHeap(), 0, g_selftest.leak_bytes);  // lost on purpose
        }
        for (int j = 0; j < g_selftest.steps; j++) {
            if (i - warm == (j + 1) * n / (g_selftest.steps + 1)) {
                HeapAlloc(GetProcessHeap(), 0, g_selftest.step_bytes);  // one-time growth, kept on purpose
            }
        }
        if (g_selftest.handle_leak) {
            CreateEventW(nullptr, TRUE, FALSE, nullptr);  // lost on purpose
        }
        if (i >= warm) {
            Usage u = Measure();
            int s = i - warm;
            heap[s] = (long long)u.heap_busy_bytes;
            priv[s] = (long long)u.private_bytes;
            handles[s] = (long long)u.handles;
            if ((i + 1) % step == 0) {
                Say("loop: after round %d heap busy %lld (%+lld) private %lld (%+lld) handles %lld (%+lld)\n", i + 1,
                    heap[s], heap[s] - heap[0], priv[s], priv[s] - priv[0], handles[s], handles[s] - handles[0]);
            }
        }
    }
    int grew = 0;
    for (int s = 1; s < n; s++) grew += heap[s] > heap[s - 1];
    Trend th = Floors(heap, n), tc = Floors(handles, n), tp = Floors(priv, n);
    Say("loop: %d rounds (%d warm-up), %d failed, callback allocations %ld total %ld live\n", rounds, warm, failures,
        g_total, g_live);
    Say("loop: heap busy after round %d %lld, after round %d %lld (%+lld), grew in %d of %d rounds; floors of %d parts"
        " %lld %lld %lld %lld %lld, growths %d %d %d %d %d (%s)\n", warm + 1, heap[0], rounds, heap[n - 1], th.total,
        grew, n - 1, kSegments, th.floor[0], th.floor[1], th.floor[2], th.floor[3], th.floor[4], th.growths[0],
        th.growths[1], th.growths[2], th.growths[3], th.growths[4], th.rising ? "rising" : "not rising");
    Say("loop: handles %lld -> %lld, floors %lld %lld %lld %lld %lld (%s); private bytes %lld -> %lld (%s, not"
        " gated: the allocator keeps freed spans)\n", handles[0], handles[n - 1], tc.floor[0], tc.floor[1],
        tc.floor[2], tc.floor[3], tc.floor[4], tc.rising ? "rising" : "not rising", priv[0], priv[n - 1],
        tp.rising ? "rising" : "not rising");
    SayAdlModule("after the loop");
    // Hard bounds: 64 KiB of live heap or 8 handles over the run fail even without a trend.
    bool heap_leak = th.rising || th.total > 65536;
    bool handle_leak = tc.rising || tc.total > 8;
    if (heap_leak || handle_leak) {
        Say("FAIL resources grew over the rounds:%s%s\n", heap_leak ? " heap" : "", handle_leak ? " handles" : "");
        failures++;
    } else {
        Say("loop: no sustained growth (one-time growth %+lld heap bytes, %+lld handles)\n", th.total, tc.total);
    }
    VirtualFree(heap, 0, MEM_RELEASE);
    return failures ? 1 : 0;
}

template <typename T>
void Release(T*& p)
{
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// One clear of a 64x64 RGBA8 render target, copied to a readback buffer; every pixel must be (64, 128, 191, 255).
bool ClearAndReadBack(ID3D12Device* device)
{
    const UINT W = 64, H = 64;
    bool ok = false;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    ID3D12Resource* target = nullptr;
    ID3D12Resource* readback = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES rb = {};
    rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    const float color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    D3D12_CLEAR_VALUE cv = {};
    cv.Format = td.Format;
    memcpy(cv.Color, color, sizeof(color));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                                                            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&target));
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                            nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        Say("dx12: object creation failed 0x%08lX\n", (unsigned long)hr);
    } else {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(target, nullptr, rtv);
        list->ClearRenderTargetView(rtv, color, 0, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        hr = list->Close();
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* lists[] = {list};
            queue->ExecuteCommandLists(1, lists);
            hr = queue->Signal(fence, 1);
        }
        event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (SUCCEEDED(hr) && event && SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
            DWORD w = WaitForSingleObject(event, 10000);
            Say("dx12: fence wait %s (completed value %llu)\n", w == WAIT_OBJECT_0 ? "done" : "TIMEOUT",
                (unsigned long long)fence->GetCompletedValue());
            if (w == WAIT_OBJECT_0) {
                void* data = nullptr;
                D3D12_RANGE range = {0, (SIZE_T)total};
                if (SUCCEEDED(readback->Map(0, &range, &data))) {
                    int bad = 0;
                    const unsigned char* base = (const unsigned char*)data + fp.Offset;
                    for (UINT y = 0; y < H; y++) {
                        const unsigned char* row = base + (size_t)y * fp.Footprint.RowPitch;
                        for (UINT x = 0; x < W; x++) {
                            const unsigned char* p = row + 4 * x;
                            bool good = abs(p[0] - 64) <= 1 && abs(p[1] - 128) <= 1 && abs(p[2] - 191) <= 1 &&
                                        p[3] == 255;
                            if (!good && bad++ == 0) {
                                Say("dx12: first bad pixel (%u,%u) = %u %u %u %u\n", x, y, p[0], p[1], p[2], p[3]);
                            }
                        }
                    }
                    const unsigned char* p0 = base;
                    Say("dx12: readback pixel (0,0) = %u %u %u %u, %d of %u pixels wrong\n", p0[0], p0[1], p0[2],
                        p0[3], bad, W * H);
                    D3D12_RANGE none = {0, 0};
                    readback->Unmap(0, &none);
                    ok = bad == 0;
                }
            }
        } else {
            Say("dx12: submission failed 0x%08lX, device removed reason 0x%08lX\n", (unsigned long)hr,
                (unsigned long)device->GetDeviceRemovedReason());
        }
    }
    if (event) {
        CloseHandle(event);
    }
    Release(fence);
    Release(readback);
    Release(target);
    Release(heap);
    Release(list);
    Release(alloc);
    Release(queue);
    return ok;
}

// The names of the loaded modules that matter for the D3D12 route.
void SayDriverModules()
{
    HMODULE mods[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) {
        return;
    }
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); i++) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        char lower[MAX_PATH];
        strcpy_s(lower, base);
        _strlwr_s(lower);
        if (strstr(lower, "d3d12") || strstr(lower, "dxgi") || strstr(lower, "amd") || strstr(lower, "bc250") ||
            strstr(lower, "atiadl") || strstr(lower, "nvwgf") || strstr(lower, "umd") || strstr(lower, "vkd3d") ||
            strstr(lower, "radv")) {
            Say("module: %s\n", path);
        }
    }
}

int RunDx12(const Ags& a, bool any_adapter)
{
    AGSConfiguration config = Config();
    AGSContext* ctx = nullptr;
    AGSGPUInfo info = {};
    AGSReturnCode rc = a.init(AGS_CURRENT_VERSION, &config, &ctx, &info);
    Say("agsInitialize = %d\n", (int)rc);
    if (rc != AGS_SUCCESS) {
        return 1;
    }
    SayGpuInfo(a, info);
    int want_vendor = -1, want_device = -1;
    if (info.numDevices > 0 && !any_adapter) {
        want_vendor = info.devices[0].vendorId;
        want_device = info.devices[0].deviceId;
    } else if (info.numDevices == 0 && !any_adapter) {
        Say("FAIL AGS reports no device and --any-adapter was not given\n");
        a.deinit(ctx);
        return 1;
    }
    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Say("FAIL CreateDXGIFactory1\n");
        a.deinit(ctx);
        return 1;
    }
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d = {};
        adapter->GetDesc1(&d);
        bool hardware = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
        if (hardware && (want_vendor < 0 || ((int)d.VendorId == want_vendor && (int)d.DeviceId == want_device))) {
            Say("dxgi adapter %u: \"%ls\" %04X:%04X rev %02X luid %08lX:%08lX dedicated video %llu\n", i,
                d.Description, d.VendorId, d.DeviceId, d.Revision, (unsigned long)d.AdapterLuid.HighPart,
                (unsigned long)d.AdapterLuid.LowPart, (unsigned long long)d.DedicatedVideoMemory);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    Release(factory);
    if (!adapter) {
        Say("FAIL no DXGI adapter matches the AGS device\n");
        a.deinit(ctx);
        return 1;
    }
    AGSDX12DeviceCreationParams cp = {};
    cp.pAdapter = adapter;
    cp.iid = __uuidof(ID3D12Device);
    cp.FeatureLevel = D3D_FEATURE_LEVEL_11_0;
    AGSDX12ExtensionParams ep = {};
    ep.pAppName = L"bc250 ags-client";
    ep.pEngineName = L"none";
    AGSDX12ReturnedParams rp = {};
    rc = a.create(ctx, &cp, &ep, &rp);
    unsigned int ext = 0;
    memcpy(&ext, &rp.extensionsSupported, sizeof(ext));
    Say("agsDriverExtensionsDX12_CreateDevice = %d, device %s, extensionsSupported 0x%08X\n", (int)rc,
        rp.pDevice ? "set" : "null", ext);
    SayDriverModules();
    bool ok = false;
    if (rc == AGS_SUCCESS && rp.pDevice) {
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {};
        fl.NumFeatureLevels = 5;
        fl.pFeatureLevelsRequested = levels;
        if (SUCCEEDED(rp.pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl)))) {
            Say("dx12: max feature level 0x%X\n", (unsigned)fl.MaxSupportedFeatureLevel);
        }
        LUID luid = rp.pDevice->GetAdapterLuid();
        Say("dx12: device luid %08lX:%08lX\n", (unsigned long)luid.HighPart, (unsigned long)luid.LowPart);
        ok = ClearAndReadBack(rp.pDevice);
        unsigned int refs = 12345;
        AGSReturnCode dd = a.destroy(ctx, rp.pDevice, &refs);
        Say("agsDriverExtensionsDX12_DestroyDevice = %d, remaining references %u\n", (int)dd, refs);
        ok = ok && dd == AGS_SUCCESS && refs == 0 && ext == 0;
    }
    Release(adapter);
    AGSReturnCode d = a.deinit(ctx);
    Say("agsDeInitialize = %d, callback allocations live after: %ld\n", (int)d, g_live);
    return (ok && d == AGS_SUCCESS && g_live == 0) ? 0 : 1;
}

// Loads the ADL library under the first name that AGS 6.3 finds, and injects the test snapshot.
bool InjectSnapshot()
{
    const char* names[] = {"atiadlxx.dll", "atiadlxy.dll"};
    for (const char* name : names) {
        HMODULE adl = LoadLibraryA(name);
        if (!adl) {
            continue;
        }
        typedef void(__cdecl * PFN_SetSnapshot)(const AdlSnapshot*);
        auto set = (PFN_SetSnapshot)(void*)GetProcAddress(adl, "Bc250AdlTest_SetSnapshot");
        if (!set) {
            Say("inject: %s has no Bc250AdlTest_SetSnapshot (not a test build)\n", name);
            return false;
        }
        static AdlSnapshot snapshot;
        test_snapshot::MakeSnapshot(&snapshot);
        set(&snapshot);
        Say("inject: test snapshot (%d GPUs, %d entries, %d displays) given to %s\n", snapshot.gpu_count,
            snapshot.entry_count, snapshot.display_count, name);
        return true;
    }
    Say("inject: no ADL library found\n");
    return false;
}

}  // namespace

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "info";
    bool gpu_info = true, any_adapter = false, inject = false, kmt_fixture = false, kmt_legacy = false;
    int number = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--no-gpu-info") == 0) {
            gpu_info = false;
        } else if (strcmp(argv[i], "--inject") == 0) {
            inject = true;
        } else if (strcmp(argv[i], "--kmt-fixture") == 0 || strcmp(argv[i], "--kmt-legacy") == 0) {
            kmt_fixture = true;
            kmt_legacy = strcmp(argv[i], "--kmt-legacy") == 0;
            inject = true;
        } else if (strcmp(argv[i], "--any-adapter") == 0) {
            any_adapter = true;
        } else if (strcmp(argv[i], "--guard-allocations") == 0) {
            g_guard_allocations = true;
        } else if (strcmp(argv[i], "--selftest-leak") == 0 && i + 1 < argc) {
            g_selftest.leak_bytes = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--selftest-period") == 0 && i + 1 < argc) {
            g_selftest.period = atoi(argv[++i]) > 0 ? atoi(argv[i]) : 1;
        } else if (strcmp(argv[i], "--selftest-steps") == 0 && i + 1 < argc) {
            g_selftest.steps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--selftest-step-bytes") == 0 && i + 1 < argc) {
            g_selftest.step_bytes = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--selftest-handle-leak") == 0) {
            g_selftest.handle_leak = true;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            fopen_s(&g_out, argv[++i], "w");
        } else {
            number = atoi(argv[i]);
        }
    }
    SYSTEMTIME t;
    GetSystemTime(&t);
    Say("ags-client %s, %s build, %04u-%02u-%02uT%02u:%02u:%02uZ\n", mode, sizeof(void*) == 8 ? "x64" : "x86",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    Ags a = {};
    g_expect_injected = inject;
    int rc = 1;
    if ((!inject || InjectSnapshot()) && LoadAgs(&a)) {
        if (kmt_fixture && !ags_kmt_fixture::Install(a.dll, kmt_legacy)) {
            Say("FAIL could not install the test-only KMT boundary\n");
            return 1;
        }
        if (strcmp(mode, "info") == 0) {
            rc = RunInfo(a, gpu_info);
        } else if (strcmp(mode, "expect") == 0) {
            rc = RunExpect(a, number);
        } else if (strcmp(mode, "loop") == 0) {
            rc = RunLoop(a, number > 0 ? number : 100);
        } else if (strcmp(mode, "dx12") == 0) {
            rc = RunDx12(a, any_adapter);
        } else {
            Say("usage: ags-client info [--no-gpu-info] | expect <code> | loop <n> | dx12 [--any-adapter]"
                " [--out <file>]\n");
            rc = 2;
        }
    }
    if (kmt_fixture) {
        Say("KMT fixture: private=%u refused=%u segments=%u identity=%u\n", ags_kmt_fixture::private_queries,
            ags_kmt_fixture::foreign_refusals, ags_kmt_fixture::segment_queries, ags_kmt_fixture::identity_answers);
        // AGS 6.3 asks 608 bytes first. With the KMD's identity answer it stops there and takes nothing from the
        // block; without it (the legacy policy) it asks 17248 bytes next, which the KMD refuses. Either way the
        // device list above is the one ADL gave (docs/design/fsr4-identity.md).
        if (gpu_info && (!ags_kmt_fixture::private_queries ||
                         (!ags_kmt_fixture::foreign_refusals && !ags_kmt_fixture::identity_answers))) rc = 1;
    }
    Say("%s ags-client %s\n", rc == 0 ? "PASS" : "FAIL", mode);
    if (g_out) {
        fclose(g_out);
    }
    return rc;
}
