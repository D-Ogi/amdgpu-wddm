// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <atomic>
#include <cstdint>

// BD-133: what orders the copy of a Present source and the kernel present that reads the copy.
//
// The copy is recorded on the command list that the runtime hands to pfnPresent (present-copy.h says why), and the
// kernel present of the copy is made on the device's present context (present-context.h), because the kernel driver
// refuses a present on a queue's UMD context. Two WDDM contexts, so the order is made with monitored fences:
//   - every copy surface has a completion fence of its own (PresentCopySlot::fence, C_S), monotonic for that surface;
//   - pfnPresent takes a ticket on the surface it copies into: k, the next value of C_S, captured there, and queues
//     on the present context a GPU wait for C_S >= k before it returns (PresentContext::ticket);
//   - ExecuteCommandLists of the list that carries the copy submits the work up to and including that list and then
//     signals C_S = k on the executing queue's own context (queue-engine.cpp, execute_lists).
// The present context therefore holds the kernel present until its own copy is finished, whichever queue executes
// it and in whichever order the lists arrive. dxgkrnl queues a GPU wait for a value that is not signaled yet and holds
// the commands behind it (ref\windows-driver-docs\windows-driver-docs-pr\display\context-monitoring.md:47, "GPU wait";
// the kernel driver has no hardware scheduling, driver\kmd\wddm.c:3803-3805, so the software scheduler does this).
//
// No queue waits for anything. A surface is reused only when its last copy has completed (C_S read on the CPU >= its
// last k) and its last reader has retired (the present fence read on the CPU >= the value behind that present), so a
// copy never has to wait for a present on the GPU. Both reads on the CPU are witnesses, and UINT64_MAX, the value a
// removed device's fence reads, is never one (present_copy_reuse). When no surface of the source is reusable, a new
// one is made within the budget below; when the budget or the memory is exhausted, the present is refused before its
// copy is recorded, its source published or any wait queued (engine-ddi resources.cpp, present_copy_allocation).
// An earlier form with one copy fence for every surface, or with a queue wait for the present fence, could close a
// cycle through an application's own queue wait (BD-133 reviews of 2026-10-12: the C3 split and the per-surface cap);
// with a fence per surface and no queue wait, the only waits this adds are the present context's, and each is
// satisfied by a signal that follows its copy on the queue that executes it.
//
// Rule D: no CPU signal ever completes a copy fence. A list that carries an unexecuted ticket and is reset or
// destroyed leaves its present waiting for a copy that never comes: the device is removed, and the surface, its fence
// and the present context are kept for the OS teardown (PresentContext::cancel, PresentCopyListMark::release).
//
// Pure: atomics and plain values only, so the shell's host suites test it (present-outputs-test.cpp,
// present-copy-order-test.cpp).

namespace native12 {

// The pool of one source and its budget. A swap chain of three back buffers needs one or two surfaces a source in
// the ordinary case: a surface is reusable as soon as the present that last read it has finished.
constexpr unsigned kPresentCopySurfacesPerSource = 8;
// Every copy surface of one device, live or kept, in bytes: 512 MiB is 15 surfaces of 3840x2160 at 4 bytes a pixel.
constexpr uint64_t kPresentCopyDeviceBytes = uint64_t{512} << 20;

// A monitored fence value that may be signaled or waited for: never 0 (the initial value, which every waiter has
// already passed) and never UINT64_MAX (what a removed device's fence reads). The D3D11 shell refuses both before
// its callback in the same way (dxvk\runtime-bridge.cpp, signal_present).
constexpr bool present_fence_value_valid(UINT64 value) noexcept { return value != 0 && value != UINT64_MAX; }

// The synchronization state of one copy surface. It lives in the surface's own record (engine-ddi internal.h,
// PresentCopySurface::slot) and is written by the present context only (present-context.h), under its lock, apart
// from retained, which a list reset on any thread can set.
struct PresentCopySlot {
    D3DKMT_HANDLE fence{};                      // C_S, the surface's completion fence; 0 until its first ticket
    const volatile UINT64* fence_cpu{};         // the CPU address of C_S
    UINT64 issued{};                            // k of the last ticket on the surface (0: never copied into)
    UINT64 signaled{};                          // k of the last signal queued behind a copy into it
    UINT64 reader{};                            // the present fence value behind the last present that read it
    std::atomic<bool> retained{false};          // its retirement cannot be proven: never reused, never released
};

// One copied present: its surface and k.
struct PresentCopyTicket {
    PresentCopySlot* slot{};
    UINT64 value{};
};

// Whether a surface can take the next copy without any GPU wait.
enum class PresentCopyReuse : uint32_t {
    Reusable = 0,       // never used, or its last copy completed and its last reader retired
    Busy,               // either is still outstanding, or the surface is kept
    Removed             // a fence read UINT64_MAX: the device was removed, and the read is no witness
};
inline PresentCopyReuse present_copy_reuse(const PresentCopySlot& slot, UINT64 present_cpu) noexcept {
    if (slot.retained.load(std::memory_order_acquire)) return PresentCopyReuse::Busy;
    if (present_cpu == UINT64_MAX) return PresentCopyReuse::Removed;
    if (slot.issued) {
        if (!slot.fence_cpu) return PresentCopyReuse::Busy;
        const UINT64 copied = *slot.fence_cpu;
        if (copied == UINT64_MAX) return PresentCopyReuse::Removed;
        if (copied < slot.issued) return PresentCopyReuse::Busy;
    }
    if (slot.reader && present_cpu < slot.reader) return PresentCopyReuse::Busy;
    return PresentCopyReuse::Reusable;
}

// The surface a copied present takes, among the count surfaces its source has.
enum class PresentCopyChoice : uint32_t {
    Reuse = 0,          // *index names a reusable surface
    Grow,               // none is reusable and the source may have another one
    Budget,             // none is reusable and the source has kPresentCopySurfacesPerSource: the present is refused
    Removed             // a fence read UINT64_MAX: the present fails with the removed device
};
inline PresentCopyChoice present_copy_choose(const PresentCopySlot* const* slots, unsigned count, UINT64 present_cpu,
                                             unsigned* index) noexcept {
    if (index) *index = 0;
    if (present_cpu == UINT64_MAX) return PresentCopyChoice::Removed;
    for (unsigned i = 0; i < count; ++i) {
        if (!slots || !slots[i]) continue;
        const PresentCopyReuse reuse = present_copy_reuse(*slots[i], present_cpu);
        if (reuse == PresentCopyReuse::Removed) return PresentCopyChoice::Removed;
        if (reuse != PresentCopyReuse::Reusable) continue;
        if (index) *index = i;
        return PresentCopyChoice::Reuse;
    }
    return count < kPresentCopySurfacesPerSource ? PresentCopyChoice::Grow : PresentCopyChoice::Budget;
}
// A new surface of size bytes fits next to used bytes of copy surfaces of the device.
constexpr bool present_copy_within_budget(uint64_t used, uint64_t size) noexcept {
    return size <= kPresentCopyDeviceBytes && used <= kPresentCopyDeviceBytes - size;
}

// What pfnPresent hands to the engine half of the copy (engine-ddi present_copy_allocation): the present fence as the
// CPU read it in this call, and the hook that gives a chosen surface its fence before the copy is recorded.
struct PresentCopyPick {
    UINT64 present_cpu{};
    void* state{};
    HRESULT (*prepare)(void* state, PresentCopySlot& slot) noexcept {};
};

// What the reset or the destroy of a list that still carries an unexecuted ticket calls (rule D): the shell removes
// the device and keeps the surface (native-tables.cpp, present_copy_cancelled).
using PresentCopyCancel = void (*)(void* owner, const PresentCopyTicket& ticket) noexcept;

// The ticket a command list carries, in the list's own record (engine-ddi internal.h, CommandListRecord::present_copy):
// one per list, which no other list and no other device can push out (a review of 9f45a4c1 found the earlier form, a
// process-wide table of eight slots, unsafe: a ninth outstanding mark evicted one).
//   Empty -> Pending: pfnPresent recorded a copy on the list (mark). A list that is not Empty takes no second copy.
//   Pending -> Executed: ExecuteCommandLists submitted the list and signals the ticket (take). A list that is
//   Executed and is executed again is refused, because a ticket is signaled once and never reused.
//   any -> Empty: the list's successful Reset and its destroy (release); from Pending that is rule D.
class PresentCopyListMark final {
public:
    enum State : uint32_t { Empty = 0, Pending = 1, Executed = 2 };
    constexpr PresentCopyListMark() noexcept = default;
    PresentCopyListMark(const PresentCopyListMark&) = delete;
    PresentCopyListMark& operator=(const PresentCopyListMark&) = delete;

    bool mark(const PresentCopyTicket& ticket, PresentCopyCancel cancel, void* owner) noexcept {
        if (!ticket.slot || !present_fence_value_valid(ticket.value) || !cancel) return false;
        if (state_.load(std::memory_order_acquire) != Empty) return false;
        ticket_ = ticket;
        cancel_ = cancel;
        owner_ = owner;
        state_.store(Pending, std::memory_order_release);
        return true;
    }
    bool take(PresentCopyTicket* ticket) noexcept {
        if (!ticket) return false;
        *ticket = {};
        uint32_t pending = Pending;
        if (!state_.compare_exchange_strong(pending, Executed, std::memory_order_acq_rel)) return false;
        *ticket = ticket_;
        return true;
    }
    void release() noexcept {
        const uint32_t was = state_.exchange(Empty, std::memory_order_acq_rel);
        const PresentCopyTicket ticket = ticket_;
        const PresentCopyCancel cancel = cancel_;
        void* const owner = owner_;
        ticket_ = {};
        cancel_ = nullptr;
        owner_ = nullptr;
        if (was == Pending && cancel) cancel(owner, ticket);
    }
    State state() const noexcept { return static_cast<State>(state_.load(std::memory_order_acquire)); }
    PresentCopyTicket ticket() const noexcept { return ticket_; }

private:
    std::atomic<uint32_t> state_{Empty};
    PresentCopyTicket ticket_{};
    PresentCopyCancel cancel_{};
    void* owner_{};
};

} // namespace native12
