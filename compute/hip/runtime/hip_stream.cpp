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
    if (stream == nullptr || stream == hipStreamLegacy || stream == hipStreamPerThread) {
        // The default stream of the process. The device open fills its fields.
        //
        // HIP reserves two handle values for a stream that no program created: the legacy
        // default stream and the per-thread default stream. No stream object can live at
        // address 1 or 2, so the values are safe to test for. This runtime has one process
        // default stream and no per-thread one, which is legal and stricter than HIP asks:
        // work of the per-thread stream then also orders against the legacy one. llama.cpp
        // passes hipStreamPerThread to hipMemcpyPeerAsync.
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

void stream_retain(ihipStream_t* stream) {
    if (stream == nullptr || is_null_stream(stream)) {
        return;
    }
    stream->refs++;
}

void stream_release(ihipStream_t* stream) {
    if (stream == nullptr || is_null_stream(stream)) {
        return;
    }
    if (--stream->refs > 0) {
        return;
    }
    state().live_streams--;
    delete stream;
}

hipError_t stream_drain_pending(Guard& guard, ihipStream_t* stream) {
    if (stream == nullptr) {
        return hipSuccess;
    }
    // The loop runs again only when another thread asked this stream to wait for a later event
    // while this wait was open. That wait is this launch's business as well, so it is waited
    // for, and each turn of the loop is one bounded wait and not a spin.
    for (;;) {
        const uint64_t value = stream->pending_wait;
        if (value == 0) {
            return hipSuccess;
        }
        bc250hsa_device* dev = nullptr;
        const hipError_t err = device(&dev);
        if (err != hipSuccess) {
            return err;
        }
        const hipError_t waited = guard.wait(dev, value);
        if (waited != hipSuccess) {
            return waited;
        }
        if (stream->pending_wait <= value) {
            stream->pending_wait = 0;
            return hipSuccess;
        }
    }
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
    bc250hip::Guard guard;
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
    created->refs = 1;   // the handle this call returns
    state().live_streams++;
    *stream = created;
    return hipSuccess;
}

hipError_t hipStreamCreate(hipStream_t* stream) {
    return hipStreamCreateWithFlags(stream, hipStreamDefault);
}

hipError_t hipStreamDestroy(hipStream_t stream) {
    bc250hip::Guard guard;
    if (!bc250hip::stream_valid(stream) || stream == &state().null_stream) {
        return fail(hipErrorInvalidHandle);
    }
    // A HIP program may destroy a stream with work in flight. The work keeps its kernel
    // argument buffer in the pool, so nothing is freed under the device here. The handle dies
    // at once; the object itself lives as long as a thread that waits on it still holds a
    // reference.
    stream->magic = 0;
    bc250hip::stream_release(stream);
    return hipSuccess;
}

hipError_t hipStreamSynchronize(hipStream_t stream) {
    bc250hip::Guard guard;
    bc250hip::StreamRef held;
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    held.attach(target);
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    const uint64_t value = bc250hip::stream_target_value(dev, target);
    if (value == 0) {
        return hipSuccess;
    }
    const hipError_t waited = guard.wait(dev, value);
    if (waited != hipSuccess) {
        return fail(waited);
    }
    // Only the event wait that this call waited for is cleared. A later one, which another
    // thread asked for while this wait was open, stays.
    if (target->pending_wait <= value) {
        target->pending_wait = 0;
    }
    return hipSuccess;
}

hipError_t hipStreamWaitEvent(hipStream_t stream, hipEvent_t event, unsigned int flags) {
    if (flags != 0) {
        return fail(hipErrorInvalidValue);
    }
    // Both handles are read with the lock held: another thread may be destroying them.
    bc250hip::Guard guard;
    if (event == nullptr || event->magic != BC250_HIP_EVENT_MAGIC) {
        return fail(hipErrorInvalidHandle);
    }
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

// The start of a stream capture, which this runtime does not have: there is no graph object, no
// replay and nothing to record into. It says no.
//
// Why the entry point exists at all. llama.cpp's ggml-cuda keeps every graph call inside its own
// USE_CUDA_GRAPH guard except one: ggml-cuda.cu:4598 calls cudaStreamBeginCapture outside that
// guard, under a run-time condition (`use_cuda_graph`) that is always false when the guard is
// off. A ROCm build does not notice, because its header declares the name whatever the options
// say. Ours has to declare it too, and then something has to be behind the name at link time.
// The call is unreachable; if a future ggml ever reaches it, hipErrorNotSupported travels back
// through that backend's CUDA_CHECK as a stated abort.
hipError_t hipStreamBeginCapture(hipStream_t stream, hipStreamCaptureMode mode) {
    (void)stream;
    (void)mode;
    return bc250hip::refuse("hipStreamBeginCapture", nullptr,
                            "no graph object, no replay and nothing to record into",
                            hipErrorNotSupported);
}

}  // extern "C"
