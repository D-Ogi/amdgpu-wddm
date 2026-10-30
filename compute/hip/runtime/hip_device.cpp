// hip_device.cpp - the process state, the lazy device and the device entry points.
//
// Design docs/design/m16-hip-route-b.md section 4.5: hipInit and hipGetDeviceCount open the
// device once per process, lazily, and there is one device. A machine with no BC-250 adapter
// answers at once, because the first open records its status.

#include <cerrno>
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
//
// BD-167: every number this reads is a lab build's switch (build-runtime.ps1 -Lab). A release runtime reads none of
// them and runs as with each one unset: the library's wait bound, the batching defaults.
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
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
#endif

// The name as it stands, for a switch that is a word and not a number. The same
// MEASURED note applies: the operating system's environment and not getenv. Every such switch is a lab build's, so a
// release runtime has no reader either (BD-167).
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
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
#endif

// The batching policy of the process (section 8.1 of bc250hsa.h, which keeps policy out of
// layer 1). A HIP program has no API for it, so it arrives in the environment:
//
//   BC250_HIP_BATCH=0|1             append consecutive launches into one indirect buffer
//   BC250_HIP_BATCH_MAX=<n>         dispatches per buffer, 0 or absent takes the default
//   BC250_HIP_BATCH_HOLD_US=<us>    the time cap, 0 or absent takes the default
//   BC250_HIP_BARRIER=full|light    the barrier between two dispatches of one buffer
//   BC250_HIP_PM4_STATE_CACHE=0|1   write only the compute state that changed
//
// MEASURED, unit A, 2026-10-09 17:33-17:40Z (evidence/m16/perf-2026-10-09): batching on with
// the light barrier is exact over two chains of 1000 dependent kernels, 2.2 times faster on
// the launch line and 3.8 times faster on the chain line than one submission per dispatch,
// and it cuts submissions per dispatch from 1.000 to 0.032. The defaults are therefore
// batching on, 32 dispatches a buffer and the light barrier, which is what that session
// measured as arm P3. Each of the three is still a switch, and BC250_HIP_BATCH=0 with
// BC250_HIP_BARRIER=full is exactly build 1.
void apply_batch_policy(bc250hsa_device* dev) {
    char text[32];
    bc250hsa_batch_policy policy;
    std::memset(&policy, 0, sizeof(policy));
    policy.struct_bytes = static_cast<uint32_t>(sizeof(policy));
    // An absent variable takes the default, and the default is on, so only an explicit 0
    // turns batching off. read_ms answers 0 for both, which is why the text is read here.
    policy.enabled = 1u;
    policy.light_barrier = 1u;
    // BD-167: the switches are the measurement arms of a lab build. A release runtime keeps the defaults above, with
    // max_dispatches and max_hold_us 0 (the library's), and the compute state cache on.
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    if (read_text("BC250_HIP_BATCH", text, sizeof(text)) && std::strcmp(text, "0") == 0) {
        policy.enabled = 0u;
    }
    policy.max_dispatches = read_ms("BC250_HIP_BATCH_MAX");
    policy.max_hold_us = read_ms("BC250_HIP_BATCH_HOLD_US");
    if (read_text("BC250_HIP_BARRIER", text, sizeof(text)) && std::strcmp(text, "full") == 0) {
        policy.light_barrier = 0u;
    }
    if (read_text("BC250_HIP_PM4_STATE_CACHE", text, sizeof(text)) &&
        std::strcmp(text, "0") == 0) {
        state().dispatch_flags |= BC250HSA_DISPATCH_FULL_STATE;
    }
#else
    (void)text;
#endif
    const bc250hsa_status status = bc250hsa_batch_policy_set(dev, &policy);
    if (status != BC250HSA_OK) {
        std::fprintf(stderr, "amdhip64: the backend refused the batching policy (%s); the"
                             " device keeps its own default, one submission per launch\n",
                     bc250hsa_status_string(status));
    }
}

// The GPU copy policy of the process (scratch/hip-gpu-copy/CONTRACT.md section 10). The
// three names are documented beside the fields they set, in runtime_internal.h. The
// defaults are the route on, 4096 bytes, and the host-processor fill kept for an
// allocation that has a host mapping.
void apply_gpu_copy_policy(bc250hsa_device* dev) {
    char text[32];
    State& s = state();
    // BD-167: the route switch, the fill route and the threshold are measurement arms of a lab build (the switch is
    // the control arm of the route's measurement, README.md). A release runtime keeps the defaults: the route on, the
    // processor fill for a mapped allocation, 4096 bytes.
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    if (read_text("BC250_HIP_GPU_COPY", text, sizeof(text)) && std::strcmp(text, "0") == 0) {
        s.gpu_copy_enabled = false;
    }
    if (read_text("BC250_HIP_GPU_FILL", text, sizeof(text)) && std::strcmp(text, "1") == 0) {
        s.gpu_fill_mapped = true;
    }
    // 0 is a value of this one and not an absent variable: it puts every copy on the GPU
    // route, which is the arm that measures the threshold instead of assuming it. read_ms
    // cannot say the two apart, so the text is read and parsed here.
    if (read_text("BC250_HIP_GPU_COPY_MIN", text, sizeof(text))) {
        uint32_t value = 0;
        if (parse_copy_min(text, &value)) {
            s.gpu_copy_min_bytes = value;
        } else {
            std::fprintf(stderr, "amdhip64: BC250_HIP_GPU_COPY_MIN is not a number of bytes;"
                                 " the default %u stands\n",
                         s.gpu_copy_min_bytes);
        }
    }
#else
    (void)text;
#endif
    // A GPU copy waits for its own fence inside layer 1, so layer 1 needs the wait bound
    // this process uses everywhere else. Without it a copy of a short lab trial could
    // wait the library's 120 s default (bc250hsa.h rule 6).
    const bc250hsa_status status =
        bc250hsa_wait_policy_set(dev, s.wait_slice_ms, s.wait_total_ms);
    if (status != BC250HSA_OK) {
        std::fprintf(stderr, "amdhip64: the backend refused the wait policy (%s); a GPU copy"
                             " waits under the library default\n",
                     bc250hsa_status_string(status));
    }
}

// The kernel argument arena of hip_kernarg_arena.cpp. It is on by default; BC250_HIP_KERNARG_ARENA=0
// turns it off, and every launch then takes the pool of one buffer per launch, which is the
// control arm of the arena's lab measurement.
void apply_kernarg_policy() {
    // BD-167: a measurement arm of a lab build. A release runtime keeps the arena on.
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    char text[32];
    if (read_text("BC250_HIP_KERNARG_ARENA", text, sizeof(text)) && std::strcmp(text, "0") == 0) {
        state().kernarg_arena_enabled = false;
    }
#endif
}

}  // namespace

// strtoul alone is not enough here: it skips leading spaces, takes a sign, and turns "-1" into
// the largest unsigned long, which on Windows is 4294967295 and passes a range test. Only
// digits are a number of bytes, and a value it cannot hold is ERANGE and refused.
bool parse_copy_min(const char* text, uint32_t* out) {
    if (text == nullptr || out == nullptr || text[0] < '0' || text[0] > '9') {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value > 0xFFFFFFFFull) {
        return false;
    }
    *out = static_cast<uint32_t>(value);
    return true;
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
        // The log sink goes in before the first call into layer 1. log_start() installs it, and
        // until it runs every bc250hsa_log line has nowhere to go. bc250hsa_open below is the
        // first call that can write one, and a refusal inside pm4_dispatch.c states its reason
        // through that sink only, so a sink installed later by the first refuse() of this
        // runtime would lose the very line the lab needs (defect BD-110). With
        // BC250_HIP_LOG unset this reads one local static and returns.
        log_start();
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        s.wait_slice_ms = read_ms("BC250_HIP_WAIT_SLICE_MS");
        s.wait_total_ms = read_ms("BC250_HIP_WAIT_TOTAL_MS");
#endif
        // The interface version of the library we were built against. A different major value
        // means the contract moved, and a wrong contract must not reach the hardware.
        if (bc250hsa_abi_version_major() != BC250HSA_ABI_VERSION_MAJOR) {
            s.open_status = BC250HSA_EUNSUPPORTED;
        } else {
#if defined(BC250_HIP_GPU_DISABLED) && BC250_HIP_GPU_DISABLED
            // Optional containment artifact. No environment option can reopen the device.
            s.open_status = BC250HSA_EUNSUPPORTED;
            std::fprintf(stderr, "amdhip64: GPU execution disabled in this build\n");
#else
            s.open_status = bc250hsa_open(nullptr, &s.dev);
#endif
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
            apply_gpu_copy_policy(s.dev);
            apply_kernarg_policy();
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
    used += s.kernarg_arena_mem.bytes;
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

// The attributes of design section 4.9. One switch and no judgement: every value this runtime
// answers is a field hipGetDeviceProperties already fills, and the two must never disagree.
// An attribute this build does not answer is hipErrorInvalidValue and not a guessed zero,
// because a program that reads an unknown attribute has to learn that it is unknown.
hipError_t hipDeviceGetAttribute(int* value, hipDeviceAttribute_t attr, int deviceId) {
    if (value == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (deviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    hipDeviceProp_t prop;
    const hipError_t err = hipGetDeviceProperties(&prop, deviceId);
    if (err != hipSuccess) {
        return err;  // hipGetDeviceProperties already recorded it
    }
    switch (attr) {
        case hipDeviceAttributeWarpSize:
            *value = prop.warpSize;
            return hipSuccess;
        case hipDeviceAttributeMaxThreadsPerBlock:
            *value = prop.maxThreadsPerBlock;
            return hipSuccess;
        case hipDeviceAttributeMaxSharedMemoryPerBlock:
            *value = static_cast<int>(prop.sharedMemPerBlock);
            return hipSuccess;
        case hipDeviceAttributeMultiprocessorCount:
            *value = prop.multiProcessorCount;
            return hipSuccess;
        case hipDeviceAttributeClockRate:
            *value = prop.clockRate;
            return hipSuccess;
        case hipDeviceAttributeConcurrentKernels:
            *value = prop.concurrentKernels;
            return hipSuccess;
        case hipDeviceAttributeIntegrated:
            *value = prop.integrated;
            return hipSuccess;
        case hipDeviceAttributeCanMapHostMemory:
            *value = prop.canMapHostMemory;
            return hipSuccess;
        case hipDeviceAttributeComputeCapabilityMajor:
            *value = prop.major;
            return hipSuccess;
        case hipDeviceAttributeComputeCapabilityMinor:
            *value = prop.minor;
            return hipSuccess;
        // No cooperative dispatch: one hardware queue, and nothing starts every workgroup of a
        // grid at the same time. hipLaunchCooperativeKernel says the same, and llama.cpp reads
        // this attribute before it calls that entry point.
        case hipDeviceAttributeCooperativeLaunch:
            *value = 0;
            return hipSuccess;
        // No virtual memory management entry points in this build (the cuMem family).
        case hipDeviceAttributeVirtualMemoryManagementSupported:
            *value = 0;
            return hipSuccess;
        case hipDeviceAttributeManagedMemory:
            *value = 0;
            return hipSuccess;
        default:
            return fail(hipErrorInvalidValue);
    }
}

// The bus identifier of the adapter, in the form a program expects: domain, bus, device and
// function. This part is one function of one device, and layer 1 reads the three numbers from
// the adapter, so no number here is invented. The domain is 0: a Windows adapter has no
// segment number in the properties layer 1 reads.
hipError_t hipDeviceGetPCIBusId(char* pciBusId, int len, int deviceId) {
    if (pciBusId == nullptr || len <= 0) {
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
    const bc250hip::State& s = state();
    char text[32];
    std::snprintf(text, sizeof(text), "0000:%02x:%02x.%01x",
                  static_cast<unsigned>(s.props.pci_bus & 0xFFu),
                  static_cast<unsigned>(s.props.pci_device & 0xFFu),
                  static_cast<unsigned>(s.props.pci_function & 0xFu));
    // CUDA and HIP both truncate into the caller's buffer and report success, so a short buffer
    // is not an error. The result stays terminated.
    const size_t room = static_cast<size_t>(len) - 1;
    std::strncpy(pciBusId, text, room);
    pciBusId[room] = '\0';
    return hipSuccess;
}

// One device, so there is no peer. The pair of entry points below exists because llama.cpp
// links against both of them; it calls them only when GGML_CUDA_P2P is in the environment, and
// a second device would have to exist first.
hipError_t hipDeviceCanAccessPeer(int* canAccessPeer, int deviceId, int peerDeviceId) {
    if (canAccessPeer == nullptr) {
        return fail(hipErrorInvalidValue);
    }
    if (deviceId != 0 || peerDeviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    *canAccessPeer = 0;  // a device is not its own peer
    return hipSuccess;
}

hipError_t hipDeviceEnablePeerAccess(int peerDeviceId, unsigned int flags) {
    (void)flags;
    if (peerDeviceId != 0) {
        return fail(hipErrorInvalidDevice);
    }
    // The only device of this process is the calling device, and a device cannot peer with
    // itself. HIP reports exactly this for that case.
    return fail(hipErrorInvalidDevice);
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

// How many workgroups of this kernel a compute unit can hold at once.
//
// Why it is here and not among the launch entry points: it reads no queue and submits nothing.
// It answers from this part's properties and the kernel's own descriptor, which is where
// hipGetDeviceProperties already looks.
//
// What is modelled: the wave slots of the two SIMDs of a compute unit, the vector register file
// of a SIMD (1024 VGPRs in wave32, allocated in granules of 8), the local memory of a compute
// unit and the barrier limit of a multi-wave workgroup. hip_occupancy.cpp states the source of
// each number. Until 2026-10-30 the register file was not modelled, and a register-bound kernel
// got a number that was too large. llama.cpp's flash attention path is the caller
// (ggml/src/ggml-cuda/fattn-common.cuh:1137), and it uses the number to choose how many blocks
// work on one head in parallel.
hipError_t hipOccupancyMaxActiveBlocksPerMultiprocessor(int* numBlocks, const void* func,
                                                        int blockSize, size_t dynamicSMemSize) {
    if (numBlocks == nullptr || func == nullptr || blockSize <= 0) {
        return fail(hipErrorInvalidValue);
    }

    hipDeviceProp_t prop;
    const hipError_t props_err = hipGetDeviceProperties(&prop, 0);
    if (props_err != hipSuccess) {
        return props_err;  // hipGetDeviceProperties already recorded it
    }
    if (prop.warpSize <= 0 || prop.sharedMemPerBlock == 0) {
        return fail(hipErrorNotInitialized);
    }

    bc250hip::Guard guard;
    bc250hip::State& s = state();
    const auto found = s.functions.find(func);
    if (found == s.functions.end()) {
        return fail(hipErrorInvalidDeviceFunction);
    }

    // The register count is in the kernel descriptor, so the code object loads here when no
    // launch has loaded it yet: llama.cpp asks before its first launch of a kernel. A code object
    // that does not load leaves the register limit out, and the launch reports the load error.
    bc250hip::Function& entry = found->second;
    if (entry.kernel == nullptr && entry.module != nullptr &&
        bc250hip::module_ensure_loaded(entry.module) == BC250HSA_OK) {
        entry.kernel = bc250hsa_module_kernel_by_name(entry.module->loaded, entry.device_name.c_str());
    }
    size_t group_bytes = dynamicSMemSize;
    if (entry.kernel != nullptr) {
        group_bytes += static_cast<size_t>(entry.kernel->group_segment_bytes);
    }
    if (group_bytes > prop.sharedMemPerBlock || blockSize > prop.maxThreadsPerBlock) {
        // The kernel does not fit at all. CUDA and HIP both answer 0 here instead of an error.
        *numBlocks = 0;
        return hipSuccess;
    }

    bc250hip::OccupancyInput in;
    in.block_threads = static_cast<uint32_t>(blockSize);
    in.wave_size = static_cast<uint32_t>(prop.warpSize);
    in.waves_per_cu = prop.maxThreadsPerMultiProcessor > 0
                          ? static_cast<uint32_t>(prop.maxThreadsPerMultiProcessor / prop.warpSize)
                          : 0u;
    in.vgprs = entry.kernel != nullptr
                   ? bc250hip::occupancy_kernel_vgprs(*entry.kernel)
                   : 0u;
    in.group_bytes = group_bytes;
    in.lds_bytes_per_cu = prop.maxSharedMemoryPerMultiProcessor;
    *numBlocks = bc250hip::occupancy_blocks_per_cu(in);
    return hipSuccess;
}

}  // extern "C"
