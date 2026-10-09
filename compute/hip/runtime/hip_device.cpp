// hip_device.cpp - the process state, the lazy device and the device entry points.
//
// Design docs/design/m16-hip-route-b.md section 4.5: hipInit and hipGetDeviceCount open the
// device once per process, lazily, and there is one device. A machine with no BC-250 adapter
// answers at once, because the first open records its status.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "runtime_internal.h"

namespace bc250hip {

State& state() {
    static State s;
    return s;
}

namespace {

// One wait bound of the process, read one time at the first device open. bc250hsa.h rule 6
// keeps policy out of layer 1, and design decision 10 asks a short trial to pass a smaller
// total than the library's 120 s default. A HIP program has no API for this, so the policy
// arrives in the environment: BC250_HIP_WAIT_TOTAL_MS and BC250_HIP_WAIT_SLICE_MS, both in
// milliseconds, 0 or absent taking the library default.
//
// MEASURED: this must be the operating system's environment and not getenv. A program that
// links the static C runtime and this DLL that links the dynamic one have two C runtime
// instances, and each one copies the environment when it starts. A _putenv_s of the program
// therefore never reaches a getenv of this DLL, while GetEnvironmentVariable reads the one
// block that both of them write.
uint32_t read_ms(const char* name) {
    char text[32];
#if defined(_WIN32)
    const DWORD bytes = GetEnvironmentVariableA(name, text, sizeof(text));
    if (bytes == 0 || bytes >= sizeof(text)) {
        return 0;
    }
#else
    const char* found = std::getenv(name);
    if (found == nullptr) {
        return 0;
    }
    std::strncpy(text, found, sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
#endif
    const unsigned long value = std::strtoul(text, nullptr, 10);
    return value > 0xFFFFFFFFul ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
}

// The name as it stands, for a switch that is a word and not a number. The same
// MEASURED note applies: the operating system's environment and not getenv.
bool read_text(const char* name, char* text, size_t bytes) {
#if defined(_WIN32)
    const DWORD got = GetEnvironmentVariableA(name, text, static_cast<DWORD>(bytes));
    return got != 0 && got < bytes;
#else
    const char* found = std::getenv(name);
    if (found == nullptr || std::strlen(found) + 1 > bytes) {
        return false;
    }
    std::strncpy(text, found, bytes - 1);
    text[bytes - 1] = '\0';
    return true;
#endif
}

// The batching policy of the process (section 8.1 of bc250hsa.h, which keeps policy out of
// layer 1). A HIP program has no API for it, so it arrives in the environment:
//
//   BC250_HIP_BATCH=1               append consecutive launches into one indirect buffer
//   BC250_HIP_BATCH_MAX=<n>         dispatches per buffer, 0 or absent takes the default
//   BC250_HIP_BATCH_HOLD_US=<us>    the time cap, 0 or absent takes the default
//   BC250_HIP_BARRIER=full|light    the barrier between two dispatches of one buffer
//
// Both defaults are the conservative ones: batching off, and the full acquire. Build 1 of
// this route has one submission per launch, and no lab trial has measured either switch on
// the hardware yet (the plan is scratch\m16-hip\lab\perf-README.md). The switches exist so
// that the trial can measure them, and the defaults change when it has.
void apply_batch_policy(bc250hsa_device* dev) {
    char text[32];
    bc250hsa_batch_policy policy;
    std::memset(&policy, 0, sizeof(policy));
    policy.struct_bytes = static_cast<uint32_t>(sizeof(policy));
    policy.enabled = read_ms("BC250_HIP_BATCH") != 0 ? 1u : 0u;
    policy.max_dispatches = read_ms("BC250_HIP_BATCH_MAX");
    policy.max_hold_us = read_ms("BC250_HIP_BATCH_HOLD_US");
    policy.light_barrier = 0u;
    if (read_text("BC250_HIP_BARRIER", text, sizeof(text)) && std::strcmp(text, "light") == 0) {
        policy.light_barrier = 1u;
    }
    if (policy.enabled == 0 && policy.light_barrier == 0) {
        return;   // the defaults of the library; no call, no refusal to report
    }
    const bc250hsa_status status = bc250hsa_batch_policy_set(dev, &policy);
    if (status != BC250HSA_OK) {
        std::fprintf(stderr, "amdhip64: the backend refused the batching policy (%s); it stays"
                             " at one submission per launch\n",
                     bc250hsa_status_string(status));
    }
}

}  // namespace

double host_now_ms() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
}

hipError_t device(bc250hsa_device** out) {
    State& s = state();
    if (!s.open_tried) {
        s.open_tried = true;
        s.wait_slice_ms = read_ms("BC250_HIP_WAIT_SLICE_MS");
        s.wait_total_ms = read_ms("BC250_HIP_WAIT_TOTAL_MS");
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
            // The null stream lives in State and is never freed, so its count is a marker and
            // not a lifetime: stream_release leaves it alone.
            s.null_stream.refs = 1;
            s.props.struct_bytes = static_cast<uint32_t>(sizeof(s.props));
            s.props_valid = bc250hsa_props_read(s.dev, &s.props) == BC250HSA_OK;
            apply_batch_policy(s.dev);
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

bool waits_under_lock() { return BC250_HIP_WAIT_UNDER_LOCK_BUILD != 0; }

hipError_t Guard::wait(bc250hsa_device* dev, uint64_t value) {
    State& s = state();
    // The policy belongs to the process, so it is read here and not by the caller. Both numbers
    // are written once, at the first device open, and read under the lock.
    const uint32_t slice_ms = s.wait_slice_ms;
    const uint32_t total_ms = s.wait_total_ms;
#if BC250_HIP_WAIT_UNDER_LOCK_BUILD
    // The negative control: the first build of layer 2 waited like this, and a second thread
    // could not call the runtime at all while the first one waited.
    const bc250hsa_status status = bc250hsa_wait(dev, value, slice_ms, total_ms);
#else
    held_.unlock();
    const bc250hsa_status status = bc250hsa_wait(dev, value, slice_ms, total_ms);
    held_.lock();
#endif
    if (status == BC250HSA_OK) {
        events_stamp_retired(dev);
    }
    return translate(status);
}

void memory_info(size_t* free_bytes, size_t* total_bytes) {
    State& s = state();
    uint64_t total = s.props_valid ? s.props.vram_bytes : 0;
    uint64_t used = 0;
    // Every allocation of the process counts, host visible ones as well: the memory of this
    // part is one DRAM pool behind a carve-out, so a host-visible allocation takes the same
    // physical memory as a device-local one. The leak criterion of the step-2 trial compares
    // two of these numbers, and it therefore sees a leak of either kind.
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
    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    return err == hipSuccess ? hipSuccess : fail(err);
}

hipError_t hipGetDeviceCount(int* count) {
    if (count == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    *count = err == hipSuccess ? 1 : 0;
    return err == hipSuccess ? hipSuccess : fail(err);
}

hipError_t hipSetDevice(int deviceId) {
    if (deviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    bc250hip::Guard guard;
    state().current_device = 0;
    return hipSuccess;
}

hipError_t hipGetDevice(int* deviceId) {
    if (deviceId == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    bc250hip::Guard guard;
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
    bc250hip::Guard guard;
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
    bc250hip::Guard guard;
    bc250hsa_device* dev = nullptr;
    const hipError_t err = bc250hip::device(&dev);
    if (err != hipSuccess) {
        return fail(err);
    }
    // The snapshot is the promise of this call: everything submitted before it. Work that
    // another thread submits while this one waits belongs to the next synchronize.
    const uint64_t value = bc250hsa_fence_last_submitted(dev);
    if (value == 0) {
        return hipSuccess;
    }
    return fail(guard.wait(dev, value));
}

}  // extern "C"
