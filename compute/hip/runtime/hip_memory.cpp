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

// Waits for everything that this stream submitted. Build 1 makes every copy synchronous, so
// this is what keeps a copy behind the kernel that writes its source.
hipError_t wait_for_stream(ihipStream_t* stream) {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    const hipError_t pending = bc250hip::stream_drain_pending(stream);
    if (pending != hipSuccess) {
        return pending;
    }
    if (stream->last_fence == 0) {
        return hipSuccess;
    }
    return bc250hip::translate(bc250hsa_wait(dev, stream->last_fence, 0, 0));
}

hipError_t wait_for_device() {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    const uint64_t value = bc250hsa_fence_last_submitted(dev);
    if (value == 0) {
        return hipSuccess;
    }
    return bc250hip::translate(bc250hsa_wait(dev, value, 0, 0));
}

// The copy itself, after the wait. It takes the kind that the caller asked for, or finds the
// kind from the table when the caller said hipMemcpyDefault.
hipError_t copy_now(void* dst, const void* src, size_t bytes, hipMemcpyKind kind) {
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    uint64_t dst_offset = 0;
    uint64_t src_offset = 0;
    const Allocation* dst_device = bc250hip::find_allocation_ptr(dst, bytes, &dst_offset);
    const Allocation* src_device = bc250hip::find_allocation_ptr(src, bytes, &src_offset);

    if (kind == hipMemcpyDefault) {
        if (dst_device != nullptr && src_device != nullptr) {
            kind = hipMemcpyDeviceToDevice;
        } else if (dst_device != nullptr) {
            kind = hipMemcpyHostToDevice;
        } else if (src_device != nullptr) {
            kind = hipMemcpyDeviceToHost;
        } else {
            kind = hipMemcpyHostToHost;
        }
    }

    switch (kind) {
    case hipMemcpyHostToHost:
        std::memcpy(dst, src, bytes);
        return hipSuccess;
    case hipMemcpyHostToDevice: {
        if (dst_device == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        return bc250hip::translate(
            bc250hsa_copy_to_device(dev, &dst_device->mem, dst_offset, src, bytes));
    }
    case hipMemcpyDeviceToHost: {
        if (src_device == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        return bc250hip::translate(
            bc250hsa_copy_from_device(dev, dst, &src_device->mem, src_offset, bytes));
    }
    case hipMemcpyDeviceToDevice: {
        if (dst_device == nullptr || src_device == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        if (dst_device->mem.host == nullptr || src_device->mem.host == nullptr) {
            // A copy kernel and the copy engine are later work, named in the design.
            return hipErrorNotSupported;
        }
        std::memcpy(static_cast<unsigned char*>(dst_device->mem.host) + dst_offset,
                    static_cast<const unsigned char*>(src_device->mem.host) + src_offset, bytes);
        bc250hsa_write_barrier();
        return hipSuccess;
    }
    default:
        break;
    }
    return hipErrorInvalidMemcpyDirection;
}

hipError_t fill_now(void* dst, int value, size_t bytes) {
    uint64_t offset = 0;
    const Allocation* allocation = bc250hip::find_allocation_ptr(dst, bytes, &offset);
    unsigned char* base = nullptr;
    if (allocation != nullptr) {
        if (allocation->mem.host == nullptr) {
            // A fill kernel is later work, named in the design.
            return hipErrorNotSupported;
        }
        base = static_cast<unsigned char*>(allocation->mem.host) + offset;
    } else {
        allocation = find_host_allocation(dst, bytes, &offset);
        if (allocation == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        base = static_cast<unsigned char*>(allocation->mem.host) + offset;
    }
    std::memset(base, value, bytes);
    bc250hsa_write_barrier();
    return hipSuccess;
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
    out->mem = mem;
    out->host_allocation = false;
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
    std::lock_guard<std::mutex> guard(state().lock);
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
    std::lock_guard<std::mutex> guard(state().lock);
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
    allocation.host_allocation = true;
    state().by_va[allocation.mem.va] = allocation;
    state().va_by_host[allocation.mem.host] = allocation.mem.va;
    *ptr = allocation.mem.host;
    return hipSuccess;
}

static hipError_t free_allocation(uint64_t va) {
    bc250hip::State& s = state();
    const auto found = s.by_va.find(va);
    if (found == s.by_va.end()) {
        return hipErrorInvalidDevicePointer;
    }
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    // bc250hsa_free states that the caller must know that no submission still reads the memory,
    // so hipFree waits for everything in flight. HIP says the same: a free is synchronous.
    const hipError_t waited = wait_for_device();
    if (waited != hipSuccess) {
        return waited;
    }
    bc250hsa_mem mem = found->second.mem;
    if (mem.host != nullptr) {
        s.va_by_host.erase(mem.host);
    }
    s.by_va.erase(found);
    return bc250hip::translate(bc250hsa_free(dev, &mem));
}

hipError_t hipFree(void* ptr) {
    if (ptr == nullptr) {
        return hipSuccess;
    }
    std::lock_guard<std::mutex> guard(state().lock);
    return fail(free_allocation(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr))));
}

hipError_t hipHostFree(void* ptr) {
    if (ptr == nullptr) {
        return hipSuccess;
    }
    std::lock_guard<std::mutex> guard(state().lock);
    const auto found = state().va_by_host.find(ptr);
    if (found == state().va_by_host.end()) {
        return fail(hipErrorInvalidValue);
    }
    return fail(free_allocation(found->second));
}

hipError_t hipMemcpy(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind) {
    if (dst == nullptr || src == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    std::lock_guard<std::mutex> guard(state().lock);
    hipError_t err = wait_for_device();
    if (err != hipSuccess) {
        return fail(err);
    }
    err = copy_now(dst, src, sizeBytes, kind);
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
    std::lock_guard<std::mutex> guard(state().lock);
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    // Build 1 performs an asynchronous copy at once, after the work of this stream retires.
    hipError_t err = wait_for_stream(target);
    if (err != hipSuccess) {
        return fail(err);
    }
    err = copy_now(dst, src, sizeBytes, kind);
    return err == hipSuccess ? hipSuccess : fail(err);
}

hipError_t hipMemset(void* dst, int value, size_t sizeBytes) {
    if (dst == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    std::lock_guard<std::mutex> guard(state().lock);
    const hipError_t err = wait_for_device();
    if (err != hipSuccess) {
        return fail(err);
    }
    const hipError_t filled = fill_now(dst, value, sizeBytes);
    return filled == hipSuccess ? hipSuccess : fail(filled);
}

hipError_t hipMemsetAsync(void* dst, int value, size_t sizeBytes, hipStream_t stream) {
    if (dst == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (sizeBytes == 0) {
        return hipSuccess;
    }
    std::lock_guard<std::mutex> guard(state().lock);
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    const hipError_t err = wait_for_stream(target);
    if (err != hipSuccess) {
        return fail(err);
    }
    const hipError_t filled = fill_now(dst, value, sizeBytes);
    return filled == hipSuccess ? hipSuccess : fail(filled);
}

hipError_t hipMemGetInfo(size_t* freeBytes, size_t* totalBytes) {
    if (freeBytes == nullptr || totalBytes == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    bc250hip::memory_info(freeBytes, totalBytes);
    return hipSuccess;
}

}  // extern "C"
