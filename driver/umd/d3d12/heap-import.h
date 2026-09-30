// SPDX-License-Identifier: MIT
#pragma once
#include "engine-ddi/engine-ddi.h"
#include "../dxvk/runtime-domain.h"
#include "paging.h"
#include <atomic>
namespace native12 {
struct Device;
// Calls run inside the owner's DeviceScope, including hosted GIPA and RuntimeDomain scopes, on any
// number of the device's DDI threads at once. lock_ guards the record list and every record's flags
// and is a leaf: no runtime callback, Vulkan call or paging operation runs under it. A record in a
// callback is busy; a record being released belongs to the releasing thread, whose own re-entrant
// Unlock2 (RADV's vkFreeMemory) is still admitted. The paging queue is created once, on first use,
// under its own lock (the one lock held across a runtime callback: CreatePagingQueueCb, a kernel
// call that does not enter this module). No callback or Vulkan destruction runs in the destructor.
// Where the latest allocate() of the calling thread ended, for the diagnostic trace: the stage that
// returned, not a cause.
enum class ImportStage : uint32_t {
    Done,Request,Surface,Probe,MemoryType,PagingQueue,AllocateCallback,Map,MapReady,AddressAlignment,Import,
    Resident                                    // appended: trace values of the others stay as they were
};
// Where the latest free() ended, by the same rule.
enum class FreeStage : uint32_t { Done,Request,Record,VulkanFree,Unmap,Deallocate };
struct FreeReport {
    FreeStage stage{};
    bool surface{};
    bool owner_expired{};                       // a linear primary outside its resource's own DDI
};
// A linear primary's release reached the shell outside the DDI of its runtime resource. No runtime
// callback was made; the allocation stays owned by its record until the device's metadata goes.
inline constexpr HRESULT kOwnerExpired=HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
struct ImportReport {
    ImportStage stage{};
    uint32_t memory_type{UINT32_MAX};
    uint64_t bytes{},alignment{},address{};
};
class RuntimeHeapImports final {
    struct Record;
    // Per thread: its caller reads a report right after the call that wrote it, on the same thread.
    inline static thread_local ImportReport report_{};
    inline static thread_local FreeReport free_report_{};
    // The owner scope of the calling thread: the imports whose resource DDI runs on it, if any.
    inline static thread_local const RuntimeHeapImports* scope_owner_{};
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
    mutable SRWLOCK lock_=SRWLOCK_INIT;         // records_ and the records' flags
    SRWLOCK paging_open_lock_=SRWLOCK_INIT;     // the first allocate() opens the paging queue
    Record* records_{};
    std::atomic<bool> active_{true},paging_open_{};
    bool initialized_{};
    Record* find(D3DKMT_HANDLE) const noexcept;   // under lock_
    HRESULT release(Record&) noexcept;
    HRESULT release_owned(Record&,VkDeviceMemory) noexcept;
    void erase(Record*) noexcept;
public:
    RuntimeHeapImports(Device&,bc250::umd::RuntimeDomain&,VkPhysicalDevice,VkDevice,
                       VkInstance,PFN_vkGetInstanceProcAddr,void* host_identity) noexcept;
    ~RuntimeHeapImports();
    RuntimeHeapImports(const RuntimeHeapImports&)=delete;
    RuntimeHeapImports& operator=(const RuntimeHeapImports&)=delete;
    HRESULT initialize() noexcept;
    HRESULT allocate(const engine_ddi::MemoryRequest*,engine_ddi::ImportedMemory*) noexcept;
    const ImportReport& last_report() const noexcept {return report_;}
    const FreeReport& last_free_report() const noexcept {return free_report_;}
    HRESULT free(const engine_ddi::ImportedMemory*) noexcept;
    // The owner scope: the span of one pfnCreateHeapAndResource or pfnDestroyHeapAndResource, on the
    // thread that runs it. Only inside it, and on that thread, may a linear primary be released, by its
    // runtime resource: one created in this span, or the one the span destroys (its allocation handle,
    // 0 for any other resource). Leaving the span takes the authority from every record it gave one,
    // whatever happened inside. Spans of different threads run at once, as their DDIs do (resource
    // creation is free-threaded). Spans do not nest on one thread: a second begin while one is open
    // there is refused (false) and changes nothing, and only the caller whose begin was admitted may end.
    bool begin_owner_scope(D3DKMT_HANDLE destroyed) noexcept;
    void end_owner_scope() noexcept;
    // Only these two operations are provided for RADV's borrowed allocation map.
    bool owns_allocation(D3DKMT_HANDLE) const noexcept;
    // Backing for a view inside a reserved address range. Admits only an import the engine holds:
    // complete, not retired, not in a callback, not a linear primary. Until every admitted borrow
    // is returned, free() of that import answers E_PENDING and changes nothing.
    bool borrow_backing(D3DKMT_HANDLE) noexcept;
    void return_backing(D3DKMT_HANDLE) noexcept;
    HRESULT dispatch(uint32_t operation,void* argument) noexcept;
    // Frees only records already retired by free(), or never handed to engine.
    // An active imported heap makes close fail; no GPU retirement is invented.
    HRESULT close_after_engine_retirement() noexcept;
    // Terminal CPU metadata release. Invalidates all runtime authority, does not
    // free outstanding Vulkan/kernel objects, and reports their retained owners.
    unsigned discard_metadata() noexcept;
};
class OwnerScope final {
    RuntimeHeapImports* imports_{};
    bool entered_{};
public:
    OwnerScope(RuntimeHeapImports* imports,D3DKMT_HANDLE destroyed) noexcept:imports_(imports) {
        entered_=imports_ && imports_->begin_owner_scope(destroyed);
    }
    ~OwnerScope() noexcept {if(entered_)imports_->end_owner_scope();}
    OwnerScope(const OwnerScope&)=delete;
    OwnerScope& operator=(const OwnerScope&)=delete;
    // Imports exist and did not admit this scope: the resource DDI must not run.
    bool refused() const noexcept {return imports_ && !entered_;}
};
// The two resource DDIs, as the shell's table runs them: the engine's entry inside an owner scope,
// and not at all when the scope is refused.
template<class Engine> HRESULT create_in_owner_scope(RuntimeHeapImports* imports,Engine&& engine) noexcept {
    OwnerScope scope(imports,0);
    if(scope.refused())return E_UNEXPECTED;
    return engine();
}
template<class Engine> HRESULT destroy_in_owner_scope(RuntimeHeapImports* imports,D3DKMT_HANDLE primary,
                                                      Engine&& engine) noexcept {
    OwnerScope scope(imports,primary);
    if(scope.refused())return E_UNEXPECTED;
    engine();return S_OK;
}
}
