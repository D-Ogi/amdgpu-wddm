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

hipError_t kernarg_acquire(uint32_t bytes, uint32_t alignment, KernargBuffer** out) {
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
        if (!buffer.busy || buffer.fence <= retired) {
            buffer.busy = true;
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
    // wait policy of the process instead of growing without a limit.
    const hipError_t waited = wait_fence(dev, oldest->fence);
    if (waited != hipSuccess) {
        return waited;
    }
    oldest->busy = true;
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

    std::lock_guard<std::mutex> guard(state().lock);
    bc250hip::State& s = state();
    bc250hsa_device* dev = nullptr;
    hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }

    const auto found = s.functions.find(function);
    if (found == s.functions.end()) {
        return fail(hipErrorInvalidDeviceFunction);
    }
    bc250hip::Function& entry = found->second;
    if (entry.module == nullptr) {
        return fail(hipErrorInvalidDeviceFunction);
    }

    if (entry.kernel == nullptr) {
        const bc250hsa_status load = bc250hip::module_ensure_loaded(entry.module);
        if (load != BC250HSA_OK) {
            return fail(bc250hip::translate(load));
        }
        entry.kernel =
            bc250hsa_module_kernel_by_name(entry.module->loaded, entry.device_name.c_str());
        if (entry.kernel == nullptr) {
            return fail(hipErrorInvalidDeviceFunction);
        }
    }
    const bc250hsa_kernel* kernel = entry.kernel;

    ihipStream_t* target = bc250hip::resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }

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
    err = bc250hip::kernarg_acquire(kernarg_bytes, kernarg_align, &buffer);
    if (err != hipSuccess) {
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
                     kernel->name != nullptr ? kernel->name : entry.device_name.c_str());
        bc250hip::kernarg_release(buffer, 0);
        return fail(hipErrorNotSupported);
    }
    bc250hsa_write_barrier();

    err = bc250hip::stream_drain_pending(target);
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

}  // extern "C"
