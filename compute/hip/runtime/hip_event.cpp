// hip_event.cpp - events over the stream fence values.
//
// Design docs/design/m16-hip-route-b.md section 4.5: an event records a fence value, and its
// host timestamp is taken at the moment that value retires. A GPU timestamp through a second
// RELEASE_MEM is the better answer; it is named in section 7 of the design and it needs one
// cost measurement.
//
// Why the timestamp cannot wait for the first question about the event: hipEventElapsedTime
// asks about two events one after the other, and both values have usually retired long before.
// A timestamp taken at that moment would be the time of the question and not the time of the
// work, so the two stamps would be microseconds apart whatever the kernels did. An event whose
// value has not retired yet therefore goes on a pending list, and every wait and every fence
// read of layer 2 stamps the events the device has passed (events_stamp_retired).

#include <algorithm>

#include "runtime_internal.h"

namespace bc250hip {

void events_stamp_retired(bc250hsa_device* dev) {
    State& s = state();
    if (s.pending_events.empty() || dev == nullptr) {
        return;
    }
    const uint64_t retired = bc250hsa_fence_read(dev);
    if (retired == UINT64_MAX) {
        return;  // a lost device retires nothing; the fault path reports it
    }
    const double now = host_now_ms();
    for (auto it = s.pending_events.begin(); it != s.pending_events.end();) {
        ihipEvent_t* event = *it;
        if (event != nullptr && event->fence_value <= retired) {
            event->host_ms = now;
            event->pending = 0;
            it = s.pending_events.erase(it);
        } else {
            ++it;
        }
    }
}

void event_retain(ihipEvent_t* event) {
    if (event == nullptr) {
        return;
    }
    event->refs++;
}

void event_release(ihipEvent_t* event) {
    if (event == nullptr) {
        return;
    }
    if (--event->refs > 0) {
        return;
    }
    state().live_events--;
    delete event;
}

void events_forget(ihipEvent_t* event) {
    State& s = state();
    s.pending_events.erase(
        std::remove(s.pending_events.begin(), s.pending_events.end(), event),
        s.pending_events.end());
}

}  // namespace bc250hip

namespace {

bool valid(const ihipEvent_t* event) {
    return event != nullptr && event->magic == BC250_HIP_EVENT_MAGIC;
}

// Waits for the event's value, so that its timestamp exists. The caller holds the lock and a
// reference to the event, because the wait opens the lock.
//
// An event that nothing recorded is complete and waits for nothing, which is the HIP contract
// and what hipStreamWaitEvent already answers. Only the timing path refuses such an event, and
// it refuses it itself: there is no timestamp to subtract. MEASURED 2026-10-09: this rule in
// the wrong place stopped llama-bench against the mock. ggml creates its events with
// hipEventCreateWithFlags(hipEventDisableTiming) and calls hipEventSynchronize on one of them
// before anything records it (ggml-cuda.cu, ggml_backend_cuda_device_event_synchronize), and
// hipErrorInvalidHandle there is a stated abort through its own CUDA_CHECK.
hipError_t retire(bc250hip::Guard& guard, ihipEvent_t* event) {
    if (event->recorded == 0 || event->pending == 0) {
        return hipSuccess;
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    const hipError_t waited = guard.wait(dev, event->fence_value);
    if (waited != hipSuccess) {
        return waited;
    }
    // wait_fence stamps every event the device has passed, this one among them. The fallback
    // covers an implementation of layer 1 whose fence read lags its own wait.
    if (event->pending != 0) {
        event->host_ms = bc250hip::host_now_ms();
        event->pending = 0;
        bc250hip::events_forget(event);
    }
    return hipSuccess;
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
    created->pending = 0;
    created->fence_value = 0;
    created->host_ms = 0.0;
    created->refs = 1;   // the handle this call returns
    {
        bc250hip::Guard guard;
        state().live_events++;
    }
    *event = created;
    return hipSuccess;
}

hipError_t hipEventCreate(hipEvent_t* event) {
    return hipEventCreateWithFlags(event, hipEventDefault);
}

hipError_t hipEventDestroy(hipEvent_t event) {
    bc250hip::Guard guard;
    if (!valid(event)) {
        return fail(hipErrorInvalidHandle);
    }
    bc250hip::events_forget(event);
    // The handle dies here. The object itself lives as long as a thread that waits on it still
    // holds a reference.
    event->magic = 0;
    bc250hip::event_release(event);
    return hipSuccess;
}

hipError_t hipEventRecord(hipEvent_t event, hipStream_t stream) {
    bc250hip::Guard guard;
    if (!valid(event)) {
        return fail(hipErrorInvalidHandle);
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    bc250hip::events_forget(event);
    // The event covers everything the stream owes, the event wait it carries among it. The
    // legacy null stream owes the work of the whole device.
    event->fence_value = bc250hip::stream_target_value(dev, target);
    event->recorded = 1;
    event->host_ms = 0.0;
    event->pending = 0;
    if (event->fence_value == 0 || event->fence_value <= bc250hsa_fence_read(dev)) {
        // Nothing of this stream is in flight, so the event is complete at once.
        event->host_ms = bc250hip::host_now_ms();
        return hipSuccess;
    }
    event->pending = 1;
    state().pending_events.push_back(event);
    return hipSuccess;
}

hipError_t hipEventSynchronize(hipEvent_t event) {
    bc250hip::Guard guard;
    if (!valid(event)) {
        return fail(hipErrorInvalidHandle);
    }
    // The reference keeps the event alive across the wait, which opens the lock: another thread
    // may call hipEventDestroy on it in the meantime.
    bc250hip::EventRef held;
    held.attach(event);
    return fail(retire(guard, event));
}

hipError_t hipEventElapsedTime(float* ms, hipEvent_t start, hipEvent_t stop) {
    if (ms == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    bc250hip::Guard guard;
    if (!valid(start) || !valid(stop)) {
        return fail(hipErrorInvalidHandle);
    }
    if ((start->flags & static_cast<unsigned>(hipEventDisableTiming)) != 0 ||
        (stop->flags & static_cast<unsigned>(hipEventDisableTiming)) != 0) {
        return fail(hipErrorInvalidHandle);
    }
    if (start->recorded == 0 || stop->recorded == 0) {
        // No timestamp exists for an event that nothing recorded, so there is nothing to
        // subtract. HIP answers the invalid handle here, not a zero time.
        return fail(hipErrorInvalidHandle);
    }
    bc250hip::EventRef held_start;
    bc250hip::EventRef held_stop;
    held_start.attach(start);
    held_stop.attach(stop);
    hipError_t err = retire(guard, start);
    if (err != hipSuccess) {
        return fail(err);
    }
    err = retire(guard, stop);
    if (err != hipSuccess) {
        return fail(err);
    }
    *ms = static_cast<float>(stop->host_ms - start->host_ms);
    return hipSuccess;
}

}  // extern "C"
