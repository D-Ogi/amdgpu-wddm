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

// The copy itself, after the wait. An explicit kind is the caller's word and the runtime does
// not argue with it: it looks up only the side that the kind calls device memory. Only
// hipMemcpyDefault makes the runtime guess, and then a host mapping wins over a numerically
// equal device address.
hipError_t copy_now(void* dst, const void* src, size_t bytes, hipMemcpyKind kind) {
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
    // hipMemset takes one pointer and no kind, so the runtime has to guess here as well. A host
    // mapping wins over a numerically equal device address, which is why this lookup comes
    // first and not second.
    const Allocation* allocation = find_host_allocation(dst, bytes, &offset);
    unsigned char* base = nullptr;
    if (allocation == nullptr) {
        allocation = bc250hip::find_allocation_ptr(dst, bytes, &offset);
        if (allocation == nullptr) {
            return hipErrorInvalidDevicePointer;
        }
        if (allocation->mem.host == nullptr) {
            // A fill kernel is later work, named in the design.
            return hipErrorNotSupported;
        }
    }
    base = static_cast<unsigned char*>(allocation->mem.host) + offset;
    std::memset(base, value, bytes);
    bc250hsa_write_barrier();
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
    // bc250hsa_free states that the caller must know that no submission still reads the memory,
    // so hipFree waits for everything in flight. HIP says the same: a free is synchronous.
    const hipError_t waited = wait_for_device(guard);
    if (waited != hipSuccess) {
        return waited;
    }
    // The wait opened the lock, so the table is read again: another thread may have freed the
    // same pointer, which is the program's mistake and not ours to crash on.
    const auto found = s.by_va.find(va);
    if (found == s.by_va.end()) {
        return hipErrorInvalidDevicePointer;
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
    bc250hip::Guard guard;
    const hipError_t err = wait_for_device(guard);
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
    const hipError_t filled = fill_now(dst, value, sizeBytes);
    return filled == hipSuccess ? hipSuccess : fail(filled);
}

// A two-dimensional copy, row by row. ggml-hip uses it for a tensor whose rows are contiguous
// but whose row stride is not the row length, so it is on the ordinary path of a run and not a
// corner. Build 1 has no two-dimensional copy in layer 1 and no copy engine, so this is a loop
// of one-dimensional copies under one wait, which is what the one-dimensional entry points do
// as well.
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
        const hipError_t copied = copy_now(row_dst, row_src, width, kind);
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
    return fail(hipErrorNotSupported);
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
    return fail(hipErrorNotSupported);
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
    return fail(hipErrorNotSupported);
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
