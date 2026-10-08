// hip_stream.cpp - software streams over one hardware queue.
//
// Design docs/design/m16-hip-route-b.md section 4.5: a stream is an ordered list of
// submissions and one last fence value. One hardware queue stays underneath, so submissions
// are already ordered and hipStreamWaitEvent only has to hold the next submission of this
// stream back until the recorded value retires.

#include <cstring>

#include "runtime_internal.h"

namespace bc250hip {

bool stream_valid(const ihipStream_t* stream) {
    return stream != nullptr && stream->magic == BC250_HIP_STREAM_MAGIC;
}

ihipStream_t* resolve_stream(hipStream_t stream) {
    if (stream == nullptr) {
        // The default stream of the process. The device open fills its fields.
        return &state().null_stream;
    }
    return stream_valid(stream) ? stream : nullptr;
}

bool is_null_stream(const ihipStream_t* stream) { return stream == &state().null_stream; }

uint64_t stream_target_value(bc250hsa_device* dev, const ihipStream_t* stream) {
    if (stream == nullptr) {
        return 0;
    }
    // The legacy null stream synchronises with every other stream, which HIP states and which
    // its own last_fence cannot express: work submitted on another stream never touches it.
    // The device's last submitted value is that promise, and it is what hipDeviceSynchronize
    // already waits for.
    uint64_t value = is_null_stream(stream) ? bc250hsa_fence_last_submitted(dev)
                                            : stream->last_fence;
    if (stream->pending_wait > value) {
        // An event wait that no later launch has drained yet. A program that waits on this
        // stream waits for the event as well, or a stream with nothing of its own submitted
        // would answer at once while the recorded work still runs.
        value = stream->pending_wait;
    }
    return value;
}

hipError_t stream_drain_pending(ihipStream_t* stream) {
    if (stream == nullptr || stream->pending_wait == 0) {
        return hipSuccess;
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    const hipError_t waited = wait_fence(dev, stream->pending_wait);
    if (waited != hipSuccess) {
        return waited;
    }
    stream->pending_wait = 0;
    return hipSuccess;
}

}  // namespace bc250hip

using bc250hip::fail;
using bc250hip::state;

extern "C" {

hipError_t hipStreamCreateWithFlags(hipStream_t* stream, unsigned int flags) {
    if (stream == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if ((flags & ~static_cast<unsigned>(hipStreamNonBlocking)) != 0) {
        return fail(hipErrorInvalidValue);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    ihipStream_t* created = new ihipStream_t();
    created->magic = BC250_HIP_STREAM_MAGIC;
    created->flags = flags;
    created->last_fence = 0;
    created->pending_wait = 0;
    *stream = created;
    return hipSuccess;
}

hipError_t hipStreamCreate(hipStream_t* stream) {
    return hipStreamCreateWithFlags(stream, hipStreamDefault);
}

hipError_t hipStreamDestroy(hipStream_t stream) {
    if (!bc250hip::stream_valid(stream)) {
        return fail(hipErrorInvalidHandle);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    if (stream == &state().null_stream) {
        return fail(hipErrorInvalidHandle);
    }
    // A HIP program may destroy a stream with work in flight. The work keeps its kernel
    // argument buffer in the pool, so nothing is freed under the device here.
    stream->magic = 0;
    delete stream;
    return hipSuccess;
}

hipError_t hipStreamSynchronize(hipStream_t stream) {
    std::lock_guard<std::mutex> guard(state().lock);
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    const uint64_t value = bc250hip::stream_target_value(dev, target);
    if (value == 0) {
        return hipSuccess;
    }
    const hipError_t waited = bc250hip::wait_fence(dev, value);
    if (waited != hipSuccess) {
        return fail(waited);
    }
    target->pending_wait = 0;
    return hipSuccess;
}

hipError_t hipStreamWaitEvent(hipStream_t stream, hipEvent_t event, unsigned int flags) {
    if (flags != 0) {
        return fail(hipErrorInvalidValue);
    }
    if (event == nullptr || event->magic != BC250_HIP_EVENT_MAGIC) {
        return fail(hipErrorInvalidHandle);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    if (event->recorded == 0) {
        // An event that nothing recorded is already complete, which is what HIP says.
        return hipSuccess;
    }
    if (event->fence_value > target->pending_wait) {
        target->pending_wait = event->fence_value;
    }
    return hipSuccess;
}

}  // extern "C"
