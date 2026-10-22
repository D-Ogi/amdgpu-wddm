// SPDX-License-Identifier: MIT
// amdgpu_wddm_d3d12pso: our own client for the pipeline-state path of the D3D12 driver (BD-158, asynchronous
// pipeline compilation). It does what a game does at load time and nothing else: it creates N distinct pipeline
// states in one loop on one thread, and then draws once with each of them and checks every pixel.
//
//   usage: amdgpu_wddm_d3d12pso [--adapter warp|bc250|hw|index:N] [--count N] [--threads N] [--deadline SECONDS]
//                               [--label TEXT] [--quiet] | --selftest | --help
//
// What it measures, and why those three numbers:
//   create  the wall time of CreateGraphicsPipelineState, on the thread that calls it. With the asynchronous path
//           off this holds the whole engine create (the DXIL to SPIR-V conversion and the pipeline compile). With
//           it on the driver returns after the copy of the description, so this time collapses.
//   set     the wall time of SetPipelineState for the first use of that pipeline. This is where the driver waits
//           for a compile that is not finished, so the wait the asynchronous path moved shows up here and nowhere
//           else. A game's load loop pays create + set, so the sum of the two columns is the number that must go
//           down, not either column alone.
//   gpu     submit to fence of the draw that uses the pipeline, for scale: it must stay small next to the other
//           two, or the measurement is about the queue and not about compilation.
// The split between the conversion in amdgpu_wddm_vkd3d.dll and the shader compile in amdgpu_wddm_radv.dll is not
// visible from here (one engine call covers both). Read it from the driver's own counters in the shell log at
// device destruction, or from ETW; this client measures what the application's threads pay.
//
// The programs: 16 pixel programs, variant v with 4 * (v + 1) unrolled rounds of exact 32-bit arithmetic
// (shaders\d3d12pso.hlsl, built by build.ps1 with the SDK's dxc into gen\). Pipeline i uses variant i % 16 and
// state permutation i / 16, so every pipeline is a different D3D12 object, every variant is a different program,
// and the pixels of a draw depend on the variant alone. The expected word of a pixel is computed on the CPU by the
// same arithmetic, so a wrong picture is a failure of this driver and not a difference from another renderer.
#include <windows.h>
#include <bcrypt.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "gen/variants.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT kSide = 64;                      // the render target, 64x64 R32_UINT, as in the engine's own tests
constexpr UINT32 kTag = 0xB0250000u;            // carried by the vertex buffer, so the picture names this client
constexpr unsigned kPermutations = 16;
constexpr unsigned kMaxCount = kPsoVariantCount * kPermutations;

// The pixel program's arithmetic, on the CPU. Must stay the exact mirror of mix_round in shaders\d3d12pso.hlsl.
UINT32 mix_round(UINT32 value, UINT32 round) {
    value += round;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

UINT32 expected_word(UINT32 seed, UINT x, UINT y, unsigned rounds) {
    UINT32 value = seed ^ kTag ^ ((y << 16) | x);
    for (unsigned round = 0; round < rounds; ++round) value = mix_round(value, static_cast<UINT32>(round));
    return value;
}

// A label for a program in the --selftest record. Not a security claim: it only has to differ when the bytes do.
UINT64 fnv1a(const unsigned char* bytes, size_t size) {
    UINT64 hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) { hash ^= bytes[i]; hash *= 0x100000001b3ull; }
    return hash;
}

UINT64 g_qpf = 1;
UINT64 now() { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); return static_cast<UINT64>(t.QuadPart); }
double micros(UINT64 from, UINT64 to) { return to > from ? static_cast<double>(to - from) * 1e6 / static_cast<double>(g_qpf) : 0.0; }

struct Options {
    std::string adapter = "bc250";
    unsigned count = 32;
    unsigned threads = 1;
    unsigned deadline = 150;                    // seconds; a lab trial of this kind stays inside three minutes
    std::string label = "run";
    bool quiet = false;
};

const char* kUsage =
    "amdgpu_wddm_d3d12pso [--adapter warp|bc250|hw|index:N] [--count N] [--threads N] [--deadline SECONDS]\n"
    "                     [--label TEXT] [--quiet] | --selftest | --help\n"
    "  --count    pipeline states to create, 1 to 256 (default 32)\n"
    "  --threads  application threads that share the create loop, 1 to 64 (default 1, which is what a game does)\n"
    "  --selftest programs only, no device: the variant table and that every program differs\n";

bool number(const char* text, unsigned low, unsigned high, unsigned* out) {
    if (!text || !*text) return false;
    unsigned value = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || value > 1000000u) return false;
        value = value * 10 + static_cast<unsigned>(*p - '0');
    }
    if (value < low || value > high) return false;
    *out = value;
    return true;
}

int selftest() {
    std::printf("variants %u permutations %u max count %u\n", kPsoVariantCount, kPermutations, kMaxCount);
    std::printf("vertex program %u bytes fnv %016llx\n", static_cast<unsigned>(sizeof(g_pso_vs)),
                static_cast<unsigned long long>(fnv1a(g_pso_vs, sizeof(g_pso_vs))));
    for (unsigned v = 0; v < kPsoVariantCount; ++v) {
        const PsoVariant& variant = g_pso_variants[v];
        if (!variant.code || !variant.size || !variant.rounds) { std::printf("variant %u is empty\n", v); return 4; }
        std::printf("variant %2u rounds %3u %6u bytes fnv %016llx\n", v, variant.rounds, variant.size,
                    static_cast<unsigned long long>(fnv1a(variant.code, variant.size)));
    }
    // The whole measurement rests on this: the same bytes twice would be one compile and one cache hit, not two
    // compiles. Compared in full, not by a hash.
    for (unsigned a = 0; a < kPsoVariantCount; ++a)
        for (unsigned b = a + 1; b < kPsoVariantCount; ++b)
            if (g_pso_variants[a].size == g_pso_variants[b].size &&
                !std::memcmp(g_pso_variants[a].code, g_pso_variants[b].code, g_pso_variants[a].size)) {
                std::printf("variants %u and %u are the same program\n", a, b);
                return 4;
            }
    // And on the mirror of the shader arithmetic being deterministic: two calls, one value, and the rounds matter.
    if (expected_word(0x12345678u, 7, 9, 4) != expected_word(0x12345678u, 7, 9, 4) ||
        expected_word(0x12345678u, 7, 9, 4) == expected_word(0x12345678u, 7, 9, 8)) {
        std::printf("the CPU mirror of the pixel program is not deterministic\n");
        return 4;
    }
    std::printf("D3D12PSO selftest PASS variants=%u\n", kPsoVariantCount);
    return 0;
}

HRESULT pick_adapter(IDXGIFactory4* factory, const std::string& which, decltype(&D3D12CreateDevice) create,
                     ComPtr<IDXGIAdapter1>* out) {
    if (which == "warp") return factory->EnumWarpAdapter(IID_PPV_ARGS(out->ReleaseAndGetAddressOf()));
    unsigned wanted = ~0u;
    if (which.rfind("index:", 0) == 0 && !number(which.c_str() + 6, 0, 64, &wanted)) return E_INVALIDARG;
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT hr = factory->EnumAdapters1(index, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) return hr;
        DXGI_ADAPTER_DESC1 d{};
        if (FAILED(candidate->GetDesc1(&d))) continue;
        bool take = false;
        if (wanted != ~0u) take = index == wanted;
        else if (which == "bc250") take = d.VendorId == 0x1002 && d.DeviceId == 0x13fe;
        else if (which == "hw") take = !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId != 0x1414 &&
                                      SUCCEEDED(create(candidate.Get(), D3D_FEATURE_LEVEL_12_1, __uuidof(ID3D12Device), nullptr));
        if (take) { *out = candidate; return S_OK; }
    }
    return DXGI_ERROR_NOT_FOUND;
}

// Four state bits that make a different pipeline for the engine and cannot change the words this client compares:
// the triangle covers the whole target, culling is off and there is no depth target.
void permute(D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, unsigned permutation) {
    desc.RasterizerState.FrontCounterClockwise = (permutation & 1) ? TRUE : FALSE;
    desc.RasterizerState.DepthClipEnable = (permutation & 2) ? FALSE : TRUE;
    desc.RasterizerState.DepthBias = (permutation & 4) ? 1 : 0;
    desc.RasterizerState.MultisampleEnable = (permutation & 8) ? TRUE : FALSE;
}

struct Pipeline {
    ComPtr<ID3D12PipelineState> state;
    unsigned variant = 0;
    unsigned permutation = 0;
    double create_us = 0;
    double set_us = 0;
    double gpu_us = 0;
    unsigned different = 0;                     // words that did not match the CPU's
    HRESULT hr = S_OK;
};

double total_of(const std::vector<Pipeline>& list, double Pipeline::*member) {
    double sum = 0;
    for (const Pipeline& p : list) sum += p.*member;
    return sum;
}

double largest_of(const std::vector<Pipeline>& list, double Pipeline::*member) {
    double most = 0;
    for (const Pipeline& p : list) most = (std::max)(most, p.*member);
    return most;
}

double median_of(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

} // namespace

int main(int argc, char** argv) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_qpf = frequency.QuadPart ? static_cast<UINT64>(frequency.QuadPart) : 1;
    Options o;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!std::strcmp(a, "--help")) { std::printf("%s", kUsage); return 0; }
        if (!std::strcmp(a, "--selftest")) return selftest();
        if (!std::strcmp(a, "--quiet")) { o.quiet = true; continue; }
        if (!value) { std::fprintf(stderr, "%s needs a value\n", a); return 2; }
        if (!std::strcmp(a, "--adapter")) o.adapter = argv[++i];
        else if (!std::strcmp(a, "--label")) o.label = argv[++i];
        else if (!std::strcmp(a, "--count")) { if (!number(value, 1, kMaxCount, &o.count)) { std::fprintf(stderr, "--count 1 to %u\n", kMaxCount); return 2; } ++i; }
        else if (!std::strcmp(a, "--threads")) { if (!number(value, 1, 64, &o.threads)) { std::fprintf(stderr, "--threads 1 to 64\n"); return 2; } ++i; }
        else if (!std::strcmp(a, "--deadline")) { if (!number(value, 5, 1200, &o.deadline)) { std::fprintf(stderr, "--deadline 5 to 1200\n"); return 2; } ++i; }
        else { std::fprintf(stderr, "unknown argument %s\n%s", a, kUsage); return 2; }
    }
    if (o.adapter != "warp" && o.adapter != "bc250" && o.adapter != "hw" && o.adapter.rfind("index:", 0) != 0) {
        std::fprintf(stderr, "unknown adapter %s\n%s", o.adapter.c_str(), kUsage);
        return 2;
    }
    const ULONGLONG start_ticks = GetTickCount64();
    const ULONGLONG deadline = start_ticks + o.deadline * 1000ull;

    unsigned char random[4]{};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return 3;
    UINT32 seed = 0;
    std::memcpy(&seed, random, sizeof(seed));

    wchar_t runtime_path[MAX_PATH]{};
    if (!GetSystemDirectoryW(runtime_path, MAX_PATH) || wcscat_s(runtime_path, L"\\d3d12.dll")) return 3;
    const HMODULE runtime = LoadLibraryExW(runtime_path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!runtime) return 3;
    decltype(&D3D12CreateDevice) create_device = nullptr;
    decltype(&D3D12SerializeRootSignature) serialize = nullptr;
    {
        const auto device_proc = GetProcAddress(runtime, "D3D12CreateDevice");
        const auto serialize_proc = GetProcAddress(runtime, "D3D12SerializeRootSignature");
        static_assert(sizeof(create_device) == sizeof(device_proc));
        std::memcpy(&create_device, &device_proc, sizeof(create_device));
        std::memcpy(&serialize, &serialize_proc, sizeof(serialize));
    }
    if (!create_device || !serialize) return 3;

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter;
    HRESULT hr = pick_adapter(factory.Get(), o.adapter, create_device, &adapter);
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=adapter-%s hr=%08lx\n", o.adapter.c_str(), static_cast<unsigned long>(hr)); return 3; }
    DXGI_ADAPTER_DESC1 adapter_desc{};
    adapter->GetDesc1(&adapter_desc);
    char adapter_text[160]{};
    {
        char name[96]{};
        WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        sprintf_s(adapter_text, "%04x:%04x %s", adapter_desc.VendorId, adapter_desc.DeviceId, name);
    }
    ComPtr<ID3D12Device> device;
    hr = create_device(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device));
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=D3D12CreateDevice-FL12_1 hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
    std::printf("d3d12pso label=%s adapter=%s count=%u threads=%u variants=%u seed=%08x\n",
                o.label.c_str(), adapter_text, o.count, o.threads, kPsoVariantCount, seed);
    std::fflush(stdout);

    // The root signature: one 32-bit constant, the seed, read by every pixel program.
    D3D12_ROOT_PARAMETER parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 1;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{};
    signature.NumParameters = 1;
    signature.pParameters = &parameter;
    signature.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, errors;
    hr = serialize(&signature, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &errors);
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=SerializeRootSignature hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
    ComPtr<ID3D12RootSignature> root;
    hr = device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=CreateRootSignature hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }

    const D3D12_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TAG", 0, DXGI_FORMAT_R32_UINT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    const auto description_of = [&](unsigned index) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root.Get();
        desc.VS = {g_pso_vs, sizeof(g_pso_vs)};
        desc.PS = {g_pso_variants[index % kPsoVariantCount].code, g_pso_variants[index % kPsoVariantCount].size};
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = 0xffffffffu;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.InputLayout = {elements, 2};
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
        desc.SampleDesc.Count = 1;
        permute(desc, index / kPsoVariantCount);
        return desc;
    };

    // The create loop, the part a game runs at load time. One thread by default; with --threads N the same range is
    // split between N threads, which is the second half of the project rule: our own client exercises the driver's
    // concurrency, both from one caller and from several.
    std::vector<Pipeline> pipelines(o.count);
    for (unsigned i = 0; i < o.count; ++i) {
        pipelines[i].variant = i % kPsoVariantCount;
        pipelines[i].permutation = i / kPsoVariantCount;
    }
    std::atomic<unsigned> next{0};
    const auto create_range = [&]() {
        for (unsigned i = next.fetch_add(1); i < o.count; i = next.fetch_add(1)) {
            Pipeline& p = pipelines[i];
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = description_of(i);
            const UINT64 before = now();
            p.hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&p.state));
            p.create_us = micros(before, now());
        }
    };
    const UINT64 create_start = now();
    if (o.threads <= 1) {
        create_range();
    } else {
        std::vector<std::thread> workers;
        workers.reserve(o.threads);
        for (unsigned t = 0; t < o.threads; ++t) workers.emplace_back(create_range);
        for (std::thread& w : workers) w.join();
    }
    const double create_wall_us = micros(create_start, now());
    unsigned refused = 0;
    for (const Pipeline& p : pipelines) if (FAILED(p.hr) || !p.state) ++refused;
    if (refused) {
        for (unsigned i = 0; i < o.count; ++i)
            if (FAILED(pipelines[i].hr) || !pipelines[i].state)
                std::printf("pso %3u variant %2u create REFUSED hr=%08lx\n", i, pipelines[i].variant, static_cast<unsigned long>(pipelines[i].hr));
        std::printf("D3D12PSO overall FAIL reason=create-refused refused=%u of %u\n", refused, o.count);
        return 4;
    }
    std::printf("creates %u in %.0f us wall (sum %.0f us, median %.0f us, longest %.0f us)\n", o.count, create_wall_us,
                total_of(pipelines, &Pipeline::create_us),
                median_of([&] { std::vector<double> v; for (const Pipeline& p : pipelines) v.push_back(p.create_us); return v; }()),
                largest_of(pipelines, &Pipeline::create_us));
    std::fflush(stdout);

    // The target, the vertex buffer and the readback, made once and used by every draw.
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = kSide;
    texture.Height = kSide;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R32_UINT;
    texture.SampleDesc.Count = 1;
    texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cleared{};
    cleared.Format = DXGI_FORMAT_R32_UINT;
    ComPtr<ID3D12Resource> target;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET, &cleared, IID_PPV_ARGS(&target));
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=target hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
    D3D12_DESCRIPTOR_HEAP_DESC views{};
    views.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    views.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> view_heap;
    hr = device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&view_heap));
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=rtv-heap hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
    const D3D12_CPU_DESCRIPTOR_HANDLE view = view_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, view);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &placed, &rows, &row_bytes, &total);
    if (rows != kSide || row_bytes != kSide * 4ull || placed.Offset || placed.Footprint.RowPitch < kSide * 4 || !total ||
        UINT64{kSide - 1} * placed.Footprint.RowPitch + UINT64{kSide} * 4 > total) {
        std::printf("D3D12PSO overall ERROR reason=footprint rows=%u pitch=%u total=%llu\n", rows, placed.Footprint.RowPitch,
                    static_cast<unsigned long long>(total));
        return 3;
    }
    const UINT pitch = placed.Footprint.RowPitch;

    struct Vertex { float x, y; UINT32 tag; };
    static_assert(sizeof(Vertex) == 12);
    const Vertex corners[3]{{-1.0f, -1.0f, kTag}, {-1.0f, 3.0f, kTag}, {3.0f, -1.0f, kTag}};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = sizeof(corners);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> upload;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=vertices hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
    buffer.Width = total;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=readback hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
    const D3D12_RANGE empty{0, 0};
    const D3D12_RANGE whole{0, static_cast<SIZE_T>(total)};
    {
        void* data = nullptr;
        hr = upload->Map(0, &empty, &data);
        if (FAILED(hr) || !data) { std::printf("D3D12PSO overall ERROR reason=map-vertices hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
        std::memcpy(data, corners, sizeof(corners));
        const D3D12_RANGE written{0, sizeof(corners)};
        upload->Unmap(0, &written);
    }

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> allocator;
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    ComPtr<ID3D12GraphicsCommandList> list;
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    // A new list records at once, and an allocator may not be reset while a list of it records. The loop below opens
    // every list itself, so this one is closed here and the first pass of the loop starts like every other.
    if (SUCCEEDED(hr)) hr = list->Close();
    ComPtr<ID3D12Fence> fence;
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    const HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !completed) { std::printf("D3D12PSO overall ERROR reason=queue hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }

    // One draw per pipeline, each on its own submission, so the first use of every pipeline is timed by itself.
    const D3D12_VERTEX_BUFFER_VIEW vertices{upload->GetGPUVirtualAddress(), sizeof(corners), sizeof(Vertex)};
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<FLOAT>(kSide), static_cast<FLOAT>(kSide), 0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(kSide), static_cast<LONG>(kSide)};
    const FLOAT zero[4]{};
    unsigned checked = 0, failed = 0;
    for (unsigned i = 0; i < o.count; ++i) {
        Pipeline& p = pipelines[i];
        if (GetTickCount64() > deadline) {
            std::printf("D3D12PSO overall ERROR reason=deadline after=%u of %u pipelines\n", i, o.count);
            return 3;
        }
        {   // The destination holds the complement of every expected word before the draw, so an untouched word fails.
            void* data = nullptr;
            hr = readback->Map(0, &empty, &data);
            if (FAILED(hr) || !data) { std::printf("D3D12PSO overall ERROR reason=map-prefill hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
            for (UINT y = 0; y < kSide; ++y)
                for (UINT x = 0; x < kSide; ++x) {
                    const UINT32 word = ~expected_word(seed, x, y, g_pso_variants[p.variant].rounds);
                    std::memcpy(static_cast<unsigned char*>(data) + y * pitch + x * 4, &word, 4);
                }
            readback->Unmap(0, &whole);
        }
        hr = allocator->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(allocator.Get(), nullptr);
        if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=reset hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
        list->SetGraphicsRootSignature(root.Get());
        const UINT64 before_set = now();
        list->SetPipelineState(p.state.Get());           // the driver waits here for a compile that is not finished
        p.set_us = micros(before_set, now());
        list->SetGraphicsRoot32BitConstant(0, seed, 0);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->IASetVertexBuffers(0, 1, &vertices);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &view, FALSE, nullptr);
        list->ClearRenderTargetView(view, zero, 0, nullptr);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = placed;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = target.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        hr = list->Close();
        if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=close hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
        ID3D12CommandList* commands[]{list.Get()};
        const UINT64 before_gpu = now();
        queue->ExecuteCommandLists(1, commands);
        hr = queue->Signal(fence.Get(), i + 1);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(i + 1, completed);
        if (FAILED(hr)) { std::printf("D3D12PSO overall ERROR reason=signal hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
        {   // Bounded: a GPU that does not finish must end this client, not hold the lab.
            const ULONGLONG end = (std::min)(deadline, GetTickCount64() + 20000);
            DWORD waited = WAIT_TIMEOUT;
            while (GetTickCount64() < end && waited == WAIT_TIMEOUT) waited = WaitForSingleObject(completed, 20);
            p.gpu_us = micros(before_gpu, now());
            const UINT64 value = fence->GetCompletedValue();
            if (value == UINT64_MAX) { std::printf("D3D12PSO overall ERROR reason=device-removed after=%u hr=%08lx\n", i, static_cast<unsigned long>(device->GetDeviceRemovedReason())); return 3; }
            if (waited != WAIT_OBJECT_0 || value < i + 1) { std::printf("D3D12PSO overall ERROR reason=fence-timeout pso=%u value=%llu\n", i, static_cast<unsigned long long>(value)); return 3; }
        }
        {
            void* data = nullptr;
            hr = readback->Map(0, &whole, &data);
            if (FAILED(hr) || !data) { std::printf("D3D12PSO overall ERROR reason=map-readback hr=%08lx\n", static_cast<unsigned long>(hr)); return 3; }
            UINT first_x = 0, first_y = 0;
            UINT32 first_word = 0;
            for (UINT y = 0; y < kSide; ++y)
                for (UINT x = 0; x < kSide; ++x) {
                    UINT32 word = 0;
                    std::memcpy(&word, static_cast<const unsigned char*>(data) + y * pitch + x * 4, 4);
                    if (word != expected_word(seed, x, y, g_pso_variants[p.variant].rounds) && !p.different++) {
                        first_x = x; first_y = y; first_word = word;
                    }
                }
            readback->Unmap(0, &empty);
            ++checked;
            if (p.different) {
                ++failed;
                std::printf("pso %3u variant %2u rounds %3u words different %u first x %u y %u got %08x want %08x\n",
                            i, p.variant, g_pso_variants[p.variant].rounds, p.different, first_x, first_y, first_word,
                            expected_word(seed, first_x, first_y, g_pso_variants[p.variant].rounds));
            }
        }
        if (!o.quiet)
            std::printf("pso %3u variant %2u perm %2u rounds %3u create %8.0f us set %8.0f us gpu %7.0f us pixels %s\n",
                        i, p.variant, p.permutation, g_pso_variants[p.variant].rounds, p.create_us, p.set_us, p.gpu_us,
                        p.different ? "DIFFERENT" : "ok");
        std::fflush(stdout);
    }
    CloseHandle(completed);

    const double creates = total_of(pipelines, &Pipeline::create_us);
    const double sets = total_of(pipelines, &Pipeline::set_us);
    const double gpu = total_of(pipelines, &Pipeline::gpu_us);
    std::printf("creates sum %.0f us wall %.0f us longest %.0f us\n", creates, create_wall_us, largest_of(pipelines, &Pipeline::create_us));
    std::printf("first-use SetPipelineState sum %.0f us longest %.0f us\n", sets, largest_of(pipelines, &Pipeline::set_us));
    std::printf("gpu draw and readback sum %.0f us longest %.0f us\n", gpu, largest_of(pipelines, &Pipeline::gpu_us));
    std::printf("caller-thread cost of a pipeline, create plus first use: %.0f us for %u (%.0f us each)\n",
                creates + sets, o.count, (creates + sets) / o.count);
    std::printf("pixels checked %u of %u, different %u, run %llu ms\n", checked, o.count, failed,
                static_cast<unsigned long long>(GetTickCount64() - start_ticks));
    std::printf("D3D12PSO overall %s label=%s count=%u threads=%u create_wall_us=%.0f create_sum_us=%.0f set_sum_us=%.0f caller_us=%.0f gpu_us=%.0f different=%u\n",
                failed ? "FAIL" : "PASS", o.label.c_str(), o.count, o.threads, create_wall_us, creates, sets, creates + sets, gpu, failed);
    return failed ? 4 : 0;
}
