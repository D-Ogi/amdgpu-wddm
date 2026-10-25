// SPDX-License-Identifier: MIT
// Direct engine witness for OPTIONS19: no RTV/DSV, SampleDesc.Count, per-sample UAV writes.
#include "test-engine-limits.h"
#include "fixture-limits-vs.h"
#include "fixture-limits-ps.h"
#include <cstdio>

namespace harness {
void test_no_output_samples(Env& env) {
    using namespace engine_limits;
    Objects objects;
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 caps{};
    HRESULT hr = env.engine->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &caps, sizeof(caps));
    check(hr == S_OK && (caps.SupportedSampleCountsWithNoOutputs & 1), "no-output samples: OPTIONS19 and single sample");
    if (FAILED(hr)) return;
    check((caps.SupportedSampleCountsWithNoOutputs & 0x1d) == 0x1d,
          "DDI0102 acceptance: all no-output counts 1/4/8/16 must be supported; a skipped arm cannot pass");
    D3D12DDI_ROOT_PARAMETER_0013 parameter{};
    parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    parameter.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    D3D12DDI_ROOT_SIGNATURE_0013 signature{1, &parameter, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    ID3D12RootSignature* rs = nullptr;
    hr = root(env.engine, objects, signature, &rs);
    constexpr UINT bytes = 4 * 16 * 16 * sizeof(UINT);
    ID3D12Resource *output = nullptr, *upload = nullptr, *readback = nullptr;
    if (SUCCEEDED(hr)) hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, &output);
    if (SUCCEEDED(hr)) hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ, &upload);
    if (SUCCEEDED(hr)) hr = buffer(env.engine, objects, D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, &readback);
    void* mapped = nullptr;
    if (SUCCEEDED(hr)) hr = upload->Map(0, nullptr, &mapped);
    if (SUCCEEDED(hr)) { std::memset(mapped, 0, bytes); upload->Unmap(0, nullptr); }
    Commands commands;
    if (SUCCEEDED(hr)) hr = commands.open(env, objects);
    checkf(hr == S_OK, "no-output samples: setup (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    auto* list = commands.list;
    list->CopyBufferRegion(output, 0, upload, 0, bytes);
    transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetGraphicsRootSignature(rs);
    const D3D12_VIEWPORT viewport{0, 0, 16, 16, 0, 1}; const D3D12_RECT scissor{0, 0, 16, 16};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT counts[] = {1, 4, 8, 16}; bool drawn[4]{};
    for (UINT arm = 0; arm != 4; ++arm) {
        if (!(caps.SupportedSampleCountsWithNoOutputs & counts[arm])) {
            std::printf("SKIP no-output samples: count %u absent from native mask %#x\n", counts[arm], caps.SupportedSampleCountsWithNoOutputs);
            continue;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rs; desc.VS = {g_limits_vs, sizeof(g_limits_vs)}; desc.PS = {g_limits_ps, sizeof(g_limits_ps)};
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE; desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; desc.SampleDesc.Count = counts[arm];
        ID3D12PipelineState* pso = nullptr;
        hr = env.engine->CreateGraphicsPipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&pso));
        objects.hold(pso);
        checkf(hr == S_OK, "no-output samples: SampleDesc.Count=%u pipeline (hr %08lx)", counts[arm], static_cast<unsigned long>(hr));
        if (FAILED(hr)) continue;
        list->SetPipelineState(pso); list->SetGraphicsRootUnorderedAccessView(0, output->GetGPUVirtualAddress() + arm * 1024);
        list->DrawInstanced(3, 1, 0, 0); drawn[arm] = true;
    }
    transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback, 0, output, 0, bytes);
    if (!commands.finish("no-output samples")) return;
    hr = readback->Map(0, nullptr, &mapped);
    check(hr == S_OK, "no-output samples: readback Map"); if (FAILED(hr)) return;
    const auto* words = static_cast<const UINT*>(mapped);
    for (UINT arm = 0; arm != 4; ++arm) if (drawn[arm]) {
        char tag[64]{};
        std::snprintf(tag, sizeof(tag), "no-output-samples-%u", counts[arm]);
        save_readback_artifact(tag, words + arm * 256, 16, 16, 64, DXGI_FORMAT_R32_UINT);
        UINT bad = 0, expected = (1u << counts[arm]) - 1;
        for (UINT pixel = 0; pixel != 256; ++pixel) bad += words[arm * 256 + pixel] != expected;
        checkf(!bad, "no-output samples: count %u covers every sample in 256 pixels (%u mismatches)", counts[arm], bad);
    }
    readback->Unmap(0, nullptr);
}
}
