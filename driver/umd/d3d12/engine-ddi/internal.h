// SPDX-License-Identifier: MIT
// engine-ddi internals shared by its translation units. Not part of the boundary (engine-ddi.h is).
#pragma once
#include "engine-ddi.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace engine_ddi {

// One line on stderr, prefixed "engine-ddi: ". The native build has no other log sink yet.
void log_line(const char* format, ...) noexcept;
// A refusal: the same line, also on the debugger's output. A game's stderr goes nowhere, and refusals are rare.
void log_refusal(const char* format, ...) noexcept;

// Diagnostic lines written once a process, through log_refusal. first() is true for its first caller only; once
// it was, done() and first() cost one relaxed load. Constant-initialized: a namespace-scope instance needs no guard.
class LogOnce {
public:
    constexpr LogOnce() noexcept = default;
    bool done() const noexcept { return done_.load(std::memory_order_relaxed); }
    bool first() noexcept { return !done() && !done_.exchange(true, std::memory_order_relaxed); }

private:
    std::atomic<bool> done_{false};
};

// A set of DXGI_FORMAT values, lock-free: each value below 256 on its own, every larger one as a single member.
// For lines written once per format and for engine answers kept per format. Constant-initialized, as LogOnce.
class FormatSet {
public:
    constexpr FormatSet() noexcept = default;
    bool contains(uint32_t value) const noexcept {
        return (words_[word(value)].load(std::memory_order_relaxed) & bit(value)) != 0;
    }
    // True when value was not a member yet: for each value, its first caller.
    bool insert(uint32_t value) noexcept {
        return !contains(value) &&
               !(words_[word(value)].fetch_or(bit(value), std::memory_order_relaxed) & bit(value));
    }

private:
    static constexpr uint32_t word(uint32_t value) noexcept { return value < 256 ? value / 64 : 4; }
    static constexpr uint64_t bit(uint32_t value) noexcept { return uint64_t{1} << (value < 256 ? value % 64 : 0); }
    std::atomic<uint64_t> words_[5]{};
};

// A value-initialized T on the heap, or null. Written out rather than new (std::nothrow) T{}, whose value
// initialization /analyze models before the null check (C28182).
template <class T, class... A> T* make_new(A&&... args) noexcept {
    void* p = ::operator new(sizeof(T), std::nothrow);
    return p ? new (p) T{std::forward<A>(args)...} : nullptr;
}

// Room for count trivial T: N inline (no allocation), more on the heap. For the arrays the hot slots translate on
// every call (resource_barrier, execute_command_lists); a std::vector there cost an operator new and a delete per
// call (session 246: ~0.26 ms per frame of ResourceBarrier at preset LOW). The elements start uninitialized.
template <class T, size_t N> class InlineArray {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
    T inline_[N];
    T* heap_ = nullptr;

public:
    InlineArray() noexcept {}
    InlineArray(const InlineArray&) = delete;
    InlineArray& operator=(const InlineArray&) = delete;
    ~InlineArray() { ::operator delete(heap_); }
    // Room for count elements, once, before data(): false when count needs the heap and the heap has none.
    bool reserve(size_t count) noexcept {
        if (count <= N) return true;
        if (count > SIZE_MAX / sizeof(T)) return false;
        heap_ = static_cast<T*>(::operator new(count * sizeof(T), std::nothrow));
        return heap_ != nullptr;
    }
    T* data() noexcept { return heap_ ? heap_ : inline_; }
};

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
    // The memory of a linear primary: the shell may release it only inside the destroy that ends it, so
    // that destroy waits for retirement, within in_ddi_bound_ms, instead of recording the release at once.
    bool in_ddi = false;
};

// One pending release. It is allocated together with the heap it belongs to, while creation can still fail, so
// that recording a release needs no allocation: tracking can never fail and turn into permission to free.
struct PendingRelease {
    ReleasePayload payload;
    uint64_t mask;                              // queue slots that hold a mark
    uint64_t marks[kMaxEngineQueues];           // the value each slot's retirement fence must reach
    bool stuck;                                 // retirement can never be proven: pending for the device's life
    PendingRelease* next;
    // Two-phase retirement (set_release_policy). phase 1: the marks of the destroy's own snapshot; phase 2:
    // the marks re-recorded once those were reached. Without the policy a release stays at phase 0 and its
    // one snapshot decides, as before. The two counts and the times are for the log line only.
    uint32_t phase;
    uint32_t queues[2];                         // slots that held a mark in each phase
    uint64_t snapshot_qpc[2];
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
    std::atomic<uint64_t> last_done;            // the largest retirement fence value read so far (observed_value)
    SRWLOCK submit_lock;                        // keeps ExecuteCommandLists and its Signal together
    uint64_t init_waited;                       // the init_fence value this queue last waited for; submit_lock
    bool closing;                               // destroy has begun: never borrowed again; DeviceContext::lock
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    std::atomic<uint64_t> harness_completed;    // nonzero: returned instead of the fence value
    std::atomic<bool> harness_fail_signal;      // the next retirement Signal is skipped and fails
#endif
};
// The retirement fence value of a queue (a retirement point of the engine, V8).
uint64_t completed_value(EngineQueue* queue) noexcept;
// completed_value() plus a logged check that the value neither goes back nor passes the queue's last signal.
uint64_t observed_value(EngineQueue* queue) noexcept;
// Submits lists on the queue followed by the signal of its retirement fence (queue.cpp). With no list it signals
// the fence after whatever the queue did last, such as a tile mapping (tiles.cpp). The caller holds submit_lock.
HRESULT submit_locked(EngineQueue* queue, UINT count, ID3D12CommandList* const* lists) noexcept;

struct ResourceRecord;
struct Replay;                                  // replay.h: deferred command-list replay

// The retire policy's decision at a submission's retirement point (set_retire_policy): true leaves the pending
// releases to the resource DDIs. Never with the hand-off off, nothing pending, the backlog bound reached, or the
// age bound passed since the last resource DDI's pass (resource_tick 0: none yet). Ticks are GetTickCount64.
inline bool retire_defers(bool handoff, uint32_t pending, uint32_t backlog_bound, uint64_t now, uint64_t resource_tick,
                          uint32_t age_bound_ms) noexcept {
    if (!handoff || !pending || pending >= backlog_bound || !resource_tick) return false;
    return now >= resource_tick && now - resource_tick < age_bound_ms;
}

class DeviceContext {
public:
    ID3D12Device* device = nullptr;             // one reference each
    ID3D12Device4* device4 = nullptr;
    ID3D12Device5* device5 = nullptr;
    ID3D12Device7* device7 = nullptr;           // null if the engine has none: AddToStateObject answers E_NOTIMPL
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

    // Initialization of committed render targets and depth-stencil resources (queue.cpp). All guarded by
    // init_lock, which is taken before lock and before any queue's submit_lock.
    SRWLOCK init_lock = SRWLOCK_INIT;
    ResourceRecord* init_head = nullptr;        // waiting for their discard; intrusive through ResourceRecord
    struct InitList {
        ID3D12CommandAllocator* allocator;      // DIRECT
        ID3D12GraphicsCommandList* list;
        uint64_t value;                         // the init_fence value after its last batch: idle once reached
    };
    std::vector<InitList> init_lists;           // grows only while every list still has a batch in flight
    ID3D12Fence* init_fence = nullptr;          // signalled after each batch, on the queue that ran it
    uint64_t init_value = 0;                    // the last value signalled on init_fence
    bool init_broken = false;                   // a batch failed: no further batches

    // The empty local root signature of ray tracing pipelines (state-objects.cpp): created by the first create that
    // needs it, one reference, released with the context. Guarded by empty_local_lock, which is taken alone.
    SRWLOCK empty_local_lock = SRWLOCK_INIT;
    ID3D12RootSignature* empty_local = nullptr;

    uint32_t in_ddi_bound_ms = 2000;            // ReleasePayload::in_ddi; changed by the harness only
    ReleaseObserver observer = nullptr;         // harness only
    void* observer_user = nullptr;

    // The release policy (set_release_policy): written before the context is used on another thread, read
    // only after. The counters are diagnostic (the log line at powers of two).
    bool release_two_phase = false;
    std::atomic<uint64_t> releases_recorded{0};  // releases that took a first-phase snapshot
    std::atomic<uint64_t> releases_second{0};    // releases that reached a second phase
    std::atomic<uint64_t> releases_run{0};       // releases whose memory went back to the shell
    // The retire policy (set_retire_policy): written before the context is used on another thread, read only after.
    bool retire_handoff = false;
    uint32_t retire_backlog_bound = 0;
    uint32_t retire_age_bound_ms = 0;
    // GetTickCount64 at the last resource DDI's pass; stored only when it changes, so that resource DDIs on many
    // threads do not write one cache line on every call.
    std::atomic<uint64_t> retire_resource_tick{0};
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    std::atomic<uint64_t> retire_deferred{0};   // submission passes left to the resource DDIs
#endif
    // Deferred command-list replay (set_replay_policy, replay.h), null when off: written before the context is used
    // on another thread, and at teardown; read by every recording slot.
    Replay* replay = nullptr;

    // Every report is logged with thread and time, so it can be placed between the begin and end records
    // of the entry that made it. A reported error can surface to the application at a later call.
    static long long report_time() noexcept {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }
    void report(HRESULT hr) const noexcept {
        log_line("device error reported: %08lx (thread %lu, qpc %lld)", static_cast<unsigned long>(hr),
                 GetCurrentThreadId(), report_time());
        if (hooks.report_device_error) hooks.report_device_error(hooks.shell, hr);
    }
    void report_list(D3D12DDI_HRTCOMMANDLIST list, HRESULT hr) const noexcept {
        log_line("list error reported: %08lx (thread %lu, qpc %lld)", static_cast<unsigned long>(hr),
                 GetCurrentThreadId(), report_time());
        if (hooks.report_list_error) hooks.report_list_error(hooks.shell, list, hr);
    }
    bool lost() const noexcept { return hooks.is_device_lost && hooks.is_device_lost(hooks.shell); }

    // Hands a release to the release sequence. Under the lock it takes one snapshot of every queue: a queue whose
    // retirement fence has not reached the work submitted before the call gets a mark. With no mark it runs the
    // release now, outside the lock; otherwise it records the node. Never allocates. A payload with in_ddi
    // takes the snapshot again, with the lock released in between, until no mark is left or in_ddi_bound_ms
    // have passed; after the bound the device error is reported and the node is recorded like any other.
    void release(PendingRelease* node) noexcept;
    // Runs the release sequence for everything that has retired (a retirement point). With the release
    // policy's two_phase, a node whose first-phase marks are reached has its second phase recorded here
    // instead and stays pending until those marks are reached too (set_release_policy).
    void process_retired() noexcept;
    // Records the marks of every engine queue in the node, under lock. Returns the number of slots that
    // hold one; node->stuck is set when a queue proves nothing (removed device, lost retirement).
    uint32_t snapshot_marks(PendingRelease* node) noexcept;
    // The retirement point of a submission call (execute_command_lists, update_tile_mappings, copy_tile_mappings):
    // process_retired, unless the retire policy hands it to the resource DDIs (retire_defers).
    void retire_after_submit() noexcept;
    // The retirement point of pfnCreateHeapAndResource and pfnDestroyHeapAndResource: notes the time for the
    // policy's age bound, then process_retired.
    void retire_at_resource() noexcept;
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
    ID3D12Heap* heap;                           // one reference; MapHeap and UnmapHeap go to the engine (V10)
    D3D12DDIARG_CREATEHEAP_0001 desc;
    bool imported;
    bool dedicated;
    bool linear;                                // the memory of one linear primary: nothing else is placed on it
    ImportedMemory memory;
    uint64_t id;
    PendingRelease* release_node;               // allocated with the backing, handed to the release sequence
    IUnknown* retained;                         // a destroyed committed resource still named by an initialization
                                                // batch: released with the heap, before it, after retirement
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

// Reserved: a tiled resource with no heap memory of its own (tiles.cpp maps heaps into it); backing is null.
enum class ResourceKind : uint32_t { Committed = 1, Placed = 2, Reserved = 3 };
struct ResourceRecord {
    RecordHeader h;                             // engine: the ID3D12Resource
    Backing* backing;                           // one reference; null for a reserved resource
    uint64_t offset;                            // in the backing
    D3D12_RESOURCE_DESC1 desc;
    D3D12DDI_HRTRESOURCE rt;
    ResourceKind kind;
    uint32_t init_state;                        // kInitNone, kInitQueued or kInitRecorded; init_lock
    ResourceRecord* init_prev;                  // DeviceContext's initialization list while kInitQueued
    ResourceRecord* init_next;
    uint32_t linear_row_pitch;                  // bytes; 0 unless the image is the linear primary
    uint64_t linear_size;                       // the size of its backing as asked of the shell
};
inline constexpr uint32_t kInitNone = 0;
inline constexpr uint32_t kInitQueued = 1;
inline constexpr uint32_t kInitRecorded = 2;    // named by a batch that may still run

// Initialization of committed render targets and depth-stencil resources (queue.cpp). queue_initialization at
// the create; cancel_initialization at the destroy, before the engine resource is released: it returns true when a
// batch names the resource, which must then outlive the batch's retirement. flush_initializations runs at the start
// of execute_command_lists: it submits the pending discards and returns the init_fence value the executing queue
// must wait for (0: none).
void queue_initialization(DeviceContext* context, ResourceRecord* resource) noexcept;
bool cancel_initialization(DeviceContext* context, ResourceRecord* resource) noexcept;
uint64_t flush_initializations(DeviceContext* context, EngineQueue* queue) noexcept;
void release_initialization(DeviceContext* context) noexcept;       // context destroy: the internal objects

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

// ---- Shaders and graphics state (pipelines.cpp, graphics.cpp). None of these records holds an engine object. ----
// RecordHeader::flags of a state record whose create reported a failure: pipelines that name it are refused.
inline constexpr uint32_t kRecordInvalid = 0x1;

struct ShaderObject;                            // pipelines.cpp: the rebuilt container and what pipelines read
struct ShaderRecord {
    RecordHeader h;
    ShaderObject* object;                       // engine-ddi's own allocation, freed by DestroyShader; null when the
                                                // intake failed
};

struct ElementLayoutRecord {
    RecordHeader h;
    UINT count;
    UINT reserved;
    // count D3D12DDIARG_INPUT_ELEMENT_DESC follow (CalcPrivateElementLayoutSize sized them)
    D3D12DDIARG_INPUT_ELEMENT_DESC* elements() noexcept { return reinterpret_cast<D3D12DDIARG_INPUT_ELEMENT_DESC*>(this + 1); }
    const D3D12DDIARG_INPUT_ELEMENT_DESC* elements() const noexcept {
        return reinterpret_cast<const D3D12DDIARG_INPUT_ELEMENT_DESC*>(this + 1);
    }
};
struct BlendStateRecord {
    RecordHeader h;
    D3D12_BLEND_DESC desc;
};
struct DepthStencilStateRecord {
    RecordHeader h;
    D3D12_DEPTH_STENCIL_DESC desc;
    BOOL depth_bounds;                          // DepthBoundsTestEnable (0025): pipelines refuse it, no DDI support
};
struct RasterizerStateRecord {
    RecordHeader h;
    D3D12_RASTERIZER_DESC desc;
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
    bool recording;                             // between a Reset and a Close that both succeeded
    // Deferred replay (replay.h): the ring and the position where the list's last pending entry ends, 0 if none.
    std::atomic<uint64_t> replay_tail{0};
    ID3D12GraphicsCommandList* list() const noexcept { return static_cast<ID3D12GraphicsCommandList*>(h.engine); }
};

struct QueryHeapRecord {
    RecordHeader h;                             // engine: ID3D12QueryHeap
    D3D12_QUERY_HEAP_TYPE type;
    UINT count;
};

struct CommandSignatureRecord {
    RecordHeader h;                             // engine: ID3D12CommandSignature
    UINT stride;
};

// Ray tracing state objects (state-objects.cpp). A create that fails leaves an inert record (no engine object,
// kRecordInvalid), which DestroyStateObject accepts.
struct StateObjectTranslation;                  // state-objects.cpp: the API description rebuilt for the engine
struct StateObjectRecord {
    RecordHeader h;                             // engine: ID3D12StateObject
    ID3D12StateObjectProperties* properties;    // one reference; null for an inert record
    StateObjectTranslation* translation;        // engine-ddi's own allocation, freed by DestroyStateObject
    D3D12DDI_HRTSTATEOBJECT_0054 rt;
    bool executable;                            // a RAYTRACING_PIPELINE, which SetPipelineState1 takes; not a COLLECTION
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
void fill_core_graphics(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_graphics(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_commands(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_commands(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_queries(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_queries(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;
void fill_core_tiles(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_core_state_objects(D3D12DDI_DEVICE_FUNCS_CORE_0088* table) noexcept;
void fill_list_tiles(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* table, uint32_t table_index) noexcept;

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
uint32_t harness_pending_initializations(DeviceContext* context) noexcept;
bool harness_retirement_lost(DeviceContext* context) noexcept;
// Submission retirement points that the retire policy left to the resource DDIs (set_retire_policy).
uint64_t harness_deferred_retire_points(DeviceContext* context) noexcept;
// Fault injection: completed_value returns value instead of the fence's (0 turns it off); the next retirement
// Signal of execute_command_lists is skipped and fails with E_FAIL after the engine's ExecuteCommandLists ran.
void harness_force_completed(EngineQueue* queue, uint64_t value) noexcept;
void harness_fail_next_signal(EngineQueue* queue) noexcept;
void harness_set_in_ddi_bound(DeviceContext* context, uint32_t milliseconds) noexcept;
// Called by CreateStateObject and AddToStateObject with the API description immediately before the engine's
// CreateStateObject or AddToStateObject (parent: the engine object grown from, null for a create), on the calling
// thread; null (the default) calls nothing. The description lives only for the call. Set it before the creates it
// watches, from the thread that makes them.
using StateObjectObserver = void (*)(const D3D12_STATE_OBJECT_DESC& desc, ID3D12StateObject* parent, void* user);
void harness_set_state_object_observer(StateObjectObserver observer, void* user) noexcept;
// Called by log_refusal with each line's text (without the "engine-ddi: " prefix) after the line was written, on the
// calling thread; null (the default) calls nothing. Process-wide, like the once-only lines it lets a test count.
using LogObserver = void (*)(const char* text, void* user);
void harness_set_log_observer(LogObserver observer, void* user) noexcept;
#endif

} // namespace engine_ddi
