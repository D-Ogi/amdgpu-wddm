// SPDX-License-Identifier: MIT
// The highest advertised sampler descriptor must be addressable alongside a static sampler.
#include "test-engine-limits.h"
#include "fixture-limits-cs.h"

namespace harness {
void test_descriptor_limits(Env& env) {
    using namespace engine_limits;
    Objects objects;
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 caps{};
    HRESULT hr = env.engine->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &caps, sizeof(caps));
    check(hr == S_OK && caps.MaxSamplerDescriptorHeapSizeWithStaticSamplers >= 2048,
          "descriptor limits: OPTIONS19 static sampler limit >=2048");
    if (FAILED(hr) || !caps.MaxSamplerDescriptorHeapSizeWithStaticSamplers) return;
    check(caps.MaxSamplerDescriptorHeapSize >= 4000 && caps.MaxSamplerDescriptorHeapSizeWithStaticSamplers >= 2048 &&
              caps.MaxViewDescriptorHeapSize >= 1000000,
          "DDI0102 acceptance: sampler/static/view capacities meet 4000/2048/1000000 minima");
    const UINT count = caps.MaxSamplerDescriptorHeapSizeWithStaticSamplers;
    ID3D12DescriptorHeap *samplers = nullptr, *views = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, count, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    hr = env.engine->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&samplers));
    objects.hold(samplers);
    checkf(hr == S_OK, "descriptor limits: allocate advertised sampler maximum %u (hr %08lx)", count, static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    hr = env.engine->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&views));
    objects.hold(views);
    const D3D12DDI_DESCRIPTOR_RANGE_0013 ranges[] = {
        {D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE, 0},
        {D3D12DDI_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE, 0}};
    D3D12DDI_ROOT_PARAMETER_0013 parameters[3]{};
    for (UINT i = 0; i < 2; ++i) {
        parameters[i].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = {1, &ranges[i]};
        parameters[i].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    }
    parameters[2].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
    parameters[2].Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    parameters[2].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    D3D12DDI_STATIC_SAMPLER fixed{};
    fixed.Filter = D3D12DDI_FILTER_MIN_MAG_MIP_POINT;
    fixed.AddressU = fixed.AddressV = fixed.AddressW = D3D12DDI_TEXTURE_ADDRESS_MODE_CLAMP;
    fixed.MaxAnisotropy = 1; fixed.ComparisonFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS;
    fixed.BorderColor = D3D12DDI_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    fixed.ShaderRegister = 1; fixed.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    // Occupy the full live-static budget; the shader reads its last slot as well as the dynamic heap's last slot.
    constexpr UINT static_count = 2032;
    std::vector<D3D12DDI_STATIC_SAMPLER> statics(static_count, fixed);
    for (UINT i = 0; i < static_count; ++i) statics[i].ShaderRegister = i + 1;
    D3D12DDI_ROOT_SIGNATURE_0013 signature{3, parameters, static_count, statics.data(), D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    ID3D12RootSignature* rs = nullptr;
    if (SUCCEEDED(hr)) hr = root(env.engine, objects, signature, &rs);
    ID3D12PipelineState* pso = nullptr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = rs; pipeline.CS = {g_limits_cs, sizeof(g_limits_cs)};
    if (SUCCEEDED(hr)) hr = env.engine->CreateComputePipelineState(&pipeline, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
    objects.hold(pso);
    D3D12_RESOURCE_DESC texture_desc{};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; texture_desc.Width = 2;
    texture_desc.Height = texture_desc.DepthOrArraySize = texture_desc.MipLevels = texture_desc.SampleDesc.Count = 1;
    texture_desc.Format = DXGI_FORMAT_R32_FLOAT;
    D3D12_HEAP_PROPERTIES texture_heap{}; texture_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    texture_heap.CreationNodeMask = texture_heap.VisibleNodeMask = 1;
    ID3D12Resource *texture = nullptr, *upload = nullptr, *output = nullptr, *readback = nullptr;
    if (SUCCEEDED(hr)) hr = env.engine->CreateCommittedResource(&texture_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&texture));
    objects.hold(texture);
    if (SUCCEEDED(hr)) hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ, &upload);
    if (SUCCEEDED(hr)) hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_DEFAULT, 16, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &output);
    if (SUCCEEDED(hr)) hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    void* mapped = nullptr;
    if (SUCCEEDED(hr)) hr = upload->Map(0, nullptr, &mapped);
    if (SUCCEEDED(hr)) { const float values[] = {0.25f, 0.75f}; std::memset(mapped, 0, 256); std::memcpy(mapped, values, sizeof(values)); upload->Unmap(0, nullptr); }
    Commands commands;
    if (SUCCEEDED(hr)) hr = commands.open(env, objects);
    checkf(hr == S_OK, "descriptor limits: texture and compute setup (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    env.engine->CreateShaderResourceView(texture, &srv, views->GetCPUDescriptorHandleForHeapStart());
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    auto cpu = samplers->GetCPUDescriptorHandleForHeapStart();
    const UINT increment = env.engine->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    env.engine->CreateSampler(&sampler, cpu);
    // Write the last descriptor with a different addressing mode. Aliasing/wrapping to slot0 must fail readback.
    cpu.ptr += static_cast<SIZE_T>(count - 1) * increment;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    env.engine->CreateSampler(&sampler, cpu);
    auto* list = commands.list;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = upload; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_FLOAT, 2, 1, 1, 256};
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = texture; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    transition(list, texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = {views, samplers}; list->SetDescriptorHeaps(2, heaps);
    list->SetComputeRootSignature(rs); list->SetPipelineState(pso);
    list->SetComputeRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
    for (UINT arm = 0; arm < 2; ++arm) {
        auto gpu = samplers->GetGPUDescriptorHandleForHeapStart();
        if (arm) gpu.ptr += static_cast<UINT64>(count - 1) * increment;
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetComputeRootUnorderedAccessView(2, output->GetGPUVirtualAddress() + 8 * arm);
        list->Dispatch(1, 1, 1);
    }
    transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback, 0, output, 0, 16);
    if (!commands.finish("descriptor limits")) return;
    hr = readback->Map(0, nullptr, &mapped);
    bool correct = false;
    if (SUCCEEDED(hr)) {
        const auto* values = static_cast<const float*>(mapped);
        save_readback_artifact("descriptor-limits", values, 4, 1, 16, DXGI_FORMAT_R32_FLOAT);
        correct = values[0] == 0.25f && values[1] == 0.75f && values[2] == 0.75f && values[3] == 0.75f;
        readback->Unmap(0, nullptr);
    }
    checkf(correct, "descriptor limits: slots0/%u and static s2032 produce correct samples with 2032 static descriptors", count - 1);
    // Above-limit allocation must not succeed, including when static samplers are absent.
    if (caps.MaxSamplerDescriptorHeapSize != UINT_MAX) {
        heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, caps.MaxSamplerDescriptorHeapSize + 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        ID3D12DescriptorHeap* excess = nullptr;
        hr = env.engine->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&excess));
        objects.hold(excess); check(FAILED(hr), "descriptor limits: advertised maximum+1 is refused");
    }
}
}
