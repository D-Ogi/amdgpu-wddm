// hip_perf.cpp - the two measurement entry points of layer 2.
//
// bc250hipGetCounters and bc250hipResetCounters report what the submission layer did, which
// no HIP entry point answers: how many times one kernel launch entered the kernel driver.
// That ratio is the number the off-GPU cost of this route is judged by, and
// compute/hip/samples/hipbench.hip prints it beside its own timings.
//
// The counters are the process counters of bc250hsa.h section 3. They are interlocked in
// layer 1, so these two calls need no process lock of their own; they take one anyway, to
// open the device the same way every other entry point does and so that a reset cannot land
// between a launch and its submission.

#include <cstring>

#include "runtime_internal.h"

using bc250hip::fail;

extern "C" {

hipError_t bc250hipGetCounters(bc250hipCounters* out) {
    if (out == nullptr || out->struct_bytes != static_cast<unsigned>(sizeof(*out))) {
        return fail(hipErrorInvalidValue);
    }
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
    out->dispatches = counters.dispatches_built;
    out->submissions = counters.submissions;
    out->batches = counters.batches_submitted;
    out->dispatches_batched = counters.dispatches_batched;
    out->waits = counters.waits;
    out->waits_fast = counters.waits_fast;
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
