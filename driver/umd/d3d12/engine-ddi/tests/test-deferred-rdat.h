// SPDX-License-Identifier: MIT
// Included by test-raytracing.cpp. Direct engine API is intentional: the shell's
// translation retains imported collections and would mask the engine defect.
#pragma once
#include "fixture-deferred-rdat.h"

namespace harness {
namespace deferred_rdat {
struct Objects {
    std::vector<IUnknown*> values;
    ~Objects() { for (auto i = values.rbegin(); i != values.rend(); ++i) (*i)->Release(); }
    template <typename T> void hold(T* value) { if (value) values.push_back(value); }
};

inline HRESULT buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 size, ID3D12Resource** result) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ :
        type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           __uuidof(ID3D12Resource), reinterpret_cast<void**>(result));
}

inline HRESULT chain(ID3D12Device5* device, ID3D12RootSignature* root, bool release_parents,
                     Objects& objects, ID3D12StateObject** pipeline) {
    std::vector<BYTE> caller(std::begin(g_fixture_deferred_rdat), std::end(g_fixture_deferred_rdat));
    D3D12_DXIL_LIBRARY_DESC library{{caller.data(), caller.size()}, 0, nullptr};
    D3D12_GLOBAL_ROOT_SIGNATURE global{root};
    D3D12_STATE_OBJECT_CONFIG config{D3D12_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS};
    D3D12_STATE_SUBOBJECT subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
        {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global},
        {D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &library},
    };
    D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, 3, subobjects};
    ID3D12StateObject* current = nullptr;
    HRESULT hr = device->CreateStateObject(&desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(&current));
    std::fill(caller.begin(), caller.end(), BYTE{0xdd});
    std::vector<BYTE>().swap(caller);
    if (FAILED(hr)) return hr;
    // No config exists in the fixture or this description. A successful collection
    // with no shader identifier witnesses the engine's missing-config defer route.
    for (unsigned generation = 0; generation < 3; ++generation) {
        ID3D12StateObjectProperties* properties = nullptr;
        hr = current->QueryInterface(__uuidof(ID3D12StateObjectProperties), reinterpret_cast<void**>(&properties));
        const bool deferred = SUCCEEDED(hr) && !properties->GetShaderIdentifier(L"deferred_raygen");
        if (properties) properties->Release();
        checkf(deferred, "deferred RDAT: %s generation %u has no compiled shader identifier (missing config)",
               release_parents ? "released-parent" : "retained-parent control", generation);
        if (!deferred) { current->Release(); return E_FAIL; }
        if (generation == 2) break;
        D3D12_EXISTING_COLLECTION_DESC import{current, 0, nullptr};
        D3D12_STATE_SUBOBJECT next_subobjects[] = {
            {D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG, &config},
            {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &import},
        };
        D3D12_STATE_OBJECT_DESC next_desc{D3D12_STATE_OBJECT_TYPE_COLLECTION, 2, next_subobjects};
        ID3D12StateObject* next = nullptr;
        hr = device->CreateStateObject(&next_desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(&next));
        if (release_parents || FAILED(hr)) current->Release();
        else objects.hold(current);
        if (FAILED(hr)) return hr;
        current = next;
    }
    D3D12_EXISTING_COLLECTION_DESC import{current, 0, nullptr};
    D3D12_RAYTRACING_SHADER_CONFIG shader_config{4, 8};
    D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
    D3D12_STATE_SUBOBJECT final_subobjects[] = {
        {D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION, &import},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shader_config},
        {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipeline_config},
    };
    D3D12_STATE_OBJECT_DESC final_desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, 3, final_subobjects};
    hr = device->CreateStateObject(&final_desc, __uuidof(ID3D12StateObject), reinterpret_cast<void**>(pipeline));
    if (release_parents || FAILED(hr)) current->Release();
    else objects.hold(current);
    objects.hold(*pipeline);
    return hr;
}

inline void run(Env& env) {
    Objects objects;
    ID3D12Device5* device = nullptr;
    HRESULT hr = env.engine->QueryInterface(__uuidof(ID3D12Device5), reinterpret_cast<void**>(&device));
    objects.hold(device);
    if (FAILED(hr)) { check(false, "deferred RDAT: ID3D12Device5"); return; }
    D3D12DDI_ROOT_PARAMETER_0013 parameter{};
    parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    parameter.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    D3D12DDI_ROOT_SIGNATURE_0013 root_desc{1, &parameter, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    std::vector<uint8_t> root_bytes;
    ID3D12RootSignature* root = nullptr;
    hr = engine_ddi::serialize_root_signature(&root_desc, root_bytes);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, root_bytes.data(), root_bytes.size(),
        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&root));
    objects.hold(root);
    checkf(hr == S_OK, "deferred RDAT: global root signature (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    ID3D12StateObject* pipelines[2]{};
    // Control first; its ancestors remain alive until after GPU completion.
    for (unsigned arm = 0; arm < 2; ++arm) {
        hr = chain(device, root, arm != 0, objects, &pipelines[arm]);
        checkf(hr == S_OK, "deferred RDAT: %s links after two deferred imports (hr %08lx)",
               arm ? "released-parent" : "retained-parent control", static_cast<unsigned long>(hr));
        if (FAILED(hr)) return;
    }
    ID3D12Resource *upload = nullptr, *output = nullptr, *readback = nullptr;
    hr = buffer(device, D3D12_HEAP_TYPE_UPLOAD, 128, &upload);
    objects.hold(upload);
    if (SUCCEEDED(hr)) hr = buffer(device, D3D12_HEAP_TYPE_DEFAULT, 512, &output);
    objects.hold(output);
    if (SUCCEEDED(hr)) hr = buffer(device, D3D12_HEAP_TYPE_READBACK, 512, &readback);
    objects.hold(readback);
    checkf(hr == S_OK, "deferred RDAT: dispatch buffers (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    constexpr UINT value = 0xb0252310u;
    void* mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr)) { check(false, "deferred RDAT: upload map"); return; }
    bool identifiers = true;
    for (unsigned arm = 0; arm < 2; ++arm) {
        ID3D12StateObjectProperties* properties = nullptr;
        hr = pipelines[arm]->QueryInterface(__uuidof(ID3D12StateObjectProperties), reinterpret_cast<void**>(&properties));
        const void* id = SUCCEEDED(hr) ? properties->GetShaderIdentifier(L"deferred_raygen") : nullptr;
        identifiers = identifiers && id;
        if (id) std::memcpy(static_cast<BYTE*>(mapped) + 64 * arm, id, D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        std::memcpy(static_cast<BYTE*>(mapped) + 64 * arm + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, &value, sizeof(value));
        if (properties) properties->Release();
    }
    upload->Unmap(0, nullptr);
    check(identifiers, "deferred RDAT: both final pipelines expose raygen identifiers");
    if (!identifiers) return;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList4* list = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, __uuidof(ID3D12CommandAllocator),
                                       reinterpret_cast<void**>(&allocator));
    objects.hold(allocator);
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator, nullptr,
        __uuidof(ID3D12GraphicsCommandList4), reinterpret_cast<void**>(&list));
    objects.hold(list);
    BC250_VKD3D_COMMAND_QUEUE_DESC queue_desc{sizeof(queue_desc), D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0};
    if (SUCCEEDED(hr)) hr = env.funcs.CreateCommandQueue(env.engine, &queue_desc, nullptr,
        __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    objects.hold(queue);
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                              reinterpret_cast<void**>(&fence));
    objects.hold(fence);
    checkf(hr == S_OK, "deferred RDAT: direct engine recording and queue (hr %08lx)", static_cast<unsigned long>(hr));
    if (FAILED(hr)) return;
    list->SetComputeRootSignature(root);
    for (unsigned arm = 0; arm < 2; ++arm) {
        list->SetPipelineState1(pipelines[arm]);
        list->SetComputeRootUnorderedAccessView(0, output->GetGPUVirtualAddress() + 256 * arm);
        D3D12_DISPATCH_RAYS_DESC rays{};
        rays.RayGenerationShaderRecord = {upload->GetGPUVirtualAddress() + 64 * arm, 36};
        rays.Width = 64;
        rays.Height = rays.Depth = 1;
        list->DispatchRays(&rays);
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {output, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE};
    list->ResourceBarrier(1, &barrier);
    list->CopyBufferRegion(readback, 0, output, 0, 512);
    hr = list->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* submitted[] = {list};
        queue->ExecuteCommandLists(1, submitted);
        hr = queue->Signal(fence, 1);
    }
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, nullptr);
    const bool complete = hr == S_OK && fence_reached(fence->GetCompletedValue(), 1);
    checkf(complete, "deferred RDAT: both dispatches complete (hr %08lx)", static_cast<unsigned long>(hr));
    if (!complete) return;
    mapped = nullptr;
    hr = readback->Map(0, nullptr, &mapped);
    unsigned differences = 0;
    if (SUCCEEDED(hr)) {
        const auto* words = static_cast<const UINT*>(mapped);
        for (unsigned i = 0; i < 64; ++i)
            differences += words[i] != value + i || words[64 + i] != value + i || words[i] != words[64 + i];
        readback->Unmap(0, nullptr);
    }
    checkf(hr == S_OK && !differences,
           "deferred RDAT: released ancestors match retained control and 64 expected local-root words (%u differ)",
           differences);
}
} // namespace deferred_rdat
} // namespace harness
