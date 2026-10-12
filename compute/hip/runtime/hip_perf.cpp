// hip_perf.cpp - the two measurement entry points of layer 2.
//
// bc250hipGetCounters and bc250hipResetCounters report what the submission layer did, which
// no HIP entry point answers: how many times one kernel launch entered the kernel driver.
// That ratio is the number the off-GPU cost of this route is judged by, and
// compute/hip/samples/hipbench.hip prints it beside its own timings. The duration counters
// say where that cost goes: the time inside the waits, inside D3DKMTSubmitCommand and inside
// each copy route, in ticks of one clock.
//
// The counters are the process counters of bc250hsa.h section 3. They are interlocked in
// layer 1, so these two calls need no process lock of their own; they take one anyway, to
// open the device the same way every other entry point does and so that a reset cannot land
// between a launch and its submission.

#include <cstddef>
#include <cstring>

#include "runtime_internal.h"

using bc250hip::fail;

namespace {

// The sizes bc250hipCounters has had (hip_runtime.h). The structure only grows at its end, so
// the first struct_bytes bytes of the newest layout are the older layout, and a caller built
// against an older header gets exactly the fields it knows and nothing written past them.
constexpr size_t kCountersBytesFirst = offsetof(bc250hipCounters, gpu_copy_d2h);
constexpr size_t kCountersBytesCopyRoute = offsetof(bc250hipCounters, ticks_per_second);
static_assert(kCountersBytesFirst == 56, "the first bc250hipCounters layout is 56 bytes");
static_assert(kCountersBytesCopyRoute == 160, "the GPU copy layout is 160 bytes");
static_assert(sizeof(bc250hipCounters) == 256, "the duration layout is 256 bytes");

bool known_counters_size(unsigned bytes) {
    return bytes == kCountersBytesFirst || bytes == kCountersBytesCopyRoute ||
           bytes == sizeof(bc250hipCounters);
}

}  // namespace

extern "C" {

hipError_t bc250hipGetCounters(bc250hipCounters* out) {
    if (out == nullptr || !known_counters_size(out->struct_bytes)) {
        return fail(hipErrorInvalidValue);
    }
    const unsigned caller_bytes = out->struct_bytes;
    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    bc250hsa_counters counters;
    std::memset(&counters, 0, sizeof(counters));
    counters.struct_bytes = static_cast<uint32_t>(sizeof(counters));
    const bc250hsa_status status = bc250hsa_counters_read(&counters);
    if (status != BC250HSA_OK) {
        return fail(bc250hip::translate(status));
    }
    // The newest layout is filled whole and handed over as far as the caller's layout reaches.
    bc250hipCounters full;
    std::memset(&full, 0, sizeof(full));
    full.struct_bytes = caller_bytes;
    full.dispatches = counters.dispatches_built;
    full.submissions = counters.submissions;
    full.batches = counters.batches_submitted;
    full.dispatches_batched = counters.dispatches_batched;
    full.waits = counters.waits;
    full.waits_fast = counters.waits_fast;
    full.gpu_copy_d2h = counters.gpu_copy_d2h;
    full.gpu_copy_d2h_bytes = counters.gpu_copy_d2h_bytes;
    full.gpu_copy_d2h_staged = counters.gpu_copy_d2h_staged;
    full.gpu_copy_d2h_staged_bytes = counters.gpu_copy_d2h_staged_bytes;
    full.gpu_copy_d2d = counters.gpu_copy_d2d;
    full.gpu_copy_d2d_bytes = counters.gpu_copy_d2d_bytes;
    full.gpu_fill = counters.gpu_fill;
    full.gpu_fill_bytes = counters.gpu_fill_bytes;
    full.cpu_copy_fallback = counters.cpu_copy_fallback;
    full.cpu_copy_fallback_bytes = counters.cpu_copy_fallback_bytes;
    full.gpu_copy_packets = counters.gpu_copy_packets;
    full.gpu_copy_forced_flushes = counters.gpu_copy_forced_flushes;
    full.gpu_copy_control_failed = counters.gpu_copy_control_failed;
    full.ticks_per_second = counters.ticks_per_second;
    full.waits_fast_ticks = counters.waits_fast_ticks;
    full.waits_fast_ticks_max = counters.waits_fast_ticks_max;
    full.waits_other_ticks = counters.waits_other_ticks;
    full.waits_other_ticks_max = counters.waits_other_ticks_max;
    full.submit_calls = counters.submit_calls;
    full.submit_ticks = counters.submit_ticks;
    full.gpu_copy_d2h_ticks = counters.gpu_copy_d2h_ticks;
    full.gpu_copy_d2h_staged_ticks = counters.gpu_copy_d2h_staged_ticks;
    full.gpu_copy_d2d_ticks = counters.gpu_copy_d2d_ticks;
    full.gpu_fill_ticks = counters.gpu_fill_ticks;
    full.cpu_copy_fallback_ticks = counters.cpu_copy_fallback_ticks;
    std::memcpy(out, &full, caller_bytes);
    return hipSuccess;
}

hipError_t bc250hipResetCounters(void) {
    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    bc250hsa_counters_reset();
    return hipSuccess;
}

}  // extern "C"
