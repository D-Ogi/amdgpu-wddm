// SPDX-License-Identifier: MIT
#pragma once
#include "engine-ddi/engine-ddi.h"
#include "../dxvk/runtime-domain.h"
#include "paging.h"
namespace native12 {
struct Device;
// Calls require the owner's serialized DeviceScope, including hosted GIPA and
// RuntimeDomain scopes. No callback or Vulkan destruction runs in the destructor.
class RuntimeHeapImports final {
    struct Record;
    D3D12DDI_HRTDEVICE runtime_{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 callbacks_{};
    D3DDDI_DEVICECALLBACKS kernel_{};
    bc250::umd::RuntimeDomain& domain_;
    PagingDomain paging_;
    VkPhysicalDevice physical_{};
    VkDevice device_{};
    VkInstance instance_{};
    PFN_vkGetInstanceProcAddr gipa_{};
    void* identity_{};
    VkPhysicalDeviceMemoryProperties properties_{};
    PFN_vkAllocateMemory allocate_{};
    PFN_vkFreeMemory free_{};
    PFN_vkCreateBuffer create_buffer_{};
    PFN_vkDestroyBuffer destroy_buffer_{};
    PFN_vkGetBufferMemoryRequirements requirements_{};
    Record* records_{};
    bool active_{true},initialized_{},paging_open_{};
    Record* find(D3DKMT_HANDLE) const noexcept;
    HRESULT release(Record&) noexcept;
    void erase(Record*) noexcept;
public:
    RuntimeHeapImports(Device&,bc250::umd::RuntimeDomain&,VkPhysicalDevice,VkDevice,
                       VkInstance,PFN_vkGetInstanceProcAddr,void* host_identity) noexcept;
    ~RuntimeHeapImports();
    RuntimeHeapImports(const RuntimeHeapImports&)=delete;
    RuntimeHeapImports& operator=(const RuntimeHeapImports&)=delete;
    HRESULT initialize() noexcept;
    HRESULT allocate(const engine_ddi::MemoryRequest*,engine_ddi::ImportedMemory*) noexcept;
    HRESULT free(const engine_ddi::ImportedMemory*) noexcept;
    // Only these two operations are provided for RADV's borrowed allocation map.
    bool owns_allocation(D3DKMT_HANDLE) const noexcept;
    HRESULT dispatch(uint32_t operation,void* argument) noexcept;
    // Frees only records already retired by free(), or never handed to engine.
    // An active imported heap makes close fail; no GPU retirement is invented.
    HRESULT close_after_engine_retirement() noexcept;
    // Terminal CPU metadata release. Invalidates all runtime authority, does not
    // free outstanding Vulkan/kernel objects, and reports their retained owners.
    unsigned discard_metadata() noexcept;
};
}
