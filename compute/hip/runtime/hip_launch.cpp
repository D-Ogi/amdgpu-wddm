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

// The arena of hip_kernarg_arena.cpp, with its one allocation made on the first launch. A
// slice the arena cannot give (too large, the next block still held, or a second waiter) comes
// from the pool, so the arena never makes a launch fail that the pool alone would run.
hipError_t kernarg_take(Guard& guard, uint32_t bytes, uint32_t alignment, KernargLease* out) {
    State& s = state();
    bc250hsa_device* dev = nullptr;
    const hipError_t err = device(&dev);
    if (err != hipSuccess) {
        return err;
    }
    *out = KernargLease{};
    if (s.kernarg_arena_enabled && s.kernarg_arena_mem.host == nullptr) {
        const uint64_t arena_bytes =
            static_cast<uint64_t>(KernargArena::kBlocks) * KernargArena::kBlockBytes;
        const bc250hsa_status status =
            bc250hsa_alloc(dev, arena_bytes, 4096u, BC250HSA_MEM_HOST, &s.kernarg_arena_mem);
        if (status != BC250HSA_OK || s.kernarg_arena_mem.host == nullptr) {
            log_line(BC250HSA_LOG_ERROR, "the kernel argument arena does not allocate (%s); "
                                         "every launch takes the pool",
                     bc250hsa_status_string(status));
            s.kernarg_arena_mem = bc250hsa_mem{};
            s.kernarg_arena_enabled = false;
        }
    }
    // Two rounds: the second one sees the arena after the wait of the first. A third would
    // wait again for a block another thread took meanwhile, and the pool is the better answer.
    for (int round = 0; s.kernarg_arena_enabled && round < 2; ++round) {
        const uint64_t retired = bc250hsa_fence_read(dev);
        if (retired == UINT64_MAX) {
            return translate(BC250HSA_EDEVICELOST);
        }
        uint32_t offset = 0;
        uint64_t wait_for = 0;
        const ArenaTake taken =
            arena_take(s.kernarg_arena, bytes, alignment, retired, &offset, &wait_for);
        if (taken == ArenaTake::kTaken) {
            out->host = static_cast<unsigned char*>(s.kernarg_arena_mem.host) + offset;
            out->va = s.kernarg_arena_mem.va + offset;
            out->offset = offset;
            return hipSuccess;
        }
        if (taken == ArenaTake::kFallback) {
            break;
        }
        // The claim keeps every other thread off this block while the lock is released. The
        // arena is a member of State and never moves, so the offsets stay valid.
        const hipError_t waited = guard.wait(dev, wait_for);
        arena_unclaim(s.kernarg_arena, offset);
        if (waited != hipSuccess) {
            return waited;
        }
    }
    KernargBuffer* buffer = nullptr;
    const hipError_t pooled = kernarg_acquire(guard, bytes, alignment, &buffer);
    if (pooled != hipSuccess) {
        return pooled;
    }
    out->pooled = buffer;
    out->host = static_cast<unsigned char*>(buffer->mem.host);
    out->va = buffer->mem.va;
    return hipSuccess;
}

void kernarg_settle(KernargLease& lease, uint64_t fence) {
    if (lease.pooled != nullptr) {
        kernarg_release(lease.pooled, fence);
    } else if (lease.host != nullptr) {
        arena_settle(state().kernarg_arena, lease.offset, fence);
    }
    lease = KernargLease{};
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
            log_line(BC250HSA_LOG_ERROR, "the code object of kernel '%s' does not load: %s",
                     entry.device_name.c_str(), bc250hsa_status_string(load));
            return translate(load);
        }
        entry.kernel =
            bc250hsa_module_kernel_by_name(entry.module->loaded, entry.device_name.c_str());
        if (entry.kernel == nullptr) {
            log_line(BC250HSA_LOG_ERROR,
                     "the code object loaded, and it holds no kernel named '%s'",
                     entry.device_name.c_str());
            return hipErrorInvalidDeviceFunction;
        }
    }
    *out = entry.kernel;
    return hipSuccess;
}

hipError_t resolve_kernel_ref(const KernelRef& ref, const bc250hsa_kernel** out) {
    if (ref.host_stub != nullptr) {
        return resolve_kernel(ref.host_stub, out);
    }
    // A hipFunction_t. Its module loaded at hipModuleLoadData, so the kernel is already known;
    // the lookup only proves that hipModuleUnload has not taken the handle away.
    const auto found = state().module_functions.find(ref.module_function);
    if (found == state().module_functions.end() || found->second->kernel == nullptr) {
        return hipErrorInvalidHandle;   // AMD's hipErrorInvalidResourceHandle has this value
    }
    *out = found->second->kernel;
    return hipSuccess;
}

// The same kernel after a wait, or a refusal. A fat binary that another thread unregistered
// while this launch waited makes the launch invalid, and that is better than a dispatch of a
// kernel whose code object is gone.
static hipError_t same_kernel_after_wait(const KernelRef& ref, const bc250hsa_kernel* kernel) {
    const bc250hsa_kernel* again = nullptr;
    const hipError_t err = resolve_kernel_ref(ref, &again);
    if (err != hipSuccess) {
        return err;
    }
    return again == kernel ? hipSuccess : hipErrorInvalidDeviceFunction;
}

// The explicit arguments of a packed buffer as the pointer array that bc250hsa_kernarg_pack
// takes: one pointer per explicit argument, into the buffer at the argument's own metadata
// offset. The packer then copies each one exactly as it copies the arguments of hipLaunchKernel,
// so the hidden arguments of a packed launch are written by the same code. A buffer that ends
// before an argument ends is refused: the kernel would read bytes the caller never wrote.
static hipError_t packed_arg_pointers(const bc250hsa_kernel* kernel, const void* packed,
                                      size_t packed_bytes, std::vector<void*>& out) {
    out.clear();
    out.reserve(kernel->explicit_arg_count);
    for (uint32_t i = 0; i < kernel->arg_count; ++i) {
        const bc250hsa_arg& arg = kernel->args[i];
        if (arg.size == 0u || arg.kind > BC250HSA_ARG_QUEUE) {
            continue;   // the packer skips the same entries (kernarg.c)
        }
        if (static_cast<uint64_t>(arg.offset) + arg.size > packed_bytes) {
            return hipErrorInvalidValue;
        }
        out.push_back(const_cast<unsigned char*>(static_cast<const unsigned char*>(packed)) +
                      arg.offset);
    }
    return hipSuccess;
}

// The name of one stage of a launch in a refusal line, "hipLaunchKernel/dispatch_submit": the
// evidence of earlier lab runs is searched for these names.
static std::string stage(const char* call, const char* step) {
    return std::string(call) + "/" + step;
}

hipError_t launch_kernel(Guard& guard, const char* call, const KernelRef& ref,
                         const uint32_t grid[3], const uint32_t block[3], size_t shared_bytes,
                         hipStream_t stream, void** args, const void* packed,
                         size_t packed_bytes) {
    if (grid[0] == 0 || grid[1] == 0 || grid[2] == 0 || block[0] == 0 || block[1] == 0 ||
        block[2] == 0) {
        return fail(hipErrorInvalidValue);
    }
    if (shared_bytes > UINT32_MAX) {
        return fail(hipErrorInvalidValue);
    }

    bc250hsa_device* dev = nullptr;
    hipError_t err = device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }

    const bc250hsa_kernel* kernel = nullptr;
    err = resolve_kernel_ref(ref, &kernel);
    if (err != hipSuccess) {
        return refuse(call, nullptr, ref.host_stub != nullptr
                                         ? "the host stub names no kernel of a loaded code object"
                                         : "the function handle names no loaded kernel",
                      err);
    }

    // A null args with no packed buffer goes to the packer as before, which decides whether the
    // kernel can do without arguments.
    std::vector<void*> packed_args;
    if (args == nullptr && packed != nullptr) {
        err = packed_arg_pointers(kernel, packed, packed_bytes, packed_args);
        if (err != hipSuccess) {
            return refuse(call, kernel->name,
                          "the packed argument buffer ends before an argument ends", err);
        }
        args = packed_args.data();
    }

    // The stream is held for the whole launch: the two waits below open the lock, and another
    // thread may call hipStreamDestroy on it meanwhile.
    StreamRef held;
    ihipStream_t* target = resolve_stream(stream);
    if (target == nullptr) {
        return fail(hipErrorInvalidHandle);
    }
    held.attach(target);

    bc250hsa_launch launch;
    std::memset(&launch, 0, sizeof(launch));
    launch.struct_bytes = static_cast<uint32_t>(sizeof(launch));
    for (int d = 0; d < 3; ++d) {
        launch.grid[d] = grid[d];
        launch.block[d] = block[d];
    }
    launch.dynamic_group_bytes = static_cast<uint32_t>(shared_bytes);

    uint32_t kernarg_bytes = 0;
    uint32_t kernarg_align = 0;
    bc250hsa_status status =
        bc250hsa_kernarg_requirements(kernel, &kernarg_bytes, &kernarg_align);
    if (status != BC250HSA_OK) {
        return refuse_status(stage(call, "kernarg_requirements").c_str(), kernel->name, status);
    }

    KernargLease lease;
    err = kernarg_take(guard, kernarg_bytes, kernarg_align, &lease);
    if (err != hipSuccess) {
        return refuse(call, kernel->name, "no kernel argument memory in the arena or the pool",
                      err);
    }
    // A full arena or pool waited, and a wait opens the lock.
    err = same_kernel_after_wait(ref, kernel);
    if (err != hipSuccess) {
        kernarg_settle(lease, 0);
        return fail(err);
    }
    if (lease.host == nullptr) {
        kernarg_settle(lease, 0);
        return fail(hipErrorOutOfMemory);
    }

    bc250hsa_pack_result result;
    std::memset(&result, 0, sizeof(result));
    result.struct_bytes = static_cast<uint32_t>(sizeof(result));
    status = bc250hsa_kernarg_pack(kernel, &launch, args, kernel->explicit_arg_count,
                                   lease.host, kernarg_bytes, lease.va, &result);
    if (status != BC250HSA_OK) {
        kernarg_settle(lease, 0);
        return refuse_status(stage(call, "kernarg_pack").c_str(), kernel->name, status);
    }
    if (result.hostcall_buffer_requested != 0) {
        // Device-side printf needs a host call service, which this build does not have. The
        // counter of the library answers kill criterion K4 of the route document; the launch
        // itself is refused, because a kernel that reads a null host call buffer faults.
        std::fprintf(stderr, "amdhip64: kernel '%s' asks for a host call buffer (device printf), "
                             "which this build does not support\n",
                     kernel->name != nullptr ? kernel->name : "<unnamed>");
        kernarg_settle(lease, 0);
        return refuse(call, kernel->name, "the kernel asks for a host call buffer (device printf)",
                      hipErrorNotSupported);
    }
    bc250hsa_write_barrier();

    err = stream_drain_pending(guard, target);
    if (err != hipSuccess) {
        kernarg_settle(lease, 0);
        return fail(err);
    }
    // The event wait of this stream waited as well, and the packed buffer must still belong to
    // the kernel this launch resolved.
    err = same_kernel_after_wait(ref, kernel);
    if (err != hipSuccess) {
        kernarg_settle(lease, 0);
        return fail(err);
    }

    bc250hsa_dispatch dispatch;
    std::memset(&dispatch, 0, sizeof(dispatch));
    dispatch.struct_bytes = static_cast<uint32_t>(sizeof(dispatch));
    dispatch.flags = state().dispatch_flags;
    dispatch.kernel = kernel;
    dispatch.kernarg_va = lease.va;
    // The AQL dispatch packet of section 7.1 of bc250hsa.h, which a kernel that reads its own
    // blockDim needs. It sits inside this same kernel argument slice, so it retires with the
    // dispatch that reads it and the arena or the pool owns its lifetime.
    if (result.dispatch_packet_requested != 0 && !BC250_HIP_NO_DISPATCH_PACKET_BUILD) {
        dispatch.dispatch_packet_va = lease.va + result.dispatch_packet_offset;
    }
    dispatch.launch = launch;

    uint64_t fence = 0;
    State& runtime = state();
    if (runtime.journal_dispatch_id == UINT64_MAX) {
        kernarg_settle(lease, 0);
        return fail(hipErrorNotSupported);
    }
    // The record of State, reused: clear() keeps the capacity, so a launch makes no heap
    // allocation for it after the first one. The lock stays held from here to the submission,
    // and the backend copies the record before it returns (bc250hsa.h, the comment of
    // bc250hsa_dispatch_submit_recorded), so no other launch writes it while it is in use.
    std::vector<unsigned char>& record = runtime.launch_record;
    err = prepare_dispatch_record(dispatch, lease.host, kernarg_bytes,
                                  ++runtime.journal_dispatch_id, runtime.by_va, record);
    if (err != hipSuccess) {
        kernarg_settle(lease, 0);
        return refuse(stage(call, "journal").c_str(), kernel->name, "cannot preserve complete dispatch diagnostics", err);
    }
    status = bc250hsa_dispatch_submit_recorded(dev, &dispatch, record.data(),
                                               static_cast<uint32_t>(record.size()), &fence);
    if (status != BC250HSA_OK) {
        kernarg_settle(lease, 0);
        return refuse_status(stage(call, "dispatch_submit").c_str(), kernel->name, status);
    }
    kernarg_settle(lease, fence);
    target->last_fence = fence;
    if (packed != nullptr) {
        runtime.face.packed_launches++;
    }
    return hipSuccess;
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
    const uint32_t grid[3] = {gridDim.x, gridDim.y, gridDim.z};
    const uint32_t block[3] = {blockDim.x, blockDim.y, blockDim.z};
    bc250hip::Guard guard;
    return bc250hip::launch_kernel(guard, "hipLaunchKernel", bc250hip::KernelRef{function, nullptr},
                                   grid, block, sharedMemBytes, stream, args, nullptr, 0);
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
                return bc250hip::refuse("hipFuncSetAttribute", found->second.device_name.c_str(),
                                        "the dynamic group memory asked for is above the"
                                        " hardware limit of this part", hipErrorInvalidValue);
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
    return bc250hip::refuse("hipLaunchCooperativeKernel", nullptr,
                            "one hardware queue, no cooperative dispatch packet and no reserved"
                            " occupancy", hipErrorNotSupported);
}

}  // extern "C"
