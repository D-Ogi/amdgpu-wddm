// hip_memory.cpp - the allocation table and the memory entry points.
//
// Design docs/design/m16-hip-route-b.md section 4.5: hipMalloc asks for a device-local
// allocation that a host mapping may reach, hipHostMalloc asks for a host-visible one, and a
// copy goes through the mapping. An asynchronous copy is synchronous in build 1, which is legal
// and slow.
//
// A HIP device pointer is the GPU virtual address of the allocation. A program may pass an
// interior pointer (`buffer + offset`) to a copy, so the table lookup accepts a range inside an
// allocation and returns the offset.
//
// Open question 1 of design section 7 is whether D3DKMTLock2 works on a device-local allocation
// on this part. The step-2 probe answers it. Until then hipMalloc tries the device-local path
// first and falls back to a host-visible allocation when no mapping is possible, so that a HIP
// program runs whatever the answer is. The fallback writes one line, because it changes where
// the memory lives.

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "runtime_internal.h"

namespace bc250hip {

const Allocation* find_allocation(uint64_t va, uint64_t bytes, uint64_t* offset) {
    State& s = state();
    if (s.by_va.empty() || va == 0) {
        return nullptr;
    }
    auto it = s.by_va.upper_bound(va);
    if (it == s.by_va.begin()) {
        return nullptr;
    }
    --it;
    const Allocation& allocation = it->second;
    const uint64_t base = allocation.mem.va;
    if (va < base || va - base > allocation.mem.bytes) {
        return nullptr;
    }
    const uint64_t start = va - base;
    if (bytes > allocation.mem.bytes - start) {
        return nullptr;
    }
    if (offset != nullptr) {
        *offset = start;
    }
    return &allocation;
}

const Allocation* find_allocation_ptr(const void* ptr, uint64_t bytes, uint64_t* offset) {
    return find_allocation(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr)), bytes,
                           offset);
}

}  // namespace bc250hip

namespace {

using bc250hip::Allocation;
using bc250hip::state;

// An allocation that a host pointer points into, for a pointer that hipHostMalloc returned.
const Allocation* find_host_allocation(const void* ptr, uint64_t bytes, uint64_t* offset) {
    if (ptr == nullptr) {
        return nullptr;
    }
    const unsigned char* want = static_cast<const unsigned char*>(ptr);
    for (const auto& entry : state().by_va) {
        const Allocation& allocation = entry.second;
        if (allocation.mem.host == nullptr) {
            continue;
        }
        const unsigned char* base = static_cast<const unsigned char*>(allocation.mem.host);
        if (want < base || want > base + allocation.mem.bytes) {
            continue;
        }
        const uint64_t start = static_cast<uint64_t>(want - base);
        if (bytes > allocation.mem.bytes - start) {
            return nullptr;
        }
        if (offset != nullptr) {
            *offset = start;
        }
        return &allocation;
    }
    return nullptr;
}

// Waits for everything that this stream owes. Build 1 makes every copy synchronous, so this is
// what keeps a copy behind the kernel that writes its source. It covers the event wait that the
// stream still carries, and for the legacy null stream it covers the whole device.
hipError_t wait_for_stream(bc250hip::Guard& guard, ihipStream_t* stream) {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    const uint64_t value = bc250hip::stream_target_value(dev, stream);
    if (value == 0) {
        return hipSuccess;
    }
    const hipError_t waited = guard.wait(dev, value);
    if (waited != hipSuccess) {
        return waited;
    }
    // Only the event wait that this call waited for is cleared; a later one stays.
    if (stream->pending_wait <= value) {
        stream->pending_wait = 0;
    }
    return hipSuccess;
}

// The work of the whole device, as it stood when this call started. Work that another thread
// submits while this one waits belongs to the next call.
hipError_t wait_for_device(bc250hip::Guard& guard) {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    const uint64_t value = bc250hsa_fence_last_submitted(dev);
    if (value == 0) {
        return hipSuccess;
    }
    return guard.wait(dev, value);
}

// Is this pointer one that hipHostMalloc returned? A host mapping and a GPU virtual address
// share one numeric space, so this question comes first whenever the runtime has to guess a
// direction. The GPU window of layer 1 may lie inside the host user address space
// (bc250hsa.h, section 4), and then a device virtual address can hold the value of a host
// pointer of the same process.
bool known_host_pointer(const void* ptr) {
    return ptr != nullptr && state().va_by_host.find(ptr) != state().va_by_host.end();
}

// --------------------------------------------------------------------------------------------
// The GPU copy route (scratch/hip-gpu-copy/CONTRACT.md)
//
// Reading device memory with the host processor is an uncached read stream: MEASURED
// 0.21 GB/s, against 15.6 GB/s for a write of the same mapping
// (evidence/m16/perf-rerun-2026-10-09). llama.cpp reads n_vocab floats of logits out of the
// device for every generated token, which for Gemma 12B is 1 MiB; a separate 1 MiB
// device-to-host microbenchmark MEASURED that read at 4.88 ms (cost-breakdown/BREAKDOWN.md
// has the per-token figures). From gpu_copy_min_bytes upwards the bytes therefore move with the
// command processor, and the host processor touches device memory only below that size or when
// the GPU route refuses.
//
// Three routes, in the order this file tries them:
//   1. the destination is one of our own host-visible allocations, so the command processor
//      writes it directly and nothing is staged (CONTRACT.md section 5a). This is the
//      llama.cpp logits case: ggml takes ggml_backend_dev_host_buffer_type, which is our
//      hipHostMalloc;
//   2. the destination is a program's own memory, so the bytes go through layer 1's staging
//      ring (section 5b);
//   3. the host-processor copy of build 1, which is always correct and slow.
//
// A GPU call submits and waits inside layer 1, so it runs with the process lock released
// (runtime_internal.h, Guard::unlocked). The allocations it names are pinned for that time and
// their handles are copied first, because another thread owns the table while the call runs.
// Layer 1 answers with a receipt as well as a status (bc250hsa.h section 5.1), and the receipt
// decides what may follow a failure: the host processor does the work instead only when
// nothing of the GPU attempt is outstanding and the device still retires fences.
// --------------------------------------------------------------------------------------------

// Every fallback writes one line, once per reason per process, and, once the host processor
// has moved the bytes, raises the counter that says how many bytes took the slow path and how
// long they took. The five copy lines are not all copy traffic: a host-to-device copy, a
// host-to-host copy and the default host-processor fill of a mapped allocation are on none of
// them. A fallback count above zero is expected below the threshold, with the route switched
// off, and for the short rows of a two-dimensional copy.
enum FallbackReason {
    kFallbackSmall = 0,       // below the threshold, where a submission costs more
    kFallbackSwitch,          // BC250_HIP_GPU_COPY=0
    kFallbackNoCoherence,     // the control of CONTRACT.md section 8 did not pass
    kFallbackRefused,         // layer 1 refused this particular copy
    kFallbackOverlap,         // the two ranges overlap, which a DMA_DATA may not do
    kFallbackReasonCount
};

void fallback(FallbackReason reason, const char* what, bc250hsa_status status) {
    static bool said[kFallbackReasonCount] = { false, false, false, false, false };
    if (!said[reason]) {
        said[reason] = true;
        bc250hip::log_line(BC250HSA_LOG_WARN,
                           "%s takes the host-processor path (%s); every further copy of this"
                           " reason is counted and not said again",
                           what, status == BC250HSA_OK ? "policy" : bc250hsa_status_string(status));
    }
}

// The host processor has done a copy or a fill that fallback() announced, and it succeeded. A
// host-processor copy that failed is not counted (CONTRACT.md deviation 13.16). The ticks run
// from `start`, which the caller takes before it tries the GPU route, so a GPU attempt that
// layer 1 refused is inside the same line as the host-processor work that replaced it, and
// every copy adds its time to the one line its count is on (bc250hsa.h section 3).
void fallback_done(size_t bytes, uint64_t start) {
    bc250hsa_count_cpu_copy_fallback(bytes, bc250hsa_ticks() - start);
}

// Do the two ranges overlap? Layer 1 refuses a copy packet over an overlap, so this runtime
// answers the question before it builds one.
bool ranges_overlap(uint64_t a, uint64_t b, size_t bytes) {
    if (bytes == 0) {
        return false;
    }
    return (a <= b) ? (b - a < bytes) : (a - b < bytes);
}

// A status that forbids the host-processor fallback. A lost device and a wait that ran out of
// its bound both mean the device is not retiring fences, and a memcpy of a mapping of memory
// such a device still owns would answer stale bytes as a successful copy. BC250HSA_EBUSY is in
// the set because layer 1 reports a timed-out wait for a ring or staging slot that way
// (kmt_memory.c, stage_take, and submit.c, take_slot): the status is different, the device
// state it reports is the same one.
bool no_fallback(bc250hsa_status status) {
    return status == BC250HSA_EDEVICELOST || status == BC250HSA_ETIMEOUT ||
           status == BC250HSA_EBUSY;
}

// May the host processor do the work of a GPU call that failed? Only when layer 1 says that
// nothing of the call is outstanding and the device still retires fences. Work the device
// accepted and did not retire may still write the destination after a host-processor copy
// has answered success, and the program would read the late bytes as its own (CONTRACT.md
// section 7). That holds whatever the status is: BC250HSA_EOS after a submission is such a
// case, and so is any status of a device that layer 1 has marked lost.
bool may_fall_back(bc250hsa_status status, const bc250hsa_op_receipt& receipt) {
    return receipt.state == BC250HSA_OP_NOT_SUBMITTED && !no_fallback(status);
}

bc250hsa_op_receipt new_receipt() {
    bc250hsa_op_receipt receipt;
    std::memset(&receipt, 0, sizeof(receipt));
    receipt.struct_bytes = static_cast<uint32_t>(sizeof(receipt));
    return receipt;
}

// A GPU call that failed where the host processor may not stand in. The line says whether the
// device still owes work of it, which is what a lab reader needs to tell a refusal from a
// write that may still land.
hipError_t gpu_failed(const char* what, bc250hsa_status status,
                      const bc250hsa_op_receipt& receipt) {
    if (receipt.state == BC250HSA_OP_SUBMITTED) {
        bc250hip::log_line(BC250HSA_LOG_ERROR,
                           "%s fails (%s): the device accepted the work and fence value %llu"
                           " has not retired, so the host processor does not redo it and the"
                           " memory stays in use until that value retires",
                           what, bc250hsa_status_string(status),
                           static_cast<unsigned long long>(receipt.fence_value));
    } else {
        bc250hip::log_line(BC250HSA_LOG_ERROR,
                           "%s fails (%s) and the host processor does not redo it, because the"
                           " device does not retire its work",
                           what, bc250hsa_status_string(status));
    }
    return bc250hip::translate(status);
}

// The exclusion of an unretired copy (bc250hsa.h section 5.1), on the side of the host
// processor. A GPU copy whose wait ended without retirement may still read its source and write
// its destination. Layer 1 holds every later submission back until it retires, but a
// host-processor copy or fill of this file is no submission: it could read the destination
// before the copy's bytes land, or write over the source the copy still reads, and answer
// success with wrong bytes. So every host-processor path of this file asks layer 1 first and
// moves nothing while such a copy has not retired, whether it is the configured route or the
// fallback after a refusal. The answer is the status of the GPU attempt that layer 1 refused
// (`status`), or else the status the copy's own wait ended with, so the program sees the error
// the GPU route gives. The read never waits for a fence and is one compare while no copy is
// unretired; once the fence has reached the copy, the routes are what they were. A device that
// layer 1 marked lost is refused the same way, whatever its fence reads and whether a copy is
// unretired or not, with the loss as the answer: a call on a stream with nothing of its own
// outstanding waits for nothing, so this read is the one place that sees the loss.
hipError_t hold_for_unretired_copy(bc250hsa_device* dev, const char* what,
                                   bc250hsa_status status) {
    uint64_t pending = 0;
    const bc250hsa_status unretired = bc250hsa_copy_unretired(dev, &pending);
    if (unretired == BC250HSA_OK) {
        return hipSuccess;
    }
    if (status == BC250HSA_OK || unretired == BC250HSA_EDEVICELOST) {
        status = unretired;
    }
    bc250hsa_count_cpu_copy_exclusion_refusal();
    // One line at the 1st, 2nd, 4th, 8th ... refusal of the process, as layer 1 says the holds
    // of its copy lock. Every caller holds the process lock, so the count needs no atomic.
    static uint64_t refusals = 0;
    ++refusals;
    if ((refusals & (refusals - 1u)) == 0u) {
        if (unretired == BC250HSA_EDEVICELOST) {
            bc250hip::log_line(BC250HSA_LOG_ERROR,
                               "copy exclusion: refused %s on the host processor: the device is"
                               " lost (the unretired copy's fence value is %llu, 0 for none), so"
                               " no byte moves; refusal %llu of this process",
                               what, static_cast<unsigned long long>(pending),
                               static_cast<unsigned long long>(refusals));
        } else {
            bc250hip::log_line(BC250HSA_LOG_ERROR,
                               "copy exclusion: refused %s on the host processor: the copy with"
                               " fence value %llu has not retired (%s), so no byte moves; refusal"
                               " %llu of this process",
                               what, static_cast<unsigned long long>(pending),
                               bc250hsa_status_string(status),
                               static_cast<unsigned long long>(refusals));
        }
    }
    return bc250hip::translate(status);
}

// The allocations one GPU call names, pinned while layer 1 runs the call with the process lock
// released. It is made and destroyed with the lock held. settle() keeps the fence value of work
// the device accepted, also when its wait failed, so that a hipFree of either allocation waits
// for that value; the destructor lets go and wakes a hipFree that waits for the pins.
class Pins {
public:
    Pins() = default;
    Pins(const Pins&) = delete;
    Pins& operator=(const Pins&) = delete;
    ~Pins() {
        bool woke = false;
        for (int i = 0; i < count_; ++i) {
            if (--held_[i]->pins == 0) {
                woke = true;
            }
        }
        if (woke) {
            state().unpinned.notify_all();
        }
    }
    void add(const Allocation* allocation) {
        allocation->pins++;
        held_[count_++] = allocation;
    }
    void settle(const bc250hsa_op_receipt& receipt) {
        if (receipt.state == BC250HSA_OP_NOT_SUBMITTED) {
            return;
        }
        for (int i = 0; i < count_; ++i) {
            if (held_[i]->busy_fence < receipt.fence_value) {
                held_[i]->busy_fence = receipt.fence_value;
            }
        }
    }

private:
    const Allocation* held_[2] = { nullptr, nullptr };
    int               count_ = 0;
};

// Is the GPU route allowed for a copy of this size? `must` is true when the host-processor
// path cannot do the work at all, and then the size does not matter.
bool gpu_route(size_t bytes, bool must) {
    const bc250hip::State& s = state();
    if (!s.gpu_copy_enabled) {
        return false;
    }
    return must || bytes >= s.gpu_copy_min_bytes;
}

// A device-to-host copy. The destination lookup is the one piece the old code did not have:
// copy_now with an explicit hipMemcpyDeviceToHost resolved only the source, so the
// destination of the hot path was never known to be one of ours. It stays a lookup of a host
// mapping and never of a virtual address, so the guess rule of known_host_pointer is
// unchanged.
hipError_t copy_device_to_host(bc250hip::Guard& guard, bc250hsa_device* dev, void* dst,
                               const Allocation* src, uint64_t src_offset, size_t bytes) {
    const uint64_t start = bc250hsa_ticks();
    bc250hsa_status refused = BC250HSA_OK;   // a GPU attempt that layer 1 refused, if any
    if (gpu_route(bytes, false)) {
        uint64_t dst_offset = 0;
        const Allocation* dst_host = find_host_allocation(dst, bytes, &dst_offset);
        const bool direct =
            dst_host != nullptr && (dst_host->mem.flags & BC250HSA_MEM_HOST) != 0;
        const bc250hsa_mem src_mem = src->mem;
        const bc250hsa_mem dst_mem = direct ? dst_host->mem : bc250hsa_mem{};
        bc250hsa_op_receipt receipt = new_receipt();
        bc250hsa_status status = BC250HSA_OK;
        Pins pins;
        pins.add(src);
        if (direct) {
            pins.add(dst_host);
            guard.unlocked([&] {
                status = bc250hsa_copy_device_to_device(dev, &dst_mem, dst_offset, &src_mem,
                                                        src_offset, bytes,
                                                        BC250HSA_COPY_HOST_READS_DST, &receipt);
            });
        } else {
            guard.unlocked([&] {
                status = bc250hsa_copy_from_device_gpu(dev, dst, &src_mem, src_offset, bytes,
                                                       &receipt);
            });
        }
        pins.settle(receipt);
        if (status == BC250HSA_OK) {
            return hipSuccess;
        }
        if (!may_fall_back(status, receipt)) {
            return gpu_failed("hipMemcpy device to host", status, receipt);
        }
        refused = status;
    }
    const hipError_t held = hold_for_unretired_copy(dev, "hipMemcpy device to host", refused);
    if (held != hipSuccess) {
        return held;
    }
    if (refused != BC250HSA_OK) {
        fallback(refused == BC250HSA_EUNSUPPORTED ? kFallbackNoCoherence : kFallbackRefused,
                 "hipMemcpy device to host", refused);
    } else {
        fallback(state().gpu_copy_enabled ? kFallbackSmall : kFallbackSwitch,
                 "hipMemcpy device to host", BC250HSA_OK);
    }
    // The lock is held again here, so the source is still in the table: a hipFree on another
    // thread waited for the pins above and cannot run before this returns.
    const bc250hsa_status copied =
        bc250hsa_copy_from_device(dev, dst, &src->mem, src_offset, bytes);
    if (copied != BC250HSA_OK) {
        return bc250hip::translate(copied);
    }
    fallback_done(bytes, start);
    return hipSuccess;
}

// A device-to-device copy. Two ranges that overlap are never a DMA_DATA: HIP leaves such a
// copy undefined, and the host-processor path at least gives the answer memcpy gives. The
// destination may be the GPU address of a hipHostMalloc buffer (hipHostGetDevicePointer), and
// layer 1 gates such a destination on the coherence control as it gates a device-to-host
// copy (bc250hsa.h section 5.1), so a failed control lands here as BC250HSA_EUNSUPPORTED with
// nothing submitted.
hipError_t copy_device_to_device(bc250hip::Guard& guard, bc250hsa_device* dev,
                                 const Allocation* dst, uint64_t dst_offset,
                                 const Allocation* src, uint64_t src_offset, size_t bytes) {
    const uint64_t start = bc250hsa_ticks();
    const bool mapped = dst->mem.host != nullptr && src->mem.host != nullptr;
    const bool overlap =
        ranges_overlap(src->mem.va + src_offset, dst->mem.va + dst_offset, bytes);
    FallbackReason reason = kFallbackOverlap;
    bc250hsa_status refused = BC250HSA_OK;   // a GPU attempt that layer 1 refused, if any
    if (overlap) {
        if (!mapped) {
            return bc250hip::refuse("hipMemcpy device to device", nullptr,
                                    "the two ranges overlap and neither has a host mapping,"
                                    " and a copy engine packet over an overlap is undefined",
                                    hipErrorInvalidValue);
        }
    } else if (gpu_route(bytes, !mapped)) {
        const bc250hsa_mem dst_mem = dst->mem;
        const bc250hsa_mem src_mem = src->mem;
        bc250hsa_op_receipt receipt = new_receipt();
        bc250hsa_status status = BC250HSA_OK;
        Pins pins;
        pins.add(dst);
        pins.add(src);
        guard.unlocked([&] {
            status = bc250hsa_copy_device_to_device(dev, &dst_mem, dst_offset, &src_mem,
                                                    src_offset, bytes, 0u, &receipt);
        });
        pins.settle(receipt);
        if (status == BC250HSA_OK) {
            return hipSuccess;
        }
        if (!may_fall_back(status, receipt)) {
            return gpu_failed("hipMemcpy device to device", status, receipt);
        }
        if (!mapped) {
            return bc250hip::refuse_status("hipMemcpy device to device", nullptr, status);
        }
        refused = status;
        reason = status == BC250HSA_EUNSUPPORTED ? kFallbackNoCoherence : kFallbackRefused;
    } else {
        reason = state().gpu_copy_enabled ? kFallbackSmall : kFallbackSwitch;
    }
    if (!mapped) {
        return bc250hip::refuse("hipMemcpy device to device", nullptr,
                                "one of the two allocations has no host mapping and the GPU"
                                " copy is not available",
                                hipErrorNotSupported);
    }
    const hipError_t held = hold_for_unretired_copy(dev, "hipMemcpy device to device", refused);
    if (held != hipSuccess) {
        return held;
    }
    fallback(reason, "hipMemcpy device to device", refused);
    std::memcpy(static_cast<unsigned char*>(dst->mem.host) + dst_offset,
                static_cast<const unsigned char*>(src->mem.host) + src_offset, bytes);
    bc250hsa_write_barrier();
    fallback_done(bytes, start);
    return hipSuccess;
}

// The copy itself, after the wait. An explicit kind is the caller's word and the runtime does
// not argue with it: it looks up only the side that the kind calls device memory. Only
// hipMemcpyDefault makes the runtime guess, and then a host mapping wins over a numerically
// equal device address.
hipError_t copy_now(bc250hip::Guard& guard, void* dst, const void* src, size_t bytes,
                    hipMemcpyKind kind) {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    uint64_t dst_offset = 0;
    uint64_t src_offset = 0;
    const Allocation* dst_device = nullptr;
    const Allocation* src_device = nullptr;

    if (kind == hipMemcpyDefault) {
        if (!known_host_pointer(dst)) {
            dst_device = bc250hip::find_allocation_ptr(dst, bytes, &dst_offset);
        }
        if (!known_host_pointer(src)) {
            src_device = bc250hip::find_allocation_ptr(src, bytes, &src_offset);
        }
        if (dst_device != nullptr && src_device != nullptr) {
            kind = hipMemcpyDeviceToDevice;
        } else if (dst_device != nullptr) {
            kind = hipMemcpyHostToDevice;
        } else if (src_device != nullptr) {
            kind = hipMemcpyDeviceToHost;
        } else {
            kind = hipMemcpyHostToHost;
        }
    } else {
        if (kind == hipMemcpyHostToDevice || kind == hipMemcpyDeviceToDevice) {
            dst_device = bc250hip::find_allocation_ptr(dst, bytes, &dst_offset);
        }
        if (kind == hipMemcpyDeviceToHost || kind == hipMemcpyDeviceToDevice) {
            src_device = bc250hip::find_allocation_ptr(src, bytes, &src_offset);
        }
    }

    switch (kind) {
    case hipMemcpyHostToHost: {
        // Either pointer may be one of this runtime's host allocations, which a copy of the
        // device writes directly (CONTRACT.md section 5a).
        const hipError_t held = hold_for_unretired_copy(dev, "hipMemcpy host to host",
                                                        BC250HSA_OK);
        if (held != hipSuccess) {
            return held;
        }
        std::memcpy(dst, src, bytes);
        return hipSuccess;
    }
    case hipMemcpyHostToDevice: {
        if (dst_device == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        const hipError_t held = hold_for_unretired_copy(dev, "hipMemcpy host to device",
                                                        BC250HSA_OK);
        if (held != hipSuccess) {
            return held;
        }
        return bc250hip::translate(
            bc250hsa_copy_to_device(dev, &dst_device->mem, dst_offset, src, bytes));
    }
    case hipMemcpyDeviceToHost: {
        if (src_device == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        return copy_device_to_host(guard, dev, dst, src_device, src_offset, bytes);
    }
    case hipMemcpyDeviceToDevice: {
        if (dst_device == nullptr || src_device == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        return copy_device_to_device(guard, dev, dst_device, dst_offset, src_device, src_offset,
                                     bytes);
    }
    default:
        break;
    }
    return hipErrorInvalidMemcpyDirection;
}

hipError_t fill_now(bc250hip::Guard& guard, void* dst, int value, size_t bytes) {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    uint64_t offset = 0;
    // hipMemset takes one pointer and no kind, so the runtime has to guess here as well. A host
    // mapping wins over a numerically equal device address, which is why this lookup comes
    // first and not second.
    const Allocation* allocation = find_host_allocation(dst, bytes, &offset);
    const bool host_pointer = allocation != nullptr;
    unsigned char* base = nullptr;
    if (allocation == nullptr) {
        allocation = bc250hip::find_allocation_ptr(dst, bytes, &offset);
        if (allocation == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
    }
    // The command processor fills device memory that the host processor cannot reach at all,
    // and, with BC250_HIP_GPU_FILL=1, device memory it can. The default keeps the
    // host-processor fill for a mapped allocation, because a write of that mapping is
    // MEASURED at 15.6 GB/s and there is nothing to win (runtime_internal.h).
    const bool must = allocation->mem.host == nullptr;
    uint64_t start = 0;
    bc250hsa_status refused = BC250HSA_OK;   // a GPU fill that layer 1 refused, if any
    if (!host_pointer && (must || state().gpu_fill_mapped) && gpu_route(bytes, must)) {
        start = bc250hsa_ticks();
        const bc250hsa_mem mem = allocation->mem;
        const uint8_t byte = static_cast<uint8_t>(value & 0xFF);
        bc250hsa_op_receipt receipt = new_receipt();
        bc250hsa_status status = BC250HSA_OK;
        Pins pins;
        pins.add(allocation);
        guard.unlocked([&] {
            status = bc250hsa_fill_device(dev, &mem, offset, byte, bytes, &receipt);
        });
        pins.settle(receipt);
        if (status == BC250HSA_OK) {
            return hipSuccess;
        }
        if (!may_fall_back(status, receipt)) {
            return gpu_failed("hipMemset", status, receipt);
        }
        if (must) {
            return bc250hip::translate(status);
        }
        refused = status;
    }
    if (must) {
        return bc250hip::refuse("hipMemset", nullptr,
                                "the allocation has no host mapping and the GPU fill is turned"
                                " off",
                                hipErrorNotSupported);
    }
    const hipError_t held = hold_for_unretired_copy(dev, "hipMemset", refused);
    if (held != hipSuccess) {
        return held;
    }
    if (refused != BC250HSA_OK) {
        // A fill of a destination with a host mapping needs the coherence control too, because
        // the program may read those bytes through that mapping (bc250hsa.h section 5.1), so
        // BC250HSA_EUNSUPPORTED here has the same meaning it has for a copy.
        fallback(refused == BC250HSA_EUNSUPPORTED ? kFallbackNoCoherence : kFallbackRefused,
                 "hipMemset", refused);
    }
    base = static_cast<unsigned char*>(allocation->mem.host) + offset;
    std::memset(base, value, bytes);
    bc250hsa_write_barrier();
    // Only a fill the GPU route refused is a fallback. The host-processor fill of a mapped
    // allocation is the default of gpu_fill_mapped and was never counted.
    if (refused != BC250HSA_OK) {
        fallback_done(bytes, start);
    }
    return hipSuccess;
}

// A HIP device pointer is a GPU virtual address, and a Windows process addresses its own image
// and heap with numbers of the same size. When the GPU address window of layer 1 lies inside
// the host user address space, one number can mean both things, and then hipMemcpyDefault and
// hipMemset cannot tell them apart (bc250hsa.h, section 4). This asks the operating system
// whether the address is already part of this process, and says so one time. A program that
// passes the direction of every copy is not affected.
void warn_on_address_collision(uint64_t va) {
    static bool said = false;
    if (said || va == 0) {
        return;
    }
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION info;
    std::memset(&info, 0, sizeof(info));
    if (VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(va)), &info, sizeof(info)) ==
            0 ||
        info.State == MEM_FREE) {
        return;
    }
#else
    return;
#endif
    said = true;
    std::fprintf(stderr, "amdhip64: the GPU address window overlaps the host address space of "
                         "this process. Pass the direction of every copy, because "
                         "hipMemcpyDefault cannot tell a device pointer from a host pointer "
                         "here\n");
}

// hipMalloc. It asks for device-local memory that a mapping may reach, and falls back to a
// host-visible allocation when the device-local one cannot be mapped.
hipError_t allocate_device(size_t bytes, Allocation* out) {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    bc250hsa_mem mem;
    std::memset(&mem, 0, sizeof(mem));
    bc250hsa_status status = bc250hsa_alloc(dev, bytes, 0, BC250HSA_MEM_MAPPABLE, &mem);
    if (status == BC250HSA_OK && mem.host == nullptr) {
        void* host = nullptr;
        const bc250hsa_status mapped = bc250hsa_map(dev, &mem, &host);
        if (mapped == BC250HSA_OK) {
            // The out parameter is the only result that bc250hsa.h promises, so the handle of
            // this runtime carries what the call returned and never what the library may have
            // written into the handle itself.
            mem.host = host;
        }
        if (mapped != BC250HSA_OK) {
            static bool said = false;
            if (!said) {
                said = true;
                std::fprintf(stderr, "amdhip64: device-local memory has no host mapping here (%s); "
                                     "hipMalloc uses host-visible memory instead\n",
                             bc250hsa_status_string(mapped));
            }
            bc250hsa_free(dev, &mem);
            std::memset(&mem, 0, sizeof(mem));
            status = bc250hsa_alloc(dev, bytes, 0, BC250HSA_MEM_HOST, &mem);
        }
    }
    if (status != BC250HSA_OK) {
        return bc250hip::translate(status);
    }
    warn_on_address_collision(mem.va);
    out->mem = mem;
    return hipSuccess;
}

}  // namespace

using bc250hip::fail;

extern "C" {

hipError_t hipMalloc(void** ptr, size_t size) {
    if (ptr == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (size == 0) {
        *ptr = nullptr;
        return hipSuccess;
    }
    bc250hip::Guard guard;
    Allocation allocation;
    const hipError_t err = allocate_device(size, &allocation);
    if (err != hipSuccess) {
        return fail(err);
    }
    state().by_va[allocation.mem.va] = allocation;
    *ptr = reinterpret_cast<void*>(static_cast<uintptr_t>(allocation.mem.va));
    return hipSuccess;
}

hipError_t hipHostMalloc(void** ptr, size_t size, unsigned int flags) {
    if (ptr == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    (void)flags;  // every flag of step 2 asks for the same host-visible allocation
    if (size == 0) {
        *ptr = nullptr;
        return hipSuccess;
    }
    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    Allocation allocation;
    std::memset(&allocation.mem, 0, sizeof(allocation.mem));
    const bc250hsa_status status =
        bc250hsa_alloc(dev, size, 0, BC250HSA_MEM_HOST, &allocation.mem);
    if (status != BC250HSA_OK) {
        return fail(bc250hip::translate(status));
    }
    if (allocation.mem.host == nullptr) {
        bc250hsa_free(dev, &allocation.mem);
        return fail(hipErrorOutOfMemory);
    }
    state().by_va[allocation.mem.va] = allocation;
    state().va_by_host[allocation.mem.host] = allocation.mem.va;
    *ptr = allocation.mem.host;
    return hipSuccess;
}

// bc250hsa_free states that the caller must know that no submission still reads or writes the
// memory, so hipFree waits for three things before it frees: the work of the whole device as
// it stood when the call started (HIP says the same: a free is synchronous), every GPU copy or
// fill that pins the allocation now, because such a call runs with the lock released and may
// not have submitted yet, and the last fence value one of them left with the device, which a
// failed wait of that call did not retire. A wait that fails returns its error and leaves the
// allocation in the table: memory the device may still write is never freed.
static hipError_t free_allocation(bc250hip::Guard& guard, uint64_t va) {
    bc250hip::State& s = state();
    if (s.by_va.find(va) == s.by_va.end()) {
        return hipErrorInvalidDevicePointer;
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    uint64_t need = bc250hsa_fence_last_submitted(dev);
    uint64_t waited = 0;
    for (;;) {
        if (need > waited) {
            const hipError_t result = guard.wait(dev, need);
            if (result != hipSuccess) {
                return result;
            }
            waited = need;
        }
        // Every wait opens the lock, so the table is read again each time: another thread may
        // have freed the same pointer, which is the program's mistake and not ours to crash on.
        const auto found = s.by_va.find(va);
        if (found == s.by_va.end()) {
            return hipErrorInvalidDevicePointer;
        }
        if (found->second.pins > 0) {
            guard.wait_until(s.unpinned, [&s, va] {
                const auto again = s.by_va.find(va);
                return again == s.by_va.end() || again->second.pins == 0;
            });
            continue;
        }
        if (found->second.busy_fence > waited) {
            need = found->second.busy_fence;
            continue;
        }
        bc250hsa_mem mem = found->second.mem;
        if (mem.host != nullptr) {
            s.va_by_host.erase(mem.host);
        }
        s.by_va.erase(found);
        return bc250hip::translate(bc250hsa_free(dev, &mem));
    }
}

hipError_t hipFree(void* ptr) {
    if (ptr == nullptr) {
        return hipSuccess;
    }
    bc250hip::Guard guard;
    return fail(free_allocation(guard, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr))));
}

hipError_t hipHostFree(void* ptr) {
    if (ptr == nullptr) {
        return hipSuccess;
    }
    bc250hip::Guard guard;
    const auto found = state().va_by_host.find(ptr);
    if (found == state().va_by_host.end()) {
        return fail(hipErrorInvalidValue);
    }
    return fail(free_allocation(guard, found->second));
}

hipError_t hipMemcpy(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind) {
    if (dst == nullptr || src == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    bc250hip::Guard guard;
    hipError_t err = wait_for_device(guard);
    if (err != hipSuccess) {
        return fail(err);
    }
    // The lookup and the copy both happen after the wait, with the lock held, so a pointer that
    // another thread freed during the wait is answered and never copied through.
    err = copy_now(guard, dst, src, sizeBytes, kind);
    return err == hipSuccess ? hipSuccess : fail(err);
}

hipError_t hipMemcpyAsync(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind,
                          hipStream_t stream) {
    if (dst == nullptr || src == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    bc250hip::Guard guard;
    bc250hip::StreamRef held;
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    held.attach(target);
    // Build 1 performs an asynchronous copy at once, after the work of this stream retires.
    hipError_t err = wait_for_stream(guard, target);
    if (err != hipSuccess) {
        return fail(err);
    }
    err = copy_now(guard, dst, src, sizeBytes, kind);
    return err == hipSuccess ? hipSuccess : fail(err);
}

hipError_t hipMemset(void* dst, int value, size_t sizeBytes) {
    if (dst == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    bc250hip::Guard guard;
    const hipError_t err = wait_for_device(guard);
    if (err != hipSuccess) {
        return fail(err);
    }
    const hipError_t filled = fill_now(guard, dst, value, sizeBytes);
    return filled == hipSuccess ? hipSuccess : fail(filled);
}

hipError_t hipMemsetAsync(void* dst, int value, size_t sizeBytes, hipStream_t stream) {
    if (dst == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    bc250hip::Guard guard;
    bc250hip::StreamRef held;
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    held.attach(target);
    const hipError_t err = wait_for_stream(guard, target);
    if (err != hipSuccess) {
        return fail(err);
    }
    const hipError_t filled = fill_now(guard, dst, value, sizeBytes);
    return filled == hipSuccess ? hipSuccess : fail(filled);
}

// A two-dimensional copy, row by row. ggml-hip uses it for a tensor whose rows are contiguous
// but whose row stride is not the row length, so it is on the ordinary path of a run and not a
// corner.
//
// Each row is still its own call of copy_now, which means its own submission and its own fence
// on the GPU route. CONTRACT.md deviation 13.10 states that and its cost: the one-fence row
// list the copy operation list was built for has no caller yet. Two consequences a reader of a
// lab report needs: a copy of h rows costs h submissions, and a row below the GPU threshold
// counts one host-processor fallback per row, so a two-dimensional copy of small rows is the
// one shape where a non-zero fallback count is expected and not a finding.
hipError_t hipMemcpy2DAsync(void* dst, size_t dpitch, const void* src, size_t spitch,
                            size_t width, size_t height, hipMemcpyKind kind,
                            hipStream_t stream) {
    if (dst == nullptr || src == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (width == 0 || height == 0) {
        return hipSuccess;
    }
    // A row must fit in its pitch, or the copy would read or write outside the rows it was
    // given. HIP reports this as an invalid value.
    if (width > dpitch || width > spitch) {
        return fail(hipErrorInvalidValue);
    }
    bc250hip::Guard guard;
    bc250hip::StreamRef held;
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    held.attach(target);
    const hipError_t err = wait_for_stream(guard, target);
    if (err != hipSuccess) {
        return fail(err);
    }
    // One lookup per row, which also means one bounds check per row: a height that runs past
    // the end of either allocation is answered on the row that does it, and the rows before it
    // are already copied. HIP gives the same partial result for the same mistake.
    for (size_t row = 0; row < height; ++row) {
        void* row_dst = static_cast<char*>(dst) + row * dpitch;
        const void* row_src = static_cast<const char*>(src) + row * spitch;
        const hipError_t copied = copy_now(guard, row_dst, row_src, width, kind);
        if (copied != hipSuccess) {
            return fail(copied);
        }
    }
    return hipSuccess;
}

// A copy between two devices. This process has one device, so the only legal call names device
// 0 on both sides, and that call is an ordinary device-to-device copy. llama.cpp reaches this
// entry point when a tensor moves between two of its backend buffers.
hipError_t hipMemcpyPeerAsync(void* dst, int dstDeviceId, const void* src, int srcDeviceId,
                              size_t sizeBytes, hipStream_t stream) {
    if (dst == nullptr || src == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (dstDeviceId != 0 || srcDeviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    return hipMemcpyAsync(dst, src, sizeBytes, hipMemcpyDeviceToDevice, stream);
}

// The device address of memory hipHostMalloc returned. Every allocation of this runtime is one
// allocation with one GPU virtual address, and a host-visible one carries a host mapping of the
// same memory, so the answer is the address of the allocation plus the offset the caller is
// pointing at. An interior pointer is therefore legal, as it is for a copy.
hipError_t hipHostGetDevicePointer(void** devPtr, void* hstPtr, unsigned int flags) {
    if (devPtr == nullptr || hstPtr == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (flags != 0) {
        return fail(hipErrorInvalidValue);
    }
    bc250hip::Guard guard;
    uint64_t offset = 0;
    const Allocation* allocation = find_host_allocation(hstPtr, 0, &offset);
    if (allocation == nullptr) {
        // Not memory of this runtime. hipHostRegister is the entry point that would make a
        // program's own memory reachable, and it says no in this build.
        return fail(hipErrorInvalidValue);
    }
    *devPtr = reinterpret_cast<void*>(static_cast<uintptr_t>(allocation->mem.va + offset));
    return hipSuccess;
}

// Page-locking a program's own memory and giving the GPU an address for it is not in layer 1:
// bc250hsa.h allocates and maps its own memory and has no entry point that takes an existing
// host range. The honest answer is therefore no, and not a pretended success that would hand
// the GPU an address the hardware cannot translate.
//
// llama.cpp calls this only when GGML_CUDA_REGISTER_HOST is in the environment. It clears the
// error and runs without registered host memory, which costs one copy through a staging buffer
// per upload (ggml-cuda.cu, ggml_backend_cuda_register_host_buffer).
hipError_t hipHostRegister(void* hostPtr, size_t sizeBytes, unsigned int flags) {
    (void)sizeBytes;
    (void)flags;
    if (hostPtr == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    return bc250hip::refuse("hipHostRegister", nullptr,
                            "layer 1 has no entry point that maps an existing host range",
                            hipErrorNotSupported);
}

hipError_t hipHostUnregister(void* hostPtr) {
    if (hostPtr == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    // Nothing can be registered, so nothing can be unregistered.
    return fail(hipErrorHostMemoryNotRegistered);
}

// Managed memory, which the hardware would have to fault on and migrate page by page. This
// driver has no page fault handler for a compute queue, so there is no managed memory.
//
// hipErrorNotSupported is the exact answer llama.cpp tests for: with it,
// ggml_cuda_device_malloc falls back to hipMalloc and says so one time
// ("hipMallocManaged unsupported, falling back to hipMalloc"). Any other error code would
// become an abort of the program.
hipError_t hipMallocManaged(void** ptr, size_t size, unsigned int flags) {
    (void)size;
    (void)flags;
    if (ptr == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    *ptr = nullptr;
    return bc250hip::refuse("hipMallocManaged", nullptr,
                            "no page fault handler for a compute queue, so no managed memory",
                            hipErrorNotSupported);
}

// Advice about managed memory, which this build does not have. llama.cpp ignores the result of
// this call and clears the error afterwards.
hipError_t hipMemAdvise(const void* devPtr, size_t count, hipMemoryAdvise advice, int deviceId) {
    (void)count;
    (void)advice;
    if (devPtr == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (deviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    return bc250hip::refuse("hipMemAdvise", nullptr,
                            "advice about managed memory, which this build does not have",
                            hipErrorNotSupported);
}

hipError_t hipMemGetInfo(size_t* freeBytes, size_t* totalBytes) {
    if (freeBytes == nullptr || totalBytes == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    bc250hip::memory_info(freeBytes, totalBytes);
    return hipSuccess;
}

}  // extern "C"
