// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <cstdint>
namespace native12 {
// A device-wide proof of GPU retirement, taken without naming a context (M15.8, fix F2 of the trial 245
// report). engine-ddi proves retirement from its own D3D12 fence on each engine queue, which covers every
// ExecuteCommandLists of the device; it cannot cover a submission the engine or the ICD makes on a context
// outside that set (trial 153 found the engine's internal null-cookie context). The shell can: the ICD
// signals a monitored fence of the submitting context after every native submit and publishes the value
// through BC250_HOST_PUBLISH_PROGRESS (radv_wddm2_cs.c:1267-1287), on application and internal contexts
// alike, and the shell's hosted dispatch sees every one of those publications and holds the fence's CPU
// mapping (hosted-dispatch.cpp, BC250_HOST_PUBLISH_PROGRESS in operation()).
//
// A snapshot therefore names, per monitored fence, the largest value published on it that the fence has not
// reached yet: everything submitted on every context of this device before the snapshot has retired once
// every mark is reached. A fence that has caught up needs no mark, so a device that is idle between frames
// gives an empty snapshot, which is retired at once.
inline constexpr unsigned kMaxProgressMarks = 8;
struct ProgressMark {
    uint64_t sync{};                            // the fence's identity inside the device (not its KMT handle,
                                                // which the kernel may hand out again after a destroy)
    uint64_t value{};                           // the largest value published on it at the snapshot
};
struct ProgressSnapshot {
    unsigned count{};
    bool complete{true};                        // false: more unretired fences than marks fit, so the
                                                // snapshot proves nothing and must be taken again
    ProgressMark marks[kMaxProgressMarks]{};
};
// The source of such snapshots. Its two calls take the source's own lock, so a caller must hold no lock of
// its own across them (the heap imports' lock is a leaf and the hosted dispatch takes it through borrow).
struct ProgressSource {
    void* owner{};
    void (*snapshot)(void* owner, ProgressSnapshot* out) noexcept {};
    bool (*retired)(void* owner, const ProgressSnapshot* snapshot) noexcept {};
    bool usable() const noexcept { return owner && snapshot && retired; }
};
}
