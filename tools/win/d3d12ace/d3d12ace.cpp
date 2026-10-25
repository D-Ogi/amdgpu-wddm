// SPDX-License-Identifier: MIT
/*
 * d3d12ace: the D3D12 compute and copy queues of the BC-250 driver on the compute node, step by step.
 *
 * Why this client exists. bc250kmd runs COMPUTE work on WDDM node 2, a MEC compute queue
 * (docs/design/compute-node.md). The hosted Vulkan driver of the D3D12 route shows a compute queue family when
 * that node is open, the D3D12 engine puts every D3D12 COMPUTE and COPY queue on that family, and the shell
 * opens the context of such a queue on node 2. With node 2 closed (the kernel driver's compute queue setting) the
 * caps show no compute node and the family is hidden, and so does RADV_BC250_ACE_QUEUES=0 in a lab-flavour ICD:
 * then every queue runs on the graphics node 0, as before. This client checks both through the standard D3D12
 * runtime, before a game does: that a compute queue computes the same words as the direct queue, that fences
 * order work across the queues in both directions, also when a Wait is queued before the Signal it waits for,
 * that the queues run at the same time, that several threads can drive several queues, and that the work of the
 * compute and copy queues ran on the node that the run expects.
 *
 * Every check is a word-by-word comparison with a CPU oracle. One compute shader (stage.hlsl) does all the GPU
 * work, with the function of tools/win/vkace: data[i] = rounds of a mixing function over (i ^ seed) in mode 0, or
 * over (data[i] ^ seed) in mode 1. A chain of mode 1 stages depends on the order of its stages, so a missing or a
 * broken cross-queue wait shows as wrong words, not only as a hang.
 *
 * The steps (--step N runs one; with no --step, steps 1 to 6 run in order):
 *   1 queues    a DIRECT, a COMPUTE and a COPY queue on the device; the adapter's nodes by name.
 *   2 same      the same dispatch on the DIRECT and the COMPUTE queue: both equal the oracle. Then direct ->
 *               compute (read-modify-write) -> copy -> direct (readback), each stage behind a fence value of the
 *               stage before it.
 *   3 chain     three stages on one fence, then the CPU: direct -> compute -> direct eight times, compute ->
 *               direct -> compute eight times, compute -> copy -> direct four times. In half of the runs the
 *               middle stage is submitted first, so its Wait comes before the Signal that it waits for.
 *   4 overlap   one long dispatch on each queue, alone and then both at once: the GPU times, the wall time,
 *               and how long the two dispatches overlapped when the queues give a clock calibration.
 *   5 threads   T threads (--threads), each with its own queue created in the thread (thread 0 DIRECT, the
 *               others COMPUTE), walk one fence in turns: each stage reads the result of the stage before it.
 *               Each thread also runs a private chain on its own buffer. All threads record at the same time
 *               and then submit all their work at once, so most Waits come before their Signals.
 *   6 node      where the work ran: the packets of this process per node (D3DKMTQueryStatistics) around
 *               dispatches on the COMPUTE queue only, copies on the COPY queue only and dispatches on the
 *               DIRECT queue only. --expect-node 2: the compute and the copy work ran on node 2 and the direct
 *               work did not. --expect-node 0 (the kill switch): the compute and the copy work ran on node 0
 *               and nothing ran on node 2.
 *
 * The run ends with the packets of the whole run per node; with --expect-node 0, node 2 must have none.
 *
 * The runtime is the d3d12.dll of System32, loaded by its full path. The "module" lines name the D3D12 and
 * user-mode driver modules in the process, with their SHA-256. Every line is flushed when it is written, so a
 * hang leaves the last step in the log. A watchdog ends the process at the deadline (--deadline, 170 s at
 * most). A GPU wait that times out ends the process too, without device teardown, because a teardown can wait
 * for the same work. The client opens no window.
 *
 * Exit: 0 every check passed, 1 a check failed, 2 usage, 3 system or D3D12 error before the device exists,
 * 4 deadline, 5 no hardware adapter (--any-device only).
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

#include "stage_dxil.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kBc250Vendor = 0x1002, kBc250Device = 0x13FE;
constexpr uint32_t kGroup = 64;
constexpr int kAnyNode = -1;
constexpr unsigned kComputeNode = 2;

LARGE_INTEGER qpcFreq, qpcStart;
unsigned deadlineSec = 170, fenceMs = 10000, threadCount = 4;
HANDLE deadlineEvent;
CRITICAL_SECTION outLock;
volatile LONG checks, failures;
int step = 0;
int expectNode = 2;
bool anyDevice = false, isBc250 = false;

ComPtr<ID3D12Device> dev;
ComPtr<ID3D12CommandQueue> gfxQ, cmpQ, cpyQ;
ComPtr<ID3D12RootSignature> rootSig;
ComPtr<ID3D12PipelineState> pso;
LUID luid;
HANDLE processHandle;
UINT nodeCount;
std::vector<DXGK_NODEMETADATA> nodes;

/* ---- output ------------------------------------------------------------------------------------------- */

double NowMs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - qpcStart.QuadPart) * 1000.0 / static_cast<double>(qpcFreq.QuadPart);
}

void Emit(const char* kind, const char* fmt, va_list ap)
{
    char text[1024];
    vsnprintf(text, sizeof text, fmt, ap);
    EnterCriticalSection(&outLock);
    printf("%-4s %8.1f ms step %d: %s\n", kind, NowMs(), step, text);
    fflush(stdout);
    LeaveCriticalSection(&outLock);
}

void Info(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Emit("INFO", fmt, ap);
    va_end(ap);
}

bool Check(bool ok, const char* fmt, ...)
{
    va_list ap;
    InterlockedIncrement(&checks);
    if (!ok) InterlockedIncrement(&failures);
    va_start(ap, fmt);
    Emit(ok ? "PASS" : "FAIL", fmt, ap);
    va_end(ap);
    return ok;
}

/* The end of the process with no device teardown: work that did not finish can make a release wait. */
[[noreturn]] void Exit(int code, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Emit(code == 0 ? "INFO" : "FAIL", fmt, ap);
    va_end(ap);
    printf("d3d12ace result=%s checks=%ld failures=%ld exit=%d\n", code == 0 ? "pass" : "fail", checks, failures, code);
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    ExitProcess(static_cast<UINT>(code));
}

/* Once the device exists, a failed call is a failed check of the driver (exit 1); before, a system error. */
[[noreturn]] void FailHr(const char* what, HRESULT hr)
{
    const HRESULT removed = dev ? dev->GetDeviceRemovedReason() : S_OK;
    InterlockedIncrement(&failures);
    Exit(dev ? 1 : 3, "%s returned 0x%08lX (device removed reason 0x%08lX)", what, static_cast<unsigned long>(hr),
         static_cast<unsigned long>(removed));
}

#define HR(call) do { const HRESULT hr_ = (call); if (FAILED(hr_)) FailHr(#call, hr_); } while (0)

DWORD WINAPI Watchdog(LPVOID)
{
    if (WaitForSingleObject(deadlineEvent, deadlineSec * 1000u) == WAIT_TIMEOUT) {
        fprintf(stderr, "d3d12ace: deadline of %u s exceeded, ending the process\n", deadlineSec);
        Exit(4, "deadline of %u s exceeded", deadlineSec);
    }
    return 0;
}

/* ---- the driver witness ------------------------------------------------------------------------------- */

bool Sha256File(const wchar_t* path, char out[65])
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    static unsigned char buffer[65536];
    unsigned char digest[32];
    DWORD read;
    bool ok = true;
    if (file == INVALID_HANDLE_VALUE) return false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0 ||
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        ok = false;
    } else {
        while (ReadFile(file, buffer, sizeof buffer, &read, nullptr) && read)
            if (BCryptHashData(hash, buffer, read, 0) != 0) { ok = false; break; }
        if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) != 0) ok = false;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    if (ok)
        for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return ok;
}

/* The D3D12 runtime and the user-mode driver modules that it loaded, by path and SHA-256. */
void PrintModules()
{
    static HMODULE mods[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &bytes)) {
        printf("module        list unavailable (error %lu)\n", GetLastError());
        return;
    }
    const DWORD count = bytes / sizeof(HMODULE) < 1024 ? bytes / sizeof(HMODULE) : 1024;
    for (DWORD i = 0; i < count; i++) {
        wchar_t path[MAX_PATH * 2], lower[MAX_PATH * 2];
        if (!GetModuleFileNameW(mods[i], path, MAX_PATH * 2)) continue;
        size_t k = 0;
        for (; path[k] && k + 1 < MAX_PATH * 2; k++) lower[k] = static_cast<wchar_t>(towlower(path[k]));
        lower[k] = 0;
        const wchar_t* name = wcsrchr(lower, L'\\');
        name = name ? name + 1 : lower;
        if (!wcsstr(name, L"amdgpu_wddm") && !wcsstr(name, L"vkd3d") && !wcsstr(name, L"d3d12") && !wcsstr(name, L"radv"))
            continue;
        char sha[65] = "unreadable";
        Sha256File(path, sha);
        printf("module        %s %ls\n", sha, path);
    }
    fflush(stdout);
}

/* ---- the nodes of the adapter ------------------------------------------------------------------------- */

const char* EngineName(DXGK_ENGINE_TYPE type)
{
    static const char* names[] = { "other", "3d", "video decode", "video encode", "video processing", "scene assembly",
                                   "copy", "overlay", "crypto" };
    return static_cast<unsigned>(type) < sizeof names / sizeof names[0] ? names[type] : "unknown";
}

void ReadNodes()
{
    D3DKMT_QUERYSTATISTICS q{};
    q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    q.AdapterLuid = luid;
    NTSTATUS status = D3DKMTQueryStatistics(&q);
    if (status != 0) Exit(3, "D3DKMTQueryStatistics(ADAPTER) returned 0x%08lX", static_cast<unsigned long>(status));
    nodeCount = q.QueryResult.AdapterInformation.NodeCount;
    nodes.assign(nodeCount, DXGK_NODEMETADATA{});
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0) return;
    for (UINT n = 0; n < nodeCount; n++) {
        D3DKMT_NODEMETADATA md{};
        md.NodeOrdinalAndAdapterIndex = n;
        D3DKMT_QUERYADAPTERINFO qi{};
        qi.hAdapter = open.hAdapter;
        qi.Type = KMTQAITYPE_NODEMETADATA;
        qi.pPrivateDriverData = &md;
        qi.PrivateDriverDataSize = sizeof md;
        if (D3DKMTQueryAdapterInfo(&qi) == 0) nodes[n] = md.NodeData;
    }
    D3DKMT_CLOSEADAPTER close{};
    close.hAdapter = open.hAdapter;
    D3DKMTCloseAdapter(&close);
}

struct NodeStat {
    uint64_t queuePackets = 0, dmaPackets = 0, runningTime = 0;
    bool valid = false;
};

std::vector<NodeStat> Snapshot()
{
    std::vector<NodeStat> s(nodeCount);
    for (UINT n = 0; n < nodeCount; n++) {
        D3DKMT_QUERYSTATISTICS q{};
        q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
        q.AdapterLuid = luid;
        q.hProcess = processHandle;
        q.QueryProcessNode.NodeId = n;
        if (D3DKMTQueryStatistics(&q) != 0) continue;
        const D3DKMT_QUERYSTATISTICS_PROCESS_NODE_INFORMATION& r = q.QueryResult.ProcessNodeInformation;
        for (int k = 0; k < D3DKMT_QUERYSTATISTICS_QUEUE_PACKET_TYPE_MAX; k++)
            s[n].queuePackets += r.PacketStatistics.QueuePacket[k].PacketCompleted;
        for (int k = 0; k < D3DKMT_QUERYSTATISTICS_DMA_PACKET_TYPE_MAX; k++)
            s[n].dmaPackets += r.PacketStatistics.DmaPacket[k].PacketCompleted;
        s[n].runningTime = static_cast<uint64_t>(r.RunningTime.QuadPart);
        s[n].valid = true;
    }
    return s;
}

/* The packets that this process completed on each node between two snapshots. */
std::vector<uint64_t> Delta(const std::vector<NodeStat>& before, const std::vector<NodeStat>& after, const char* what)
{
    std::vector<uint64_t> packets(nodeCount);
    for (UINT n = 0; n < nodeCount; n++) {
        if (!before[n].valid || !after[n].valid) {
            Info("%s: node %u: no statistics", what, n);
            continue;
        }
        const uint64_t q = after[n].queuePackets - before[n].queuePackets, d = after[n].dmaPackets - before[n].dmaPackets;
        const uint64_t t = after[n].runningTime - before[n].runningTime;
        packets[n] = q + d;
        // An adapter with many nodes (another vendor's) prints only the nodes that changed.
        if (nodeCount > 4 && !q && !d && !t) continue;
        Info("%s: node %u (%ls, %s): +%llu queue packets, +%llu DMA packets, +%llu running time", what, n,
             nodes[n].FriendlyName, EngineName(nodes[n].EngineType), static_cast<unsigned long long>(q),
             static_cast<unsigned long long>(d), static_cast<unsigned long long>(t));
    }
    return packets;
}

uint64_t On(const std::vector<uint64_t>& packets, unsigned node) { return node < packets.size() ? packets[node] : 0; }

/* ---- the oracle --------------------------------------------------------------------------------------- */

struct Params {
    uint32_t seed, count, rounds, mode;
};

uint32_t Mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

uint32_t Stage(uint32_t in, uint32_t seed, uint32_t rounds)
{
    uint32_t v = in ^ seed;
    for (uint32_t r = 0; r < rounds; r++) v = Mix(v + r);
    return v;
}

uint32_t Seed(uint32_t k) { return (k + 1u) * 0x9e3779b9u ^ 0x5bd1e995u; }

/* want[] after one more stage of p. */
void Apply(std::vector<uint32_t>& want, const Params& p)
{
    for (uint32_t i = 0; i < p.count; i++) want[i] = Stage(p.mode ? want[i] : i, p.seed, p.rounds);
}

/* ---- buffers, lists and fences ------------------------------------------------------------------------ */

struct Buf {
    ComPtr<ID3D12Resource> gpu, rb;
    uint32_t count = 0;
    uint32_t* map = nullptr;
};

ComPtr<ID3D12Resource> Buffer(D3D12_HEAP_TYPE heap, uint64_t bytes, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    HR(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)));
    return r;
}

/* A GPU buffer (COMMON between lists) and its persistently mapped readback copy. */
Buf MakeBuf(uint32_t n)
{
    Buf b;
    b.count = n;
    b.gpu = Buffer(D3D12_HEAP_TYPE_DEFAULT, n * 4ull, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    b.rb = Buffer(D3D12_HEAP_TYPE_READBACK, n * 4ull, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RANGE range{ 0, n * 4ull };
    void* p = nullptr;
    HR(b.rb->Map(0, &range, &p));
    b.map = static_cast<uint32_t*>(p);
    return b;
}

/* Words that no stage gives, so that a readback that did not happen cannot pass. */
void Sentinel(Buf& b)
{
    for (uint32_t i = 0; i < b.count; i++) b.map[i] = 0xdeadbeefu ^ (i * 0x01000193u);
}

uint32_t Bad(const Buf& b, const std::vector<uint32_t>& want, uint32_t* first = nullptr)
{
    uint32_t bad = 0;
    for (uint32_t i = 0; i < b.count; i++)
        if (b.map[i] != want[i]) {
            if (!bad && first) *first = i;
            bad++;
        }
    return bad;
}

struct Rec {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
};

Rec Begin(D3D12_COMMAND_LIST_TYPE type)
{
    Rec r;
    HR(dev->CreateCommandAllocator(type, IID_PPV_ARGS(&r.alloc)));
    HR(dev->CreateCommandList(0, type, r.alloc.Get(), nullptr, IID_PPV_ARGS(&r.list)));
    return r;
}

void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    l->ResourceBarrier(1, &b);
}

/* One stage on a DIRECT or COMPUTE list: the buffer is COMMON before and after; optionally timestamps around
 * the dispatch, resolved into tsOut, and the readback copy of the result. */
void RecordStage(ID3D12GraphicsCommandList* l, Buf& b, const Params& p, bool readback, ID3D12QueryHeap* ts = nullptr,
                 ID3D12Resource* tsOut = nullptr)
{
    Barrier(l, b.gpu.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    l->SetComputeRootSignature(rootSig.Get());
    l->SetPipelineState(pso.Get());
    l->SetComputeRoot32BitConstants(0, 4, &p, 0);
    l->SetComputeRootUnorderedAccessView(1, b.gpu->GetGPUVirtualAddress());
    if (ts) l->EndQuery(ts, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    l->Dispatch((p.count + kGroup - 1) / kGroup, 1, 1);
    if (ts) {
        l->EndQuery(ts, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        l->ResolveQueryData(ts, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, tsOut, 0);
    }
    if (readback) {
        Barrier(l, b.gpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        l->CopyBufferRegion(b.rb.Get(), 0, b.gpu.Get(), 0, b.count * 4ull);
        Barrier(l, b.gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    } else {
        Barrier(l, b.gpu.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    }
}

/* src -> dst on any list type, COPY included; both COMMON before and after. */
void RecordCopy(ID3D12GraphicsCommandList* l, Buf& src, Buf& dst)
{
    Barrier(l, src.gpu.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(l, dst.gpu.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    l->CopyBufferRegion(dst.gpu.Get(), 0, src.gpu.Get(), 0, src.count * 4ull);
    Barrier(l, dst.gpu.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    Barrier(l, src.gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

void RecordReadback(ID3D12GraphicsCommandList* l, Buf& b)
{
    Barrier(l, b.gpu.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    l->CopyBufferRegion(b.rb.Get(), 0, b.gpu.Get(), 0, b.count * 4ull);
    Barrier(l, b.gpu.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
}

void Exec(ID3D12CommandQueue* q, Rec& r)
{
    ID3D12CommandList* l = r.list.Get();
    q->ExecuteCommandLists(1, &l);
}

ComPtr<ID3D12Fence> MakeFence()
{
    ComPtr<ID3D12Fence> f;
    HR(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f)));
    return f;
}

/* A bounded CPU wait for a fence value; a timeout or a removed device ends the process without teardown. */
void WaitFence(ID3D12Fence* f, UINT64 value, const char* what)
{
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) Exit(1, "CreateEvent failed (%lu)", GetLastError());
    HR(f->SetEventOnCompletion(value, event));
    const DWORD w = WaitForSingleObject(event, fenceMs);
    CloseHandle(event);
    const UINT64 done = f->GetCompletedValue();
    if (done == UINT64_MAX) {
        InterlockedIncrement(&failures);
        Exit(1, "%s: the fence reads UINT64_MAX (device removed, reason 0x%08lX)", what,
             static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
    }
    if (w != WAIT_OBJECT_0 || done < value) {
        InterlockedIncrement(&failures);
        Exit(1, "GPU wait for %s timed out after %u ms (fence %llu, expected %llu)", what, fenceMs,
             static_cast<unsigned long long>(done), static_cast<unsigned long long>(value));
    }
}

const char* TypeName(D3D12_COMMAND_LIST_TYPE t)
{
    return t == D3D12_COMMAND_LIST_TYPE_DIRECT ? "direct" : t == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute" : "copy";
}

/* ---- step 1: queues ----------------------------------------------------------------------------------- */

void Step1()
{
    step = 1;
    Check(gfxQ->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT && cmpQ->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COMPUTE &&
              cpyQ->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_COPY,
          "a direct, a compute and a copy queue");
    for (UINT n = 0; n < nodeCount; n++)
        Info("adapter node %u: '%ls', engine %s", n, nodes[n].FriendlyName, EngineName(nodes[n].EngineType));
    if (expectNode == static_cast<int>(kComputeNode))
        Check(nodeCount > kComputeNode, "the adapter has node %u (%u nodes)", kComputeNode, nodeCount);
    UINT64 fg = 0, fc = 0;
    HR(gfxQ->GetTimestampFrequency(&fg));
    HR(cmpQ->GetTimestampFrequency(&fc));
    Check(fg && fc, "timestamp frequency: direct %llu Hz, compute %llu Hz", static_cast<unsigned long long>(fg),
          static_cast<unsigned long long>(fc));
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof o3)))
        Info("copy queue timestamps %s", o3.CopyQueueTimestampQueriesSupported ? "supported" : "not supported");
}

/* ---- step 2: the same words ----------------------------------------------------------------------------- */

void Step2()
{
    step = 2;
    const uint32_t n = 1u << 16, rounds = 8;
    Buf a = MakeBuf(n), b = MakeBuf(n), c = MakeBuf(n);
    Sentinel(a);
    Sentinel(b);
    const Params p0{ Seed(200), n, rounds, 0 };
    std::vector<uint32_t> want(n);
    Apply(want, p0);

    Rec ra = Begin(D3D12_COMMAND_LIST_TYPE_DIRECT), rb = Begin(D3D12_COMMAND_LIST_TYPE_COMPUTE);
    RecordStage(ra.list.Get(), a, p0, true);
    RecordStage(rb.list.Get(), b, p0, true);
    HR(ra.list->Close());
    HR(rb.list->Close());
    ComPtr<ID3D12Fence> fa = MakeFence(), fb = MakeFence();
    Exec(gfxQ.Get(), ra);
    HR(gfxQ->Signal(fa.Get(), 1));
    Exec(cmpQ.Get(), rb);
    HR(cmpQ->Signal(fb.Get(), 1));
    WaitFence(fa.Get(), 1, "the direct dispatch");
    WaitFence(fb.Get(), 1, "the compute dispatch");
    const uint32_t badA = Bad(a, want), badB = Bad(b, want);
    Check(!badA && !badB, "the same dispatch: direct %u, compute %u of %u words differ from the oracle", badA, badB, n);

    /* direct (mode 0 into c) -> compute (mode 1 on c, readback) -> copy (c into b) -> direct (readback of b). */
    Sentinel(b);
    Sentinel(c);
    const Params p1{ Seed(201), n, rounds, 0 }, p2{ Seed(202), n, rounds, 1 };
    std::vector<uint32_t> want2(n);
    Apply(want2, p1);
    Apply(want2, p2);
    Rec s1 = Begin(D3D12_COMMAND_LIST_TYPE_DIRECT), s2 = Begin(D3D12_COMMAND_LIST_TYPE_COMPUTE),
        s3 = Begin(D3D12_COMMAND_LIST_TYPE_COPY), s4 = Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);
    RecordStage(s1.list.Get(), c, p1, false);
    RecordStage(s2.list.Get(), c, p2, true);
    RecordCopy(s3.list.Get(), c, b);
    RecordReadback(s4.list.Get(), b);
    for (Rec* r : { &s1, &s2, &s3, &s4 }) HR(r->list->Close());
    ComPtr<ID3D12Fence> f = MakeFence();
    Exec(gfxQ.Get(), s1);
    HR(gfxQ->Signal(f.Get(), 1));
    HR(cmpQ->Wait(f.Get(), 1));
    Exec(cmpQ.Get(), s2);
    HR(cmpQ->Signal(f.Get(), 2));
    HR(cpyQ->Wait(f.Get(), 2));
    Exec(cpyQ.Get(), s3);
    HR(cpyQ->Signal(f.Get(), 3));
    HR(gfxQ->Wait(f.Get(), 3));
    Exec(gfxQ.Get(), s4);
    HR(gfxQ->Signal(f.Get(), 4));
    WaitFence(f.Get(), 4, "direct -> compute -> copy -> direct");
    const uint32_t badC = Bad(c, want2), badCopy = Bad(b, want2);
    Check(!badC, "compute read-modify-write behind the direct stage: %u of %u words differ", badC, n);
    Check(!badCopy, "copy queue behind the compute stage, read back on the direct queue: %u of %u words differ", badCopy, n);
}

/* ---- step 3: chains ------------------------------------------------------------------------------------- */

struct ChainStage {
    ID3D12CommandQueue* q;
    D3D12_COMMAND_LIST_TYPE type;
    bool copy;
};

/* Three stages on one fence: stage j waits for value j (j > 0) and signals j + 1. With copy, stage 1 copies a into
 * b and stage 2 works on b; otherwise all three stages work on a. order[] is the submission order. */
uint32_t RunChain(const ChainStage (&s)[3], const int (&order)[3], Buf& a, Buf& b, uint32_t run, uint32_t* first)
{
    const uint32_t n = a.count, rounds = 4;
    const bool copy = s[1].copy;
    Buf& out = copy ? b : a;
    Sentinel(out);
    const Params p[3] = { { Seed(300 + 3 * run), n, rounds, 0 }, { Seed(301 + 3 * run), n, rounds, 1 },
                          { Seed(302 + 3 * run), n, rounds, 1 } };
    std::vector<uint32_t> want(n);
    Apply(want, p[0]);
    if (!copy) Apply(want, p[1]);
    Apply(want, p[2]);
    Rec r[3];
    for (int j = 0; j < 3; j++) {
        r[j] = Begin(s[j].type);
        if (s[j].copy) RecordCopy(r[j].list.Get(), a, b);
        else RecordStage(r[j].list.Get(), j == 2 ? out : a, p[j], j == 2);
        HR(r[j].list->Close());
    }
    ComPtr<ID3D12Fence> f = MakeFence();
    for (int j : order) {
        if (j) HR(s[j].q->Wait(f.Get(), static_cast<UINT64>(j)));
        Exec(s[j].q, r[j]);
        HR(s[j].q->Signal(f.Get(), static_cast<UINT64>(j) + 1));
    }
    WaitFence(f.Get(), 3, "a chain");
    return Bad(out, want, first);
}

void Step3()
{
    step = 3;
    const uint32_t n = 1u << 14;
    Buf a = MakeBuf(n), b = MakeBuf(n);
    const ChainStage dcd[3] = { { gfxQ.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, false },
                                { cmpQ.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, false },
                                { gfxQ.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, false } };
    const ChainStage cdc[3] = { { cmpQ.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, false },
                                { gfxQ.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, false },
                                { cmpQ.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, false } };
    const ChainStage cyd[3] = { { cmpQ.Get(), D3D12_COMMAND_LIST_TYPE_COMPUTE, false },
                                { cpyQ.Get(), D3D12_COMMAND_LIST_TYPE_COPY, true },
                                { gfxQ.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT, false } };
    const struct {
        const char* name;
        const ChainStage (*stages)[3];
        uint32_t runs;
        int middleFirst[3];
    } kinds[] = { { "direct -> compute -> direct", &dcd, 8, { 1, 0, 2 } },
                  { "compute -> direct -> compute", &cdc, 8, { 1, 0, 2 } },
                  { "compute -> copy -> direct", &cyd, 4, { 1, 2, 0 } } };
    const int inOrder[3] = { 0, 1, 2 };
    uint32_t run = 0;
    for (const auto& k : kinds) {
        uint32_t bad = 0, badRuns = 0;
        for (uint32_t i = 0; i < k.runs; i++, run++) {
            uint32_t first = 0;
            const uint32_t b1 = RunChain(*k.stages, (i & 1) ? k.middleFirst : inOrder, a, b, run, &first);
            if (b1) {
                Info("%s, run %u (%s): %u words differ, the first at %u", k.name, i,
                     (i & 1) ? "middle stage first" : "in order", b1, first);
                badRuns++;
            }
            bad += b1;
        }
        Check(!bad, "%s: %u runs (half with the middle stage submitted first), %u runs and %u words differ", k.name,
              k.runs, badRuns, bad);
    }
}

/* ---- step 4: overlap ------------------------------------------------------------------------------------ */

struct LongRun {
    double gpuMs = 0, wallMs = 0, startQpc = 0, endQpc = 0;
    bool calibrated = false;
};

/* One long dispatch on queue 0 or 1 alone (which), or one on each queue (which < 0) submitted back to back. */
void RunLong(int which, Buf (&bufs)[2], uint32_t rounds, LongRun (&out)[2])
{
    ID3D12CommandQueue* queues[2] = { gfxQ.Get(), cmpQ.Get() };
    const D3D12_COMMAND_LIST_TYPE types[2] = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE };
    Rec r[2];
    ComPtr<ID3D12QueryHeap> heaps[2];
    ComPtr<ID3D12Resource> tsOut[2];
    ComPtr<ID3D12Fence> fences[2];
    UINT64 gpuCal[2] = {}, cpuCal[2] = {}, freq[2] = {};
    bool cal[2] = {};
    for (int k = 0; k < 2; k++) {
        if (which >= 0 && k != which) continue;
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2;
        HR(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&heaps[k])));
        tsOut[k] = Buffer(D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        fences[k] = MakeFence();
        r[k] = Begin(types[k]);
        const Params p{ Seed(400 + static_cast<uint32_t>(k)), bufs[k].count, rounds, 0 };
        RecordStage(r[k].list.Get(), bufs[k], p, false, heaps[k].Get(), tsOut[k].Get());
        HR(r[k].list->Close());
        HR(queues[k]->GetTimestampFrequency(&freq[k]));
        cal[k] = SUCCEEDED(queues[k]->GetClockCalibration(&gpuCal[k], &cpuCal[k]));
    }
    const double t0 = NowMs();
    for (int k = 0; k < 2; k++)
        if (which < 0 || k == which) {
            Exec(queues[k], r[k]);
            HR(queues[k]->Signal(fences[k].Get(), 1));
        }
    for (int k = 0; k < 2; k++)
        if (which < 0 || k == which) {
            WaitFence(fences[k].Get(), 1, "a long dispatch");
            out[k].wallMs = NowMs() - t0;
            void* p = nullptr;
            D3D12_RANGE range{ 0, 16 };
            HR(tsOut[k]->Map(0, &range, &p));
            const UINT64* ts = static_cast<const UINT64*>(p);
            out[k].gpuMs = ts[1] > ts[0] ? static_cast<double>(ts[1] - ts[0]) * 1000.0 / static_cast<double>(freq[k]) : 0.0;
            out[k].calibrated = cal[k];
            if (cal[k]) {
                const double scale = static_cast<double>(qpcFreq.QuadPart) / static_cast<double>(freq[k]);
                out[k].startQpc = static_cast<double>(cpuCal[k]) + (static_cast<double>(ts[0]) - static_cast<double>(gpuCal[k])) * scale;
                out[k].endQpc = static_cast<double>(cpuCal[k]) + (static_cast<double>(ts[1]) - static_cast<double>(gpuCal[k])) * scale;
            }
            D3D12_RANGE none{ 0, 0 };
            tsOut[k]->Unmap(0, &none);
        }
}

uint32_t SampleBad(const Buf& b, uint32_t seed, uint32_t rounds, uint32_t* sampled)
{
    uint32_t bad = 0;
    *sampled = 0;
    for (uint32_t i = 0; i < b.count; i += 4093) {
        (*sampled)++;
        bad += b.map[i] != Stage(i, seed, rounds);
    }
    return bad;
}

void Step4()
{
    step = 4;
    const uint32_t n = 1u << 20;
    Buf bufs[2] = { MakeBuf(n), MakeBuf(n) };
    LongRun r[2];

    /* Calibration: about 80 ms of GPU time for one dispatch on the direct queue. */
    uint32_t rounds = 256;
    RunLong(0, bufs, rounds, r);
    if (r[0].gpuMs > 0.01) {
        const double scaled = rounds * 80.0 / r[0].gpuMs;
        rounds = scaled < 256.0 ? 256u : scaled > 65536.0 ? 65536u : static_cast<uint32_t>(scaled);
    }
    Info("calibration: 256 rounds took %.2f ms of GPU time; the long dispatch is %u rounds", r[0].gpuMs, rounds);

    LongRun g[2], c[2], both[2];
    RunLong(0, bufs, rounds, g);
    RunLong(1, bufs, rounds, c);
    Check(g[0].gpuMs > 0 && c[1].gpuMs > 0, "alone: direct %.2f ms, compute %.2f ms of GPU time (wall %.1f, %.1f ms)",
          g[0].gpuMs, c[1].gpuMs, g[0].wallMs, c[1].wallMs);

    // The readback copies are not part of this step: the results are read back with one list after the run.
    RunLong(-1, bufs, rounds, both);
    {
        Rec rb = Begin(D3D12_COMMAND_LIST_TYPE_DIRECT);
        Sentinel(bufs[0]);
        Sentinel(bufs[1]);
        RecordReadback(rb.list.Get(), bufs[0]);
        RecordReadback(rb.list.Get(), bufs[1]);
        HR(rb.list->Close());
        ComPtr<ID3D12Fence> f = MakeFence();
        Exec(gfxQ.Get(), rb);
        HR(gfxQ->Signal(f.Get(), 1));
        WaitFence(f.Get(), 1, "the readback of the long dispatches");
    }
    uint32_t sampled = 0;
    const uint32_t bad0 = SampleBad(bufs[0], Seed(400), rounds, &sampled), bad1 = SampleBad(bufs[1], Seed(401), rounds, &sampled);
    Check(!bad0 && !bad1, "together: both results equal the oracle on %u sampled words each (%u, %u differ)", sampled, bad0, bad1);
    const double wall = both[0].wallMs > both[1].wallMs ? both[0].wallMs : both[1].wallMs;
    Info("together: direct %.2f ms, compute %.2f ms of GPU time, wall %.1f ms; alone in sum %.2f ms of GPU time",
         both[0].gpuMs, both[1].gpuMs, wall, g[0].gpuMs + c[1].gpuMs);
    if (both[0].calibrated && both[1].calibrated) {
        const double lo = both[0].startQpc > both[1].startQpc ? both[0].startQpc : both[1].startQpc;
        const double hi = both[0].endQpc < both[1].endQpc ? both[0].endQpc : both[1].endQpc;
        const double ms = 1000.0 / static_cast<double>(qpcFreq.QuadPart);
        Info("together: the two dispatches overlapped for %.2f ms (compute starts %+.2f ms after direct)",
             hi > lo ? (hi - lo) * ms : 0.0, (both[1].startQpc - both[0].startQpc) * ms);
    } else {
        Info("together: no clock calibration on both queues, the overlap is not measured");
    }
}

/* ---- step 5: threads ------------------------------------------------------------------------------------ */

constexpr uint32_t kTurns = 8;

struct Walker {
    unsigned k = 0;
    D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> q;
    Rec recs[kTurns];
    Buf* shared = nullptr;
    Buf priv;
    ID3D12Fence* fence = nullptr;
    HANDLE ready = nullptr, go = nullptr;
    uint32_t n = 0, rounds = 0;
};

uint32_t SharedValue(unsigned k, uint32_t turn) { return turn * threadCount + k + 1; }

DWORD WINAPI Walk(LPVOID arg)
{
    Walker* w = static_cast<Walker*>(arg);
    D3D12_COMMAND_QUEUE_DESC d{};
    d.Type = w->type;
    HR(dev->CreateCommandQueue(&d, IID_PPV_ARGS(&w->q)));
    const uint32_t last = kTurns * threadCount;
    for (uint32_t t = 0; t < kTurns; t++) {
        const uint32_t s = SharedValue(w->k, t);
        w->recs[t] = Begin(w->type);
        const Params shared{ Seed(500 + s), w->n, w->rounds, s == 1 ? 0u : 1u };
        const Params priv{ Seed(1000 + w->k * 64 + t), w->n, w->rounds, t == 0 ? 0u : 1u };
        RecordStage(w->recs[t].list.Get(), *w->shared, shared, s == last);
        RecordStage(w->recs[t].list.Get(), w->priv, priv, t == kTurns - 1);
        HR(w->recs[t].list->Close());
    }
    SetEvent(w->ready);
    WaitForSingleObject(w->go, INFINITE);
    for (uint32_t t = 0; t < kTurns; t++) {
        const uint32_t s = SharedValue(w->k, t);
        if (s > 1) HR(w->q->Wait(w->fence, s - 1));
        Exec(w->q.Get(), w->recs[t]);
        HR(w->q->Signal(w->fence, s));
    }
    return 0;
}

void Step5()
{
    step = 5;
    const uint32_t n = 1u << 14, rounds = 4, last = kTurns * threadCount;
    Buf shared = MakeBuf(n);
    Sentinel(shared);
    ComPtr<ID3D12Fence> f = MakeFence();
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!go) Exit(1, "CreateEvent failed (%lu)", GetLastError());
    std::vector<Walker> walkers(threadCount);
    std::vector<HANDLE> threads, ready;
    for (unsigned k = 0; k < threadCount; k++) {
        Walker& w = walkers[k];
        w.k = k;
        w.type = k == 0 ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
        w.shared = &shared;
        w.priv = MakeBuf(n);
        Sentinel(w.priv);
        w.fence = f.Get();
        w.go = go;
        w.n = n;
        w.rounds = rounds;
        w.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!w.ready) Exit(1, "CreateEvent failed (%lu)", GetLastError());
        ready.push_back(w.ready);
    }
    for (unsigned k = 0; k < threadCount; k++) {
        HANDLE t = CreateThread(nullptr, 0, Walk, &walkers[k], 0, nullptr);
        if (!t) Exit(1, "CreateThread failed (%lu)", GetLastError());
        threads.push_back(t);
    }
    if (WaitForMultipleObjects(threadCount, ready.data(), TRUE, fenceMs) != WAIT_OBJECT_0) {
        InterlockedIncrement(&failures);
        Exit(1, "the threads did not record their lists within %u ms", fenceMs);
    }
    Info("%u threads (1 direct, %u compute queues) recorded %u lists each; submitting", threadCount, threadCount - 1, kTurns);
    const double t0 = NowMs();
    SetEvent(go);
    WaitFence(f.Get(), last, "the threads' chain");
    const double wall = NowMs() - t0;
    if (WaitForMultipleObjects(threadCount, threads.data(), TRUE, fenceMs) != WAIT_OBJECT_0) {
        InterlockedIncrement(&failures);
        Exit(1, "the threads did not end within %u ms", fenceMs);
    }
    for (HANDLE t : threads) CloseHandle(t);
    for (HANDLE e : ready) CloseHandle(e);
    CloseHandle(go);

    std::vector<uint32_t> want(n);
    for (uint32_t s = 1; s <= last; s++) Apply(want, Params{ Seed(500 + s), n, rounds, s == 1 ? 0u : 1u });
    uint32_t first = 0;
    const uint32_t bad = Bad(shared, want, &first);
    Check(!bad, "the shared chain of %u stages over %u queues in %.1f ms: %u of %u words differ%s", last, threadCount, wall,
          bad, n, bad ? " (see the first below)" : "");
    if (bad) Info("the first wrong word is %u: 0x%08x, the oracle 0x%08x", first, shared.map[first], want[first]);
    uint32_t badPriv = 0;
    for (Walker& w : walkers) {
        std::vector<uint32_t> mine(n);
        for (uint32_t t = 0; t < kTurns; t++) Apply(mine, Params{ Seed(1000 + w.k * 64 + t), n, rounds, t == 0 ? 0u : 1u });
        const uint32_t b = Bad(w.priv, mine);
        if (b) Info("thread %u (%s): %u of %u private words differ", w.k, TypeName(w.type), b, n);
        badPriv += b;
    }
    Check(!badPriv, "the private chains of the %u threads: %u words differ", threadCount, badPriv);
}

/* ---- step 6: node ---------------------------------------------------------------------------------------- */

std::vector<uint64_t> Window(const char* what, ID3D12CommandQueue* q, D3D12_COMMAND_LIST_TYPE type, Buf& a, Buf& b)
{
    constexpr uint32_t kSubmits = 16;
    Rec r[kSubmits];
    for (uint32_t i = 0; i < kSubmits; i++) {
        r[i] = Begin(type);
        if (type == D3D12_COMMAND_LIST_TYPE_COPY) RecordCopy(r[i].list.Get(), a, b);
        else RecordStage(r[i].list.Get(), a, Params{ Seed(600 + i), a.count, 4, i ? 1u : 0u }, false);
        HR(r[i].list->Close());
    }
    ComPtr<ID3D12Fence> f = MakeFence();
    // Let the counters of the work before this window settle.
    Sleep(100);
    const std::vector<NodeStat> before = Snapshot();
    for (uint32_t i = 0; i < kSubmits; i++) {
        Exec(q, r[i]);
        HR(q->Signal(f.Get(), i + 1));
    }
    WaitFence(f.Get(), kSubmits, what);
    Sleep(100);
    return Delta(before, Snapshot(), what);
}

void Step6()
{
    step = 6;
    Buf a = MakeBuf(1u << 14), b = MakeBuf(1u << 14);
    const std::vector<uint64_t> compute = Window("16 dispatches on the compute queue only", cmpQ.Get(),
                                                 D3D12_COMMAND_LIST_TYPE_COMPUTE, a, b);
    const std::vector<uint64_t> copy = Window("16 copies on the copy queue only", cpyQ.Get(), D3D12_COMMAND_LIST_TYPE_COPY, a, b);
    const std::vector<uint64_t> direct = Window("16 dispatches on the direct queue only", gfxQ.Get(),
                                                D3D12_COMMAND_LIST_TYPE_DIRECT, a, b);
    if (expectNode == kAnyNode) {
        Info("no node is expected (--expect-node any): the packets above are for information");
        return;
    }
    if (expectNode == static_cast<int>(kComputeNode)) {
        Check(On(compute, kComputeNode) > 0, "the compute queue ran on node %u: +%llu packets there", kComputeNode,
              static_cast<unsigned long long>(On(compute, kComputeNode)));
        Check(On(copy, kComputeNode) > 0, "the copy queue ran on node %u: +%llu packets there", kComputeNode,
              static_cast<unsigned long long>(On(copy, kComputeNode)));
        Check(On(direct, 0) > 0 && On(direct, kComputeNode) == 0,
              "the direct queue ran on node 0 and not on node %u: +%llu and +%llu packets", kComputeNode,
              static_cast<unsigned long long>(On(direct, 0)), static_cast<unsigned long long>(On(direct, kComputeNode)));
        Info("node 0 during the compute window: +%llu packets, during the copy window: +%llu packets",
             static_cast<unsigned long long>(On(compute, 0)), static_cast<unsigned long long>(On(copy, 0)));
    } else {
        Check(On(compute, 0) > 0 && On(compute, kComputeNode) == 0,
              "the compute queue ran on node 0 and not on node %u: +%llu and +%llu packets", kComputeNode,
              static_cast<unsigned long long>(On(compute, 0)), static_cast<unsigned long long>(On(compute, kComputeNode)));
        Check(On(copy, 0) > 0 && On(copy, kComputeNode) == 0,
              "the copy queue ran on node 0 and not on node %u: +%llu and +%llu packets", kComputeNode,
              static_cast<unsigned long long>(On(copy, 0)), static_cast<unsigned long long>(On(copy, kComputeNode)));
        Check(On(direct, 0) > 0, "the direct queue ran on node 0: +%llu packets", static_cast<unsigned long long>(On(direct, 0)));
    }
}

/* ---- main ------------------------------------------------------------------------------------------------ */

[[noreturn]] void Usage(FILE* f, int code)
{
    fprintf(f,
            "usage: d3d12ace [--step N] [--expect-node 2|0|any] [--threads T] [--any-device] [--deadline S] [--fence-ms MS]\n"
            "  --step N          run step N only: 1 queues, 2 same, 3 chain, 4 overlap, 5 threads, 6 node.\n"
            "                    Default: steps 1 to 6\n"
            "  --expect-node X   where the compute and copy queues run: 2 (the compute node), 0 (the graphics node:\n"
            "                    node 2 closed, or RADV_BC250_ACE_QUEUES=0 in a lab-flavour ICD) or any (no check). Default: 2 on a BC-250, else any\n"
            "  --threads T       the threads of step 5, 2 to 8 (default 4)\n"
            "  --any-device      use the first hardware adapter when no BC-250 is present; exit 5 when there is none\n"
            "  --deadline S      end the process after S seconds, 5 to 170 (default 170)\n"
            "  --fence-ms MS     the bound of one GPU wait, 100 to 60000 (default 10000)\n");
    fflush(f);
    exit(code);
}

}  // namespace

int main(int argc, char** argv)
{
    int only = 0;
    const char* expect = nullptr;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) Usage(stdout, 0);
        const bool value = !strcmp(a, "--step") || !strcmp(a, "--expect-node") || !strcmp(a, "--threads") ||
                           !strcmp(a, "--deadline") || !strcmp(a, "--fence-ms");
        if (value && i + 1 >= argc) Usage(stderr, 2);
        if (!strcmp(a, "--step")) only = atoi(argv[++i]);
        else if (!strcmp(a, "--expect-node")) expect = argv[++i];
        else if (!strcmp(a, "--threads")) threadCount = static_cast<unsigned>(atoi(argv[++i]));
        else if (!strcmp(a, "--deadline")) deadlineSec = static_cast<unsigned>(atoi(argv[++i]));
        else if (!strcmp(a, "--fence-ms")) fenceMs = static_cast<unsigned>(atoi(argv[++i]));
        else if (!strcmp(a, "--any-device")) anyDevice = true;
        else Usage(stderr, 2);
    }
    if (expect && strcmp(expect, "2") && strcmp(expect, "0") && strcmp(expect, "any")) Usage(stderr, 2);
    if (only < 0 || only > 6 || threadCount < 2 || threadCount > 8 || deadlineSec < 5 || deadlineSec > 170 ||
        fenceMs < 100 || fenceMs > 60000) Usage(stderr, 2);

    InitializeCriticalSection(&outLock);
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);
    if (only) printf("d3d12ace step %d, deadline %u s, GPU wait bound %u ms\n", only, deadlineSec, fenceMs);
    else printf("d3d12ace steps 1-6, deadline %u s, GPU wait bound %u ms, %u threads\n", deadlineSec, fenceMs, threadCount);
    char ace[16] = "";
    printf("environment   RADV_BC250_ACE_QUEUES=%s\n",
           GetEnvironmentVariableA("RADV_BC250_ACE_QUEUES", ace, sizeof ace) ? ace : "(not set)");
    fflush(stdout);
    if (!(deadlineEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr))) Exit(3, "CreateEvent failed (%lu)", GetLastError());
    if (!CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)) Exit(3, "CreateThread failed (%lu)", GetLastError());

    // The Microsoft runtime of System32 by its full path, never an application-local d3d12.dll.
    wchar_t path[MAX_PATH] = {};
    if (!GetSystemDirectoryW(path, MAX_PATH) || wcscat_s(path, L"\\d3d12.dll")) Exit(3, "no system directory");
    HMODULE runtime = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) Exit(3, "cannot load %ls (error %lu)", path, GetLastError());
    PFN_D3D12_CREATE_DEVICE createDevice = nullptr;
    FARPROC proc = GetProcAddress(runtime, "D3D12CreateDevice");
    static_assert(sizeof createDevice == sizeof proc, "function pointer size");
    memcpy(&createDevice, &proc, sizeof createDevice);
    if (!createDevice) Exit(3, "d3d12.dll has no D3D12CreateDevice");

    ComPtr<IDXGIFactory4> factory;
    HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter, fallback;
    DXGI_ADAPTER_DESC1 desc{}, fallbackDesc{};
    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i, &candidate) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(candidate->GetDesc1(&d))) continue;
        if (d.VendorId == kBc250Vendor && d.DeviceId == kBc250Device) {
            adapter = candidate;
            desc = d;
            isBc250 = true;
            break;
        }
        if (!fallback && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            fallback = candidate;
            fallbackDesc = d;
        }
    }
    if (!adapter && anyDevice && fallback) {
        adapter = fallback;
        desc = fallbackDesc;
    }
    if (!adapter) Exit(anyDevice ? 5 : 3, "no BC-250 adapter%s", anyDevice ? " and no other hardware adapter" : " (--any-device takes another one)");
    luid = desc.AdapterLuid;
    expectNode = expect ? (!strcmp(expect, "any") ? kAnyNode : atoi(expect)) : (isBc250 ? static_cast<int>(kComputeNode) : kAnyNode);
    printf("adapter       %ls (vendor 0x%04X device 0x%04X), luid %08lX:%08lX, expect node %s\n", desc.Description,
           desc.VendorId, desc.DeviceId, static_cast<unsigned long>(luid.HighPart), luid.LowPart,
           expectNode == kAnyNode ? "any" : expectNode ? "2" : "0");
    fflush(stdout);

    {
        ComPtr<ID3D12Device> device;
        const HRESULT hr = createDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) FailHr("D3D12CreateDevice", hr);
        dev = device;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&gfxQ)));
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&cmpQ)));
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    HR(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&cpyQ)));
    PrintModules();

    processHandle = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, GetCurrentProcessId());
    if (!processHandle) Exit(3, "OpenProcess failed (%lu)", GetLastError());
    ReadNodes();
    printf("nodes         %u\n", nodeCount);
    fflush(stdout);
    const std::vector<NodeStat> start = Snapshot();

    HR(dev->CreateRootSignature(0, g_stage, sizeof g_stage, IID_PPV_ARGS(&rootSig)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig.Get();
    pd.CS = { g_stage, sizeof g_stage };
    HR(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso)));

    if (!only || only == 1) Step1();
    if (!only || only == 2) Step2();
    if (!only || only == 3) Step3();
    if (!only || only == 4) Step4();
    if (!only || only == 5) Step5();
    if (!only || only == 6) Step6();

    step = 0;
    Sleep(100);
    const std::vector<uint64_t> total = Delta(start, Snapshot(), "the whole run");
    if (expectNode == 0)
        Check(On(total, kComputeNode) == 0, "the whole run put no packet on node %u (+%llu)", kComputeNode,
              static_cast<unsigned long long>(On(total, kComputeNode)));
    Check(dev->GetDeviceRemovedReason() == S_OK, "the device is not removed (reason 0x%08lX)",
          static_cast<unsigned long>(dev->GetDeviceRemovedReason()));

    pso.Reset();
    rootSig.Reset();
    cpyQ.Reset();
    cmpQ.Reset();
    gfxQ.Reset();
    dev.Reset();
    CloseHandle(processHandle);
    SetEvent(deadlineEvent);
    printf("d3d12ace result=%s checks=%ld failures=%ld exit=%d\n", failures ? "fail" : "pass", checks, failures,
           failures ? 1 : 0);
    fflush(stdout);
    return failures ? 1 : 0;
}
