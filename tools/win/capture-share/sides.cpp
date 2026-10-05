// SPDX-License-Identifier: MIT
// sides.cpp - one API device per process (D3D11 or D3D12) with the operations the shared-resource, keyed-mutex and
// fence cells need: create or open a shared texture, write a known image (optionally behind a delay workload of large
// texture copies, so that a missing GPU wait shows as stale content), read it back, keyed mutex, shared fences, GPU
// waits and signals, and the local wait controls. Every failing call is reported with its name and HRESULT; device
// removal is checked after every submission.
#include "capshare.h"
#include <d3d11on12.h>
#include <d3d12compatibility.h>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")

// The parent stops its own operations before its watchdog; the peer before the deadline it was given.
static ULONGLONG OpDeadline()
{
    if (g_opt.peer) return g_opt.deadline > 300 ? g_opt.deadline - 300 : g_opt.deadline;
    return WaitDeadline();
}

bool Side::Err(const char *call, HRESULT hr)
{
    lastCall = call;
    lastHr = hr;
    lastRemoved = RemovedReason();
    Log("CALL-FAIL api=%s call=%s hr=%s removed=%s", api == Api::D3D11 ? "11" : "12", call, HrText(hr).c_str(),
        HrText(lastRemoved).c_str());
    FlushOds();
    return false;
}

static const HRESULT TimeoutHr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);

// ================================================================================================ D3D11
class Side11 final : public Side {
public:
    Side11() { api = Api::D3D11; }
    ~Side11() override
    {
        if (event) CloseHandle(event);
    }
    bool Init(IDXGIAdapter1 *adapter) override;
    bool CreateShared(bool keyed, HANDLE *out) override;
    bool OpenShared(HANDLE h, bool keyed) override;
    bool WritePattern(Pattern p, bool delay) override;
    bool Delay() override;
    bool ReadSubmit() override;
    bool ReadCollect(Image &img) override;
    bool Finish() override;
    bool Acquire(UINT64 key) override;
    bool Release(UINT64 key) override;
    bool CreateFence(int i, HANDLE *out) override;
    bool OpenFence(int i, HANDLE h) override;
    bool GpuWait(int i, UINT64 value) override;
    bool GpuSignal(int i, UINT64 value) override;
    UINT64 Completed(int i) override { return fence[i] ? fence[i]->GetCompletedValue() : 0; }
    bool CpuWait(int i, UINT64 value, ULONGLONG deadlineTick) override;
    bool Mark() override
    {
        ctx->End(markQuery.Get());
        return Flush("Flush(mark)");
    }
    bool WaitMark(ULONGLONG deadlineTick) override;
    bool LocalWaits(std::string &result) override;

private:
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11Device1> dev1;
    ComPtr<ID3D11Device5> dev5;
    ComPtr<ID3D11DeviceContext4> ctx4;
    ComPtr<ID3D11Texture2D> shared, src[3], staging, delayA, delayB;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11Fence> fence[2];
    ComPtr<ID3D11Query> query, markQuery;
    HANDLE event = nullptr;

    D3D11_TEXTURE2D_DESC Desc(UINT bind, UINT misc, D3D11_USAGE usage, UINT cpu) const
    {
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = g_opt.w;
        d.Height = g_opt.h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = g_opt.format;
        d.SampleDesc.Count = 1;
        d.Usage = usage;
        d.BindFlags = bind;
        d.CPUAccessFlags = cpu;
        d.MiscFlags = misc;
        return d;
    }
    bool Flush(const char *call)
    {
        ctx->Flush();
        return Removed(call);
    }
    bool Removed(const char *call)
    {
        const HRESULT r = dev->GetDeviceRemovedReason();
        return r == S_OK ? true : Err(call, r);
    }
    HRESULT RemovedReason() override { return dev ? dev->GetDeviceRemovedReason() : S_OK; }
};

static void LogTexture11(const char *what, ID3D11Texture2D *t)
{
    D3D11_TEXTURE2D_DESC d = {};
    t->GetDesc(&d);
    Log("TEXTURE11 %s %ux%u format=%s mips=%u array=%u samples=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x", what, d.Width,
        d.Height, FormatText(d.Format).c_str(), d.MipLevels, d.ArraySize, d.SampleDesc.Count, (unsigned)d.Usage,
        d.BindFlags, d.CPUAccessFlags, d.MiscFlags);
}

bool Side11::Init(IDXGIAdapter1 *adapter)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                               D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &dev, &level, &ctx);
    if (FAILED(hr)) {
        Log("D3D11CreateDevice with BGRA support failed hr=%s, retrying without", HrText(hr).c_str());
        hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                               &dev, &level, &ctx);
    }
    if (FAILED(hr)) return Err("D3D11CreateDevice", hr);
    fl = FlText(level);
    dev.As(&dev1);
    dev.As(&dev5);
    ctx.As(&ctx4);
    D3D11_FEATURE_DATA_D3D11_OPTIONS o = {};
    const HRESULT ho = dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o));
    Log("DEVICE api=11 fl=%s device1=%u device5=%u context4=%u extended_resource_sharing=%s", fl.c_str(), dev1 ? 1u : 0u,
        dev5 ? 1u : 0u, ctx4 ? 1u : 0u, SUCCEEDED(ho) ? (o.ExtendedResourceSharing ? "1" : "0") : HrText(ho).c_str());
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    for (int i = 0; i < 3; ++i) {
        const std::vector<uint32_t> mem = MakeImageMemory((Pattern)i, g_opt.w, g_opt.h, g_opt.format, false);
        D3D11_SUBRESOURCE_DATA sd = {mem.data(), g_opt.w * 4, 0};
        const D3D11_TEXTURE2D_DESC d = Desc(D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DEFAULT, 0);
        hr = dev->CreateTexture2D(&d, &sd, &src[i]);
        if (FAILED(hr)) return Err("ID3D11Device::CreateTexture2D(source)", hr);
    }
    const D3D11_TEXTURE2D_DESC sd = Desc(0, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    hr = dev->CreateTexture2D(&sd, nullptr, &staging);
    if (FAILED(hr)) return Err("ID3D11Device::CreateTexture2D(staging)", hr);
    if (g_opt.delayCopies) {
        D3D11_TEXTURE2D_DESC d = Desc(D3D11_BIND_SHADER_RESOURCE, 0, D3D11_USAGE_DEFAULT, 0);
        d.Width = d.Height = g_opt.delaySize;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = dev->CreateTexture2D(&d, nullptr, &delayA);
        if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&d, nullptr, &delayB);
        if (FAILED(hr)) return Err("ID3D11Device::CreateTexture2D(delay)", hr);
    }
    const D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
    hr = dev->CreateQuery(&qd, &query);
    if (SUCCEEDED(hr)) hr = dev->CreateQuery(&qd, &markQuery);
    if (FAILED(hr)) return Err("ID3D11Device::CreateQuery(event)", hr);
    return true;
}

bool Side11::WaitMark(ULONGLONG deadlineTick)
{
    for (;;) {
        const HRESULT hr = ctx->GetData(markQuery.Get(), nullptr, 0, 0);
        if (hr == S_OK) return Removed("WaitMark(removed)");
        if (FAILED(hr)) return Err("ID3D11DeviceContext::GetData(mark)", hr);
        if (!Remaining(deadlineTick)) {
            const HRESULT r = dev->GetDeviceRemovedReason();
            return Err("WaitMark(gpu-never-passed-the-wait)", r != S_OK ? r : TimeoutHr);
        }
        Sleep(1);
    }
}

bool Side11::CreateShared(bool keyed, HANDLE *out)
{
    UINT misc = keyed ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
    if (!g_opt.kmt) misc |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    const D3D11_TEXTURE2D_DESC d = Desc(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, misc, D3D11_USAGE_DEFAULT, 0);
    const std::vector<uint32_t> mem = MakeImageMemory(Pattern::Poison, g_opt.w, g_opt.h, g_opt.format, false);
    D3D11_SUBRESOURCE_DATA sd = {mem.data(), g_opt.w * 4, 0};
    HRESULT hr = dev->CreateTexture2D(&d, &sd, &shared);
    if (FAILED(hr)) return Err("ID3D11Device::CreateTexture2D(shared)", hr);
    LogTexture11("created", shared.Get());
    if (keyed) {
        hr = shared.As(&mutex);
        if (FAILED(hr)) return Err("QueryInterface(IDXGIKeyedMutex)", hr);
    }
    if (g_opt.kmt) {
        ComPtr<IDXGIResource> r;
        hr = shared.As(&r);
        if (SUCCEEDED(hr)) hr = r->GetSharedHandle(out);
        if (FAILED(hr)) return Err("IDXGIResource::GetSharedHandle", hr);
    } else {
        ComPtr<IDXGIResource1> r;
        hr = shared.As(&r);
        if (FAILED(hr)) return Err("QueryInterface(IDXGIResource1)", hr);
        hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, out);
        if (FAILED(hr)) return Err("IDXGIResource1::CreateSharedHandle", hr);
    }
    return true;
}

bool Side11::OpenShared(HANDLE h, bool keyed)
{
    HRESULT hr;
    if (g_opt.kmt) {
        hr = dev->OpenSharedResource(h, IID_PPV_ARGS(&shared));
        if (FAILED(hr)) return Err("ID3D11Device::OpenSharedResource", hr);
    } else {
        if (!dev1) return Err("QueryInterface(ID3D11Device1)", E_NOINTERFACE);
        hr = dev1->OpenSharedResource1(h, IID_PPV_ARGS(&shared));
        if (FAILED(hr)) return Err("ID3D11Device1::OpenSharedResource1", hr);
    }
    LogTexture11("opened", shared.Get());
    D3D11_TEXTURE2D_DESC d = {};
    shared->GetDesc(&d);
    if (d.Width != g_opt.w || d.Height != g_opt.h || d.Format != g_opt.format)
        Log("OPENED description differs from the creator's %ux%u %s", g_opt.w, g_opt.h, FormatText(g_opt.format).c_str());
    if (keyed) {
        hr = shared.As(&mutex);
        if (FAILED(hr)) return Err("QueryInterface(IDXGIKeyedMutex)", hr);
    }
    return Removed("OpenSharedResource1(removed)");
}

bool Side11::Delay()
{
    if (!delayA) return true;
    for (unsigned i = 0; i < g_opt.delayCopies; ++i) ctx->CopyResource(delayB.Get(), delayA.Get());
    return Flush("Flush(delay)");
}

bool Side11::WritePattern(Pattern p, bool delay)
{
    if (delay && delayA)
        for (unsigned i = 0; i < g_opt.delayCopies; ++i) ctx->CopyResource(delayB.Get(), delayA.Get());
    ctx->CopyResource(shared.Get(), src[(int)p].Get());
    return Flush("Flush(write)");
}

bool Side11::ReadSubmit()
{
    ctx->CopyResource(staging.Get(), shared.Get());
    return Flush("Flush(read)");
}

bool Side11::ReadCollect(Image &img)
{
    D3D11_MAPPED_SUBRESOURCE m = {};
    const HRESULT hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr)) return Err("ID3D11DeviceContext::Map(staging)", hr);
    const bool ok = ImageFromRows(img, m.pData, m.RowPitch, (int)g_opt.w, (int)g_opt.h, g_opt.format);
    ctx->Unmap(staging.Get(), 0);
    if (!ok) return Err("ImageFromRows(format)", E_INVALIDARG);
    return Removed("Map(removed)");
}

bool Side11::Finish()
{
    ctx->End(query.Get());
    ctx->Flush();
    const ULONGLONG deadline = OpDeadline();
    for (;;) {
        const HRESULT hr = ctx->GetData(query.Get(), nullptr, 0, 0);
        if (hr == S_OK) break;
        if (FAILED(hr)) return Err("ID3D11DeviceContext::GetData(event)", hr);
        if (!Remaining(deadline)) {
            const HRESULT r = dev->GetDeviceRemovedReason();
            return Err("Finish(event-query)", r != S_OK ? r : TimeoutHr);
        }
        Sleep(1);
    }
    return Removed("Finish(removed)");
}

bool Side11::Acquire(UINT64 key)
{
    if (!mutex) return Err("IDXGIKeyedMutex(missing)", E_NOINTERFACE);
    const HRESULT hr = mutex->AcquireSync(key, Remaining(OpDeadline()));
    // WAIT_TIMEOUT (0x102) and WAIT_ABANDONED (0x80) are success codes: only S_OK means the key was acquired.
    if (hr != S_OK) return Err("IDXGIKeyedMutex::AcquireSync", hr);
    return Removed("AcquireSync(removed)");
}

bool Side11::Release(UINT64 key)
{
    const HRESULT hr = mutex->ReleaseSync(key);
    if (hr != S_OK) return Err("IDXGIKeyedMutex::ReleaseSync", hr);
    return Removed("ReleaseSync(removed)");
}

bool Side11::CreateFence(int i, HANDLE *out)
{
    if (!dev5) return Err("QueryInterface(ID3D11Device5)", E_NOINTERFACE);
    HRESULT hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence[i]));
    if (FAILED(hr)) return Err("ID3D11Device5::CreateFence(shared)", hr);
    hr = fence[i]->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, out);
    if (FAILED(hr)) return Err("ID3D11Fence::CreateSharedHandle", hr);
    return true;
}

bool Side11::OpenFence(int i, HANDLE h)
{
    if (!dev5) return Err("QueryInterface(ID3D11Device5)", E_NOINTERFACE);
    const HRESULT hr = dev5->OpenSharedFence(h, IID_PPV_ARGS(&fence[i]));
    if (FAILED(hr)) return Err("ID3D11Device5::OpenSharedFence", hr);
    return Removed("OpenSharedFence(removed)");
}

bool Side11::GpuWait(int i, UINT64 value)
{
    if (!ctx4) return Err("QueryInterface(ID3D11DeviceContext4)", E_NOINTERFACE);
    const HRESULT hr = ctx4->Wait(fence[i].Get(), value);
    if (FAILED(hr)) return Err("ID3D11DeviceContext4::Wait", hr);
    return Flush("Flush(wait)");
}

bool Side11::GpuSignal(int i, UINT64 value)
{
    if (!ctx4) return Err("QueryInterface(ID3D11DeviceContext4)", E_NOINTERFACE);
    const HRESULT hr = ctx4->Signal(fence[i].Get(), value);
    if (FAILED(hr)) return Err("ID3D11DeviceContext4::Signal", hr);
    return Flush("Flush(signal)");
}

bool Side11::CpuWait(int i, UINT64 value, ULONGLONG deadlineTick)
{
    if (fence[i]->GetCompletedValue() >= value) return Removed("CpuWait(removed)");
    const HRESULT hr = fence[i]->SetEventOnCompletion(value, event);
    if (FAILED(hr)) return Err("ID3D11Fence::SetEventOnCompletion", hr);
    if (WaitForSingleObject(event, Remaining(deadlineTick)) != WAIT_OBJECT_0) {
        const HRESULT r = dev->GetDeviceRemovedReason();
        return Err("CpuWait(fence-never-reached)", r != S_OK ? r : TimeoutHr);
    }
    return Removed("CpuWait(removed)");
}

bool Side11::LocalWaits(std::string &result)
{
    if (!dev5 || !ctx4) return Err("QueryInterface(ID3D11Device5/ID3D11DeviceContext4)", E_NOINTERFACE);
    ComPtr<ID3D11Fence> ctl;
    SetStage("w11-create");
    HRESULT hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctl));
    if (FAILED(hr)) return Err("ID3D11Device5::CreateFence(local)", hr);
    SetStage("w11-settled");
    hr = ctx4->Signal(ctl.Get(), 1);
    if (FAILED(hr)) return Err("ID3D11DeviceContext4::Signal(local)", hr);
    hr = ctx4->Wait(ctl.Get(), 1);
    if (FAILED(hr)) return Err("ID3D11DeviceContext4::Wait(settled)", hr);
    if (!Finish()) return false;
    if (ctl->GetCompletedValue() < 1) {
        hr = ctl->SetEventOnCompletion(1, event);
        if (FAILED(hr)) return Err("ID3D11Fence::SetEventOnCompletion(local)", hr);
        if (WaitForSingleObject(event, std::min<DWORD>(Remaining(OpDeadline()), 5000)) != WAIT_OBJECT_0)
            return Err("local-fence-never-reached-1", TimeoutHr);
    }
    result = "settled=ok completed=" + std::to_string(ctl->GetCompletedValue());
    return Removed("LocalWaits(removed)");
}

// ================================================================================================ D3D12
class Side12 final : public Side {
public:
    Side12() { api = Api::D3D12; }
    ~Side12() override
    {
        if (event) CloseHandle(event);
        if (event2) CloseHandle(event2);
        if (event3) CloseHandle(event3);
    }
    bool Init(IDXGIAdapter1 *adapter) override;
    bool CreateShared(bool keyed, HANDLE *out) override;
    bool OpenShared(HANDLE h, bool keyed) override;
    bool WritePattern(Pattern p, bool delay) override;
    bool Delay() override;
    bool ReadSubmit() override;
    bool ReadCollect(Image &img) override;
    bool Finish() override;
    bool Acquire(UINT64 key) override;
    bool Release(UINT64 key) override;
    bool CreateFence(int i, HANDLE *out) override;
    bool OpenFence(int i, HANDLE h) override;
    bool GpuWait(int i, UINT64 value) override;
    bool GpuSignal(int i, UINT64 value) override;
    UINT64 Completed(int i) override { return fence[i] ? fence[i]->GetCompletedValue() : 0; }
    bool CpuWait(int i, UINT64 value, ULONGLONG deadlineTick) override;
    bool Mark() override
    {
        const HRESULT hr = queue->Signal(mark.Get(), ++markValue);
        if (FAILED(hr)) return Err("ID3D12CommandQueue::Signal(mark)", hr);
        return Removed("Signal(mark,removed)");
    }
    bool WaitMark(ULONGLONG deadlineTick) override;
    bool LocalWaits(std::string &result) override;

private:
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> local, mark, fence[2];
    UINT64 localValue = 0, markValue = 0;
    HANDLE event = nullptr, event2 = nullptr, event3 = nullptr;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists;
    ComPtr<ID3D12Resource> shared, upload[3], readback, delayA, delayB;
    D3D12_RESOURCE_DESC desc = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 footprintBytes = 0;
    // keyed mutex through D3D11On12 (D3D12 itself has no public keyed-mutex call)
    ComPtr<ID3D11Device> on12Device;
    ComPtr<ID3D11DeviceContext> on12Context;
    ComPtr<ID3D11On12Device> on12;
    ComPtr<ID3D11Texture2D> wrapped;
    ComPtr<IDXGIKeyedMutex> mutex;

    bool Removed(const char *call)
    {
        const HRESULT r = dev->GetDeviceRemovedReason();
        return r == S_OK ? true : Err(call, r);
    }
    HRESULT RemovedReason() override { return dev ? dev->GetDeviceRemovedReason() : S_OK; }
    bool Buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &out,
                const char *call);
    bool Begin(ID3D12GraphicsCommandList **out);
    bool Submit(ID3D12GraphicsCommandList *l, ID3D12CommandQueue *q, const char *call);
    void RecordDelay(ID3D12GraphicsCommandList *l);
    bool WaitLocal(UINT64 value, DWORD capMs, const char *call);
    bool EnsureMutex();
};

static void Barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

static void LogResource12(const char *what, ID3D12Resource *r)
{
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
    const HRESULT hr = r->GetHeapProperties(&hp, &hf);
    Log("RESOURCE12 %s dim=%d %llux%u format=%s mips=%u layout=%d flags=0x%x alignment=%llu heap=%s type=%d "
        "heap_flags=0x%x",
        what, (int)d.Dimension, (unsigned long long)d.Width, d.Height, FormatText(d.Format).c_str(), d.MipLevels,
        (int)d.Layout, (unsigned)d.Flags, (unsigned long long)d.Alignment, SUCCEEDED(hr) ? "ok" : HrText(hr).c_str(),
        (int)hp.Type, (unsigned)hf);
}

bool Side12::Buffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &out,
                    const char *call)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = type;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out));
    return SUCCEEDED(hr) ? true : Err(call, hr);
}

bool Side12::Init(IDXGIAdapter1 *adapter)
{
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) return Err("D3D12CreateDevice", hr);
    static const D3D_FEATURE_LEVEL req[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fls = {ARRAYSIZE(req), req, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(dev->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fls, sizeof(fls))))
        fls.MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    fl = FlText(fls.MaxSupportedFeatureLevel);
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
    const HRESULT h4 = dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4));
    Log("DEVICE api=12 max_fl=%s shared_resource_compatibility_tier=%s", fl.c_str(),
        SUCCEEDED(h4) ? std::to_string((int)o4.SharedResourceCompatibilityTier).c_str() : HrText(h4).c_str());
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) return Err("ID3D12Device::CreateCommandQueue", hr);
    hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&local));
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mark));
    if (FAILED(hr)) return Err("ID3D12Device::CreateFence(local)", hr);
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    event2 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    event3 = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = g_opt.w;
    desc.Height = g_opt.h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = g_opt.format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (g_opt.simultaneous) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &footprintBytes);
    for (int i = 0; i < 3; ++i) {
        if (!Buffer(D3D12_HEAP_TYPE_UPLOAD, footprintBytes, D3D12_RESOURCE_STATE_GENERIC_READ, upload[i],
                    "ID3D12Device::CreateCommittedResource(upload)"))
            return false;
        void *p = nullptr;
        const D3D12_RANGE none = {0, 0};
        hr = upload[i]->Map(0, &none, &p);
        if (FAILED(hr)) return Err("ID3D12Resource::Map(upload)", hr);
        const std::vector<uint32_t> mem = MakeImageMemory((Pattern)i, g_opt.w, g_opt.h, g_opt.format, false);
        for (UINT y = 0; y < g_opt.h; ++y)
            memcpy((unsigned char *)p + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch,
                   mem.data() + (size_t)y * g_opt.w, (size_t)g_opt.w * 4);
        upload[i]->Unmap(0, nullptr);
    }
    if (!Buffer(D3D12_HEAP_TYPE_READBACK, footprintBytes, D3D12_RESOURCE_STATE_COPY_DEST, readback,
                "ID3D12Device::CreateCommittedResource(readback)"))
        return false;
    if (g_opt.delayCopies) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d = desc;
        d.Width = d.Height = g_opt.delaySize;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&delayA));
        if (SUCCEEDED(hr))
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                              IID_PPV_ARGS(&delayB));
        if (FAILED(hr)) return Err("ID3D12Device::CreateCommittedResource(delay)", hr);
    }
    return true;
}

// One allocator and list per submission: nothing ever waits for an allocator, so a queue blocked on a shared-fence
// wait never stalls the next recording.
bool Side12::Begin(ID3D12GraphicsCommandList **out)
{
    ComPtr<ID3D12CommandAllocator> a;
    HRESULT hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    if (FAILED(hr)) return Err("ID3D12Device::CreateCommandAllocator", hr);
    ComPtr<ID3D12GraphicsCommandList> l;
    hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a.Get(), nullptr, IID_PPV_ARGS(&l));
    if (FAILED(hr)) return Err("ID3D12Device::CreateCommandList", hr);
    allocators.push_back(a);
    lists.push_back(l);
    *out = l.Get();
    return true;
}

bool Side12::Submit(ID3D12GraphicsCommandList *l, ID3D12CommandQueue *q, const char *call)
{
    const HRESULT hr = l->Close();
    if (FAILED(hr)) return Err("ID3D12GraphicsCommandList::Close", hr);
    ID3D12CommandList *ls[] = {l};
    q->ExecuteCommandLists(1, ls);
    return Removed(call);
}

void Side12::RecordDelay(ID3D12GraphicsCommandList *l)
{
    if (!delayA) return;
    Barrier(l, delayA.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(l, delayB.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    for (unsigned i = 0; i < g_opt.delayCopies; ++i) l->CopyResource(delayB.Get(), delayA.Get());
    Barrier(l, delayA.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Barrier(l, delayB.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
}

bool Side12::CreateShared(bool keyed, HANDLE *out)
{
    if (g_opt.kmt) return Err("legacy-handles-are-D3D11-only", E_INVALIDARG);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr;
    if (keyed) {
        ComPtr<ID3D12CompatibilityDevice> compat;
        hr = dev.As(&compat);
        if (FAILED(hr)) return Err("QueryInterface(ID3D12CompatibilityDevice)", hr);
        D3D11_RESOURCE_FLAGS f11 = {D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
                                    D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, 0, 0};
        hr = compat->CreateSharedResource(&hp, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &f11,
                                          D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX, nullptr, nullptr,
                                          IID_PPV_ARGS(&shared));
        if (FAILED(hr)) return Err("ID3D12CompatibilityDevice::CreateSharedResource(keyed)", hr);
    } else {
        hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                          IID_PPV_ARGS(&shared));
        if (FAILED(hr)) return Err("ID3D12Device::CreateCommittedResource(shared)", hr);
    }
    LogResource12("created", shared.Get());
    hr = dev->CreateSharedHandle(shared.Get(), nullptr, GENERIC_ALL, nullptr, out);
    if (FAILED(hr)) return Err("ID3D12Device::CreateSharedHandle(resource)", hr);
    return Removed("CreateSharedHandle(removed)");
}

bool Side12::OpenShared(HANDLE h, bool keyed)
{
    (void)keyed; // the keyed mutex is reached through D3D11On12 at the first Acquire
    if (g_opt.kmt) return Err("legacy-handles-are-D3D11-only", E_INVALIDARG);
    const HRESULT hr = dev->OpenSharedHandle(h, IID_PPV_ARGS(&shared));
    if (FAILED(hr)) return Err("ID3D12Device::OpenSharedHandle(resource)", hr);
    LogResource12("opened", shared.Get());
    const D3D12_RESOURCE_DESC d = shared->GetDesc();
    if (d.Width != g_opt.w || d.Height != g_opt.h || d.Format != g_opt.format)
        return Err("opened-description-differs", E_UNEXPECTED);
    return Removed("OpenSharedHandle(removed)");
}

bool Side12::WritePattern(Pattern p, bool delay)
{
    ID3D12GraphicsCommandList *l = nullptr;
    if (!Begin(&l)) return false;
    if (delay) RecordDelay(l);
    Barrier(l, shared.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = shared.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload[(int)p].Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(l, shared.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    return Submit(l, queue.Get(), "ExecuteCommandLists(write)");
}

bool Side12::Delay()
{
    if (!delayA) return true;
    ID3D12GraphicsCommandList *l = nullptr;
    if (!Begin(&l)) return false;
    RecordDelay(l);
    return Submit(l, queue.Get(), "ExecuteCommandLists(delay)");
}

bool Side12::ReadSubmit()
{
    ID3D12GraphicsCommandList *l = nullptr;
    if (!Begin(&l)) return false;
    Barrier(l, shared.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = shared.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    l->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(l, shared.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    return Submit(l, queue.Get(), "ExecuteCommandLists(read)");
}

bool Side12::ReadCollect(Image &img)
{
    if (!Finish()) return false;
    void *p = nullptr;
    const D3D12_RANGE range = {0, (SIZE_T)footprintBytes};
    const HRESULT hr = readback->Map(0, &range, &p);
    if (FAILED(hr)) return Err("ID3D12Resource::Map(readback)", hr);
    const bool ok = ImageFromRows(img, (const unsigned char *)p + footprint.Offset, footprint.Footprint.RowPitch,
                                  (int)g_opt.w, (int)g_opt.h, g_opt.format);
    const D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    return ok ? true : Err("ImageFromRows(format)", E_INVALIDARG);
}

bool Side12::WaitLocal(UINT64 value, DWORD capMs, const char *call)
{
    if (local->GetCompletedValue() >= value) return true;
    const HRESULT hr = local->SetEventOnCompletion(value, event);
    if (FAILED(hr)) return Err("ID3D12Fence::SetEventOnCompletion(local)", hr);
    if (WaitForSingleObject(event, std::min<DWORD>(Remaining(OpDeadline()), capMs)) != WAIT_OBJECT_0) {
        const HRESULT r = dev->GetDeviceRemovedReason();
        return Err(call, r != S_OK ? r : TimeoutHr);
    }
    return true;
}

bool Side12::Finish()
{
    ++localValue;
    const HRESULT hr = queue->Signal(local.Get(), localValue);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Signal(local)", hr);
    if (!WaitLocal(localValue, INFINITE, "Finish(local-fence)")) return false;
    return Removed("Finish(removed)");
}

bool Side12::EnsureMutex()
{
    if (mutex) return true;
    IUnknown *queues[] = {queue.Get()};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11On12CreateDevice(dev.Get(), D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, queues, 1, 0, &on12Device,
                                       &on12Context, &level);
    if (FAILED(hr)) return Err("D3D11On12CreateDevice", hr);
    hr = on12Device.As(&on12);
    if (FAILED(hr)) return Err("QueryInterface(ID3D11On12Device)", hr);
    Log("D3D11On12 device fl=%s", FlText(level));
    const D3D11_RESOURCE_FLAGS f = {0, D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, 0, 0};
    hr = on12->CreateWrappedResource(shared.Get(), &f, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                     IID_PPV_ARGS(&wrapped));
    if (FAILED(hr)) return Err("ID3D11On12Device::CreateWrappedResource(keyed)", hr);
    hr = wrapped.As(&mutex);
    if (FAILED(hr)) return Err("QueryInterface(IDXGIKeyedMutex,wrapped)", hr);
    return Removed("D3D11On12(removed)");
}

bool Side12::Acquire(UINT64 key)
{
    if (!EnsureMutex()) return false;
    const HRESULT hr = mutex->AcquireSync(key, Remaining(OpDeadline()));
    if (hr != S_OK) return Err("IDXGIKeyedMutex::AcquireSync(11on12)", hr);
    on12Context->Flush(); // the acquire's queue wait goes ahead of the D3D12 work that follows
    return Removed("AcquireSync(removed)");
}

bool Side12::Release(UINT64 key)
{
    const HRESULT hr = mutex->ReleaseSync(key);
    if (hr != S_OK) return Err("IDXGIKeyedMutex::ReleaseSync(11on12)", hr);
    on12Context->Flush();
    return Removed("ReleaseSync(removed)");
}

bool Side12::CreateFence(int i, HANDLE *out)
{
    HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence[i]));
    if (FAILED(hr)) return Err("ID3D12Device::CreateFence(shared)", hr);
    hr = dev->CreateSharedHandle(fence[i].Get(), nullptr, GENERIC_ALL, nullptr, out);
    if (FAILED(hr)) return Err("ID3D12Device::CreateSharedHandle(fence)", hr);
    return true;
}

bool Side12::OpenFence(int i, HANDLE h)
{
    const HRESULT hr = dev->OpenSharedHandle(h, IID_PPV_ARGS(&fence[i]));
    if (FAILED(hr)) return Err("ID3D12Device::OpenSharedHandle(fence)", hr);
    return Removed("OpenSharedHandle(fence,removed)");
}

bool Side12::GpuWait(int i, UINT64 value)
{
    const HRESULT hr = queue->Wait(fence[i].Get(), value);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Wait(shared)", hr);
    return Removed("ID3D12CommandQueue::Wait(removed)");
}

bool Side12::GpuSignal(int i, UINT64 value)
{
    const HRESULT hr = queue->Signal(fence[i].Get(), value);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Signal(shared)", hr);
    return Removed("ID3D12CommandQueue::Signal(removed)");
}

bool Side12::CpuWait(int i, UINT64 value, ULONGLONG deadlineTick)
{
    if (fence[i]->GetCompletedValue() >= value) return Removed("CpuWait(removed)");
    const HRESULT hr = fence[i]->SetEventOnCompletion(value, event2);
    if (FAILED(hr)) return Err("ID3D12Fence::SetEventOnCompletion", hr);
    if (WaitForSingleObject(event2, Remaining(deadlineTick)) != WAIT_OBJECT_0) {
        const HRESULT r = dev->GetDeviceRemovedReason();
        return Err("CpuWait(fence-never-reached)", r != S_OK ? r : TimeoutHr);
    }
    return Removed("CpuWait(removed)");
}

bool Side12::WaitMark(ULONGLONG deadlineTick)
{
    if (mark->GetCompletedValue() >= markValue) return Removed("WaitMark(removed)");
    const HRESULT hr = mark->SetEventOnCompletion(markValue, event3);
    if (FAILED(hr)) return Err("ID3D12Fence::SetEventOnCompletion(mark)", hr);
    if (WaitForSingleObject(event3, Remaining(deadlineTick)) != WAIT_OBJECT_0) {
        const HRESULT r = dev->GetDeviceRemovedReason();
        return Err("WaitMark(gpu-never-passed-the-wait)", r != S_OK ? r : TimeoutHr);
    }
    return Removed("WaitMark(removed)");
}

// Three controls without any sharing: a wait whose value is already reached, a wait that is still pending when the
// queue meets it (released by a CPU Signal), and a wait released by another queue of the same device.
bool Side12::LocalWaits(std::string &result)
{
    ComPtr<ID3D12Fence> ctl;
    SetStage("w12-create");
    HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctl));
    if (FAILED(hr)) return Err("ID3D12Device::CreateFence(control)", hr);

    SetStage("w12-settled");
    hr = queue->Signal(ctl.Get(), 1);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Signal(control)", hr);
    hr = queue->Wait(ctl.Get(), 1);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Wait(settled)", hr);
    if (!Finish()) return false;
    result = "settled=ok";

    SetStage("w12-pending");
    hr = queue->Wait(ctl.Get(), 2);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Wait(pending)", hr);
    ++localValue;
    hr = queue->Signal(local.Get(), localValue);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Signal(local)", hr);
    Sleep(g_opt.gateMs);
    const UINT64 before = local->GetCompletedValue();
    const bool held = before < localValue;
    VerdictGate("w12-pending", held, held ? "" : "the queue passed a wait whose value was not signalled yet");
    hr = ctl->Signal(2);
    if (FAILED(hr)) return Err("ID3D12Fence::Signal(cpu)", hr);
    if (!WaitLocal(localValue, 5000, "pending-wait-never-released")) return false;
    if (!Removed("w12-pending(removed)")) return false;
    result += std::string(" pending=ok gate=") + (held ? "held" : "violated");

    SetStage("w12-cross-queue");
    ComPtr<ID3D12CommandQueue> second;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&second));
    if (FAILED(hr)) return Err("ID3D12Device::CreateCommandQueue(second)", hr);
    hr = queue->Wait(ctl.Get(), 3);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Wait(cross-queue)", hr);
    ++localValue;
    hr = queue->Signal(local.Get(), localValue);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Signal(local)", hr);
    ID3D12GraphicsCommandList *l = nullptr;
    if (!Begin(&l)) return false;
    RecordDelay(l);
    if (!Submit(l, second.Get(), "ExecuteCommandLists(second-queue)")) return false;
    hr = second->Signal(ctl.Get(), 3);
    if (FAILED(hr)) return Err("ID3D12CommandQueue::Signal(second-queue)", hr);
    if (!WaitLocal(localValue, 5000, "cross-queue-wait-never-released")) return false;
    if (!Removed("w12-cross-queue(removed)")) return false;
    result += " cross_queue=ok";
    return true;
}

Side *MakeSide(Api api)
{
    if (api == Api::D3D12) return new Side12();
    return new Side11();
}
