// SPDX-License-Identifier: MIT
// amdgpu_wddm_d3d12sm68: the shader model an application sees through the system D3D12 runtime, and whether shaders
// of SM 6.7 and 6.8 run on it with exact results. One console run, plain "key value" lines on stdout, one
// "result PASS|FAIL" line at the end.
//
//   1. CheckFeatureSupport(SHADER_MODEL), asked from D3D_HIGHEST_SHADER_MODEL down to the first model the runtime
//      accepts, and the OPTIONS1, OPTIONS9, OPTIONS14 and OPTIONS21 answers that SM 6.6 to 6.8 shaders depend on.
//   2. Three compute shaders, compiled at build time by the SDK's dxc (sm68\*.hlsl, embedded), each with an exact
//      readback: sm67_quad (QuadAny/QuadAll, SM 6.7), sm68_wavesize ([WaveSize(32, 64)], SM 6.8) and
//      sm68_samplecmpgrad (SampleCmpGrad, SM 6.8 with OPTIONS21.SampleCmpGradientAndBiasSupported). A test the
//      device's shader model or capability does not admit is skipped; --force attempts it anyway and records the
//      runtime's answer, which never counts as a failure.
//
// d3d12.dll and dxgi.dll come from System32 only, so the system runtime and the registered driver answer. Built with
// -AgilitySdkVersion <n>, the executable exports D3D12SDKVersion and D3D12SDKPath, and System32 d3d12.dll loads
// .\D3D12_0\D3D12Core.dll next to it, as a game that ships an Agility SDK core does.
//
// Usage: amdgpu_wddm_d3d12sm68[_agility<n>].exe [adapter-index] [--expect 6_6|6_7|6_8] [--force] [--seconds N]
//        amdgpu_wddm_d3d12sm68[_agility<n>].exe --blobs
// Exit: 0 PASS, 1 FAIL, 2 bad command line, 3 deadline (default 60 s, at most 170 s) passed in a step.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "sm67_quad.h"
#include "sm68_wavesize.h"
#include "sm68_samplecmpgrad.h"

#ifdef CAPS_AGILITY_SDK_VERSION
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = CAPS_AGILITY_SDK_VERSION; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12_0\\"; }
#endif

namespace {

template <class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** put() { return &p; }
    void** put_void() { return reinterpret_cast<void**>(&p); }
};

std::atomic<const char*> g_step{"start"};
DWORD g_deadline_ms = 60000;

void line(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stdout, format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void step(const char* name) { g_step.store(name); }

DWORD WINAPI deadline(void* done) {
    if (WaitForSingleObject(done, g_deadline_ms) != WAIT_TIMEOUT) return 0;
    line("deadline %lu ms passed in step %s", g_deadline_ms, g_step.load());
    line("result FAIL deadline");
    std::fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    return 0;
}

const char* model_name(D3D_SHADER_MODEL m) {
    switch (m) {
    case D3D_SHADER_MODEL_5_1: return "5_1";
    case D3D_SHADER_MODEL_6_0: return "6_0";
    case D3D_SHADER_MODEL_6_1: return "6_1";
    case D3D_SHADER_MODEL_6_2: return "6_2";
    case D3D_SHADER_MODEL_6_3: return "6_3";
    case D3D_SHADER_MODEL_6_4: return "6_4";
    case D3D_SHADER_MODEL_6_5: return "6_5";
    case D3D_SHADER_MODEL_6_6: return "6_6";
    case D3D_SHADER_MODEL_6_7: return "6_7";
    case D3D_SHADER_MODEL_6_8: return "6_8";
    case D3D_SHADER_MODEL_6_9: return "6_9";
    default: return "?";
    }
}

bool parse_model(const char* text, D3D_SHADER_MODEL* out) {
    for (int m = D3D_SHADER_MODEL_5_1; m <= D3D_HIGHEST_SHADER_MODEL; ++m) {
        if (!std::strcmp(model_name(static_cast<D3D_SHADER_MODEL>(m)), text)) {
            *out = static_cast<D3D_SHADER_MODEL>(m);
            return true;
        }
    }
    return false;
}

// "<name> <location> <file version>" of a loaded module, location exe_dir, system32, driver_store or other.
void module_line(const wchar_t* name) {
    HMODULE m = GetModuleHandleW(name);
    if (!m) {
        line("module %ls not-loaded", name);
        return;
    }
    wchar_t path[MAX_PATH]{}, exe[MAX_PATH]{}, sys[MAX_PATH]{};
    GetModuleFileNameW(m, path, MAX_PATH);
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    if (wchar_t* slash = std::wcsrchr(exe, L'\\')) slash[1] = 0;
    const char* where = !_wcsnicmp(path, exe, std::wcslen(exe))                 ? "exe_dir"
                        : std::wcsstr(path, L"\\DriverStore\\") != nullptr       ? "driver_store"
                        : !_wcsnicmp(path, sys, std::wcslen(sys)) && path[std::wcslen(sys)] == L'\\' ? "system32"
                                                                                  : "other";
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    std::vector<BYTE> info(size ? size : 1);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT flen = 0;
    if (size && GetFileVersionInfoW(path, 0, size, info.data()) &&
        VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&fixed), &flen) && fixed) {
        line("module %ls %s %u.%u.%u.%u", name, where, HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS),
             HIWORD(fixed->dwFileVersionLS), LOWORD(fixed->dwFileVersionLS));
    } else {
        line("module %ls %s no-version", name, where);
    }
}

// The shader model and program kind of an embedded container, from its DXIL part's program header.
bool blob_model(const unsigned char* blob, size_t size, unsigned* kind, unsigned* major, unsigned* minor) {
    if (size < 32 || std::memcmp(blob, "DXBC", 4)) return false;
    UINT32 parts = 0;
    std::memcpy(&parts, blob + 28, 4);
    for (UINT32 i = 0; i < parts && 32 + 4 * i + 4 <= size; ++i) {
        UINT32 offset = 0;
        std::memcpy(&offset, blob + 32 + 4 * i, 4);
        if (offset + 12 > size || std::memcmp(blob + offset, "DXIL", 4)) continue;
        UINT32 version = 0;
        std::memcpy(&version, blob + offset + 8, 4);
        *kind = version >> 16;
        *major = (version >> 4) & 0xF;
        *minor = version & 0xF;
        return true;
    }
    return false;
}

struct Blob { const char* name; const unsigned char* data; size_t size; unsigned minor; };
const Blob kBlobs[] = {
    {"sm67_quad", g_sm67_quad, sizeof(g_sm67_quad), 7},
    {"sm68_wavesize", g_sm68_wavesize, sizeof(g_sm68_wavesize), 8},
    {"sm68_samplecmpgrad", g_sm68_samplecmpgrad, sizeof(g_sm68_samplecmpgrad), 8},
};

// Every embedded blob is a compute shader (program kind 5) of SM 6.<minor>: the build's check, no GPU.
int check_blobs() {
    int bad = 0;
    for (const Blob& b : kBlobs) {
        unsigned kind = 0, major = 0, minor = 0;
        const bool ok = blob_model(b.data, b.size, &kind, &major, &minor) && kind == 5 && major == 6 && minor == b.minor;
        line("blob %s %zu bytes kind %u model %u_%u %s", b.name, b.size, kind, major, minor, ok ? "ok" : "BAD");
        bad += ok ? 0 : 1;
    }
    return bad ? 1 : 0;
}

struct Gpu {
    ID3D12Device* device = nullptr;
    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> list;
    Ptr<ID3D12Fence> fence;
    UINT64 value = 0;
    HANDLE event = nullptr;
    ~Gpu() { if (event) CloseHandle(event); }
};

HRESULT buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, UINT64 size, D3D12_RESOURCE_FLAGS flags,
               D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, __uuidof(ID3D12Resource),
                                           reinterpret_cast<void**>(out));
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

// Executes the recorded list and waits for it, at most 10 s.
HRESULT submit(Gpu& g) {
    HRESULT hr = g.list->Close();
    if (FAILED(hr)) return hr;
    ID3D12CommandList* lists[] = {g.list.p};
    g.queue->ExecuteCommandLists(1, lists);
    hr = g.queue->Signal(g.fence.p, ++g.value);
    if (FAILED(hr)) return hr;
    if (g.fence->GetCompletedValue() < g.value) {
        hr = g.fence->SetEventOnCompletion(g.value, g.event);
        if (FAILED(hr)) return hr;
        if (WaitForSingleObject(g.event, 10000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }
    return g.device->GetDeviceRemovedReason();
}

struct Outcome { bool ran = false; bool pass = false; };

// Runs one embedded compute shader: `threads` words into a UAV buffer, copied to a readback buffer. `bind` sets the
// root arguments past root parameter 0 (the UAV); `check` judges the words and prints what it found.
template <class Bind, class Check>
Outcome run_compute(Gpu& g, const Blob& blob, UINT groups, UINT threads, Bind bind, Check check) {
    Outcome o;
    ID3D12Device* d = g.device;
    Ptr<ID3D12RootSignature> rs;
    HRESULT hr = d->CreateRootSignature(0, blob.data, blob.size, __uuidof(ID3D12RootSignature), rs.put_void());
    Ptr<ID3D12PipelineState> pso;
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rs.p;
        desc.CS = {blob.data, blob.size};
        hr = d->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), pso.put_void());
    }
    line("test %s pipeline hr 0x%08lx", blob.name, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return o;
    const UINT64 bytes = UINT64{threads} * 4;
    Ptr<ID3D12Resource> out, readback;
    hr = buffer(d, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON, out.put());
    if (SUCCEEDED(hr))
        hr = buffer(d, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST,
                    readback.put());
    if (SUCCEEDED(hr)) hr = g.allocator->Reset();
    if (SUCCEEDED(hr)) hr = g.list->Reset(g.allocator.p, pso.p);
    if (FAILED(hr)) {
        line("test %s setup hr 0x%08lx", blob.name, static_cast<unsigned long>(hr));
        return o;
    }
    barrier(g.list.p, out.p, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.list->SetComputeRootSignature(rs.p);
    g.list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
    bind(g.list.p);
    g.list->Dispatch(groups, 1, 1);
    barrier(g.list.p, out.p, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(readback.p, 0, out.p, 0, bytes);
    hr = submit(g);
    line("test %s submit hr 0x%08lx", blob.name, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return o;
    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
    hr = readback->Map(0, &range, &mapped);
    if (FAILED(hr)) {
        line("test %s map hr 0x%08lx", blob.name, static_cast<unsigned long>(hr));
        return o;
    }
    std::vector<UINT> words(threads);
    std::memcpy(words.data(), mapped, static_cast<size_t>(bytes));
    D3D12_RANGE none{0, 0};
    readback->Unmap(0, &none);
    o.ran = true;
    o.pass = check(words);
    line("test %s %s", blob.name, o.pass ? "PASS" : "FAIL");
    return o;
}

// The SampleCmpGrad test's texture: 4x4 R32_FLOAT, texel i = i / 16 + 1 / 32, in a shader-visible descriptor heap.
struct DepthTexture {
    Ptr<ID3D12Resource> texture, upload;
    Ptr<ID3D12DescriptorHeap> heap;
};

HRESULT make_depth_texture(Gpu& g, DepthTexture& t) {
    ID3D12Device* d = g.device;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = 4;
    rd.Height = 4;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            __uuidof(ID3D12Resource), t.texture.put_void());
    if (FAILED(hr)) return hr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    d->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    hr = buffer(d, D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ,
                t.upload.put());
    if (FAILED(hr)) return hr;
    BYTE* mapped = nullptr;
    hr = t.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) return hr;
    for (UINT y = 0; y < 4; ++y)
        for (UINT x = 0; x < 4; ++x) {
            const float v = float(y * 4 + x) / 16.0f + 1.0f / 32.0f;
            std::memcpy(mapped + fp.Offset + y * fp.Footprint.RowPitch + x * 4, &v, 4);
        }
    t.upload->Unmap(0, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 1;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = d->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), t.heap.put_void());
    if (FAILED(hr)) return hr;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    d->CreateShaderResourceView(t.texture.p, &sd, t.heap->GetCPUDescriptorHandleForHeapStart());
    // Upload: copy, then make it readable by the compute shader.
    hr = g.allocator->Reset();
    if (SUCCEEDED(hr)) hr = g.list->Reset(g.allocator.p, nullptr);
    if (FAILED(hr)) return hr;
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = t.texture.p;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = t.upload.p;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(g.list.p, t.texture.p, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return submit(g);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    UINT adapter_index = 0;
    D3D_SHADER_MODEL expect = static_cast<D3D_SHADER_MODEL>(0);
    bool force = false;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!std::wcscmp(a, L"--help") || !std::wcscmp(a, L"-h")) {
            line("usage: amdgpu_wddm_d3d12sm68.exe [adapter-index] [--expect 6_6|6_7|6_8] [--force] [--seconds N]");
            line("       amdgpu_wddm_d3d12sm68.exe --blobs");
            return 0;
        }
        if (!std::wcscmp(a, L"--blobs")) return check_blobs();
        if (!std::wcscmp(a, L"--force")) {
            force = true;
        } else if (!std::wcscmp(a, L"--expect") && i + 1 < argc) {
            char text[8]{};
            std::snprintf(text, sizeof(text), "%ls", argv[++i]);
            if (!parse_model(text, &expect) || expect < D3D_SHADER_MODEL_6_0) {
                std::fprintf(stderr, "bad --expect %ls\n", argv[i]);
                return 2;
            }
        } else if (!std::wcscmp(a, L"--seconds") && i + 1 < argc) {
            wchar_t* end = nullptr;
            const unsigned long s = std::wcstoul(argv[++i], &end, 10);
            if (!end || *end || s < 5 || s > 170) {
                std::fprintf(stderr, "bad --seconds %ls (5 to 170)\n", argv[i]);
                return 2;
            }
            g_deadline_ms = static_cast<DWORD>(s * 1000);
        } else {
            wchar_t* end = nullptr;
            const unsigned long v = std::wcstoul(a, &end, 10);
            if (!end || *end || a[0] < L'0' || a[0] > L'9' || v > 63) {
                std::fprintf(stderr, "bad argument %ls\n", a);
                return 2;
            }
            adapter_index = static_cast<UINT>(v);
        }
    }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE watchdog = CreateThread(nullptr, 0, deadline, done, 0, nullptr);
#ifdef CAPS_AGILITY_SDK_VERSION
    line("tool d3d12sm68 agility %u path .\\D3D12_0\\", static_cast<unsigned>(CAPS_AGILITY_SDK_VERSION));
#else
    line("tool d3d12sm68 agility none");
#endif
    line("tool headers D3D_HIGHEST_SHADER_MODEL %s", model_name(D3D_HIGHEST_SHADER_MODEL));
    if (check_blobs()) {
        line("result FAIL blobs");
        return 1;
    }

    step("dxgi");
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using Factory = HRESULT(WINAPI*)(REFIID, void**);
    auto create_factory = dxgi ? reinterpret_cast<Factory>(reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                               : nullptr;
    auto create_device = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                                     reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                               : nullptr;
    if (!create_factory || !create_device) {
        line("load dxgi %s d3d12 %s", dxgi ? "ok" : "missing", d3d12 ? "ok" : "missing");
        line("result FAIL load");
        return 1;
    }
    Ptr<IDXGIFactory1> factory;
    Ptr<IDXGIAdapter1> adapter;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory1), factory.put_void());
    if (SUCCEEDED(hr)) hr = factory->EnumAdapters1(adapter_index, adapter.put());
    if (FAILED(hr)) {
        line("adapter %u hr 0x%08lx", adapter_index, static_cast<unsigned long>(hr));
        line("result FAIL adapter");
        return 1;
    }
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    line("adapter %u \"%ls\" vendor 0x%04x device 0x%04x", adapter_index, ad.Description, ad.VendorId, ad.DeviceId);

    step("device");
    Gpu g;
    Ptr<ID3D12Device> device;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
                                        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    for (D3D_FEATURE_LEVEL l : levels) {
        hr = create_device(adapter.p, l, __uuidof(ID3D12Device), device.put_void());
        if (SUCCEEDED(hr)) {
            level = l;
            break;
        }
    }
    line("device D3D12CreateDevice hr 0x%08lx level 0x%x", static_cast<unsigned long>(hr), static_cast<unsigned>(level));
    module_line(L"d3d12.dll");
    module_line(L"D3D12Core.dll");
    module_line(L"amdgpu_wddm_d3d12.dll");
    if (FAILED(hr)) {
        line("result FAIL device");
        return 1;
    }
    g.device = device.p;

    step("caps");
    D3D_SHADER_MODEL highest = static_cast<D3D_SHADER_MODEL>(0);
    for (int m = D3D_HIGHEST_SHADER_MODEL; m >= D3D_SHADER_MODEL_5_1; --m) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm{static_cast<D3D_SHADER_MODEL>(m)};
        hr = device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
        line("shader_model asked %s hr 0x%08lx answer %s", model_name(static_cast<D3D_SHADER_MODEL>(m)),
             static_cast<unsigned long>(hr), SUCCEEDED(hr) ? model_name(sm.HighestShaderModel) : "-");
        if (SUCCEEDED(hr)) {
            highest = sm.HighestShaderModel;
            break;
        }
    }
    line("shader_model highest %s", model_name(highest));
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1));
    line("options1 hr 0x%08lx WaveOps %d WaveLaneCountMin %u WaveLaneCountMax %u TotalLaneCount %u Int64ShaderOps %d",
         static_cast<unsigned long>(hr), o1.WaveOps, o1.WaveLaneCountMin, o1.WaveLaneCountMax, o1.TotalLaneCount,
         o1.Int64ShaderOps);
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 o9{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &o9, sizeof(o9));
    line("options9 hr 0x%08lx AtomicInt64OnTypedResourceSupported %d AtomicInt64OnGroupSharedSupported %d WaveMMATier %d",
         static_cast<unsigned long>(hr), o9.AtomicInt64OnTypedResourceSupported, o9.AtomicInt64OnGroupSharedSupported,
         static_cast<int>(o9.WaveMMATier));
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 o14{};
    hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS14, &o14, sizeof(o14));
    line("options14 hr 0x%08lx AdvancedTextureOpsSupported %d WriteableMSAATexturesSupported %d "
         "IndependentFrontAndBackStencilRefMaskSupported %d",
         static_cast<unsigned long>(hr), o14.AdvancedTextureOpsSupported, o14.WriteableMSAATexturesSupported,
         o14.IndependentFrontAndBackStencilRefMaskSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 o21{};
    const HRESULT hr21 = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &o21, sizeof(o21));
    line("options21 hr 0x%08lx WorkGraphsTier %d ExecuteIndirectTier %d SampleCmpGradientAndBiasSupported %d "
         "ExtendedCommandInfoSupported %d",
         static_cast<unsigned long>(hr21), static_cast<int>(o21.WorkGraphsTier), static_cast<int>(o21.ExecuteIndirectTier),
         o21.SampleCmpGradientAndBiasSupported, o21.ExtendedCommandInfoSupported);

    step("queue");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    hr = device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), g.queue.put_void());
    if (SUCCEEDED(hr))
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, __uuidof(ID3D12CommandAllocator),
                                            g.allocator.put_void());
    if (SUCCEEDED(hr))
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, g.allocator.p, nullptr,
                                       __uuidof(ID3D12GraphicsCommandList), g.list.put_void());
    if (SUCCEEDED(hr)) hr = g.list->Close();
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), g.fence.put_void());
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !g.event) {
        line("queue hr 0x%08lx", static_cast<unsigned long>(hr));
        line("result FAIL queue");
        return 1;
    }

    int failed = 0, ran = 0;
    const auto gate = [&](const char* name, bool admitted, const char* why) {
        if (admitted) return true;
        line("test %s skipped (%s)%s", name, why, force ? ", attempted with --force" : "");
        return force;
    };
    const auto count = [&](const Outcome& o, bool admitted) {
        if (!admitted) return;                        // a forced attempt is data, never a verdict
        ++ran;
        failed += o.ran && o.pass ? 0 : 1;
    };

    // SM 6.7: QuadAny / QuadAll, 2 groups of 64.
    step("sm67_quad");
    {
        const bool admitted = highest >= D3D_SHADER_MODEL_6_7;
        if (gate("sm67_quad", admitted, "device shader model below 6_7")) {
            const Outcome o = run_compute(g, kBlobs[0], 2, 128, [](ID3D12GraphicsCommandList*) {},
                                          [](const std::vector<UINT>& w) {
                UINT bad = 0, first = UINT_MAX;
                for (UINT i = 0; i < w.size(); ++i) {
                    const UINT index = i % 64;
                    const UINT expected = (((index / 4) % 2) == 0 ? 3u : 0u) | (index << 8);
                    if (w[i] != expected) {
                        ++bad;
                        if (first == UINT_MAX) first = i;
                    }
                }
                line("test sm67_quad words %zu mismatches %u first %d (0x%08x)", w.size(), bad,
                     first == UINT_MAX ? -1 : static_cast<int>(first), first == UINT_MAX ? 0u : w[first]);
                return bad == 0;
            });
            count(o, admitted);
        }
    }

    // SM 6.8: [WaveSize(32, 64)], 2 groups of 64.
    step("sm68_wavesize");
    {
        const bool admitted = highest >= D3D_SHADER_MODEL_6_8;
        if (gate("sm68_wavesize", admitted, "device shader model below 6_8")) {
            const Outcome o = run_compute(g, kBlobs[1], 2, 128, [](ID3D12GraphicsCommandList*) {},
                                          [&](const std::vector<UINT>& w) {
                const UINT lanes = w.empty() ? 0 : (w[0] & 0xFF);
                UINT bad = 0, first = UINT_MAX;
                for (UINT i = 0; i < w.size(); ++i) {
                    const UINT index = i % 64;
                    const UINT expected = lanes | (lanes << 8) | ((index % (lanes ? lanes : 1)) << 16) | (index << 24);
                    if ((lanes != 32 && lanes != 64) || w[i] != expected) {
                        ++bad;
                        if (first == UINT_MAX) first = i;
                    }
                }
                line("test sm68_wavesize lanes %u (range 32-64, device %u-%u) words %zu mismatches %u first %d (0x%08x)",
                     lanes, o1.WaveLaneCountMin, o1.WaveLaneCountMax, w.size(), bad,
                     first == UINT_MAX ? -1 : static_cast<int>(first), first == UINT_MAX ? 0u : w[first]);
                return bad == 0;
            });
            count(o, admitted);
        }
    }

    // SM 6.8: SampleCmpGrad on a 4x4 R32_FLOAT texture, one group of 16.
    step("sm68_samplecmpgrad");
    {
        const bool admitted = highest >= D3D_SHADER_MODEL_6_8 && SUCCEEDED(hr21) && o21.SampleCmpGradientAndBiasSupported;
        if (gate("sm68_samplecmpgrad", admitted,
                 highest < D3D_SHADER_MODEL_6_8 ? "device shader model below 6_8"
                                                : "OPTIONS21.SampleCmpGradientAndBiasSupported 0")) {
            DepthTexture t;
            hr = make_depth_texture(g, t);
            line("test sm68_samplecmpgrad texture hr 0x%08lx", static_cast<unsigned long>(hr));
            Outcome o;
            if (SUCCEEDED(hr)) {
                ID3D12DescriptorHeap* heap = t.heap.p;
                o = run_compute(g, kBlobs[2], 1, 16,
                                [heap](ID3D12GraphicsCommandList* l) {
                                    l->SetDescriptorHeaps(1, &heap);
                                    l->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
                                },
                                [](const std::vector<UINT>& w) {
                                    UINT bad = 0, first = UINT_MAX;
                                    for (UINT i = 0; i < w.size(); ++i) {
                                        const UINT expected = (i >= 8 ? 255u : 0u) | (i << 8);
                                        if (w[i] != expected) {
                                            ++bad;
                                            if (first == UINT_MAX) first = i;
                                        }
                                    }
                                    line("test sm68_samplecmpgrad words %zu mismatches %u first %d (0x%08x)", w.size(),
                                         bad, first == UINT_MAX ? -1 : static_cast<int>(first),
                                         first == UINT_MAX ? 0u : w[first]);
                                    return bad == 0;
                                });
            }
            count(o, admitted);
        }
    }

    step("end");
    hr = device->GetDeviceRemovedReason();
    line("device GetDeviceRemovedReason 0x%08lx", static_cast<unsigned long>(hr));
    const bool expect_ok = !expect || highest >= expect;
    const bool pass = expect_ok && !failed && SUCCEEDED(hr);
    line("result %s highest %s expect %s tests %d failed %d", pass ? "PASS" : "FAIL", model_name(highest),
         expect ? model_name(expect) : "none", ran, failed);
    SetEvent(done);
    if (watchdog) {
        WaitForSingleObject(watchdog, 1000);
        CloseHandle(watchdog);
    }
    return pass ? 0 : 1;
}
