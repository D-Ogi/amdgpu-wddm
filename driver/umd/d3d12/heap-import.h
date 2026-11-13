// SPDX-License-Identifier: MIT
#pragma once
#include "engine-ddi/engine-ddi.h"
#include "../dxvk/runtime-domain.h"
#include "device-progress.h"
#include "paging.h"
#include "../../contract/bc250_scanout_caps.h"
#include "../../contract/bc250_desktop_route.h"
#include <atomic>
namespace native12 {
struct Device;
struct Adapter;
enum class ScanoutModeList : unsigned;          // scanout-mode.h
// Calls run inside the owner's DeviceScope, including hosted GIPA and RuntimeDomain scopes, on any
// number of the device's DDI threads at once. lock_ guards the record list and every record's flags
// and is a leaf: no runtime callback, Vulkan call or paging operation runs under it. A record in a
// callback is busy; a record being released belongs to the releasing thread, whose own re-entrant
// Unlock2 (RADV's vkFreeMemory) is still admitted. The paging queue is created once, on first use,
// under its own lock (paging_life_lock_, the one lock held across a runtime callback: the paging
// queue's creation and destruction and the GPU wait of order_internal_submission, kernel calls that
// do not enter this module). No callback or Vulkan destruction runs in the destructor.
// Where the latest allocate() of the calling thread ended, for the diagnostic trace: the stage that
// returned, not a cause.
enum class ImportStage : uint32_t {
    Done,Request,Surface,Probe,MemoryType,PagingQueue,AllocateCallback,Map,MapReady,AddressAlignment,Import,
    Resident                                    // appended: trace values of the others stay as they were
};
// Where the latest free() ended, by the same rule.
enum class FreeStage : uint32_t { Done,Request,Record,VulkanFree,Unmap,Deallocate,
    Quarantined,                                // appended: the import is held mapped (ImportReleasePolicy)
    Unlock                                      // appended: the shell's own Unlock2 of a lock the ICD left (BD-045)
};
struct FreeReport {
    FreeStage stage{};
    bool surface{};
    bool adopted{};                             // an opened shared surface: borrowed, so no deallocate callback
    bool driver_surface{};                      // BD-133: a linear surface of ours with no runtime resource
    bool owner_expired{};                       // a linear primary outside its resource's own DDI
    // The import this free() named, for the trace that has to meet the kernel's paging journal by address
    // (instrumentation item 5 of the trial 245 report), and what the quarantine held when it returned.
    uint64_t gpu_va{},byte_size{};
    uint32_t held_count{};
    uint64_t held_bytes{};
    uint32_t released{};                        // quarantined imports this call drained (0 or more)
    bool unlocked{};                            // the release unlocked a lock the ICD had left (BD-045)
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
    // BD-052, and not part of the release gate: an imported heap keeps the driver's own residency reference
    // past its creation DDI (release_import_residency). The default since tester.36 (the 3DMark CPG fault). Since
    // BD-313 the hold ends at the first runtime MakeResident or Evict of the allocation (end_residency_hold), so it
    // lasts the whole life of engine memory only, which the runtime never names.
    bool residency_hold{};
    bool holds() const noexcept {return progress_gate || quarantine_depth;}
    static ImportReleasePolicy off() noexcept {return {};}
    // The driver's defaults, with the experiment switches of ddi-trace.h applied: import-progress-gate-off
    // and import-quarantine-off (both default on), and import-residency-release-on (default off: the hold is on;
    // lab builds only, BD-361: a release build always keeps the hold).
    static ImportReleasePolicy from_switches() noexcept;
};
// A linear primary's release reached the shell outside the DDI of its runtime resource. No runtime
// callback was made; the allocation stays owned by its record until the device's metadata goes.
inline constexpr HRESULT kOwnerExpired=HRESULT_FROM_WIN32(ERROR_INVALID_OWNER);
struct ImportReport {
    ImportStage stage{};
    uint32_t memory_type{UINT32_MAX};
    uint64_t bytes{},alignment{},address{};
    // The admission check that declined this request, as a literal of this module (null: none declined it).
    // Written for every shape allocate() refuses before it allocates anything, and logged in the same breath
    // (heap-import.cpp, refuse). BD-075: a shared create reaches exactly one of these, and the log line names
    // which one and with which heap flags, so one lab run says what the runtime asks for on a shared heap.
    const char* refusal{};
    uint32_t unimplemented_heap_flags{};        // the heap-flag bits of the refusal that this shell has no use for
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
    // The paging queue's lifetime: the first allocate() or adopt() opens it, close_after_engine_retirement
    // destroys it, and both take this lock exclusive. BD-052: order_internal_submission reads the queue's
    // handles and its fence page, which pfnDestroyPagingQueueCb unmaps, so it takes the same lock shared.
    // Without that, an internal submission concurrent with the device's close dereferenced a page the kernel
    // had already unmapped; the hosted dispatch guards the same hazard with paging_busy_ around its own
    // create and destroy. It is this module's one lock held across a runtime callback (the paging queue's
    // creation, its destruction and the GPU wait), all of them kernel thunks that do not enter this module.
    SRWLOCK paging_life_lock_=SRWLOCK_INIT;
    Record* records_{};
    // The quarantine (ImportReleasePolicy): records whose Vulkan import is gone and whose mapping and
    // runtime allocation are still held, oldest first. A quarantined record is out of records_, so no
    // other call can find it; only drain() touches it, one thread at a time (draining_).
    Record* held_{},*held_tail_{};
    uint32_t held_count_{};
    uint64_t held_bytes_{},deposits_{};
    bool draining_{};
    ImportReleasePolicy policy_{};
    // M15.14: what the scan-out decision needs. force_cpu_ is the desktop route's kill switch, which decides
    // whether the compositor is the CPU UMD and therefore whether any flip exists; it is taken once, in the
    // constructor, because the compositor reads it once. The kernel driver's trailer is not taken once
    // (increment 3): its geometry is the source mode committed now, and a game changes the mode between the
    // device and its chain, so the adapter is kept and the trailer is read for every primary. The
    // compositor's desktop-route record is read for every primary as well, through desktop_route_reader_
    // (scanout_desktop_route_read, or a host test's double). The kernel driver's mode list is asked through
    // mode_list_reader_ (scanout_mode_list_read, or a double) for a chain that is not the committed mode.
    Adapter* adapter_{};
    // BD-312: the device whose offered list an allocation leaves when this object releases it (forget_offered).
    Device* owner_{};
    unsigned long force_cpu_{};
    unsigned (*desktop_route_reader_)(bc250_desktop_route*) noexcept{};
    ScanoutModeList (*mode_list_reader_)(UINT64 luid,unsigned width,unsigned height) noexcept{};
    ProgressSource progress_{};
    std::atomic<uint64_t> forced_{};             // entries released although their progress was unretired
    // BD-201 R v2: the GPU virtual address reservations of those releases. A reservation is this driver's
    // own range and no allocation destroy frees it, so without the proof it is kept, not freed. Cumulative
    // and never decremented: it is a leak, and the count is the only record of it.
    std::atomic<uint64_t> leaked_reservations_{},leaked_reservation_bytes_{};
    // BD-052: runtime residency operations this device saw, for the trace and the lab arm. The driver makes
    // no residency operation of its own for them (note_runtime_residency).
    std::atomic<uint64_t> runtime_residencies_{},runtime_evictions_{};
    // BD-313: the ends of the residency hold (note_runtime_residency, note_runtime_eviction). hold_handbacks_: the
    // import's reference given back after a runtime MakeResident; hold_forgotten_: the reference that a runtime Evict
    // took with it; hold_failures_: hand-backs the kernel refused, each retried at the next notice.
    std::atomic<uint64_t> hold_handbacks_{},hold_forgotten_{},hold_failures_{},hold_epoch_{};
    // The end of the hold for the records of these allocations, after a runtime MakeResident (evicted false) or a
    // runtime Evict (evicted true). Never under lock_.
    void end_residency_hold(const D3DKMT_HANDLE* allocations,unsigned count,bool evicted) noexcept;
    std::atomic<bool> active_{true},paging_open_{};
    bool initialized_{};
    // BD-373 step 5: whether a committed GPU-only buffer gets the ENGINE LAYOUT record (write_engine_layout_buffers),
    // and the creator identity that the record carries.
    bool engine_layout_buffers_{};
    uint8_t device_uuid_[16]{},driver_uuid_[16]{};
    Record* find(D3DKMT_HANDLE) const noexcept;   // under lock_
    // The record an import belongs to, by the cookie it carries, under lock_. One borrowed allocation handle may
    // have several records (BD-075: two opens of the same handle), so the handle no longer identifies an import.
    Record* find_import(const engine_ddi::ImportedMemory*) const noexcept;
    // Records the declining check in report_, writes one line naming it and the request's shape, and returns
    // hr unchanged. Only for a refusal taken before any callback, probe or allocation.
    static HRESULT refuse(const char* why,HRESULT hr,const engine_ddi::MemoryRequest& request,
                          uint32_t unimplemented_heap_flags=0) noexcept;
    // The same for a refusal decided after the allocation and the mapping exist, where the address and not the
    // request's shape is the answer (BD-101, ImportStage::AddressAlignment). The caller still releases the
    // record; this writes the reason and the line.
    static HRESULT refuse_address(const char* why,HRESULT hr,uint64_t bytes,uint64_t alignment,
                                  uint64_t address) noexcept;
    // The requirements of a buffer of bytes bytes on this device, with the usage the engine's buffers need here:
    // the memory types and the alignment of a buffer import (allocate, and adopt of a buffer). No memory exists.
    HRESULT probe_buffer(uint64_t bytes,VkMemoryRequirements* needs) const noexcept;
    HRESULT release(Record&) noexcept;
    HRESULT release_import(Record&,VkDeviceMemory) noexcept;    // the Vulkan import alone
    HRESULT unlock_for_release(Record&) noexcept;               // a CPU lock the ICD left on it
    // The mapping and the runtime allocation. proved false is BD-201 R v2's release through the OS, for a
    // teardown release whose device-wide progress proof failed: see the definition.
    HRESULT release_owned(Record&,bool proved=true) noexcept;
    void detach(Record*) noexcept;                // out of records_, under lock_
    void deposit(Record*) noexcept;               // into the quarantine, under lock_
    void drain(bool all) noexcept;                // never under lock_
    void erase(Record*) noexcept;
    // BD-052: gives the paging domain's residency reference of this import back, inside the creation DDI
    // that took it, before that DDI returns. Called by the creating thread for its own record, outside
    // lock_, exactly where map() and make_resident() are called for the same mapping, so no runtime
    // callback is ever made under lock_ and the leaf rule above still holds. Unconditional: no gate, no
    // progress source may leave a heap resident when the creation DDI returns, which is the whole
    // requirement (DirectX-Specs ResourceHeaps.md, Residency DDIs, line 1011). It runs only in the arm
    // import-residency-release-on, a lab switch that a release build drops; by default ImportReleasePolicy::residency_hold keeps the reference until the
    // first runtime residency operation of the allocation (BD-313, end_residency_hold).
    HRESULT release_import_residency(Record&) noexcept;
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
    // The scan-out trailer as the kernel driver publishes it at this moment: all zero for a device with no
    // adapter (the host tests), for a kernel driver without the trailer and for a query that fails, which
    // all read as "no scan-out". One kernel query per call; allocate() calls it once per primary.
    bc250_scanout_caps scanout_caps_now() const noexcept;
    // What the constructor took from the registry, for the tests and the trace.
    unsigned long force_cpu() const noexcept {return force_cpu_;}
    // The compositor's desktop-route record as it is now: the read status (BC250_DESKTOP_ROUTE_READ_*) and
    // the record. allocate() calls it once per primary, after the off switch.
    unsigned desktop_route_now(bc250_desktop_route* record) const noexcept {
        return desktop_route_reader_(record);
    }
    // The host tests' seam: a reader in place of the session's record, which only dwm.exe can write.
    void read_desktop_route_with(unsigned (*reader)(bc250_desktop_route*) noexcept) noexcept {
        desktop_route_reader_=reader;
    }
    // Whether the adapter's video present source offers a width x height source mode now. allocate() calls
    // it only for a primary whose first answer was SourceGeometry (C71); a device without an adapter asks
    // with LUID 0, which the shell's reader answers Failed.
    ScanoutModeList mode_list_now(unsigned width,unsigned height) const noexcept;
    // The host tests' seam: a reader in place of the kernel driver's mode list.
    void read_mode_list_with(ScanoutModeList (*reader)(UINT64,unsigned,unsigned) noexcept) noexcept {
        mode_list_reader_=reader;
    }
    // What the quarantine holds now, for tests and the trace.
    uint32_t held_count() const noexcept;
    uint64_t held_bytes() const noexcept;
    uint64_t forced_releases() const noexcept {return forced_.load();}
    // BD-201 R v2: the reservations kept by a release through the OS, and their bytes. Never decremented.
    uint64_t leaked_reservations() const noexcept {return leaked_reservations_.load();}
    uint64_t leaked_reservation_bytes() const noexcept {return leaked_reservation_bytes_.load();}
    // A retirement point of the quarantine outside allocate() and free(): releases what the policy admits.
    void retire_held() noexcept {drain(false);}
    // BD-373 step 5, the default unless engine-layout-buffers-off (ddi-trace.h): from now on every committed buffer on
    // a GPU-only heap gets the ENGINE LAYOUT record of shared-buffer-record.h, with this device identity in it. A null or all-zero
    // device UUID turns the record off again, because an opener refuses a record without an identity. Set before the
    // first allocate(), by the owner that reads the switch (device-engine.cpp).
    void write_engine_layout_buffers(const uint8_t* device_uuid,const uint8_t* driver_uuid) noexcept;
    bool engine_layout_buffers() const noexcept {return engine_layout_buffers_;}
    // BD-052, the residency of an imported heap. The runtime owns it: it calls pfnMakeResident and pfnEvict
    // on this device (native-residency-ddi.cpp), and the driver "must no longer create allocations
    // associated with any d3d12 driver handle creation DDIs as resident during creation"
    // (DirectX-Specs ResourceHeaps.md @ 5a4139b, Residency DDIs, line 1011). D3D12DDI_HEAP_FLAGS has no
    // CREATE_NOT_RESIDENT bit at all (WDK 10.0.26100 d3d12umddi.h, lines 307-317): the runtime expresses
    // that flag by not calling pfnMakeResident, so the only way for the driver to honour it is to leave no
    // residency reference of its own behind (release_import_residency).
    //   The two notices below make no residency operation of the runtime's: the one residency operation of a
    // pfnMakeResident is the mandated callback with the runtime's own flags and the runtime's paging queue
    // (native-residency-ddi.cpp, RuntimeResidency::make_resident; "the driver must forward the flags passed
    // in to both MakeResident and Evict to the callback routines", line 978). A second MakeResidentCb of
    // this driver's own would take a second VidMm residency reference that the application's Evict cannot
    // drop, which is the defect itself. They count what the runtime did, for the trace and the lab arm, and,
    // under residency_hold, they end the import's own hold on the allocations they name (BD-313,
    // end_residency_hold): the one operation of ours on this path is the Evict that gives that hold back.
    void note_runtime_residency(const D3DKMT_HANDLE* allocations,unsigned count) noexcept;
    void note_runtime_eviction(const D3DKMT_HANDLE* allocations,unsigned count) noexcept;
    // BD-052: orders everything this context submits after this call behind every paging operation this
    // driver has queued (PagingDomain::order_context). Called for every internal submission the ICD makes
    // through the shell, on contexts the runtime never sees and therefore never makes wait for a map or a
    // residency operation of ours (trial 153). It is an ordering of this driver's own paging work and
    // nothing else: it is not a residency guarantee, and the residency of an application's heap after its
    // creation DDI is the runtime's (the notices above).
    // S_FALSE: the paging queue had caught up and no wait was needed, which is the steady state.
    HRESULT order_internal_submission(HANDLE context) noexcept;
    // What the above have done, for the tests, the trace and the lab arm.
    unsigned residency_releases() const noexcept {return paging_.residency_releases();}
    unsigned residency_orders() const noexcept {return paging_.ordered_waits();}
    uint64_t runtime_residencies() const noexcept {return runtime_residencies_.load();}
    uint64_t runtime_evictions() const noexcept {return runtime_evictions_.load();}
    uint64_t hold_handbacks() const noexcept {return hold_handbacks_.load();}
    uint64_t hold_forgotten() const noexcept {return hold_forgotten_.load();}
    uint64_t hold_failures() const noexcept {return hold_failures_.load();}
    HRESULT allocate(const engine_ddi::MemoryRequest*,engine_ddi::ImportedMemory*) noexcept;
    // BD-075: the open half. The allocation already exists, so this call makes no allocate callback: it maps
    // the handle the runtime opened, makes it resident, waits for the address, checks the alignment and imports
    // it into the engine's VkDevice, exactly as allocate() does after its callback. The record it leaves is
    // borrowed: free() releases the import and the mapping and makes no deallocate callback, because the runtime
    // destroys the allocation itself when the opened resource goes.
    HRESULT adopt(const engine_ddi::AdoptRequest*,engine_ddi::ImportedMemory*) noexcept;
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
