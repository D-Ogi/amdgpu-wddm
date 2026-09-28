// SPDX-License-Identifier: MIT
// engine-ddi internals shared by its translation units. Not part of the boundary (engine-ddi.h is).
#pragma once
#include "engine-ddi.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace engine_ddi {

// One line on stderr, prefixed "engine-ddi: ". The native build has no other log sink yet.
void log_line(const char* format, ...) noexcept;

// A value-initialized T on the heap, or null. Written out rather than new (std::nothrow) T{}, whose value
// initialization /analyze models before the null check (C28182).
template <class T, class... A> T* make_new(A&&... args) noexcept {
    void* p = ::operator new(sizeof(T), std::nothrow);
    return p ? new (p) T{std::forward<A>(args)...} : nullptr;
}

// ---- Release sequence (engine-ddi.h, "Release sequence of heap memory") -----------------------------------------
// Engine queues of one device, at most. create_engine_queue refuses more with E_OUTOFMEMORY, as the engine does
// when its VkQueues run out; the engine's own limit is below this (16 VkQueues per family, three families).
inline constexpr uint32_t kMaxEngineQueues = 64;
// GetCompletedValue of a fence whose device is removed. It never proves that work has retired.
inline constexpr uint64_t kFenceRemoved = UINT64_MAX;

// What one heap's final release hands back: engine objects in release order (step 2), then the shell's memory
// (step 3). id is diagnostic.
struct ReleasePayload {
    IUnknown* objects[2];
    bool has_memory;
    ImportedMemory memory;
    uint64_t id;
};

// One pending release. It is allocated together with the heap it belongs to, while creation can still fail, so
// that recording a release needs no allocation: tracking can never fail and turn into permission to free.
struct PendingRelease {
    ReleasePayload payload;
    uint64_t mask;                              // queue slots that hold a mark
    uint64_t marks[kMaxEngineQueues];           // the value each slot's retirement fence must reach
    bool stuck;                                 // retirement can never be proven: pending for the device's life
    PendingRelease* next;
};

// Pending releases of one device, an intrusive list: no call allocates. Not thread-safe: the owner holds its lock
// around every call. Steps 2 and 3 run outside the lock, through run_release.
class ReleaseQueue {
public:
    void add(PendingRelease* node) noexcept;
    // Unlinks and returns (through next) every entry whose marks are all reached. completed(slot) is the slot's
    // retirement fence value. kFenceRemoved, from a removed device or for a slot without a queue, makes the entry
    // stuck instead: the sentinel is never compared as a number.
    template <class Completed> PendingRelease* take_retired(Completed completed) noexcept {
        PendingRelease* ready = nullptr;
        PendingRelease** link = &head_;
        while (PendingRelease* n = *link) {
            bool done = !n->stuck;
            for (uint32_t s = 0; done && s < kMaxEngineQueues; ++s) {
                if (!((n->mask >> s) & 1)) continue;
                const uint64_t value = completed(s);
                if (value == kFenceRemoved) {
                    n->stuck = true;
                    done = false;
                } else if (value < n->marks[s]) {
                    done = false;
                }
            }
            if (done) {
                *link = n->next;
                n->next = ready;
                ready = n;
                --count_;
            } else {
                link = &n->next;
            }
        }
        return ready;
    }
    // A queue is gone after its final Release. Marks on its slot are satisfied if final_completed reaches them;
    // otherwise, or if final_completed is kFenceRemoved, their entries are stuck for good. The slot is then free.
    void queue_destroyed(uint32_t slot, uint64_t final_completed) noexcept;
    size_t pending() const noexcept { return count_; }
    size_t stuck() const noexcept;

private:
    PendingRelease* head_ = nullptr;
    size_t count_ = 0;
};

// Steps 2 and 3 for one payload, on the calling DDI thread: releases the engine objects, then calls free_memory
// once if there is memory, and reports a failure through report_device_error. Returns free_memory's result
// (S_OK when there is no memory).
HRESULT run_release(const ShellHooks& hooks, const ReleasePayload& payload) noexcept;

// Harness observation of the release sequence. Null in the native build.
struct ReleaseEvent {
    uint64_t id;
    DWORD thread;
    bool had_memory;
    bool deferred;                              // false: ran inside the destroy; true: at a later DDI call
    HRESULT free_result;
};
using ReleaseObserver = void (*)(void* user, const ReleaseEvent* event);

// ---- Device context and queues ----------------------------------------------------------------------------------
struct EngineQueue {
    DeviceContext* context;
    ID3D12CommandQueue* queue;
    ID3D12Fence* fence;                         // retirement fence, internal memory
    uint64_t id;
    uint32_t slot;                              // index in DeviceContext::queues
    D3D12_COMMAND_LIST_TYPE type;
    // Submission state in one word, so that a snapshot reads it coherently: bits 63..1 hold the last value
    // signalled on fence, bit 0 is set while work has been submitted that no successful signal covers yet.
    std::atomic<uint64_t> state;
    SRWLOCK submit_lock;                        // keeps ExecuteCommandLists and its Signal together
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    std::atomic<uint64_t> harness_completed;    // nonzero: returned instead of the fence value
    std::atomic<bool> harness_fail_signal;      // the next retirement Signal is skipped and fails
#endif
};
// The retirement fence value of a queue (a retirement point of the engine, V8).
uint64_t completed_value(EngineQueue* queue) noexcept;

class DeviceContext {
public:
    ID3D12Device* device = nullptr;             // one reference each
    ID3D12Device4* device4 = nullptr;
    ID3D12Device8* device8 = nullptr;
    ID3D12Device10* device10 = nullptr;
    MemoryMode mode = MemoryMode::RuntimeBacked;
    ShellHooks hooks{};
    BC250_VKD3D_ENGINE_FUNCS funcs{};
    uint32_t ddi_interface = 0;
    uint32_t ddi_version = 0;
    UINT increments[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES]{};
    std::atomic<uint32_t> live{0};              // records holding references, plus pending releases
    std::atomic<uint32_t> pending{0};           // pending releases only
    std::atomic<uint64_t> next_id{1};

    SRWLOCK lock = SRWLOCK_INIT;                // guards queues, queue_mask, retirement_lost and releases
    EngineQueue* queues[kMaxEngineQueues]{};
    uint64_t queue_mask = 0;
    // A queue went away with work it could not prove retired (removed device, failed signal, fence short of its
    // last signal). Its work may still use memory released later, so every later release is stuck.
    bool retirement_lost = false;
    ReleaseQueue releases;

    ReleaseObserver observer = nullptr;         // harness only
    void* observer_user = nullptr;

    void report(HRESULT hr) const noexcept {
        if (hooks.report_device_error) hooks.report_device_error(hooks.shell, hr);
    }
    void report_list(D3D12DDI_HRTCOMMANDLIST list, HRESULT hr) const noexcept {
        if (hooks.report_list_error) hooks.report_list_error(hooks.shell, list, hr);
    }
    bool lost() const noexcept { return hooks.is_device_lost && hooks.is_device_lost(hooks.shell); }

    // Hands a release to the release sequence. Under the lock it takes one snapshot of every queue: a queue whose
    // retirement fence has not reached the work submitted before the call gets a mark. With no mark it runs the
    // release now, outside the lock; otherwise it records the node. Never allocates.
    void release(PendingRelease* node) noexcept;
    // Runs the release sequence for everything that has retired (a retirement point).
    void process_retired() noexcept;
    void notify(const ReleasePayload& payload, bool deferred, HRESULT free_result) const noexcept;
};

// The resolver installed by fill_device_core / fill_command_list.
DeviceContext* resolve(D3D12DDI_HDEVICE device) noexcept;

// ---- Records ------------------------------------------------------------------------------------------------------
template <class R> R* record_of(void* storage, Tag tag) noexcept {
    auto* r = static_cast<R*>(storage);
    return (r && r->h.tag == tag) ? r : nullptr;
}
template <class R> R* record_of(void* storage, Tag tag, const DeviceContext* device) noexcept {
    R* r = record_of<R>(storage, tag);
    return (r && r->h.device == device) ? r : nullptr;
}
inline void poison(RecordHeader& h) noexcept {
    h.tag = Tag::Poisoned;
    h.engine = nullptr;
    h.device = nullptr;
}
inline void release_engine(RecordHeader& h) noexcept {
    if (h.engine) h.engine->Release();
    h.engine = nullptr;
}

// Heap memory shared by a heap record and the resources placed in it. engine-ddi's own allocation, not runtime
// storage, because resources can outlive the heap record that created it.
struct Backing {
    std::atomic<uint32_t> refs;
    DeviceContext* device;
    ID3D12Heap* heap;                           // one reference
    ID3D12Resource* map_buffer;                 // heap-wide buffer for MapHeap, created on first map
    SRWLOCK map_lock;
    uint32_t map_count;
    void* cpu;
    D3D12DDIARG_CREATEHEAP_0001 desc;
    bool imported;
    bool dedicated;
    ImportedMemory memory;
    uint64_t id;
    PendingRelease* release_node;               // allocated with the backing, handed to the release sequence
};
void backing_acquire(Backing* backing) noexcept;
void backing_release(Backing* backing) noexcept;

// RuntimeBacked import (engine-ddi.h, placement rules, Vulkan part). import_memory calls allocate_memory once and
// validates the result; on a validation failure it hands the memory straight back through run_release and returns
// the validation error. validate_import is the check alone.
HRESULT validate_import(const MemoryRequest& request, const ImportedMemory& memory) noexcept;
HRESULT import_memory(DeviceContext* context, const MemoryRequest& request, ImportedMemory* out) noexcept;

struct HeapRecord {
    RecordHeader h;                             // engine: the ID3D12Heap, own reference
    Backing* backing;
};

enum class ResourceKind : uint32_t { Committed = 1, Placed = 2 };
struct ResourceRecord {
    RecordHeader h;                             // engine: the ID3D12Resource
    Backing* backing;                           // one reference
    uint64_t offset;                            // in the backing
    D3D12_RESOURCE_DESC1 desc;
    D3D12DDI_HRTRESOURCE rt;
    ResourceKind kind;
    uint32_t reserved;
};

struct DescriptorHeapRecord {
    RecordHeader h;                             // engine: ID3D12DescriptorHeap
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    UINT count;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};

struct RootSignatureRecord {
    RecordHeader h;                             // engine: ID3D12RootSignature
    UINT parameters;
};

enum ShaderIntake : uint32_t { kIntakeInvalid = 0, kIntakeNative = 1, kIntakeHarnessContainer = 2 };
struct ShaderRecord {
    RecordHeader h;                             // no engine object
    uint32_t intake;                            // ShaderIntake
    uint32_t bytes;                             // payload bytes copied after the record
    uint32_t snapshot[8];                       // first min(bytes, 32) bytes, zero beyond
    uint32_t inputs, outputs, patch_constants;  // signature entry counts
    uint32_t reserved;
    // payload follows
    const void* payload() const noexcept { return this + 1; }
};

struct PipelineRecord {
    RecordHeader h;                             // engine: ID3D12PipelineState
    D3D12DDI_HRTPIPELINESTATE rt;
    bool compute;
};

struct CommandPoolRecord {
    RecordHeader h;                             // no engine object; allocators below, one reference each
    ID3D12CommandAllocator* allocators[4];      // by D3D12_COMMAND_LIST_TYPE: DIRECT, BUNDLE, COMPUTE, COPY
};

struct CommandRecorderRecord {
    RecordHeader h;
    CommandPoolRecord* pool;                    // runtime storage of the target pool
    UINT queue_flags;
};

struct CommandListRecord {
    RecordHeader h;                             // engine: ID3D12GraphicsCommandList
    D3D12DDI_HRTCOMMANDLIST rt;
    D3D12_COMMAND_LIST_TYPE type;
    uint32_t table;                             // 0 compute table, 1 graphics table
    ID3D12GraphicsCommandList* list() const noexcept { return static_cast<ID3D12GraphicsCommandList*>(h.engine); }
};

struct QueryHeapRecord {
    RecordHeader h;                             // engine: ID3D12QueryHeap
    D3D12_QUERY_HEAP_TYPE type;
    UINT count;
};

// ---- Slot groups: each fills its part of the tables -------------------------------------------------------------
void fill_core_failsafe(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_failsafe(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_resources(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_resources(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_descriptors(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_descriptors(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_root_signatures(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_core_pipelines(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_pipelines(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_commands(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_commands(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_queries(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_queries(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;

// Graphics-only slot in the compute table: reports E_INVALIDARG through report_list_error.
bool reject_in_compute_table(const CommandListRecord* list) noexcept;
// Resolves a command list handle; logs and returns null for a record that is not a live command list.
CommandListRecord* list_of(D3D12DDI_HCOMMANDLIST list, const char* slot) noexcept;

// Root signature serialization (root-signature.cpp): a DXBC container with one RTS0 part, version 1.1.
HRESULT serialize_root_signature(const D3D12DDI_ROOT_SIGNATURE_0013* desc, std::vector<uint8_t>& out) noexcept;

#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
// Harness-only entry points. The shell's DLL build never defines the macro, so none of these exist there.
ID3D12CommandQueue* harness_engine_queue(EngineQueue* queue) noexcept;   // no reference added
IUnknown* harness_engine_object(const void* storage) noexcept;           // the record's engine pointer
void harness_set_release_observer(DeviceContext* context, ReleaseObserver observer, void* user) noexcept;
uint32_t harness_pending_releases(DeviceContext* context) noexcept;
uint32_t harness_stuck_releases(DeviceContext* context) noexcept;
uint32_t harness_live_objects(DeviceContext* context) noexcept;
bool harness_retirement_lost(DeviceContext* context) noexcept;
// Fault injection: completed_value returns value instead of the fence's (0 turns it off); the next retirement
// Signal of execute_command_lists is skipped and fails with E_FAIL after the engine's ExecuteCommandLists ran.
void harness_force_completed(EngineQueue* queue, uint64_t value) noexcept;
void harness_fail_next_signal(EngineQueue* queue) noexcept;
#endif

} // namespace engine_ddi
