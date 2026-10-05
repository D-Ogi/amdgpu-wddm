// SPDX-License-Identifier: MIT
#pragma once
#include "engine-ddi/engine-ddi.h"
#include "../dxvk/runtime-domain.h"
#include "device-progress.h"
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
enum class FreeStage : uint32_t { Done,Request,Record,VulkanFree,Unmap,Deallocate,
    Quarantined                                 // appended: the import is held mapped (ImportReleasePolicy)
};
struct FreeReport {
    FreeStage stage{};
    bool surface{};
    bool owner_expired{};                       // a linear primary outside its resource's own DDI
    // The import this free() named, for the trace that has to meet the kernel's paging journal by address
    // (instrumentation item 5 of the trial 245 report), and what the quarantine held when it returned.
    uint64_t gpu_va{},byte_size{};
    uint32_t held_count{};
    uint64_t held_bytes{};
    uint32_t released{};                        // quarantined imports this call drained (0 or more)
};
// The shell's half of the release gate (M15.8, fixes F2 and F3 of the trial 245 report). A released import
// stays mapped and allocated until both hold:
//   - progress_gate: the device-wide progress taken when the release reached the shell has retired, which
//     covers every context of the device, the engine's and the ICD's internal ones included
//     (device-progress.h). Without a progress source the gate proves nothing and is not applied.
//   - quarantine_depth: this many further releases have reached the shell, or the entry is older than
//     quarantine_age_ms, or a cap is over. The depth is a delay against a race the first condition cannot
//     see (a submission that names the memory and reaches the kernel after the snapshot); the caps bound
//     how much memory the delay holds and never shorten the progress condition.
// All zero (off()) is adapter106's behaviour: the unmap and the deallocation run inside free().
struct ImportReleasePolicy {
    uint32_t quarantine_depth{};
    uint32_t quarantine_count_cap{};
    uint32_t quarantine_age_ms{};
    uint64_t quarantine_byte_cap{};
    bool progress_gate{};
    bool holds() const noexcept {return progress_gate || quarantine_depth;}
    static ImportReleasePolicy off() noexcept {return {};}
    // The driver's defaults, with the two experiment switches of ddi-trace.h applied (both default on):
    // import-progress-gate-off and import-quarantine-off.
    static ImportReleasePolicy from_switches() noexcept;
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
    mutable SRWLOCK lock_=SRWLOCK_INIT;         // records_ and the records' flags, and the quarantine
    SRWLOCK paging_open_lock_=SRWLOCK_INIT;     // the first allocate() opens the paging queue
    Record* records_{};
    // The quarantine (ImportReleasePolicy): records whose Vulkan import is gone and whose mapping and
    // runtime allocation are still held, oldest first. A quarantined record is out of records_, so no
    // other call can find it; only drain() touches it, one thread at a time (draining_).
    Record* held_{},*held_tail_{};
    uint32_t held_count_{};
    uint64_t held_bytes_{},deposits_{};
    bool draining_{};
    ImportReleasePolicy policy_{};
    ProgressSource progress_{};
    std::atomic<uint64_t> forced_{};             // entries released although their progress was unretired
    std::atomic<bool> active_{true},paging_open_{};
    bool initialized_{};
    Record* find(D3DKMT_HANDLE) const noexcept;   // under lock_
    HRESULT release(Record&) noexcept;
    HRESULT release_import(Record&,VkDeviceMemory) noexcept;    // the Vulkan import alone
    HRESULT release_owned(Record&) noexcept;                    // the mapping and the runtime allocation
    void detach(Record*) noexcept;                // out of records_, under lock_
    void deposit(Record*) noexcept;               // into the quarantine, under lock_
    void drain(bool all) noexcept;                // never under lock_
    void erase(Record*) noexcept;
public:
    RuntimeHeapImports(Device&,bc250::umd::RuntimeDomain&,VkPhysicalDevice,VkDevice,
                       VkInstance,PFN_vkGetInstanceProcAddr,void* host_identity,
                       const ImportReleasePolicy& policy=ImportReleasePolicy::from_switches()) noexcept;
    ~RuntimeHeapImports();
    RuntimeHeapImports(const RuntimeHeapImports&)=delete;
    RuntimeHeapImports& operator=(const RuntimeHeapImports&)=delete;
    HRESULT initialize() noexcept;
    // The source of the device-wide progress the gate reads. Set once, before the first free(), by the
    // owner that has it (device-engine.cpp); without it progress_gate holds nothing.
    void bind_progress(const ProgressSource& source) noexcept {progress_=source;}
    const ImportReleasePolicy& policy() const noexcept {return policy_;}
    // What the quarantine holds now, for tests and the trace.
    uint32_t held_count() const noexcept;
    uint64_t held_bytes() const noexcept;
    uint64_t forced_releases() const noexcept {return forced_.load();}
    // A retirement point of the quarantine outside allocate() and free(): releases what the policy admits.
    void retire_held() noexcept {drain(false);}
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
