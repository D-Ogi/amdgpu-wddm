// hip_device.cpp - the process state, the lazy device and the device entry points.
//
// Design docs/design/m16-hip-route-b.md section 4.5: hipInit and hipGetDeviceCount open the
// device once per process, lazily, and there is one device. A machine with no BC-250 adapter
// answers at once, because the first open records its status.

#include <chrono>
#include <cstring>

#include "runtime_internal.h"

namespace bc250hip {

State& state() {
    static State s;
    return s;
}

double host_now_ms() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
}

hipError_t device(bc250hsa_device** out) {
    State& s = state();
    if (!s.open_tried) {
        s.open_tried = true;
        // The interface version of the library we were built against. A different major value
        // means the contract moved, and a wrong contract must not reach the hardware.
        if (bc250hsa_abi_version_major() != BC250HSA_ABI_VERSION_MAJOR) {
            s.open_status = BC250HSA_EUNSUPPORTED;
        } else {
            s.open_status = bc250hsa_open(nullptr, &s.dev);
            if (s.open_status != BC250HSA_OK) {
                s.dev = nullptr;
            }
        }
        if (s.dev != nullptr) {
            s.null_stream.magic = BC250_HIP_STREAM_MAGIC;
            s.null_stream.flags = hipStreamDefault;
            s.null_stream.last_fence = 0;
            s.null_stream.pending_wait = 0;
            s.props.struct_bytes = static_cast<uint32_t>(sizeof(s.props));
            s.props_valid = bc250hsa_props_read(s.dev, &s.props) == BC250HSA_OK;
        }
    }
    if (s.dev == nullptr) {
        return translate(s.open_status == BC250HSA_OK ? BC250HSA_ENODEV : s.open_status);
    }
    if (out != nullptr) {
        *out = s.dev;
    }
    return hipSuccess;
}

void memory_info(size_t* free_bytes, size_t* total_bytes) {
    State& s = state();
    uint64_t total = s.props_valid ? s.props.vram_bytes : 0;
    uint64_t used = 0;
    for (const auto& entry : s.by_va) {
        used += entry.second.mem.bytes;
    }
    // The kernel argument pool is device memory as well, even though no HIP call made it.
    for (const bc250hip::KernargBuffer& buffer : s.kernargs) {
        used += buffer.mem.bytes;
    }
    if (total_bytes != nullptr) {
        *total_bytes = static_cast<size_t>(total);
    }
    if (free_bytes != nullptr) {
        *free_bytes = static_cast<size_t>(total > used ? total - used : 0);
    }
}

}  // namespace bc250hip

using bc250hip::fail;
using bc250hip::state;

extern "C" {

hipError_t hipInit(unsigned int flags) {
    if (flags != 0) {
        return fail(hipErrorInvalidValue);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    return err == hipSuccess ? hipSuccess : fail(err);
}

hipError_t hipGetDeviceCount(int* count) {
    if (count == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    *count = err == hipSuccess ? 1 : 0;
    return err == hipSuccess ? hipSuccess : fail(err);
}

hipError_t hipSetDevice(int deviceId) {
    if (deviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    state().current_device = 0;
    return hipSuccess;
}

hipError_t hipGetDevice(int* deviceId) {
    if (deviceId == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    *deviceId = state().current_device;
    return hipSuccess;
}

hipError_t hipGetDeviceProperties(hipDeviceProp_t* prop, int deviceId) {
    if (prop == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (deviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    std::lock_guard<std::mutex> guard(state().lock);
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    bc250hip::State& s = state();
    std::memset(prop, 0, sizeof(*prop));

    const char* name = s.props_valid && s.props.name[0] != '\0' ? s.props.name : "AMD BC-250";
#if defined(_MSC_VER)
    strncpy_s(prop->name, sizeof(prop->name), name, _TRUNCATE);
    strncpy_s(prop->gcnArchName, sizeof(prop->gcnArchName), "gfx1013", _TRUNCATE);
#else
    std::strncpy(prop->name, name, sizeof(prop->name) - 1);
    std::strncpy(prop->gcnArchName, "gfx1013", sizeof(prop->gcnArchName) - 1);
#endif

    prop->totalGlobalMem = static_cast<size_t>(s.props.vram_bytes);
    // MEASURED acceptance condition: llama.cpp's integer matrix multiply kernels refuse
    // themselves below 48 KiB of shared memory, and every prompt matrix multiply then falls
    // back to hipBLAS. This part has 65536 bytes of local memory per workgroup.
    prop->sharedMemPerBlock = s.props.lds_bytes_per_workgroup != 0
                                  ? static_cast<size_t>(s.props.lds_bytes_per_workgroup)
                                  : static_cast<size_t>(65536);
    prop->maxSharedMemoryPerMultiProcessor = prop->sharedMemPerBlock;
    // MEASURED: every kernel of the spike reports wave size 32.
    prop->warpSize = s.props.wave_size != 0 ? static_cast<int>(s.props.wave_size) : 32;
    const int max_block = s.props.max_workgroup_size != 0
                              ? static_cast<int>(s.props.max_workgroup_size)
                              : 1024;
    prop->maxThreadsPerBlock = max_block;
    prop->maxThreadsDim[0] = max_block;
    prop->maxThreadsDim[1] = max_block;
    prop->maxThreadsDim[2] = max_block;
    const int max_grid = s.props.max_workgroups_per_dim != 0
                             ? static_cast<int>(s.props.max_workgroups_per_dim)
                             : 0x7FFFFFFF;
    prop->maxGridSize[0] = max_grid;
    prop->maxGridSize[1] = max_grid;
    prop->maxGridSize[2] = max_grid;
    prop->clockRate = static_cast<int>(s.props.gfx_clock_khz);
    prop->memoryClockRate = static_cast<int>(s.props.mem_clock_khz);
    prop->memoryBusWidth = static_cast<int>(s.props.mem_bus_width);
    prop->major = static_cast<int>(s.props.gfx_ip_major);
    prop->minor = static_cast<int>(s.props.gfx_ip_minor);
    prop->multiProcessorCount = static_cast<int>(s.props.cu_count);
    prop->maxThreadsPerMultiProcessor =
        static_cast<int>(s.props.waves_per_cu * (s.props.wave_size != 0 ? s.props.wave_size : 32));
    prop->pciBusID = static_cast<int>(s.props.pci_bus);
    prop->pciDeviceID = static_cast<int>(s.props.pci_device);
    prop->canMapHostMemory = 1;
    prop->integrated = 1;   // the memory of this part is shared with the host
    // One hardware queue in build 1 (bc250hsa.h rule 5), so two kernels never overlap.
    prop->concurrentKernels = 0;
    // Fields this build does not know stay 0: regsPerBlock, l2CacheSize, totalConstMem,
    // clockInstructionRate, managedMemory, unifiedAddressing, cooperativeLaunch.
    return hipSuccess;
}

hipError_t hipDeviceSynchronize(void) {
    std::lock_guard<std::mutex> guard(state().lock);
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    const uint64_t value = bc250hsa_fence_last_submitted(dev);
    if (value == 0) {
        return hipSuccess;
    }
    // Slice and total 0 take the library defaults, which are the measured values of our Vulkan
    // driver (1000 ms slices, 120000 ms in all).
    return fail(bc250hip::translate(bc250hsa_wait(dev, value, 0, 0)));
}

}  // extern "C"
