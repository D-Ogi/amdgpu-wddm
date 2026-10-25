// SPDX-License-Identifier: MIT
// Real-engine content witnesses. Resources, SRVs and dynamic samplers cross the
// DDI bridge; direct engine command recording isolates filtering from replay.
#include "harness.h"
#if defined(D3D12DDI_BUILD_VERSION_0119)
#include "fixture-mandatory-formats.h"
#include "modern-core-harness.h"
#else
#include "fixture-mandatory-formats-legacy.h"
#endif
#include "fixture-mandatory-vs.h"
#include "fixture-mandatory-ps.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace harness {
namespace {
struct Fixture {
    Env& env;
    Device device;
    Buffer textures[3], upload, output, readback;
    void* heaps[2]{};
    void* root_storage = nullptr;
    std::vector<IUnknown*> objects;
    explicit Fixture(Env& e) : env(e) {}
    template<typename T> void hold(T* p) { if (p) objects.push_back(p); }
    ~Fixture() {
        for (auto i = objects.rbegin(); i != objects.rend(); ++i) (*i)->Release();
        if (!device.context) return;
        if (root_storage) env.core.pfnDestroyRootSignature(device.h(), {root_storage});
        for (void* h : heaps) if (h) env.core.pfnDestroyDescriptorHeap(device.h(), {h});
        for (Buffer& b : textures) destroy_buffer(env, device, b);
        for (Buffer* b : {&upload, &output, &readback}) destroy_buffer(env, device, *b);
        uint32_t live = ~0u;
        const HRESULT hr = engine_ddi::destroy_device_context(device.context, &live);
        checkf(hr == S_OK && !live && !device.shell.device_errors,
               "mandatory formats: cleanup, no live objects or DDI errors (hr %08lx, live %u, errors %u)",
               static_cast<unsigned long>(hr), live, device.shell.device_errors);
    }
    ID3D12Resource* resource(const Buffer& b) {
        return static_cast<ID3D12Resource*>(engine_ddi::harness_engine_object(b.resource));
    }
};

HRESULT texture(Fixture& f, DXGI_FORMAT format, UINT width, UINT height, UINT16 mips, Buffer& out, bool render = false) {
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = width;
    res.Height = height;
    res.DepthOrArraySize = 1;
    res.MipLevels = mips;
    res.Format = format;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    if (render) res.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    f.env.core.pfnCheckResourceAllocationInfo(f.device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return E_FAIL;
    const auto props = f.env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = info.ResourceDataSize;
    heap.Alignment = info.ResourceDataAlignment;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = render ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    const auto sizes = f.env.core.pfnCalcPrivateHeapAndResourceSizes(f.device.h(), &heap, &res, {});
    out.heap = f.env.storage.alloc(sizes.Heap);
    out.resource = f.env.storage.alloc(sizes.Resource);
    if (!out.heap || !out.resource) return E_OUTOFMEMORY;
    return f.env.core.pfnCreateHeapAndResource(f.device.h(), &heap, out.hheap(), {&out.rt}, &res, nullptr, {}, out.hres());
}

D3D12_RESOURCE_BARRIER barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    return b;
}

void draw_witness(Env& env) {
    Fixture f(env);
    HRESULT hr = open_device(env, f.device);
    if (SUCCEEDED(hr)) hr = texture(f, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, f.textures[0], true);
    if (SUCCEEDED(hr)) hr = texture(f, DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, f.textures[1], true);
    if (SUCCEEDED(hr)) hr = create_buffer(env, f.device, HeapKind::Upload, 32, false, f.upload);
    if (SUCCEEDED(hr)) hr = create_buffer(env, f.device, HeapKind::Readback, 9216, false, f.readback);
    checkf(hr == S_OK, "mandatory draw: unequal 8x8/4x4 DDI targets and index/readback buffers (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    const UINT16 indices[] = {0, 1, 2, 0, 2, 3, 0, 1, 2, 3};
    void* mapped = nullptr;
    hr = env.core.pfnMapHeap(f.device.h(), f.upload.hheap(), &mapped);
    if (FAILED(hr) || !mapped) { check(false, "mandatory draw: index upload"); return; }
    std::memcpy(mapped, indices, sizeof(indices));
    env.core.pfnUnmapHeap(f.device.h(), f.upload.hheap());
    D3D12DDI_ROOT_SIGNATURE_0013 root_desc{0, nullptr, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    std::vector<uint8_t> root_bytes;
    ID3D12RootSignature* root = nullptr;
    hr = engine_ddi::serialize_root_signature(&root_desc, root_bytes);
    if (SUCCEEDED(hr)) hr = env.engine->CreateRootSignature(0, root_bytes.data(), root_bytes.size(),
        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root));
    f.hold(root);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {g_fixture_mandatory_vs, sizeof(g_fixture_mandatory_vs)};
    desc.PS = {g_fixture_mandatory_ps, sizeof(g_fixture_mandatory_ps)};
    desc.BlendState.IndependentBlendEnable = TRUE;
    for (auto& target : desc.BlendState.RenderTarget) target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 2;
    desc.RTVFormats[0] = desc.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    ID3D12PipelineState* pipeline = nullptr;
    if (SUCCEEDED(hr)) hr = env.engine->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                                   reinterpret_cast<void**>(&pipeline));
    f.hold(pipeline);
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (SUCCEEDED(hr)) hr = env.engine->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                                           reinterpret_cast<void**>(&rtv_heap));
    f.hold(rtv_heap);
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(hr)) hr = env.engine->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator));
    f.hold(allocator);
    if (SUCCEEDED(hr)) hr = env.engine->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, pipeline,
        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
    f.hold(list);
    BC250_VKD3D_COMMAND_QUEUE_DESC queue_desc{sizeof(queue_desc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    if (SUCCEEDED(hr)) hr = env.funcs.CreateCommandQueue(env.engine, &queue_desc, nullptr,
        __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    f.hold(queue);
    if (SUCCEEDED(hr)) hr = env.engine->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                                  reinterpret_cast<void**>(&fence));
    f.hold(fence);
    checkf(hr == S_OK, "mandatory draw: pipeline and direct queue (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    const auto first = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = env.engine->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = {first, {first.ptr + increment}};
    for (UINT i = 0; i < 2; ++i) {
        env.engine->CreateRenderTargetView(f.resource(f.textures[i]), nullptr, rtvs[i]);
        auto b = barrier(f.resource(f.textures[i]), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &b);
    }
    const D3D12_VIEWPORT viewport{0, 0, 4, 4, 0, 1};
    const D3D12_RECT scissor{0, 0, 4, 4};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetGraphicsRootSignature(root);
    for (UINT arm = 0; arm < 3; ++arm) {
        const float clear[] = {0, 1, 0, 1};
        for (auto rtv : rtvs) list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
        list->IASetPrimitiveTopology(arm ? static_cast<D3D_PRIMITIVE_TOPOLOGY>(6) : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        D3D12_INDEX_BUFFER_VIEW index{f.resource(f.upload)->GetGPUVirtualAddress() + (arm == 2 ? 12 : 0),
                                     arm == 2 ? 8u : 12u, DXGI_FORMAT_R16_UINT};
        list->IASetIndexBuffer(&index);
        if (arm == 1) list->DrawInstanced(4, 1, 0, 0);
        else list->DrawIndexedInstanced(arm == 2 ? 4 : 6, 1, 0, 0, 0);
        for (UINT target = 0; target < 2; ++target) {
            auto b = barrier(f.resource(f.textures[target]), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = f.resource(f.textures[target]);
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.pResource = f.resource(f.readback);
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Offset = 3072u * arm + 2048u * target;
            destination.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, target ? 4u : 8u, target ? 4u : 8u, 1, 256};
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            b = barrier(f.resource(f.textures[target]), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ResourceBarrier(1, &b);
        }
    }
    hr = list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* submitted[] = {list};
        queue->ExecuteCommandLists(1, submitted);
        hr = queue->Signal(fence, 1);
    }
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, nullptr);
    const bool complete = hr == S_OK && fence_reached(fence->GetCompletedValue(), 1);
    checkf(complete, "mandatory draw: list control, fan and indexed fan complete (hr %08lx)", static_cast<unsigned long>(hr));
    if (!complete) return;
    mapped = nullptr;
    hr = env.core.pfnMapHeap(f.device.h(), f.readback.hheap(), &mapped);
    if (FAILED(hr) || !mapped) { check(false, "mandatory draw: readback"); return; }
    for (UINT arm = 0; arm < 3; ++arm) {
        UINT bad = 0;
        for (UINT target = 0; target < 2; ++target) {
            const UINT size = target ? 4 : 8;
            const auto* words = reinterpret_cast<const UINT*>(static_cast<const BYTE*>(mapped) + 3072u * arm + 2048u * target);
            char tag[64]{};
            std::snprintf(tag, sizeof(tag), "draw-arm%u-target%u", arm, target);
            save_readback_artifact(tag, words, size, size, 256, DXGI_FORMAT_R8G8B8A8_UNORM);
            for (UINT y = 0; y < size; ++y) for (UINT x = 0; x < size; ++x) {
                const UINT expected = x < 4 && y < 4 ? (target ? 0xffff0000u : 0xff0000ffu) : 0xff00ff00u;
                bad += words[y * 64 + x] != expected;
            }
        }
        checkf(!bad, "mandatory draw: %s, unequal target dimensions, in-range 4x4 viewport, expected pixels (%u differ)",
               arm == 0 ? "triangle-list control" : arm == 1 ? "triangle fan" : "indexed triangle fan", bad);
    }
    env.core.pfnUnmapHeap(f.device.h(), f.readback.hheap());
}
}

void test_mandatory_formats(Env& env) {
    draw_witness(env);
    Fixture f(env);
    HRESULT hr = open_device(env, f.device);
    checkf(hr == S_OK, "mandatory formats: device context (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
#if defined(D3D12DDI_BUILD_VERSION_0119)
    D3D12DDI_DEVICE_FUNCS_CORE_0119 modern{};
    if (!modern_core_table(env, modern)) { check(false, "mandatory formats: modern core table"); return; }
    constexpr UINT extra_sampler_offset = 5;
#else
    constexpr UINT extra_sampler_offset = 4;
#endif
    constexpr UINT sampler_count = extra_sampler_offset + 3;
    constexpr DXGI_FORMAT packed_format = static_cast<DXGI_FORMAT>(191);
    UINT support = 0;
    env.core.pfnCheckFormatSupport(f.device.h(), packed_format, &support);
    constexpr UINT required = D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE | D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER;
    checkf((support & required) == required, "mandatory formats: format 191 DDI sampling/gather bits (0x%x)", support);
    hr = texture(f, packed_format, 2, 2, 1, f.textures[0]);
    if (SUCCEEDED(hr)) hr = texture(f, DXGI_FORMAT_R32_FLOAT, 4, 1, 3, f.textures[1]);
    if (SUCCEEDED(hr)) hr = texture(f, DXGI_FORMAT_R32_FLOAT, 4, 4, 3, f.textures[2]);
    if (SUCCEEDED(hr)) hr = create_buffer(env, f.device, HeapKind::Upload, 4096, false, f.upload);
    if (SUCCEEDED(hr)) hr = create_buffer(env, f.device, HeapKind::Default, 512, true, f.output);
    if (SUCCEEDED(hr)) hr = create_buffer(env, f.device, HeapKind::Readback, 512, false, f.readback);
    checkf(hr == S_OK, "mandatory formats: DDI textures and copy buffers (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    void* mapped = nullptr;
    hr = env.core.pfnMapHeap(f.device.h(), f.upload.hheap(), &mapped);
    if (FAILED(hr) || !mapped) { check(false, "mandatory formats: upload map"); return; }
    auto* bytes = static_cast<BYTE*>(mapped);
    std::memset(bytes, 0, 4096);
    const UINT16 packed[] = {0x1234, 0x5678, 0x9abc, 0xdef0};
    std::memcpy(bytes, packed, 4);
    std::memcpy(bytes + 256, packed + 2, 4);
    const float boundary[] = {10, 20, 30, 40};
    std::memcpy(bytes + 512, boundary, sizeof(boundary));
    std::memcpy(bytes + 3072, boundary, 2 * sizeof(float));
    std::memcpy(bytes + 3584, boundary, sizeof(float));
    const UINT offsets[] = {1024, 2048, 2560};
    for (UINT mip = 0; mip < 3; ++mip)
        for (UINT y = 0; y < (4u >> mip); ++y)
            for (UINT x = 0; x < (4u >> mip); ++x) {
                const float value = 11.0f * (mip + 1);
                std::memcpy(bytes + offsets[mip] + 256 * y + 4 * x, &value, sizeof(value));
            }
    env.core.pfnUnmapHeap(f.device.h(), f.upload.hheap());

    ID3D12DescriptorHeap* engine_heaps[2]{};
    for (UINT k = 0; k < 2; ++k) {
        const auto type = k ? D3D12DDI_DESCRIPTOR_HEAP_TYPE_SAMPLER : D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 desc{type, k ? sampler_count : 3u, D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 1};
        f.heaps[k] = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(f.device.h(), &desc));
        hr = f.heaps[k] ? env.core.pfnCreateDescriptorHeap(f.device.h(), &desc, {f.heaps[k]}) : E_OUTOFMEMORY;
        if (FAILED(hr)) { check(false, "mandatory formats: descriptor heap"); return; }
        engine_heaps[k] = static_cast<ID3D12DescriptorHeap*>(engine_ddi::harness_engine_object(f.heaps[k]));
        const auto cpu = env.core.pfnGetCPUDescriptorHandleForHeapStart(f.device.h(), {f.heaps[k]});
        const UINT increment = env.core.pfnGetDescriptorSizeInBytes(f.device.h(), type);
        for (UINT i = 0; i < (k ? sampler_count : 3u); ++i) {
            const D3D12DDI_CPU_DESCRIPTOR_HANDLE destination{cpu.ptr + SIZE_T{i} * increment};
            if (!k) {
                D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 srv{};
                srv.hDrvResource = f.textures[i].hres();
                srv.Format = i ? DXGI_FORMAT_R32_FLOAT : packed_format;
                srv.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12DDI_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Tex2D = {0, 0, i ? 3u : 1u, 1, 0, 0.0f};
                env.core.pfnCreateShaderResourceView(f.device.h(), &srv, destination);
            } else {
#if defined(D3D12DDI_BUILD_VERSION_0119)
                if (i == 4) {
                    D3D12DDI_SAMPLER_DESC_0096 unnormalized{};
                    unnormalized.Filter = D3D12DDI_FILTER_MIN_MAG_MIP_POINT;
                    unnormalized.AddressU = unnormalized.AddressV = unnormalized.AddressW = D3D12DDI_TEXTURE_ADDRESS_MODE_CLAMP;
                    unnormalized.MaxAnisotropy = 1;
                    unnormalized.ComparisonFunc = D3D12DDI_COMPARISON_FUNC_NEVER;
                    unnormalized.Flags = D3D12DDI_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
                    D3D12DDIARG_CREATE_SAMPLER_0096 args{&unnormalized};
                    modern.pfnCreateSampler(f.device.h(), &args, destination);
                    continue;
                }
#endif
                D3D12DDI_SAMPLER_DESC sampler{};
                const UINT filters[] = {0, 0x54, 0x4, 0x10};
                const UINT extra_filters[] = {0x154, 0x1d4, 0xd4};
                sampler.Filter = static_cast<D3D12DDI_FILTER>(i < extra_sampler_offset ? filters[i] : extra_filters[i - extra_sampler_offset]);
                sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12DDI_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.MaxAnisotropy = i == 1 || i >= extra_sampler_offset ? 4u : 1u;
                sampler.ComparisonFunc = i == extra_sampler_offset + 2 ? D3D12DDI_COMPARISON_FUNC_LESS_EQUAL : D3D12DDI_COMPARISON_FUNC_NEVER;
                sampler.MaxLOD = 1000;
                D3D12DDIARG_CREATE_SAMPLER args{&sampler};
                env.core.pfnCreateSampler(f.device.h(), &args, destination);
            }
        }
    }
    check(!f.device.shell.device_errors, "mandatory formats: DDI SRVs, point and anisotropic mip-point samplers");
    if (f.device.shell.device_errors) return;
    D3D12DDI_DESCRIPTOR_RANGE_0013 ranges[] = {
        {D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE, 0},
        {D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER, 2, 0, 0, D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE, 0},
        {D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER, extra_sampler_offset - 2, 3, 0, D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE, 2},
        {D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER, 3, 7, 0, D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE, extra_sampler_offset},
    };
    D3D12DDI_ROOT_PARAMETER_0013 parameters[3]{};
    for (UINT i = 0; i < 3; ++i) parameters[i].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    for (UINT i = 0; i < 2; ++i) {
        parameters[i].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = {i ? 3u : 1u, &ranges[i]};
    }
    parameters[2].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    D3D12DDI_STATIC_SAMPLER sampler{};
    sampler.Filter = static_cast<D3D12DDI_FILTER>(0x54);
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12DDI_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 4;
    sampler.ComparisonFunc = D3D12DDI_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = 1000;
    sampler.ShaderRegister = 2;
    sampler.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    ID3D12RootSignature* root = nullptr;
#if defined(D3D12DDI_BUILD_VERSION_0119)
    D3D12DDI_STATIC_SAMPLER_0100 static_samplers[5]{};
    static_assert(offsetof(D3D12DDI_STATIC_SAMPLER_0100, Flags) == sizeof(sampler));
    std::memcpy(&static_samplers[0], &sampler, sizeof(sampler));
    static_samplers[1] = static_samplers[0];
    static_samplers[1].Filter = D3D12DDI_FILTER_MIN_MAG_MIP_POINT;
    static_samplers[1].MaxAnisotropy = 1;
    static_samplers[1].MaxLOD = 0;
    static_samplers[1].ShaderRegister = 6;
    static_samplers[1].Flags = D3D12DDI_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
    const UINT extra_filters[] = {0x154, 0x1d4, 0xd4};
    for (UINT i = 0; i < 3; ++i) {
        static_samplers[2 + i] = static_samplers[0];
        static_samplers[2 + i].Filter = static_cast<D3D12DDI_FILTER>(extra_filters[i]);
        static_samplers[2 + i].ShaderRegister = 10 + i;
        if (i == 2) static_samplers[2 + i].ComparisonFunc = D3D12DDI_COMPARISON_FUNC_LESS_EQUAL;
    }
    D3D12DDI_ROOT_SIGNATURE_0100 root_desc{3, parameters, 5, static_samplers, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100 root_args{};
    root_args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_2;
    root_args.pRootSignature_1_2 = &root_desc;
    f.root_storage = env.storage.alloc(modern.pfnCalcPrivateRootSignatureSize(f.device.h(), &root_args));
    hr = f.root_storage ? modern.pfnCreateRootSignature(f.device.h(), &root_args, {f.root_storage}) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) root = static_cast<ID3D12RootSignature*>(engine_ddi::harness_engine_object(f.root_storage));
#else
    D3D12DDI_STATIC_SAMPLER static_samplers[4] = {sampler, sampler, sampler, sampler};
    const UINT extra_filters[] = {0x154, 0x1d4, 0xd4};
    for (UINT i = 0; i < 3; ++i) {
        static_samplers[1 + i].Filter = static_cast<D3D12DDI_FILTER>(extra_filters[i]);
        static_samplers[1 + i].ShaderRegister = 10 + i;
        if (i == 2) static_samplers[1 + i].ComparisonFunc = D3D12DDI_COMPARISON_FUNC_LESS_EQUAL;
    }
    D3D12DDI_ROOT_SIGNATURE_0013 root_desc{3, parameters, 4, static_samplers, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    std::vector<uint8_t> root_bytes;
    hr = engine_ddi::serialize_root_signature(&root_desc, root_bytes);
    if (SUCCEEDED(hr)) hr = env.engine->CreateRootSignature(0, root_bytes.data(), root_bytes.size(),
        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root));
    f.hold(root);
#endif
    ID3D12PipelineState* pipeline = nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = root;
    pipeline_desc.CS = {g_fixture_mandatory_formats, sizeof(g_fixture_mandatory_formats)};
    if (SUCCEEDED(hr)) hr = env.engine->CreateComputePipelineState(&pipeline_desc, __uuidof(ID3D12PipelineState),
                                                                  reinterpret_cast<void**>(&pipeline));
    f.hold(pipeline);
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    if (SUCCEEDED(hr)) hr = env.engine->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
        __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator));
    f.hold(allocator);
    if (SUCCEEDED(hr)) hr = env.engine->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, pipeline,
        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
    f.hold(list);
    BC250_VKD3D_COMMAND_QUEUE_DESC queue_desc{sizeof(queue_desc), D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0};
    if (SUCCEEDED(hr)) hr = env.funcs.CreateCommandQueue(env.engine, &queue_desc, nullptr,
        __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    f.hold(queue);
    if (SUCCEEDED(hr)) hr = env.engine->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                                  reinterpret_cast<void**>(&fence));
    f.hold(fence);
    checkf(hr == S_OK, "mandatory formats: compute pipeline and engine queue (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    for (UINT i = 0; i < 3; ++i) {
        auto b = barrier(f.resource(f.textures[i]), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b);
        for (UINT mip = 0; mip < (i ? 3u : 1u); ++mip) {
            D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
            source.pResource = f.resource(f.upload);
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            const UINT boundary_offsets[] = {512, 3072, 3584};
            source.PlacedFootprint.Offset = i == 0 ? 0 : i == 1 ? boundary_offsets[mip] : offsets[mip];
            source.PlacedFootprint.Footprint = {i ? DXGI_FORMAT_R32_FLOAT : packed_format,
                i == 0 ? 2u : 4u >> mip, i == 0 ? 2u : i == 1 ? 1u : 4u >> mip, 1, 256};
            destination.pResource = f.resource(f.textures[i]);
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = mip;
            list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        b = barrier(f.resource(f.textures[i]), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &b);
    }
    auto b = barrier(f.resource(f.output), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &b);
    list->SetDescriptorHeaps(2, engine_heaps);
    list->SetComputeRootSignature(root);
    for (UINT i = 0; i < 2; ++i) list->SetComputeRootDescriptorTable(i, engine_heaps[i]->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(2, f.resource(f.output)->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    b = barrier(f.resource(f.output), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
    list->CopyBufferRegion(f.resource(f.readback), 0, f.resource(f.output), 0, 352);
    hr = list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* submitted[] = {list};
        queue->ExecuteCommandLists(1, submitted);
        hr = queue->Signal(fence, 1);
    }
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, nullptr);
    const bool complete = hr == S_OK && fence_reached(fence->GetCompletedValue(), 1);
    checkf(complete, "mandatory formats: dispatch/copy completed (hr %08lx)", static_cast<unsigned long>(hr));
    if (!complete) return;
    mapped = nullptr;
    hr = env.core.pfnMapHeap(f.device.h(), f.readback.hheap(), &mapped);
    if (FAILED(hr) || !mapped) { check(false, "mandatory formats: readback map"); return; }
    const auto* words = static_cast<const UINT*>(mapped);
#if defined(D3D12DDI_BUILD_VERSION_0119)
    save_readback_artifact("formats-and-filtering", words, 88, 1, 352, DXGI_FORMAT_R32_UINT);
#else
    // NO_UNNORMALIZED leaves bytes224..255 unwritten. Never capture undefined contents.
    save_readback_artifact("formats-filtering-legacy-prefix", words, 56, 1, 224, DXGI_FORMAT_R32_UINT);
    save_readback_artifact("formats-filtering-legacy-reduction", words + 64, 24, 1, 96, DXGI_FORMAT_R32_UINT);
#endif
    UINT load_bad = 0, sample_bad = 0, gather_bad = 0, boundary_bad = 0, mip_bad = 0;
    for (UINT i = 0; i < 16; ++i) {
        const UINT expected = (packed[i / 4] >> (12 - 4 * (i % 4))) & 15;
        load_bad += words[i] != expected;
        sample_bad += words[16 + i] != expected;
    }
    const UINT gather[] = {9, 13, 5, 1};
    const UINT boundary_expected[] = {10, 10, 10, 20, 20, 20, 30, 40};
    const UINT mip_expected[] = {11, 22, 22, 33};
    for (UINT i = 0; i < 4; ++i) gather_bad += words[32 + i] != gather[i];
    for (UINT i = 0; i < 8; ++i) boundary_bad += words[36 + i] != boundary_expected[i];
    for (UINT i = 0; i < 4; ++i) mip_bad += words[44 + i] != mip_expected[i] || words[48 + i] != mip_expected[i];
    checkf(!load_bad && !sample_bad && !gather_bad,
           "mandatory formats: A4B4G4R4 packed RGBA nibbles load/sample/gather (%u/%u/%u differences)",
           load_bad, sample_bad, gather_bad);
    checkf(!boundary_bad,
           "mandatory formats: point samples below/exact texel boundaries never round up (%u differences; %u %u %u %u %u %u %u %u)",
           boundary_bad, words[36], words[37], words[38], words[39], words[40], words[41], words[42], words[43]);
    checkf(!mip_bad,
           "mandatory formats: dynamic/static anisotropic point-mip LOD0.49/0.51/1.49/1.51 (%u differences; dynamic %u %u %u %u; static %u %u %u %u)",
           mip_bad, words[44], words[45], words[46], words[47], words[48], words[49], words[50], words[51]);
    for (UINT filter = 0; filter < 3; ++filter) {
        UINT bad = 0;
        for (UINT i = 0; i < 4; ++i) {
            const UINT expected = filter == 2 ? (mip_expected[i] >= 16 ? 1u : 0u) : mip_expected[i];
            bad += words[64 + 4 * filter + i] != expected || words[76 + 4 * filter + i] != expected;
        }
        checkf(!bad, "mandatory formats: %s dynamic/static anisotropic point-mip explicit LOD boundary (%u differences)",
               filter == 0 ? "minimum" : filter == 1 ? "maximum" : "comparison", bad);
    }
    checkf(words[52] == 10 && words[53] == 15 && words[54] == 10 && words[55] == 15,
           "mandatory formats: mixed min/mag point boundaries and linear controls (got %u %u %u %u, expected10/15/10/15)",
           words[52], words[53], words[54], words[55]);
#if defined(D3D12DDI_BUILD_VERSION_0119)
    bool unnormalized_ok = true;
    for (UINT i = 0; i < 4; ++i) unnormalized_ok = unnormalized_ok && words[56 + i] == 5 + i && words[60 + i] == 9 + i;
    checkf(unnormalized_ok,
           "mandatory formats: modern dynamic/static unnormalized samplers select texels at pixel coordinates (%u %u %u %u / %u %u %u %u)",
           words[56], words[57], words[58], words[59], words[60], words[61], words[62], words[63]);
#endif
    env.core.pfnUnmapHeap(f.device.h(), f.readback.hheap());
}
} // namespace harness
