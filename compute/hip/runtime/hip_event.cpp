// hip_event.cpp - events over the stream fence values.
//
// Design docs/design/m16-hip-route-b.md section 4.5: an event records the stream's last fence
// value and a host timestamp, and hipEventElapsedTime is the difference of two host timestamps,
// each taken when its value retired. A GPU timestamp through a second RELEASE_MEM is the better
// answer; it is named in section 7 of the design and it needs one cost measurement.

#include "runtime_internal.h"

namespace {

// Waits for the event's value, then takes the host timestamp one time. The caller holds the
// lock.
hipError_t retire(ihipEvent_t* event) {
    if (event->recorded == 0) {
        return hipErrorInvalidHandle;
    }
    if (event->host_ms > 0.0) {
        return hipSuccess;
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    if (event->fence_value != 0) {
        const bc250hsa_status status = bc250hsa_wait(dev, event->fence_value, 0, 0);
        if (status != BC250HSA_OK) {
            return bc250hip::translate(status);
        }
    }
    event->host_ms = bc250hip::host_now_ms();
    return hipSuccess;
}

bool valid(const ihipEvent_t* event) {
    return event != nullptr && event->magic == BC250_HIP_EVENT_MAGIC;
}

}  // namespace

using bc250hip::fail;
using bc250hip::state;

extern "C" {

hipError_t hipEventCreateWithFlags(hipEvent_t* event, unsigned int flags) {
    if (event == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    const unsigned known = static_cast<unsigned>(hipEventDisableTiming);
    if ((flags & ~known) != 0) {
        return fail(hipErrorInvalidValue);
    }
    ihipEvent_t* created = new ihipEvent_t();
    created->magic = BC250_HIP_EVENT_MAGIC;
    created->flags = flags;
    created->recorded = 0;
    created->fence_value = 0;
    created->host_ms = 0.0;
    *event = created;
    return hipSuccess;
}

hipError_t hipEventCreate(hipEvent_t* event) {
    return hipEventCreateWithFlags(event, hipEventDefault);
}

hipError_t hipEventDestroy(hipEvent_t event) {
    if (!valid(event)) {
        return fail(hipErrorInvalidHandle);
    }
    event->magic = 0;
    delete event;
    return hipSuccess;
}

hipError_t hipEventRecord(hipEvent_t event, hipStream_t stream) {
    if (!valid(event)) {
        return fail(hipErrorInvalidHandle);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    event->fence_value = target->last_fence;
    event->recorded = 1;
    event->host_ms = 0.0;
    if (event->fence_value == 0) {
        // Nothing is in flight on this stream, so the event is complete at once.
        event->host_ms = bc250hip::host_now_ms();
    }
    return hipSuccess;
}

hipError_t hipEventSynchronize(hipEvent_t event) {
    if (!valid(event)) {
        return fail(hipErrorInvalidHandle);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    return fail(retire(event));
}

hipError_t hipEventElapsedTime(float* ms, hipEvent_t start, hipEvent_t stop) {
    if (ms == nullptr || !valid(start) || !valid(stop)) {
        return fail(hipErrorInvalidHandle);
    }
    if ((start->flags & static_cast<unsigned>(hipEventDisableTiming)) != 0 ||
        (stop->flags & static_cast<unsigned>(hipEventDisableTiming)) != 0) {
        return fail(hipErrorInvalidHandle);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    hipError_t err = retire(start);
    if (err != hipSuccess) {
        return fail(err);
    }
    err = retire(stop);
    if (err != hipSuccess) {
        return fail(err);
    }
    *ms = static_cast<float>(stop->host_ms - start->host_ms);
    return hipSuccess;
}

}  // extern "C"
