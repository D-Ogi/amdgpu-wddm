// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include "ddi-trace.h"
#include <atomic>
#include <cstdint>

// BD-133, review finding 3: the self-check of the ordering of the Present copy, and the mark that the shell's
// ExecuteCommandLists finds the copy by.
//
// The copy is recorded on the command list the runtime hands to pfnPresent (present-copy.h says why), and the kernel
// present of a copied source is made on the shell's present context (present-context.h), not on a queue's context:
// two WDDM contexts. What orders them is made by ExecuteCommandLists of the list that carries the copy
// (queue-engine.cpp): the queue that executes it first waits for the present context's last signal, and after the
// submission the present context waits for that queue's progress. So the order needs to know which list carries a
// copy, and nothing else: whichever queue executes it, the present context waits for that queue.
//
// What this does: pfnPresent marks the list with the context of the queue the same call names (native-tables.cpp,
// queue_present_context), and ExecuteCommandLists asks, for every list it submits, whether that list carries a mark.
// Every way a copied present could go to the kernel without its wait is named in one line, never silently:
//   - the list is executed on another queue than the call named (Mismatch): still ordered, because the wait is for
//     the queue that executed it, but PFND3D12DDI_PRESENT_0051 takes a list and a queue as two separate handles
//     (ref\ddi-display\d3d12umddi.md:14688) and the line says the runtime paired them otherwise;
//   - a present marks a list whose mark was never taken (Remarked): the earlier present's list was not executed
//     through this shell, so its kernel present had no wait for a copy;
//   - the table is full and the oldest mark goes (Dropped): that list's execute will find no mark and order nothing.
// The lab's arm B reads the absence of these lines; the trace line of each present carries both contexts.
//
// Pure: no engine, no runtime callback, no kernel call, so it is host-tested (present-outputs-test.cpp).

namespace native12 {

// What ExecuteCommandLists found for one list.
enum class PresentCopyExecute : uint32_t {
    NotMarked = 0,      // no present recorded a copy on this list: the ordinary case of every other list
    Matched,            // the queue is the one the present named
    Mismatch            // another queue: ordered all the same (the wait is for the executing queue), and logged
};

// What a mark found in the table.
enum class PresentCopyMark : uint32_t {
    Fresh = 0,          // a free slot: the ordinary case
    Remarked,           // the list was still marked: the earlier present's list was never executed here
    Dropped             // no free slot: the oldest mark went, and its list will order nothing
};

// A fixed set of marks. Slots, not a map: at most a handful of presents are between pfnPresent and the execute of
// their list, and a full table must never make a present fail - it drops the oldest mark and counts that instead.
template <unsigned N>
class PresentCopyWatchOf {
public:
    static constexpr unsigned slots = N;
    constexpr PresentCopyWatchOf() noexcept = default;

    // pfnPresent recorded a copy on list; context is the context of the queue that call named. A list already marked
    // is re-marked in place, so a second present on the same list cannot leave two entries; the answer says so.
    PresentCopyMark mark(const void* list, void* context) noexcept {
        if (!list) return PresentCopyMark::Fresh;
        marks_.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0; i < N; ++i) {
            if (lists_[i].load(std::memory_order_acquire) != list) continue;
            contexts_[i].store(context, std::memory_order_relaxed);
            lists_[i].store(list, std::memory_order_release);
            remarks_.fetch_add(1, std::memory_order_relaxed);
            return PresentCopyMark::Remarked;
        }
        for (unsigned i = 0; i < N; ++i) {
            const void* empty = nullptr;
            if (!lists_[i].compare_exchange_strong(empty, list, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
                continue;
            contexts_[i].store(context, std::memory_order_relaxed);
            lists_[i].store(list, std::memory_order_release);
            return PresentCopyMark::Fresh;
        }
        // Every slot holds a mark whose list was never executed. The oldest goes; the count says it happened.
        const unsigned slot = next_.fetch_add(1, std::memory_order_relaxed) % N;
        contexts_[slot].store(context, std::memory_order_relaxed);
        lists_[slot].store(list, std::memory_order_release);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return PresentCopyMark::Dropped;
    }

    // ExecuteCommandLists is submitting list on the queue whose context is context. Takes the mark.
    PresentCopyExecute check(const void* list, void* context) noexcept {
        if (!list) return PresentCopyExecute::NotMarked;
        for (unsigned i = 0; i < N; ++i) {
            if (lists_[i].load(std::memory_order_acquire) != list) continue;
            void* marked = contexts_[i].load(std::memory_order_relaxed);
            lists_[i].store(nullptr, std::memory_order_release);
            if (marked == context) return PresentCopyExecute::Matched;
            mismatches_.fetch_add(1, std::memory_order_relaxed);
            return PresentCopyExecute::Mismatch;
        }
        return PresentCopyExecute::NotMarked;
    }

    // Nothing is marked right now: the one test ExecuteCommandLists makes before it looks at its list handles.
    bool idle() const noexcept {
        for (unsigned i = 0; i < N; ++i)
            if (lists_[i].load(std::memory_order_acquire)) return false;
        return true;
    }
    uint64_t marks() const noexcept { return marks_.load(std::memory_order_relaxed); }
    uint64_t mismatches() const noexcept { return mismatches_.load(std::memory_order_relaxed); }
    uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
    uint64_t remarks() const noexcept { return remarks_.load(std::memory_order_relaxed); }

private:
    std::atomic<const void*> lists_[N]{};
    std::atomic<void*> contexts_[N]{};
    std::atomic<uint64_t> marks_{0}, mismatches_{0}, dropped_{0}, remarks_{0};
    std::atomic<unsigned> next_{0};
};

using PresentCopyWatch = PresentCopyWatchOf<8>;

// The lines of the self-check, on the channel every shell refusal uses (ddi-trace.h, ddi_refusal: the
// AMDGPU_WDDM_LOG sink when the process names one, and the debugger channel always). Counts only, no handle and no
// pointer, as that channel's contract has it. Written for every case, because none is expected at all.
inline void present_copy_execute_mismatch(uint64_t mismatches, uint64_t marks) noexcept {
    ddi_refusal("present copy: a command list that a present recorded a copy on was executed on another queue than "
                "the one that present named (mismatch %llu of %llu marks): the present context waits for the queue "
                "that executed it, so the copy is still ordered before the kernel present",
                static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(marks));
}
// A mark that went before its list was executed: that present's kernel present had no wait for its copy.
inline void present_copy_mark_lost(PresentCopyMark how, uint64_t count, uint64_t marks) noexcept {
    ddi_refusal("present copy: %s (%llu of %llu marks): the kernel present of that copy had no wait for it and may "
                "read it unfinished",
                how == PresentCopyMark::Remarked ? "a present recorded a copy on a list whose earlier copy was never "
                                                   "executed through this shell"
                                                 : "the mark table was full and the oldest unexecuted copy was dropped",
                static_cast<unsigned long long>(count), static_cast<unsigned long long>(marks));
}
// A marked list was executed, and the queue published no progress for it (or none newer than before it), so the
// present context has nothing to wait for: the kernel present that follows may read the copy unfinished.
inline void present_copy_no_progress(HRESULT progress, uint64_t before, uint64_t after) noexcept {
    ddi_refusal("present copy: the queue that executed a copy published no new progress for it (%08lx, %llu before, "
                "%llu after): the present context has nothing to wait for and the kernel present may read the copy "
                "unfinished",
                static_cast<unsigned long>(progress), static_cast<unsigned long long>(before),
                static_cast<unsigned long long>(after));
}

// The one instance of the shell. Constant-initialized (every member is an atomic with a constexpr constructor),
// so the function-local static needs no guard and no order of initialization.
inline PresentCopyWatch& present_copy_watch() noexcept {
    static PresentCopyWatch watch;
    return watch;
}

} // namespace native12
