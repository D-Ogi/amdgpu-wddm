// SPDX-License-Identifier: MIT
#pragma once
#include "harness.h"
#include <cstring>

namespace harness::engine_limits {
struct Objects {
    std::vector<IUnknown*> values;
    ~Objects() { for (auto i = values.rbegin(); i != values.rend(); ++i) (*i)->Release(); }
    template<class T> void hold(T* p) { if (p) values.push_back(p); }
};
inline HRESULT buffer(ID3D12Device* device, Objects& objects, D3D12_HEAP_TYPE kind,
                      UINT64 size, D3D12_RESOURCE_STATES state, ID3D12Resource** out) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = kind; heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (kind == D3D12_HEAP_TYPE_DEFAULT) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                __uuidof(ID3D12Resource), reinterpret_cast<void**>(out));
    objects.hold(*out); return hr;
}
inline HRESULT root(ID3D12Device* device, Objects& objects, const D3D12DDI_ROOT_SIGNATURE_0013& desc,
                    ID3D12RootSignature** out) {
    std::vector<uint8_t> bytes;
    HRESULT hr = engine_ddi::serialize_root_signature(&desc, bytes);
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, bytes.data(), bytes.size(),
        __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(out));
    objects.hold(*out); return hr;
}
inline void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                       D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
struct Commands {
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;
    HRESULT open(Env& env, Objects& objects) {
        ID3D12CommandAllocator* allocator = nullptr;
        HRESULT hr = env.engine->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator));
        objects.hold(allocator);
        if (SUCCEEDED(hr)) hr = env.engine->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
            __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
        objects.hold(list);
        BC250_VKD3D_COMMAND_QUEUE_DESC desc{sizeof(desc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
        if (SUCCEEDED(hr)) hr = env.funcs.CreateCommandQueue(env.engine, &desc, nullptr,
            __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
        objects.hold(queue);
        if (SUCCEEDED(hr)) hr = env.engine->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
        objects.hold(fence); return hr;
    }
    bool finish(const char* label) {
        HRESULT hr = list->Close();
        if (SUCCEEDED(hr)) { ID3D12CommandList* lists[] = {list}; queue->ExecuteCommandLists(1, lists); hr = queue->Signal(fence, 1); }
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, nullptr);
        const bool complete = hr == S_OK && fence_reached(fence->GetCompletedValue(), 1);
        checkf(complete, "%s: queue completion (hr %08lx)", label, static_cast<unsigned long>(hr));
        return complete;
    }
};
}
