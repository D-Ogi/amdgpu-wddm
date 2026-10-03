// SPDX-License-Identifier: MIT
// engine-ddi: the entry path experiment (engine-ddi.h, "Entry path"): the direct recording entry's arms and the entry
// statistics. Shared by engine-ddi's slots and workers and by the shell's timed table entries (native-tables.cpp).
//
// Statistics, when BC250_ENTRY_STATS=1: every timed entry counts its call in the calling thread's block (EntryThread)
// and times one call in sixteen, picked by the thread's own xorshift so that a game's fixed call pattern (vertex
// buffer, index buffer, constants, table, draw) cannot line up with the sampling; the time is TSC ticks from an
// lfence-ordered rdtsc to an rdtscp. A replay worker adds its busy, spin, yield and sleep ticks to a block of its own
// (WorkerStats). Blocks are never freed: a row may read the block of a thread that has exited. The shell's Present
// (entry_frame) closes a row per phase with the deltas of every block. Off, a timed entry pays one relaxed load.
#pragma once
#include "engine-ddi.h"
#include <atomic>
#include <cstdint>
#include <intrin.h>

namespace engine_ddi {

// The timed entries: first the direct entry's slots (graphics table), then the slots the shell times around its own
// entry (native-tables.cpp). The names are the DDI table members without "pfn".
#define ENGINE_DDI_ENTRY_DIRECT_CLASSES(X)                                                                             \
    X(DrawInstanced) X(DrawIndexedInstanced) X(IaSetTopology) X(RsSetViewports) X(RsSetScissorRects)                   \
    X(OmSetBlendFactor) X(OmSetStencilRef) X(SetGraphicsRootDescriptorTable) X(SetGraphicsRoot32BitConstant)          \
    X(SetGraphicsRoot32BitConstants) X(SetGraphicsRootConstantBufferView) X(SetGraphicsRootShaderResourceView)        \
    X(SetGraphicsRootUnorderedAccessView) X(IASetIndexBuffer) X(IASetVertexBuffers)
#define ENGINE_DDI_ENTRY_TIMED_LIST_CLASSES(X)                                                                         \
    X(ResourceBarrier) X(OMSetRenderTargets) X(SetPipelineState) X(SetDescriptorHeaps) X(SetGraphicsRootSignature)    \
    X(SetComputeRootSignature) X(SetComputeRootDescriptorTable) X(SetComputeRootConstantBufferView)                    \
    X(SetComputeRoot32BitConstants) X(Dispatch) X(CopyBufferRegion) X(CopyTextureRegion) X(ClearRenderTargetView)     \
    X(ClearDepthStencilView) X(BeginQuery) X(EndQuery) X(ResolveQueryData) X(SetPredication) X(SOSetTargets)           \
    X(CloseCommandList) X(ResetCommandList)
#define ENGINE_DDI_ENTRY_TIMED_CORE_CLASSES(X)                                                                         \
    X(CopyDescriptors) X(CopyDescriptorsSimple) X(ResetCommandPool) X(CreateConstantBufferView)                        \
    X(CreateShaderResourceView) X(CreateUnorderedAccessView) X(CreateSampler) X(MapHeap) X(UnmapHeap)
#define ENGINE_DDI_ENTRY_TIMED_QUEUE_CLASSES(X) X(ExecuteCommandLists) X(SignalFence) X(WaitForFence)
#define ENGINE_DDI_ENTRY_CLASSES(X)                                                                                    \
    ENGINE_DDI_ENTRY_DIRECT_CLASSES(X) ENGINE_DDI_ENTRY_TIMED_LIST_CLASSES(X) ENGINE_DDI_ENTRY_TIMED_CORE_CLASSES(X)  \
    ENGINE_DDI_ENTRY_TIMED_QUEUE_CLASSES(X)

enum class EntryClass : uint32_t {
#define ENGINE_DDI_ENTRY_ENUM(name) name,
    ENGINE_DDI_ENTRY_CLASSES(ENGINE_DDI_ENTRY_ENUM)
#undef ENGINE_DDI_ENTRY_ENUM
    Count
};
inline constexpr uint32_t kEntryClasses = static_cast<uint32_t>(EntryClass::Count);
inline constexpr uint32_t kEntryDirectClasses = static_cast<uint32_t>(EntryClass::IASetVertexBuffers) + 1;

// Why a direct entry took the slot instead (the arm wanted the direct entry).
enum class DirectMiss : uint32_t {
    Record,                                     // not a live command list record
    Admission,                                  // the device's recording binding is not published (set_direct_entry)
    Replay,                                     // deferred replay is off
    Ring,                                       // this thread has no ring yet (or none at all)
    Switch,                                     // the list's pending entries are on another ring
    Encode,                                     // over the size limit, or arguments the slot refuses
    Count
};
inline constexpr uint32_t kDirectMisses = static_cast<uint32_t>(DirectMiss::Count);

// A counter with one writer: a plain load and store, no locked instruction (other threads only read it).
inline void entry_bump(std::atomic<uint64_t>& counter, uint64_t by = 1) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + by, std::memory_order_relaxed);
}

struct EntryThread {
    std::atomic<uint64_t> calls[kEntryClasses];
    std::atomic<uint64_t> samples[kEntryClasses];
    std::atomic<uint64_t> ticks[kEntryClasses];
    std::atomic<uint64_t> direct;               // calls the direct entry recorded
    std::atomic<uint64_t> misses[kDirectMisses];
    uint32_t rng;                               // the owner thread's alone
    DWORD tid;
};

struct WorkerStats {
    std::atomic<uint64_t> busy;                 // TSC ticks running entries
    std::atomic<uint64_t> spin;                 // spinning for work
    std::atomic<uint64_t> yield;                // yielding for work
    std::atomic<uint64_t> sleep;                // asleep (WaitOnAddress)
    std::atomic<uint64_t> entries;
    uint32_t ring;                              // the ring's index on its device
    uint32_t serial;                            // the device's replay serial (low bits)
};

// Arms of BC250_ENTRY_PATH.
enum class EntryArm : uint32_t { Thunk, Direct, DirectPoll, ThunkPad };

// Set once from the knobs (entry_knobs), except arm, which entry_frame rotates. Read on every timed call.
// C4324: a cache line of its own on purpose.
#pragma warning(push)
#pragma warning(disable : 4324)
struct alignas(64) EntryGlobals {
    std::atomic<uint32_t> arm{1};               // EntryArm, Direct unless BC250_ENTRY_PATH says otherwise
    std::atomic<bool> stats{false};
    std::atomic<uint64_t> poll_tsc{0};          // arm DirectPoll: TSC ticks between a spinning worker's looks
};
#pragma warning(pop)
inline EntryGlobals g_entry;

// Reads the knobs once per process (thread-safe); every entry point that can come first calls it.
void entry_knobs() noexcept;
// The calling thread's block, made and registered on first use (null only if it could not be allocated).
EntryThread* entry_thread_slow() noexcept;
inline thread_local EntryThread* t_entry = nullptr;
// A worker's block, registered for the rows; null when the statistics are off.
WorkerStats* entry_worker_stats(uint32_t ring, uint64_t serial) noexcept;

inline bool entry_direct_arm() noexcept {
    const uint32_t arm = g_entry.arm.load(std::memory_order_relaxed);
    return arm == static_cast<uint32_t>(EntryArm::Direct) || arm == static_cast<uint32_t>(EntryArm::DirectPoll);
}

// Counts and, one call in sixteen, times the entry it is constructed in.
class EntryTimer {
public:
    __forceinline explicit EntryTimer(EntryClass c) noexcept : class_(static_cast<uint32_t>(c)) {
        if (!g_entry.stats.load(std::memory_order_relaxed)) return;
        EntryThread* t = t_entry;
        if (!t && !(t = entry_thread_slow())) return;
        thread_ = t;
        entry_bump(t->calls[class_]);
        uint32_t x = t->rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        t->rng = x;
        if (x & 15) return;
        _mm_lfence();
        start_ = __rdtsc();
    }
    __forceinline ~EntryTimer() noexcept {
        if (!start_) return;
        unsigned int aux;
        const uint64_t end = __rdtscp(&aux);
        entry_bump(thread_->ticks[class_], end - start_);
        entry_bump(thread_->samples[class_]);
    }
    // The direct entry's outcome, on the calling thread's block (when counting).
    __forceinline void direct() noexcept {
        if (thread_) entry_bump(thread_->direct);
    }
    __forceinline void miss(DirectMiss why) noexcept {
        if (thread_) entry_bump(thread_->misses[static_cast<uint32_t>(why)]);
    }
    EntryTimer(const EntryTimer&) = delete;
    EntryTimer& operator=(const EntryTimer&) = delete;

private:
    EntryThread* thread_ = nullptr;
    uint64_t start_ = 0;
    uint32_t class_;
};

} // namespace engine_ddi
