// SPDX-License-Identifier: MIT
//
// bc250hang - M15.12 deliberate-hang client for the BC-250 Windows driver (tools/win/hangclient).
//
// Purpose: give the lab a controlled way to prove the KMD's stage-1 hang recovery
// (HangRecoveryMode=1, docs/design/hang-recovery.md). It runs a GPU compute dispatch whose
// wall-clock length is set by a root constant:
//
//   --short : a loop calibrated to finish in ~300 ms, under the KMD's 500 ms submit
//             watchdog (driver/kmd/gfx.c BC250_SUBMIT_POLL_US). This is the POSITIVE
//             CONTROL: it proves the client really drives the GPU and the device stays
//             alive, so a removal under --long is the hang and not a broken client.
//   --long  : an effectively unbounded loop (far past TdrDelay=10 s). The waves never
//             retire, the end-of-pipe never fires, so dxgkrnl runs its per-engine TDR.
//             With recovery on, the KMD kills the waves, drains the ring and removes
//             only this process's device; the client prints the device-removed reason.
//             Exactly one deliberate hang: one device, one ExecuteCommandLists, no
//             calibration and no retry, and a wait bounded by --deadline (12-120 s).
//
// It takes the SYSTEM D3D12 runtime (System32\d3d12.dll, never an app-local translator)
// and the BC-250 adapter (PCI 1002:13FE), exactly like tools/win/d3d12queue. No window,
// no resident state. The compute shader is DXBC cs_5_0 compiled at runtime by
// d3dcompiler_47.dll - the same SM5.0/DXBC path the scene client uses (fxc vs_5_0/ps_5_0).
//
// WARP (--warp) is a build/smoke mode only: it forces a tiny completing dispatch so it can
// never peg a CPU-only device. A real hang needs --lab on the BC-250. --warp --long runs the
// --long wait on such a tiny dispatch: it must end RESULT=FENCE-COMPLETED-NO-REMOVAL after
// the removal grace, which exercises that path's polling, grace and TerminateProcess exit.
//
// Why a spin loop and not an unmapped-VA read (the MEMVIOL fault class of the real reports):
// a read of unmapped memory cannot be formed through the validated system D3D12 runtime
// without a malformed descriptor the runtime rejects, so the brief's "optional MEMVIOL
// variant, only if cheap/safe" is deliberately not built. The spin loop reproduces the
// recoverable class the stage-1 kill is for (live waves -> SQ_CMD KILL -> EOP fires ->
// fence retires -> ring drains); the MEMVIOL class is a documented limit of the design note and
// a Linux wishlist measurement (row L38), not something this client can safely provoke.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

// Fixed inner loop count per outer unit; the outer count is the calibrated/overridden knob.
static const uint32_t kInner = 4096u;
static const uint32_t kGroups = 16u;          // thread groups of 64 lanes each: the GPU is clearly busy
static const uint32_t kThreadsPerGroup = 64u;
static const uint32_t kShortTargetDefaultMs = 300u;   // under the 500 ms submit watchdog
static const uint32_t kShortTargetMaxMs = 450u;       // never let the "control" trip the watchdog
static const uint32_t kLongDeadlineDefaultMs = 25000u;// > TdrDelay (10 s) + recovery margin
static const uint32_t kLongDeadlineMinMs = 12000u;    // never give up before TdrDelay + a margin
static const uint32_t kLongDeadlineMaxMs = 120000u;   // the client's own bound, well inside the 3-minute lab bound
static const uint32_t kRemovalGraceMs = 10000u;       // after a --long fence completion: time for dxgkrnl to remove

static const char kShader[] =
    "RWByteAddressBuffer Out : register(u0);\n"
    "cbuffer Params : register(b0) { uint gOuter; uint gInner; uint gSeed; uint gPad; };\n"
    "[numthreads(64,1,1)]\n"
    "void main(uint3 tid : SV_DispatchThreadID) {\n"
    "    uint acc = tid.x ^ gSeed;\n"
    "    [loop] for (uint o = 0u; o < gOuter; o++) {\n"
    "        [loop] for (uint i = 0u; i < gInner; i++) {\n"
    "            acc = acc * 1664525u + 1013904223u;\n"   // LCG: data-dependent, not reducible
    "            acc ^= (acc >> 13);\n"
    "        }\n"
    "    }\n"
    "    Out.Store(tid.x * 4u, acc);\n"                   // observable effect: defeats dead-code removal
    "}\n";

struct Params { uint32_t outer; uint32_t inner; uint32_t seed; uint32_t pad; };

static bool ck(const char* what, HRESULT hr)
{
    if (FAILED(hr)) { printf("hang-client: %s FAILED hr=0x%08lx\n", what, (unsigned long)hr); fflush(stdout); return false; }
    return true;
}

static const char* dxgi_reason(HRESULT hr)
{
    switch ((unsigned)hr) {
    case 0x00000000u: return "S_OK (device alive)";
    case 0x887A0005u: return "DXGI_ERROR_DEVICE_REMOVED";
    case 0x887A0006u: return "DXGI_ERROR_DEVICE_HUNG";
    case 0x887A0007u: return "DXGI_ERROR_DEVICE_RESET";
    case 0x887A0020u: return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case 0x887A0001u: return "DXGI_ERROR_INVALID_CALL";
    default: return "(other)";
    }
}

// One D3D12 session: device, a DIRECT queue, a compute PSO over the spin shader, a UAV
// buffer, a fence. Compute runs on a DIRECT list (allowed) to keep the client minimal.
struct Session {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12RootSignature> rootsig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    UINT64 fenceValue = 0;

    bool create(IDXGIAdapter1* adapter, decltype(&D3D12CreateDevice) create_device);
    // Records and submits one dispatch with the given outer count and a queue Signal behind
    // it. Returns false only on a command-recording/submit error (not on a hang).
    bool submit(uint32_t outer, UINT64* target);
    // Runs one dispatch with the given outer count, waits up to timeoutMs. Returns the
    // waited result in *completed (fence value) and the elapsed ms in *elapsedMs. Returns
    // false only on a command-recording/submit error (not on a hang: a hang is a result).
    bool dispatch(uint32_t outer, uint32_t timeoutMs, UINT64* completed, double* elapsedMs);
};

static double ms_since(const LARGE_INTEGER& t0)
{
    LARGE_INTEGER f, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t1);
    return 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
}

// UTC wall clock with milliseconds, to line the client's events up with the KMD log and the
// LastTime of the Parameters\HangRecovery record.
static void utc_now(char* out, size_t n)
{
    SYSTEMTIME st; GetSystemTime(&st);
    snprintf(out, n, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds);
}

bool Session::create(IDXGIAdapter1* adapter, decltype(&D3D12CreateDevice) create_device)
{
    if (!ck("D3D12CreateDevice FL11_0", create_device(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!ck("CreateCommandQueue", device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;
    if (!ck("CreateCommandAllocator", device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return false;
    if (!ck("CreateCommandList", device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) return false;
    if (!ck("close list", list->Close())) return false;

    // Root signature: 4 root constants at b0, one root UAV at u0.
    D3D12_ROOT_PARAMETER rp[2]{};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.ShaderRegister = 0; rp[0].Constants.RegisterSpace = 0; rp[0].Constants.Num32BitValues = 4;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[1].Descriptor.ShaderRegister = 0; rp[1].Descriptor.RegisterSpace = 0;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 2; rsd.pParameters = rp;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    if (!ck("D3D12SerializeRootSignature", D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr))) return false;
    if (!ck("CreateRootSignature", device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootsig)))) return false;

    // Compile the spin shader at runtime: DXBC cs_5_0 via d3dcompiler_47.dll.
    ComPtr<ID3DBlob> cs, csErr;
    HRESULT hr = D3DCompile(kShader, sizeof(kShader) - 1, "hang.hlsl", nullptr, nullptr, "main", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &csErr);
    if (FAILED(hr)) {
        printf("hang-client: D3DCompile cs_5_0 FAILED hr=0x%08lx %s\n", (unsigned long)hr,
               csErr ? (const char*)csErr->GetBufferPointer() : "");
        fflush(stdout); return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = rootsig.Get();
    pd.CS.pShaderBytecode = cs->GetBufferPointer(); pd.CS.BytecodeLength = cs->GetBufferSize();
    if (!ck("CreateComputePipelineState", device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)))) return false;

    // DEFAULT-heap UAV buffer, one uint per lane across all groups.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)kGroups * kThreadsPerGroup * sizeof(uint32_t);
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!ck("CreateCommittedResource UAV", device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&buffer)))) return false;

    if (!ck("CreateFence", device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) { printf("hang-client: CreateEvent FAILED\n"); return false; }
    return true;
}

bool Session::submit(uint32_t outer, UINT64* target)
{
    *target = 0;
    if (!ck("reset allocator", alloc->Reset())) return false;
    if (!ck("reset list", list->Reset(alloc.Get(), pso.Get()))) return false;
    Params p{ outer, kInner, 0x9E3779B9u, 0u };
    list->SetComputeRootSignature(rootsig.Get());
    list->SetPipelineState(pso.Get());
    list->SetComputeRoot32BitConstants(0, 4, &p, 0);
    list->SetComputeRootUnorderedAccessView(1, buffer->GetGPUVirtualAddress());
    list->Dispatch(kGroups, 1, 1);
    if (!ck("close list", list->Close())) return false;

    ID3D12CommandList* lists[] = { list.Get() };
    queue->ExecuteCommandLists(1, lists);
    *target = ++fenceValue;
    return ck("Signal", queue->Signal(fence.Get(), *target));
}

bool Session::dispatch(uint32_t outer, uint32_t timeoutMs, UINT64* completed, double* elapsedMs)
{
    *completed = 0; *elapsedMs = 0.0;
    UINT64 target = 0;
    if (!submit(outer, &target)) return false;

    LARGE_INTEGER freq, t0, t1; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    const UINT64 start = GetTickCount64();
    for (;;) {
        ResetEvent(event);
        // Arm the event for the target; on device removal D3D12 signals it and sets the value to UINT64_MAX.
        if (FAILED(fence->SetEventOnCompletion(target, event))) { /* removed mid-arm */ }
        WaitForSingleObject(event, 250);
        UINT64 v = fence->GetCompletedValue();
        HRESULT removed = device->GetDeviceRemovedReason();
        if (removed != S_OK) { *completed = UINT64_MAX; QueryPerformanceCounter(&t1);
            *elapsedMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart; return true; }
        if (v >= target && v != UINT64_MAX) { *completed = v; QueryPerformanceCounter(&t1);
            *elapsedMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart; return true; }
        if (GetTickCount64() - start >= timeoutMs) { *completed = v; QueryPerformanceCounter(&t1);
            *elapsedMs = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart; return true; }
    }
}

static int usage()
{
    puts("bc250hang (--lab|--warp) (--short|--long) [--millis N] [--deadline S]");
    puts("  --lab      the BC-250 adapter (PCI 1002:13FE) through System32 d3d12.dll");
    puts("  --warp     WARP, build/smoke only: forces a tiny completing dispatch, never hangs (with");
    puts("             --long: the --long wait on it, expected RESULT=FENCE-COMPLETED-NO-REMOVAL)");
    puts("  --short    positive control: a ~300 ms dispatch that completes; the device stays alive");
    puts("  --long     ONE unbounded dispatch far past TdrDelay, no calibration, no retry; prints the");
    puts("             device-removed reason");
    puts("  --millis N --short target in ms (<=450, under the 500 ms KMD submit watchdog)");
    puts("  --deadline S  --long max wait in seconds (default 25, clamped to 12..120; TdrDelay=10)");
    return 2;
}

// --long: the one deliberate hang. One ExecuteCommandLists of an unbounded dispatch on the one
// device this process created, with no calibration before it and no retry after it; the wait is
// bounded by deadlineMs. With recovery on, dxgkrnl's per-engine TDR ends in ResetEngine, the KMD
// kills the waves and the runtime marks this device removed. Should the fence complete first (the
// kill drained the ring and the queue Signal behind the dispatch ran before the removal became
// visible), the removal is still the expected end: wait up to kRemovalGraceMs more for it.
// Without a removal the process leaves by TerminateProcess, so that no runtime or UMD teardown
// waits on a job that may still be spinning: the client's own bound holds either way. outer is
// 0xFFFFFFFF on the lab; only --warp passes a tiny count (the flow check in the header).
static int run_long(Session& s, uint32_t deadlineMs, uint32_t outer)
{
    char utc[40];
    UINT64 target = 0, v = 0;
    double completedAt = -1.0, removedAt = -1.0, now = 0.0;
    HRESULT reason = S_OK;
    LARGE_INTEGER t0;

    printf("hang-client: long dispatch outer=0x%08X inner=%u groups=%u deadline=%u ms (TdrDelay~10 s), "
           "one submission\n", outer, kInner, kGroups, deadlineMs);
    fflush(stdout);
    QueryPerformanceCounter(&t0);
    if (!s.submit(outer, &target)) {
        printf("RESULT=ERROR\n"); fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 1);   // the dispatch may be on the GPU already
        return 1;
    }
    utc_now(utc, sizeof(utc));
    printf("hang-client: submitted utc=%s fence target=%llu\n", utc, (unsigned long long)target);
    fflush(stdout);
    for (;;) {
        if (completedAt < 0.0) {
            ResetEvent(s.event);
            (void)s.fence->SetEventOnCompletion(target, s.event);
            WaitForSingleObject(s.event, 250);
        } else {
            Sleep(250);     // a reached fence signals the event at once: plain 250 ms steps, no spin
        }
        v = s.fence->GetCompletedValue();
        reason = s.device->GetDeviceRemovedReason();
        now = ms_since(t0);
        if (reason != S_OK) { removedAt = now; break; }
        if (completedAt < 0.0 && v >= target) {
            completedAt = now;
            utc_now(utc, sizeof(utc));
            printf("hang-client: fence reached 0x%llx at %.1f ms utc=%s, device still alive; waiting up to %u ms "
                   "for the removal\n", (unsigned long long)v, now, utc, kRemovalGraceMs);
            fflush(stdout);
        }
        if (completedAt >= 0.0 && now - completedAt >= (double)kRemovalGraceMs) break;
        if (now >= (double)deadlineMs) break;
    }
    utc_now(utc, sizeof(utc));
    printf("hang-client: long wait ended utc=%s elapsed=%.1f ms fence=0x%llx completed_at=%.1f removed_at=%.1f "
           "reason=0x%08lx %s\n", utc, now, (unsigned long long)v, completedAt, removedAt, (unsigned long)reason,
           dxgi_reason(reason));
    if (reason != S_OK) {
        printf("RESULT=HANG-DETECTED-DEVICE-REMOVED\n"); fflush(stdout);
        return 0;       // a removed device tears down without waiting on the GPU
    }
    // Fence reached but no removal: dxgkrnl completed the job instead of aborting it (no TDR ran).
    // Neither: no TDR within the deadline (TDR off, or a delay longer than assumed).
    puts(completedAt >= 0.0 ? "RESULT=FENCE-COMPLETED-NO-REMOVAL" : "RESULT=NO-REMOVAL-WITHIN-DEADLINE");
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 1);
    return 1;
}

int main(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(); return 0; }

    bool warp = false, haveAdapter = false, shortMode = false, longMode = false, haveMode = false;
    uint32_t shortMs = kShortTargetDefaultMs, deadlineMs = kLongDeadlineDefaultMs;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--lab")) { warp = false; haveAdapter = true; }
        else if (!strcmp(argv[i], "--warp")) { warp = true; haveAdapter = true; }
        else if (!strcmp(argv[i], "--short")) { shortMode = true; haveMode = true; }
        else if (!strcmp(argv[i], "--long")) { longMode = true; haveMode = true; }
        else if (!strcmp(argv[i], "--millis") && i + 1 < argc) { shortMs = (uint32_t)strtoul(argv[++i], nullptr, 10); }
        else if (!strcmp(argv[i], "--deadline") && i + 1 < argc) { deadlineMs = (uint32_t)strtoul(argv[++i], nullptr, 10) * 1000u; }
        else return usage();
    }
    if (!haveAdapter || !haveMode || (shortMode && longMode)) return usage();
    if (shortMs == 0 || shortMs > kShortTargetMaxMs) shortMs = kShortTargetMaxMs;
    if (deadlineMs < kLongDeadlineMinMs) deadlineMs = kLongDeadlineMinMs;
    if (deadlineMs > kLongDeadlineMaxMs) deadlineMs = kLongDeadlineMaxMs;

    // System D3D12 runtime only, never an application-local d3d12.dll.
    wchar_t sys[MAX_PATH]{};
    if (!GetSystemDirectoryW(sys, MAX_PATH) || wcscat_s(sys, L"\\d3d12.dll")) { printf("hang-client: system path FAILED\n"); return 3; }
    HMODULE runtime = LoadLibraryExW(sys, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) { printf("hang-client: load system d3d12.dll FAILED\n"); return 3; }
    auto proc = GetProcAddress(runtime, "D3D12CreateDevice");
    decltype(&D3D12CreateDevice) create_device = nullptr;
    memcpy(&create_device, &proc, sizeof(create_device));
    if (!create_device) { printf("hang-client: D3D12CreateDevice export missing\n"); return 3; }

    ComPtr<IDXGIFactory4> factory;
    if (!ck("CreateDXGIFactory1", CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter;
    if (warp) {
        if (!ck("EnumWarpAdapter", factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return 3;
    } else {
        for (UINT i = 0;; i++) {
            ComPtr<IDXGIAdapter1> cand; HRESULT hr = factory->EnumAdapters1(i, &cand);
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) return 3;
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(cand->GetDesc1(&d)) && d.VendorId == 0x1002 && d.DeviceId == 0x13fe) { adapter = cand; break; }
        }
    }
    if (!adapter) { printf("hang-client: BC-250 adapter (1002:13FE) not found\n"); return 3; }
    char utc[40];
    utc_now(utc, sizeof(utc));
    printf("hang-client: runtime=System32/d3d12.dll adapter=%s mode=%s pid=%lu utc=%s\n",
           warp ? "WARP" : "BC-250", warp ? (longMode ? "warp-long-flow" : "warp-smoke") : (shortMode ? "short" : "long"),
           (unsigned long)GetCurrentProcessId(), utc);
    fflush(stdout);

    Session s;
    if (!s.create(adapter.Get(), create_device)) { printf("RESULT=ERROR\n"); return 1; }

    // --long: the one deliberate hang, straight away: no calibration dispatch before it.
    if (longMode) return run_long(s, deadlineMs, warp ? 8u : 0xFFFFFFFFu);

    // Calibrate outer->ms with a small completing dispatch, so --short lands under the watchdog
    // across the lab's 1000-1500 MHz range. WARP is not calibrated: it runs one tiny dispatch.
    UINT64 completed = 0; double ms = 0.0;
    if (warp) {
        if (!s.dispatch(8u, 5000u, &completed, &ms)) { printf("RESULT=ERROR\n"); return 1; }
        printf("hang-client: warp smoke dispatch outer=8 elapsed=%.1f ms completed=0x%llx reason=0x%08lx %s\n",
               ms, (unsigned long long)completed, (unsigned long)s.device->GetDeviceRemovedReason(),
               dxgi_reason(s.device->GetDeviceRemovedReason()));
        printf("RESULT=WARP-SMOKE-OK\n"); return 0;
    }

    // --short only: at most six completing calibration dispatches of a few ms each.
    uint32_t calib = 64u; double calibMs = 0.0;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (!s.dispatch(calib, 5000u, &completed, &calibMs)) { printf("RESULT=ERROR\n"); return 1; }
        if (s.device->GetDeviceRemovedReason() != S_OK) {
            printf("hang-client: device removed DURING calibration (outer=%u) reason=0x%08lx %s\n",
                   calib, (unsigned long)s.device->GetDeviceRemovedReason(), dxgi_reason(s.device->GetDeviceRemovedReason()));
            printf("RESULT=UNEXPECTED-REMOVAL-IN-CALIBRATION\n"); return 1;
        }
        printf("hang-client: calibration outer=%u elapsed=%.2f ms\n", calib, calibMs); fflush(stdout);
        if (calibMs >= 4.0) break;
        if (calib > 0x20000000u) break;     // already huge; stop scaling
        calib *= 8u;
    }
    double outerPerMs = calibMs > 0.01 ? (double)calib / calibMs : (double)calib;

    uint32_t outer = (uint32_t)(outerPerMs * (double)shortMs);
    if (outer < 1u) outer = 1u;
    printf("hang-client: short target=%u ms -> outer=%u (watchdog=500 ms)\n", shortMs, outer); fflush(stdout);
    if (!s.dispatch(outer, 5000u, &completed, &ms)) { printf("RESULT=ERROR\n"); return 1; }
    HRESULT reason = s.device->GetDeviceRemovedReason();
    printf("hang-client: short dispatch elapsed=%.1f ms completed=0x%llx reason=0x%08lx %s\n",
           ms, (unsigned long long)completed, (unsigned long)reason, dxgi_reason(reason));
    if (reason == S_OK && completed != UINT64_MAX) { printf("RESULT=SHORT-COMPLETED-DEVICE-ALIVE\n"); return 0; }
    printf("RESULT=SHORT-UNEXPECTED-REMOVAL\n"); return 1;   // overshot the watchdog, or a real fault
}
