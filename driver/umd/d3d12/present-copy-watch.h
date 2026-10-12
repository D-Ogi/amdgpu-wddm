// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include "ddi-trace.h"
#include <atomic>
#include <cstdint>

// BD-133, review finding 3: the self-check of the one assumption the Present copy's ordering rests on.
//
// The copy is recorded on the command list the runtime hands to pfnPresent (present-copy.h says why), and the
// kernel present is made on the context this shell answers with - the present context of the command queue the
// same call names (native-tables.cpp, queue_present_context). Copy N, present N and copy N+1 are therefore in
// order only while the runtime executes that list on that queue. PFND3D12DDI_PRESENT_0051 takes a command list
// and a command queue as two separate handles (ref\ddi-display\d3d12umddi.md:14688) and says nothing that binds
// them together, so nothing in the contract makes that pairing a fact.
//
// What this does about it: pfnPresent marks the list with the present context it answered, and the shell's
// ExecuteCommandLists asks, for every list it submits, whether that list carries a mark and whether the queue it
// is being executed on is the one that mark names. A mismatch is counted and named in one line, so the frame that
// could be read before its copy retires leaves a record instead of a silently wrong picture. The lab's own arm B
// settles the question positively: the trace line of each present carries the context, and a mismatch line would
// be the only sign of the other case.
//
// Pure: no engine, no runtime callback, no kernel call, so it is host-tested (present-outputs-test.cpp).

namespace native12 {

// What ExecuteCommandLists found for one list.
enum class PresentCopyExecute : uint32_t {
    NotMarked = 0,      // no present recorded a copy on this list: the ordinary case of every other list
    Matched,            // the queue is the one whose present context the copy was answered with
    Mismatch            // another queue: the copy and the kernel present are on two contexts, order unproven
};

// A fixed set of marks. Slots, not a map: at most a handful of presents are between pfnPresent and the execute of
// their list, and a full table must never make a present fail - it drops the oldest mark and counts that instead.
template <unsigned N>
class PresentCopyWatchOf {
public:
    static constexpr unsigned slots = N;
    constexpr PresentCopyWatchOf() noexcept = default;

    // pfnPresent recorded a copy on list and answered with context. A list already marked is re-marked in place,
    // so a second present on the same list cannot leave two entries.
    void mark(const void* list, void* context) noexcept {
        if (!list) return;
        marks_.fetch_add(1, std::memory_order_relaxed);
        for (unsigned i = 0; i < N; ++i) {
            if (lists_[i].load(std::memory_order_acquire) != list) continue;
            contexts_[i].store(context, std::memory_order_relaxed);
            lists_[i].store(list, std::memory_order_release);
            return;
        }
        for (unsigned i = 0; i < N; ++i) {
            const void* empty = nullptr;
            if (!lists_[i].compare_exchange_strong(empty, list, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed))
                continue;
            contexts_[i].store(context, std::memory_order_relaxed);
            lists_[i].store(list, std::memory_order_release);
            return;
        }
        // Every slot holds a mark whose list was never executed. The oldest goes; the count says it happened.
        const unsigned slot = next_.fetch_add(1, std::memory_order_relaxed) % N;
        contexts_[slot].store(context, std::memory_order_relaxed);
        lists_[slot].store(list, std::memory_order_release);
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    // ExecuteCommandLists is submitting list on the queue whose present context is context. Takes the mark.
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

private:
    std::atomic<const void*> lists_[N]{};
    std::atomic<void*> contexts_[N]{};
    std::atomic<uint64_t> marks_{0}, mismatches_{0}, dropped_{0};
    std::atomic<unsigned> next_{0};
};

using PresentCopyWatch = PresentCopyWatchOf<8>;

// The line one mismatch writes, on the channel every shell refusal uses (ddi-trace.h, ddi_refusal: the
// AMDGPU_WDDM_LOG sink when the process names one, and the debugger channel always). Counts only, no handle and
// no pointer, as that channel's contract has it. Written for every mismatch, because each one is a frame whose
// copy may not have retired when the kernel present read it, and they are not expected at all.
inline void present_copy_execute_mismatch(uint64_t mismatches, uint64_t marks) noexcept {
    ddi_refusal("present copy: a command list that a present recorded a copy on was executed on a queue whose "
                "present context is not the one that present answered with (mismatch %llu of %llu marks): the "
                "copy and the kernel present are on two contexts and nothing orders them",
                static_cast<unsigned long long>(mismatches), static_cast<unsigned long long>(marks));
}

// The one instance of the shell. Constant-initialized (every member is an atomic with a constexpr constructor),
// so the function-local static needs no guard and no order of initialization.
inline PresentCopyWatch& present_copy_watch() noexcept {
    static PresentCopyWatch watch;
    return watch;
}

} // namespace native12
