// hip_launch.cpp - the call configuration, hipLaunchKernel and the kernel argument pool.
//
// Design docs/design/m16-hip-route-b.md section 4.5: hipLaunchKernel asks the kernel for its
// kernel argument requirements, takes a buffer from a pool, packs the arguments through
// bc250hsa_kernarg_pack and submits one dispatch. The returned fence value becomes the stream's
// last value.
//
// -fhip-new-launch-api is the clang 22 default, so this file implements hipLaunchKernel only.
// The legacy hipSetupArgument and hipLaunchByPtr path stays unwritten.

#include <cstdio>
#include <cstring>

#include "runtime_internal.h"

namespace {

// The configuration of one `kernel<<<grid, block, shared, stream>>>` call. Clang pushes it and
// the kernel stub pops it on the same thread. The depth covers a nested call, which the
// language does not have, and refuses more.
constexpr size_t kConfigDepth = 8;

struct CallConfig {
    dim3         grid;
    dim3         block;
    size_t       shared_bytes;
    hipStream_t  stream;
};

thread_local CallConfig g_configs[kConfigDepth];
thread_local size_t g_config_depth = 0;

constexpr uint32_t kKernargPoolMax = 64;

}  // namespace

namespace bc250hip {

hipError_t kernarg_acquire(Guard& guard, uint32_t bytes, uint32_t alignment,
                           KernargBuffer** out) {
    State& s = state();
    bc250hsa_device* dev = nullptr;
    const hipError_t err = device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    if (alignment == 0) {
        alignment = 16;
    }
    const uint64_t retired = bc250hsa_fence_read(dev);
    if (retired == UINT64_MAX) {
        return translate(BC250HSA_EDEVICELOST);
    }

    KernargBuffer* oldest = nullptr;
    for (KernargBuffer& buffer : s.kernargs) {
        if (buffer.mem.bytes < bytes || (buffer.mem.va % alignment) != 0) {
            continue;
        }
        if (buffer.claimed) {
            continue;   // another thread already waits for this one
        }
        if (!buffer.busy) {
            // Taking a buffer clears its fence value, so that the rule below sees it as taken
            // and not submitted. A buffer that is not busy carries no fence value anyway; the
            // assignment is here so that one line, and not two places, states the rule.
            buffer.busy = true;
            buffer.fence = 0;
            *out = &buffer;
            return hipSuccess;
        }
        if (buffer.fence == 0) {
            // Busy with no fence value is a buffer another thread took and has not submitted
            // yet. Its arguments are being written right now, and a launch of this thread may
            // open the lock between the packing and the submission (the event wait of its
            // stream), so this must never be handed out as free.
            continue;
        }
        if (buffer.fence <= retired) {
            // The work of this buffer has retired, so the buffer is free. Clear the fence value
            // with the same assignment as above: a buffer whose old value stayed would read as
            // free for a second thread as well, and both of them would pack their arguments
            // into it.
            buffer.busy = true;
            buffer.fence = 0;
            *out = &buffer;
            return hipSuccess;
        }
        if (oldest == nullptr || buffer.fence < oldest->fence) {
            oldest = &buffer;
        }
    }

    if (s.kernargs.size() < kKernargPoolMax) {
        // The pool holds its whole capacity from the first use, so that a KernargBuffer pointer
        // stays valid while the vector grows.
        s.kernargs.reserve(kKernargPoolMax);
        // The smallest useful buffer is one page, so a kernel of a few arguments does not make
        // one allocation per launch for the rest of the process.
        const uint64_t request = bytes < 4096 ? 4096 : bytes;
        KernargBuffer buffer;
        const bc250hsa_status status =
            bc250hsa_alloc(dev, request, alignment, BC250HSA_MEM_HOST, &buffer.mem);
        if (status != BC250HSA_OK) {
            return translate(status);
        }
        buffer.busy = true;
        buffer.fence = 0;
        s.kernargs.push_back(buffer);
        *out = &s.kernargs.back();
        return hipSuccess;
    }

    if (oldest == nullptr) {
        return translate(BC250HSA_ENOMEM);
    }
    // The pool is full and every buffer is still in flight. Wait for the oldest one under the
    // wait policy of the process instead of growing without a limit. The claim makes the wait
    // safe with the lock open: no other thread takes this buffer, and the pool vector holds its
    // whole capacity from the first use, so the pointer survives.
    oldest->claimed = true;
    const hipError_t waited = guard.wait(dev, oldest->fence);
    oldest->claimed = false;
    if (waited != hipSuccess) {
        return waited;
    }
    oldest->busy = true;
    oldest->fence = 0;   // taken and not submitted, as every other way out of this function
    *out = oldest;
    return hipSuccess;
}

void kernarg_release(KernargBuffer* buffer, uint64_t fence) {
    if (buffer == nullptr) {
        return;
    }
    buffer->fence = fence;
    buffer->busy = fence != 0;
}

// The kernel behind a host stub, with the code object loaded on the first launch. A launch looks
// it up again after every wait that opened the lock, because another thread may unregister the
// fat binary in the meantime and the kernel records die with it.
static hipError_t resolve_kernel(const void* function, const bc250hsa_kernel** out) {
    State& s = state();
    const auto found = s.functions.find(function);
    if (found == s.functions.end()) {
        return hipErrorInvalidDeviceFunction;
    }
    Function& entry = found->second;
    if (entry.module == nullptr) {
        return hipErrorInvalidDeviceFunction;
    }
    if (entry.kernel == nullptr) {
        const bc250hsa_status load = module_ensure_loaded(entry.module);
        if (load != BC250HSA_OK) {
            return translate(load);
        }
        entry.kernel =
            bc250hsa_module_kernel_by_name(entry.module->loaded, entry.device_name.c_str());
        if (entry.kernel == nullptr) {
            return hipErrorInvalidDeviceFunction;
        }
    }
    *out = entry.kernel;
    return hipSuccess;
}

// The same kernel after a wait, or a refusal. A fat binary that another thread unregistered
// while this launch waited makes the launch invalid, and that is better than a dispatch of a
// kernel whose code object is gone.
static hipError_t same_kernel_after_wait(const void* function, const bc250hsa_kernel* kernel) {
    const bc250hsa_kernel* again = nullptr;
    const hipError_t err = resolve_kernel(function, &again);
    if (err != hipSuccess) {
        return err;
    }
    return again == kernel ? hipSuccess : hipErrorInvalidDeviceFunction;
}

}  // namespace bc250hip

using bc250hip::fail;
using bc250hip::state;

extern "C" {

hipError_t __hipPushCallConfiguration(dim3 gridDim, dim3 blockDim, size_t sharedMemBytes,
                                      hipStream_t stream) {
    if (g_config_depth >= kConfigDepth) {
        return fail(hipErrorInvalidValue);
    }
    CallConfig& config = g_configs[g_config_depth++];
    config.grid = gridDim;
    config.block = blockDim;
    config.shared_bytes = sharedMemBytes;
    config.stream = stream;
    return hipSuccess;
}

hipError_t __hipPopCallConfiguration(dim3* gridDim, dim3* blockDim, size_t* sharedMemBytes,
                                     hipStream_t* stream) {
    if (g_config_depth == 0) {
        return fail(hipErrorInvalidValue);
    }
    const CallConfig& config = g_configs[--g_config_depth];
    if (gridDim != nullptr) {
        *gridDim = config.grid;
    }
    if (blockDim != nullptr) {
        *blockDim = config.block;
    }
    if (sharedMemBytes != nullptr) {
        *sharedMemBytes = config.shared_bytes;
    }
    if (stream != nullptr) {
        *stream = config.stream;
    }
    return hipSuccess;
}

hipError_t hipLaunchKernel(const void* function, dim3 gridDim, dim3 blockDim, void** args,
                           size_t sharedMemBytes, hipStream_t stream) {
    if (function == nullptr) {
        return fail(hipErrorInvalidDeviceFunction);
    }
    if (gridDim.x == 0 || gridDim.y == 0 || gridDim.z == 0 || blockDim.x == 0 ||
        blockDim.y == 0 || blockDim.z == 0) {
        return fail(hipErrorInvalidValue);
    }
    if (sharedMemBytes > UINT32_MAX) {
        return fail(hipErrorInvalidValue);
    }

    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }

    const bc250hsa_kernel* kernel = nullptr;
    err = bc250hip::resolve_kernel(function, &kernel);
    if (err != hipSuccess) {
        return fail(err);
    }

    // The stream is held for the whole launch: the two waits below open the lock, and another
    // thread may call hipStreamDestroy on it meanwhile.
    bc250hip::StreamRef held;
    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    held.attach(target);

    bc250hsa_launch launch;
    std::memset(&launch, 0, sizeof(launch));
    launch.struct_bytes = static_cast<uint32_t>(sizeof(launch));
    launch.grid[0] = gridDim.x;
    launch.grid[1] = gridDim.y;
    launch.grid[2] = gridDim.z;
    launch.block[0] = blockDim.x;
    launch.block[1] = blockDim.y;
    launch.block[2] = blockDim.z;
    launch.dynamic_group_bytes = static_cast<uint32_t>(sharedMemBytes);

    uint32_t kernarg_bytes = 0;
    uint32_t kernarg_align = 0;
    bc250hsa_status status =
        bc250hsa_kernarg_requirements(kernel, &kernarg_bytes, &kernarg_align);
    if (status != BC250HSA_OK) {
        return fail(bc250hip::translate(status));
    }

    bc250hip::KernargBuffer* buffer = nullptr;
    err = bc250hip::kernarg_acquire(guard, kernarg_bytes, kernarg_align, &buffer);
    if (err != hipSuccess) {
        return fail(err);
    }
    // A full pool waited, and a wait opens the lock.
    err = bc250hip::same_kernel_after_wait(function, kernel);
    if (err != hipSuccess) {
        bc250hip::kernarg_release(buffer, 0);
        return fail(err);
    }
    if (buffer->mem.host == nullptr) {
        bc250hip::kernarg_release(buffer, 0);
        return fail(hipErrorOutOfMemory);
    }

    bc250hsa_pack_result packed;
    std::memset(&packed, 0, sizeof(packed));
    packed.struct_bytes = static_cast<uint32_t>(sizeof(packed));
    status = bc250hsa_kernarg_pack(kernel, &launch, args, kernel->explicit_arg_count,
                                   buffer->mem.host, kernarg_bytes, &packed);
    if (status != BC250HSA_OK) {
        bc250hip::kernarg_release(buffer, 0);
        return fail(bc250hip::translate(status));
    }
    if (packed.hostcall_buffer_requested != 0) {
        // Device-side printf needs a host call service, which this build does not have. The
        // counter of the library answers kill criterion K4 of the route document; the launch
        // itself is refused, because a kernel that reads a null host call buffer faults.
        std::fprintf(stderr, "amdhip64: kernel '%s' asks for a host call buffer (device printf), "
                             "which this build does not support\n",
                     kernel->name != nullptr ? kernel->name : "<unnamed>");
        bc250hip::kernarg_release(buffer, 0);
        return fail(hipErrorNotSupported);
    }
    bc250hsa_write_barrier();

    err = bc250hip::stream_drain_pending(guard, target);
    if (err != hipSuccess) {
        bc250hip::kernarg_release(buffer, 0);
        return fail(err);
    }
    // The event wait of this stream waited as well, and the packed buffer must still belong to
    // the kernel this launch resolved.
    err = bc250hip::same_kernel_after_wait(function, kernel);
    if (err != hipSuccess) {
        bc250hip::kernarg_release(buffer, 0);
        return fail(err);
    }

    bc250hsa_dispatch dispatch;
    std::memset(&dispatch, 0, sizeof(dispatch));
    dispatch.struct_bytes = static_cast<uint32_t>(sizeof(dispatch));
    dispatch.flags = 0;
    dispatch.kernel = kernel;
    dispatch.kernarg_va = buffer->mem.va;
    dispatch.launch = launch;

    uint64_t fence = 0;
    status = bc250hsa_dispatch_submit(dev, &dispatch, &fence);
    if (status != BC250HSA_OK) {
        bc250hip::kernarg_release(buffer, 0);
        return fail(bc250hip::translate(status));
    }
    bc250hip::kernarg_release(buffer, fence);
    target->last_fence = fence;
    return hipSuccess;
}

// The dynamic group memory ceiling of one kernel. On a CUDA part a kernel may not ask for more
// than 48 KiB of dynamic shared memory until a program lifts the ceiling with this call, and
// llama.cpp lifts it for every kernel that needs more (CUDA_SET_SHARED_MEMORY_LIMIT in
// common.cuh). This part has no such ceiling: the hardware gives 64 KiB of group memory per
// workgroup and layer 1 checks each dispatch against it.
//
// So the call records the value and refuses one that the hardware could never give. That is
// worth more than a plain success: a program learns about an impossible request at the call
// that makes it.
// The process lock is not recursive, so the group memory limit is read before the lock is
// taken: hipGetDeviceProperties takes the lock itself.
hipError_t hipFuncSetAttribute(const void* func, hipFuncAttribute attr, int value) {
    if (func == nullptr) {
        return fail(hipErrorInvalidDeviceFunction);
    }
    size_t group_limit = 0;
    if (attr == hipFuncAttributeMaxDynamicSharedMemorySize) {
        hipDeviceProp_t prop;
        const hipError_t err = hipGetDeviceProperties(&prop, 0);
        if (err != hipSuccess) {
            return err;  // hipGetDeviceProperties already recorded it
        }
        group_limit = prop.sharedMemPerBlock;
    }

    bc250hip::Guard guard;
    bc250hip::State& s = state();
    const auto found = s.functions.find(func);
    if (found == s.functions.end()) {
        return fail(hipErrorInvalidDeviceFunction);
    }
    switch (attr) {
        case hipFuncAttributeMaxDynamicSharedMemorySize:
            if (value < 0 || static_cast<size_t>(value) > group_limit) {
                return fail(hipErrorInvalidValue);
            }
            found->second.dynamic_group_max = value;
            return hipSuccess;
        case hipFuncAttributePreferredSharedMemoryCarveout:
            // This part has no cache that group memory is carved out of, so there is nothing to
            // prefer. The request is legal and has no effect.
            return hipSuccess;
        default:
            return fail(hipErrorInvalidValue);
    }
}

// A cooperative start, which guarantees that every workgroup of the grid runs at the same time
// so that the grid can synchronise inside itself. This build cannot promise it: one hardware
// queue, no cooperative dispatch packet and no reserved occupancy.
//
// hipDeviceGetAttribute answers 0 for hipDeviceAttributeCooperativeLaunch, and llama.cpp reads
// that attribute at start-up and keeps the plain path (softmax.cu). The entry point exists
// because the backend links against it, and it says no instead of starting a grid that would
// deadlock inside its own barrier.
hipError_t hipLaunchCooperativeKernel(const void* function, dim3 gridDim, dim3 blockDim,
                                      void** args, size_t sharedMemBytes, hipStream_t stream) {
    (void)gridDim;
    (void)blockDim;
    (void)args;
    (void)sharedMemBytes;
    (void)stream;
    if (function == nullptr) {
        return fail(hipErrorInvalidDeviceFunction);
    }
    return fail(hipErrorNotSupported);
}

}  // extern "C"
