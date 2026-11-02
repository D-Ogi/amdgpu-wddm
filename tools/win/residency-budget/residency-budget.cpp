// BD-052 lab probe: does an application's Evict, and CREATE_NOT_RESIDENT, change the process's local video
// memory usage? One case per run, one flushed JSON line per step, exit 0 when the case's pass rule holds.
//   residency-budget.exe --case a|b|c [--log <file>] [--adapter-vendor <hex>]
// a: CreateHeap 512 MiB, use, Evict, MakeResident, content check.
// b: CreateHeap 512 MiB with CREATE_NOT_RESIDENT, EnqueueMakeResident, use, content check.
// c: 20 heaps of 64 MiB, each used at once after creation with no wait (the trial 153 shape), removal check.
// The GPU use is a copy from an upload buffer and back to a readback buffer, so no shader compiler is needed.
// Usage figures come from IDXGIAdapter3::QueryVideoMemoryInfo(0, LOCAL). A sample after a residency change is
// polled for up to 1000 ms, because the figure is not guaranteed to change in the same instant.
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {
constexpr uint64_t MiB = 1024ull * 1024ull;
constexpr uint64_t kPatternBytes = 64 * 1024;
FILE* g_log = nullptr;
LARGE_INTEGER g_t0, g_f;

double ms() {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_t0.QuadPart) * 1000.0 / double(g_f.QuadPart);
}
void line(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::fputs(buf, stdout); std::fputc('\n', stdout); std::fflush(stdout);
    if (g_log) { std::fputs(buf, g_log); std::fputc('\n', g_log); std::fflush(g_log); }
}
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

struct Ctx {
    IDXGIAdapter3* adapter = nullptr;
    ID3D12Device* device = nullptr;
    ID3D12Device3* device3 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    uint64_t fence_value = 0;
    ID3D12Resource* upload = nullptr;
    ID3D12Resource* readback = nullptr;
};

uint64_t usage(Ctx& c) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(c.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return UINT64_MAX;
    return info.CurrentUsage;
}
uint64_t budget(Ctx& c) {
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(c.adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) return 0;
    return info.Budget;
}
long long delta_mib(uint64_t now, uint64_t base) { return (long long)((int64_t)(now - base) / (int64_t)MiB); }
// Polls until pred(usage) or 1000 ms; returns the last sample and the time it took.
template <class P> uint64_t settle(Ctx& c, P pred, double* waited) {
    const double t = ms(); uint64_t u = usage(c);
    while (!pred(u) && ms() - t < 1000.0) { Sleep(10); u = usage(c); }
    *waited = ms() - t; return u;
}
void step(Ctx& c, const char* cs, const char* name, uint64_t base, uint64_t now, double waited, HRESULT hr) {
    line("{\"case\":\"%s\",\"step\":\"%s\",\"t_ms\":%.1f,\"usage\":%llu,\"budget\":%llu,\"delta_mib\":%lld,"
         "\"settle_ms\":%.1f,\"hr\":\"0x%08lX\",\"removed\":\"0x%08lX\"}",
         cs, name, ms(), (unsigned long long)now, (unsigned long long)budget(c), delta_mib(now, base), waited,
         (unsigned long)hr, (unsigned long)c.device->GetDeviceRemovedReason());
}
HRESULT wait_idle(Ctx& c) {
    const uint64_t v = ++c.fence_value;
    HRESULT hr = c.queue->Signal(c.fence, v); if (FAILED(hr)) return hr;
    if (c.fence->GetCompletedValue() < v) {
        hr = c.fence->SetEventOnCompletion(v, c.event); if (FAILED(hr)) return hr;
        if (WaitForSingleObject(c.event, 20000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    return c.device->GetDeviceRemovedReason();
}
ID3D12Resource* buffer(Ctx& c, D3D12_HEAP_TYPE type, uint64_t size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = type;
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    if (FAILED(c.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}
ID3D12Heap* heap(Ctx& c, uint64_t size, D3D12_HEAP_FLAGS flags, HRESULT* hr) {
    D3D12_HEAP_DESC d{}; d.SizeInBytes = size; d.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    d.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; d.Flags = flags | D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ID3D12Heap* h = nullptr; *hr = c.device->CreateHeap(&d, IID_PPV_ARGS(&h)); return h;
}
ID3D12Resource* placed(Ctx& c, ID3D12Heap* h, uint64_t size, HRESULT* hr) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1;
    d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    *hr = c.device->CreatePlacedResource(h, 0, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    return r;
}
uint32_t pattern_word(uint32_t seed, uint32_t i) { return 0x9E3779B9u * (i + 1) ^ seed; }
bool fill_upload(Ctx& c, uint32_t seed) {
    void* p = nullptr; D3D12_RANGE none{0, 0};
    if (FAILED(c.upload->Map(0, &none, &p))) return false;
    auto* w = static_cast<uint32_t*>(p);
    for (uint32_t i = 0; i < kPatternBytes / 4; ++i) w[i] = pattern_word(seed, i);
    c.upload->Unmap(0, nullptr); return true;
}
// Records upload -> target, or target -> readback. A buffer in COMMON promotes to COPY_DEST/COPY_SOURCE and
// decays back at the end of the list, so no barrier is needed for a single copy per list.
HRESULT copy(Ctx& c, ID3D12Resource* dst, ID3D12Resource* src, bool wait) {
    HRESULT hr = c.alloc->Reset(); if (FAILED(hr)) return hr;
    hr = c.list->Reset(c.alloc, nullptr); if (FAILED(hr)) return hr;
    c.list->CopyBufferRegion(dst, 0, src, 0, kPatternBytes);
    hr = c.list->Close(); if (FAILED(hr)) return hr;
    ID3D12CommandList* l = c.list; c.queue->ExecuteCommandLists(1, &l);
    return wait ? wait_idle(c) : S_OK;
}
bool check_readback(Ctx& c, uint32_t seed) {
    void* p = nullptr; D3D12_RANGE r{0, kPatternBytes};
    if (FAILED(c.readback->Map(0, &r, &p))) return false;
    const auto* w = static_cast<const uint32_t*>(p); bool ok = true;
    for (uint32_t i = 0; i < kPatternBytes / 4 && ok; ++i) ok = w[i] == pattern_word(seed, i);
    D3D12_RANGE none{0, 0}; c.readback->Unmap(0, &none); return ok;
}

int init(Ctx& c, unsigned vendor) {
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) return 10;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d);
        const bool soft = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (!soft && (!vendor || d.VendorId == vendor)) {
            char name[128]{}; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
            for (char* q = name; *q; ++q) if (*q == '"' || *q == '\\') *q = '_';
            line("{\"adapter\":\"%s\",\"vendor\":\"0x%04X\",\"device\":\"0x%04X\",\"dedicated_mib\":%llu}", name, d.VendorId,
                 d.DeviceId, (unsigned long long)(d.DedicatedVideoMemory / MiB));
            break;
        }
        a->Release(); a = nullptr;
    }
    f->Release();
    if (!a) return 11;
    a->QueryInterface(IID_PPV_ARGS(&c.adapter));
    HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&c.device));
    a->Release();
    if (FAILED(hr) || !c.adapter) { line("{\"error\":\"D3D12CreateDevice\",\"hr\":\"0x%08lX\"}", (unsigned long)hr); return 12; }
    c.device->QueryInterface(IID_PPV_ARGS(&c.device3));
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(c.device->CreateCommandQueue(&q, IID_PPV_ARGS(&c.queue)))) return 13;
    if (FAILED(c.device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&c.alloc)))) return 13;
    if (FAILED(c.device->CreateCommandList(0, q.Type, c.alloc, nullptr, IID_PPV_ARGS(&c.list)))) return 13;
    c.list->Close();
    if (FAILED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)))) return 13;
    c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    c.upload = buffer(c, D3D12_HEAP_TYPE_UPLOAD, kPatternBytes, D3D12_RESOURCE_STATE_GENERIC_READ);
    c.readback = buffer(c, D3D12_HEAP_TYPE_READBACK, kPatternBytes, D3D12_RESOURCE_STATE_COPY_DEST);
    return c.upload && c.readback && c.event ? 0 : 14;
}

// a: a resident heap, then Evict and MakeResident. Pass: the heap counts after creation and use, it stops
// counting after Evict, it counts again after MakeResident, and its content survives.
int case_a(Ctx& c) {
    double w = 0; HRESULT hr = S_OK;
    const uint64_t size = 512 * MiB, base = usage(c);
    step(c, "a", "base", base, base, 0, S_OK);
    ID3D12Heap* h = heap(c, size, D3D12_HEAP_FLAG_NONE, &hr);
    uint64_t u = settle(c, [&](uint64_t x) { return delta_mib(x, base) >= 500; }, &w);
    step(c, "a", "after_create", base, u, w, hr); if (!h) return 2;
    const long long created = delta_mib(u, base);
    ID3D12Resource* r = placed(c, h, size, &hr);
    if (r && fill_upload(c, 0xA5A5u)) hr = copy(c, r, c.upload, true);
    u = usage(c); step(c, "a", "after_use", base, u, 0, hr); if (!r || FAILED(hr)) return 3;
    const long long used = delta_mib(u, base);
    ID3D12Pageable* pg = h; hr = c.device->Evict(1, &pg);
    u = settle(c, [&](uint64_t x) { return delta_mib(x, base) <= 8; }, &w);
    step(c, "a", "after_evict", base, u, w, hr);
    const long long evicted = delta_mib(u, base);
    hr = c.device->MakeResident(1, &pg);
    u = settle(c, [&](uint64_t x) { return delta_mib(x, base) >= 500; }, &w);
    step(c, "a", "after_remake", base, u, w, hr);
    const long long remade = delta_mib(u, base);
    HRESULT back = copy(c, c.readback, r, true);
    const bool content = SUCCEEDED(back) && check_readback(c, 0xA5A5u);
    step(c, "a", content ? "content_ok" : "content_bad", base, usage(c), 0, back);
    release(r); release(h);
    const bool pass = created >= 500 && used >= 500 && evicted <= 8 && remade >= 500 && content &&
                      c.device->GetDeviceRemovedReason() == S_OK;
    line("{\"case\":\"a\",\"verdict\":\"%s\",\"created_mib\":%lld,\"used_mib\":%lld,\"evicted_mib\":%lld,"
         "\"remade_mib\":%lld,\"content\":%s}", pass ? "PASS" : "FAIL", created, used, evicted, remade,
         content ? "true" : "false");
    return pass ? 0 : 1;
}

// b: CREATE_NOT_RESIDENT. Pass: nothing counts at creation, the heap counts after EnqueueMakeResident, and a
// copy through it gives the pattern back.
int case_b(Ctx& c) {
    double w = 0; HRESULT hr = S_OK;
    const uint64_t size = 512 * MiB, base = usage(c);
    step(c, "b", "base", base, base, 0, S_OK);
    if (!c.device3) { line("{\"case\":\"b\",\"verdict\":\"FAIL\",\"error\":\"no ID3D12Device3\"}"); return 4; }
    ID3D12Heap* h = heap(c, size, D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT, &hr);
    Sleep(250);   // a late residency of ours would show here; nothing should change
    uint64_t u = usage(c); step(c, "b", "after_create", base, u, 250, hr); if (!h) return 2;
    const long long created = delta_mib(u, base);
    ID3D12Pageable* pg = h; const uint64_t v = ++c.fence_value;
    hr = c.device3->EnqueueMakeResident(D3D12_RESIDENCY_FLAG_NONE, 1, &pg, c.fence, v);
    if (SUCCEEDED(hr)) hr = c.queue->Wait(c.fence, v);
    if (SUCCEEDED(hr)) hr = wait_idle(c);
    u = settle(c, [&](uint64_t x) { return delta_mib(x, base) >= 500; }, &w);
    step(c, "b", "after_make", base, u, w, hr);
    const long long made = delta_mib(u, base);
    ID3D12Resource* r = placed(c, h, size, &hr);
    if (r && fill_upload(c, 0x5A5Au)) hr = copy(c, r, c.upload, true);
    HRESULT back = SUCCEEDED(hr) ? copy(c, c.readback, r, true) : hr;
    const bool content = r && SUCCEEDED(back) && check_readback(c, 0x5A5Au);
    u = usage(c); step(c, "b", content ? "after_use_content_ok" : "after_use_content_bad", base, u, 0, back);
    release(r); release(h);
    const bool pass = created <= 8 && made >= 500 && content && c.device->GetDeviceRemovedReason() == S_OK;
    line("{\"case\":\"b\",\"verdict\":\"%s\",\"created_mib\":%lld,\"made_mib\":%lld,\"content\":%s}",
         pass ? "PASS" : "FAIL", created, made, content ? "true" : "false");
    return pass ? 0 : 1;
}

// c: twenty fresh 64 MiB heaps, each written by a copy submitted at once after its creation, no wait in
// between; then all are read back. Pass: no removal at any iteration and every pattern comes back.
int case_c(Ctx& c) {
    const uint64_t size = 64 * MiB, base = usage(c);
    step(c, "c", "base", base, base, 0, S_OK);
    std::vector<ID3D12Heap*> heaps; std::vector<ID3D12Resource*> res; bool ok = true; HRESULT hr = S_OK;
    for (uint32_t i = 0; i < 20 && ok; ++i) {
        ID3D12Heap* h = heap(c, size, D3D12_HEAP_FLAG_NONE, &hr);
        ID3D12Resource* r = h ? placed(c, h, size, &hr) : nullptr;
        if (r && fill_upload(c, 0x1000u + i)) hr = copy(c, r, c.upload, true);
        const HRESULT removed = c.device->GetDeviceRemovedReason();
        line("{\"case\":\"c\",\"iteration\":%u,\"hr\":\"0x%08lX\",\"removed\":\"0x%08lX\",\"delta_mib\":%lld}", i,
             (unsigned long)hr, (unsigned long)removed, delta_mib(usage(c), base));
        if (h) heaps.push_back(h);
        if (r) res.push_back(r);
        ok = r && SUCCEEDED(hr) && removed == S_OK;
    }
    uint32_t good = 0;
    for (uint32_t i = 0; ok && i < res.size(); ++i) {
        const HRESULT back = copy(c, c.readback, res[i], true);
        if (SUCCEEDED(back) && check_readback(c, 0x1000u + i)) ++good; else ok = false;
    }
    for (auto* r : res) r->Release();
    for (auto* h : heaps) h->Release();
    const bool pass = ok && good == 20 && c.device->GetDeviceRemovedReason() == S_OK;
    line("{\"case\":\"c\",\"verdict\":\"%s\",\"iterations\":%zu,\"content_ok\":%u,\"removed\":\"0x%08lX\"}",
         pass ? "PASS" : "FAIL", res.size(), good, (unsigned long)c.device->GetDeviceRemovedReason());
    return pass ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&g_f); QueryPerformanceCounter(&g_t0);
    const char* which = nullptr; const char* log = nullptr; unsigned vendor = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--case")) which = argv[i + 1];
        else if (!std::strcmp(argv[i], "--log")) log = argv[i + 1];
        else if (!std::strcmp(argv[i], "--adapter-vendor")) vendor = (unsigned)std::strtoul(argv[i + 1], nullptr, 16);
    }
    if (!which || (std::strcmp(which, "a") && std::strcmp(which, "b") && std::strcmp(which, "c"))) {
        std::fputs("usage: residency-budget --case a|b|c [--log file] [--adapter-vendor hex]\n", stderr); return 64;
    }
    if (log) g_log = std::fopen(log, "w");
    char image[MAX_PATH]{}; GetModuleFileNameA(nullptr, image, MAX_PATH);
    for (char* q = image; *q; ++q) if (*q == '\\' || *q == '"') *q = '/';
    line("{\"probe\":\"residency-budget\",\"image\":\"%s\",\"case\":\"%s\"}", image, which);
    Ctx c; int rc = init(c, vendor);
    if (!rc) rc = !std::strcmp(which, "a") ? case_a(c) : !std::strcmp(which, "b") ? case_b(c) : case_c(c);
    line("{\"exit\":%d}", rc);
    if (g_log) std::fclose(g_log);
    return rc;
}
