// amd_abi.cpp - the entry points of amdhip64_7.dll that AMD's header declares and ours does not.
//
// The ROCm user libraries of TheRock (torch_hip, rocBLAS, MIOpen, rocRAND, rocSOLVER, rocSPARSE,
// hipBLASLt, rocFFT) import 133 HIP names from amdhip64_7.dll (scratch census imp-hip.tsv,
// nightly 10.3.0a20261030). 49 of them are entry points of this runtime with the same types in
// both headers. This file defines the others, compiled against AMD's own hip_runtime_api.h
// (HIP 7.18, from the rocm_sdk_core wheel, MIT): a definition whose types differ from AMD's
// declaration does not compile. Two names exist in both headers with a different meaning, so
// they are defined here as bc250amd_<name> and amdhip64_7.def exports them under AMD's name:
//
//   hipDeviceGetAttribute    AMD's attribute numbers, answered from the R0600 properties
//   hipEventCreateWithFlags  AMD's flags that change nothing on this device are accepted
//
// The runtime itself is behind amd_face.h. AMD's amdhip64_7.dll and PAL are never loaded
// (decision D015): this file is our implementation of AMD's interface, not a wrapper of AMD's.
//
// What this build does not have answers hipErrorNotSupported through bc250face::refuse, by
// name: graphs, IPC, virtual memory management, memory pools, CU masks, stream wait values,
// the JIT linker and cooperative module launches.

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <hip/hip_runtime_api.h>
#include <hip/hip_ext.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <climits>
#include <cstdio>
#include <cstring>
#include <type_traits>

#include "amd_face.h"

#if HIP_TARGET_API_VERSION < 600
#error "the AMD face implements the R0600 device properties"
#endif

namespace {

hipError_t ret(int err) { return static_cast<hipError_t>(err); }

hipError_t not_supported(const char* call, const char* why) {
    return ret(bc250face::refuse(call, why, hipErrorNotSupported));
}

hipError_t invalid() { return ret(bc250face::fail(hipErrorInvalidValue)); }

// The one context of the one device. HIP's context calls are deprecated and exist for code
// written against CUDA's driver interface; the handle only has to be stable and non-null.
char g_context;
hipCtx_t the_context() { return reinterpret_cast<hipCtx_t>(&g_context); }

thread_local hipStreamCaptureMode g_capture_mode = hipStreamCaptureModeGlobal;

void fill_arch(hipDeviceArch_t* arch) {
    std::memset(arch, 0, sizeof(*arch));
    // The feature flags of GFX10 as AMD's runtime states them for the gfx101x family.
    arch->hasGlobalInt32Atomics = 1;
    arch->hasGlobalFloatAtomicExch = 1;
    arch->hasSharedInt32Atomics = 1;
    arch->hasSharedFloatAtomicExch = 1;
    arch->hasFloatAtomicAdd = 1;
    arch->hasGlobalInt64Atomics = 1;
    arch->hasSharedInt64Atomics = 1;
    arch->hasDoubles = 1;
    arch->hasWarpVote = 1;
    arch->hasWarpBallot = 1;
    arch->hasWarpShuffle = 1;
    arch->hasFunnelShift = 0;
    arch->hasThreadFenceSystem = 1;
    arch->hasSyncThreadsExt = 0;
    arch->hasSurfaceFuncs = 0;
    arch->has3dGrid = 1;
    arch->hasDynamicParallelism = 0;
}

hipError_t fill_props(hipDeviceProp_tR0600* prop) {
    bc250face::DeviceInfo info;
    const int err = bc250face::device_info(&info);
    if (err != hipSuccess) {
        return ret(err);
    }
    std::memset(prop, 0, sizeof(*prop));
    std::snprintf(prop->name, sizeof(prop->name), "%s", info.name);
    // The code the ROCm libraries ship for this device is gfx1010 code, and they choose their
    // kernels and their tuning tables by this name (rocBLAS Tensile, MIOpen, the kpack TOC). The
    // loader takes gfx1010 code under the target alias of bc250hsa.h, and this name is what keeps
    // a library from asking for gfx1013 code that no package has. xnack- is the only mode of a
    // Windows display adapter.
    std::snprintf(prop->gcnArchName, sizeof(prop->gcnArchName), "%s", "gfx1010:xnack-");
    const int lds = static_cast<int>(info.lds_bytes_per_workgroup);
    const int max_block = static_cast<int>(info.max_workgroup_size);
    prop->totalGlobalMem = static_cast<size_t>(info.total_bytes);
    prop->sharedMemPerBlock = static_cast<size_t>(lds);
    // 1024 VGPRs of 32 lanes per SIMD in wave32 and two SIMDs per compute unit, in registers of
    // one work item: 2 x 32768.
    prop->regsPerBlock = 65536;
    prop->regsPerMultiprocessor = 65536;
    prop->warpSize = static_cast<int>(info.wave_size);
    prop->memPitch = static_cast<size_t>(INT_MAX);
    prop->maxThreadsPerBlock = max_block;
    prop->maxThreadsDim[0] = max_block;
    prop->maxThreadsDim[1] = max_block;
    prop->maxThreadsDim[2] = max_block;
    prop->maxGridSize[0] = INT_MAX;
    prop->maxGridSize[1] = 65536;
    prop->maxGridSize[2] = 65536;
    prop->clockRate = static_cast<int>(info.gfx_clock_khz);
    prop->totalConstMem = static_cast<size_t>(INT_MAX);
    prop->major = static_cast<int>(info.gfx_major);
    prop->minor = static_cast<int>(info.gfx_minor);
    prop->textureAlignment = 256;
    prop->texturePitchAlignment = 256;
    prop->surfaceAlignment = 256;
    prop->deviceOverlap = 0;
    prop->asyncEngineCount = 0;        // copies and kernels share the one queue
    prop->multiProcessorCount = static_cast<int>(info.cu_count);
    prop->kernelExecTimeoutEnabled = 1;   // Windows TDR
    prop->integrated = 1;              // one DRAM behind a carve-out, as hip_device.cpp says
    prop->canMapHostMemory = 1;
    prop->computeMode = 0;
    prop->concurrentKernels = 0;       // one hardware queue (bc250hsa.h rule 5)
    prop->ECCEnabled = 0;
    prop->pciBusID = static_cast<int>(info.pci_bus);
    prop->pciDeviceID = static_cast<int>(info.pci_device);
    prop->pciDomainID = 0;
    prop->unifiedAddressing = 0;       // a hipMalloc pointer is a GPU address, not a host one
    prop->memoryClockRate = static_cast<int>(info.mem_clock_khz);
    prop->memoryBusWidth = static_cast<int>(info.mem_bus_width);
    prop->l2CacheSize = 0;             // not measured on this part
    prop->maxThreadsPerMultiProcessor = static_cast<int>(info.waves_per_cu * info.wave_size);
    prop->sharedMemPerMultiprocessor = static_cast<size_t>(lds);
    prop->sharedMemPerBlockOptin = static_cast<size_t>(lds);
    prop->maxSharedMemoryPerMultiProcessor = static_cast<size_t>(lds);
    prop->maxBlocksPerMultiProcessor = static_cast<int>(info.waves_per_cu);
    prop->managedMemory = 0;
    prop->cooperativeLaunch = 0;
    prop->hostRegisterSupported = 0;
    prop->memoryPoolsSupported = 0;
    prop->clockInstructionRate = 0;    // not measured on this part
    fill_arch(&prop->arch);
    // 1 tells a library that the host can read device memory through a device pointer. torch
    // then reads a one-element tensor that way after a stream synchronize
    // (_local_scalar_dense_cuda), but a hipMalloc pointer of this runtime is a GPU address
    // only: with 1 here every .item() was an access violation (test_amd_face_props).
    prop->isLargeBar = 0;
    prop->asicRevision = 0;
    return hipSuccess;
}

// The `extra` form of a module launch: { HIP_LAUNCH_PARAM_BUFFER_POINTER, buffer,
// HIP_LAUNCH_PARAM_BUFFER_SIZE, &size, HIP_LAUNCH_PARAM_END }.
bool parse_extra(void** extra, const void** buffer, size_t* bytes) {
    *buffer = nullptr;
    *bytes = 0;
    bool sized = false;
    for (int i = 0; i < 16; i += 2) {
        if (extra[i] == HIP_LAUNCH_PARAM_END) {
            return *buffer != nullptr && sized;
        }
        if (extra[i] == HIP_LAUNCH_PARAM_BUFFER_POINTER) {
            *buffer = extra[i + 1];
        } else if (extra[i] == HIP_LAUNCH_PARAM_BUFFER_SIZE) {
            if (extra[i + 1] == nullptr) {
                return false;
            }
            *bytes = *static_cast<const size_t*>(extra[i + 1]);
            sized = true;
        } else {
            return false;
        }
    }
    return false;
}

hipError_t module_launch(hipFunction_t f, const uint32_t grid[3], const uint32_t block[3],
                         unsigned int shared, hipStream_t stream, void** params, void** extra) {
    if (params != nullptr && extra != nullptr) {
        return invalid();
    }
    const void* packed = nullptr;
    size_t packed_bytes = 0;
    if (extra != nullptr && !parse_extra(extra, &packed, &packed_bytes)) {
        return invalid();
    }
    return ret(bc250face::launch(f, grid, block, shared, stream, params, packed, packed_bytes));
}

hipError_t kernel_attributes(const void* stub, const void* function, hipFuncAttributes* attr) {
    bc250face::KernelInfo info;
    const int err = bc250face::kernel_info(stub, function, &info);
    if (err != hipSuccess) {
        return ret(err);
    }
    std::memset(attr, 0, sizeof(*attr));
    bc250face::DeviceInfo device;
    if (bc250face::device_info(&device) == hipSuccess) {
        attr->binaryVersion = static_cast<int>(device.gfx_major * 10 + device.gfx_minor);
        attr->ptxVersion = attr->binaryVersion;
    }
    attr->localSizeBytes = info.private_segment_bytes;
    attr->sharedSizeBytes = info.group_segment_bytes;
    attr->maxThreadsPerBlock = static_cast<int>(info.max_threads_per_block);
    attr->numRegs = static_cast<int>(info.vgpr_count);
    attr->maxDynamicSharedSizeBytes =
        info.dynamic_group_max >= 0
            ? info.dynamic_group_max
            : static_cast<int>(info.lds_limit > info.group_segment_bytes
                                   ? info.lds_limit - info.group_segment_bytes
                                   : 0u);
    return hipSuccess;
}

hipError_t pointer_kind(const void* ptr, bc250face::PointerInfo* info) {
    return ret(bc250face::pointer_info(ptr, info));
}

}  // namespace

// The two names whose meaning differs between the headers. The asserts make the compiler check
// each against AMD's declaration, as a definition under the AMD name would.
extern "C" hipError_t bc250amd_hipDeviceGetAttribute(int* pi, hipDeviceAttribute_t attr,
                                                     int deviceId);
extern "C" hipError_t bc250amd_hipEventCreateWithFlags(hipEvent_t* event, unsigned flags);
// AMD's hipLaunchCooperativeKernel takes its LDS size as unsigned int, ours as size_t (amd_face.h).
using AmdLaunchCooperative = hipError_t (*)(const void*, dim3, dim3, void**, unsigned int,
                                            hipStream_t);
extern "C" hipError_t bc250amd_hipLaunchCooperativeKernel(const void* f, dim3 gridDim,
                                                          dim3 blockDim, void** kernelParams,
                                                          unsigned int sharedMemBytes,
                                                          hipStream_t stream);
static_assert(std::is_same<decltype(&bc250amd_hipLaunchCooperativeKernel),
                           decltype(static_cast<AmdLaunchCooperative>(
                               &hipLaunchCooperativeKernel))>::value,
              "bc250amd_hipLaunchCooperativeKernel must have AMD's type");
static_assert(std::is_same<decltype(&bc250amd_hipDeviceGetAttribute),
                           decltype(&hipDeviceGetAttribute)>::value,
              "bc250amd_hipDeviceGetAttribute must have AMD's type");
static_assert(std::is_same<decltype(&bc250amd_hipEventCreateWithFlags),
                           decltype(&hipEventCreateWithFlags)>::value,
              "bc250amd_hipEventCreateWithFlags must have AMD's type");
static_assert(sizeof(bc250face::Counters) == 104, "the counter layout is fixed");

extern "C" {

// ---------------------------------------------------------------------------------------------
// Device, context, version
// ---------------------------------------------------------------------------------------------

hipError_t hipGetDevicePropertiesR0600(hipDeviceProp_tR0600* prop, int deviceId) {
    if (prop == nullptr) {
        return invalid();
    }
    if (deviceId != 0) {
        return ret(bc250face::fail(hipErrorInvalidDevice));
    }
    return fill_props(prop);
}

hipError_t bc250amd_hipDeviceGetAttribute(int* pi, hipDeviceAttribute_t attr, int deviceId) {
    if (pi == nullptr) {
        return invalid();
    }
    hipDeviceProp_tR0600 p;
    const hipError_t err = hipGetDevicePropertiesR0600(&p, deviceId);
    if (err != hipSuccess) {
        return err;
    }
    // Every value is a field of the R0600 properties above or a fixed answer of this build, so
    // the attribute and the property never disagree.
    switch (attr) {
        case hipDeviceAttributeEccEnabled: *pi = p.ECCEnabled; break;
        case hipDeviceAttributeAccessPolicyMaxWindowSize: *pi = 0; break;
        case hipDeviceAttributeAsyncEngineCount: *pi = p.asyncEngineCount; break;
        case hipDeviceAttributeCanMapHostMemory: *pi = p.canMapHostMemory; break;
        case hipDeviceAttributeCanUseHostPointerForRegisteredMem: *pi = 0; break;
        case hipDeviceAttributeClockRate: *pi = p.clockRate; break;
        case hipDeviceAttributeComputeMode: *pi = p.computeMode; break;
        case hipDeviceAttributeComputePreemptionSupported: *pi = 0; break;
        case hipDeviceAttributeConcurrentKernels: *pi = p.concurrentKernels; break;
        case hipDeviceAttributeConcurrentManagedAccess: *pi = 0; break;
        case hipDeviceAttributeCooperativeLaunch: *pi = p.cooperativeLaunch; break;
        case hipDeviceAttributeCooperativeMultiDeviceLaunch: *pi = 0; break;
        case hipDeviceAttributeDeviceOverlap: *pi = p.deviceOverlap; break;
        case hipDeviceAttributeDirectManagedMemAccessFromHost: *pi = 0; break;
        case hipDeviceAttributeGlobalL1CacheSupported: *pi = p.globalL1CacheSupported; break;
        case hipDeviceAttributeHostNativeAtomicSupported: *pi = 0; break;
        case hipDeviceAttributeIntegrated: *pi = p.integrated; break;
        case hipDeviceAttributeIsMultiGpuBoard: *pi = 0; break;
        case hipDeviceAttributeKernelExecTimeout: *pi = p.kernelExecTimeoutEnabled; break;
        case hipDeviceAttributeL2CacheSize: *pi = p.l2CacheSize; break;
        case hipDeviceAttributeLocalL1CacheSupported: *pi = p.localL1CacheSupported; break;
        case hipDeviceAttributeComputeCapabilityMajor: *pi = p.major; break;
        case hipDeviceAttributeComputeCapabilityMinor: *pi = p.minor; break;
        case hipDeviceAttributeManagedMemory: *pi = p.managedMemory; break;
        case hipDeviceAttributeMaxBlocksPerMultiProcessor: *pi = p.maxBlocksPerMultiProcessor; break;
        case hipDeviceAttributeMaxBlockDimX: *pi = p.maxThreadsDim[0]; break;
        case hipDeviceAttributeMaxBlockDimY: *pi = p.maxThreadsDim[1]; break;
        case hipDeviceAttributeMaxBlockDimZ: *pi = p.maxThreadsDim[2]; break;
        case hipDeviceAttributeMaxGridDimX: *pi = p.maxGridSize[0]; break;
        case hipDeviceAttributeMaxGridDimY: *pi = p.maxGridSize[1]; break;
        case hipDeviceAttributeMaxGridDimZ: *pi = p.maxGridSize[2]; break;
        case hipDeviceAttributeMaxThreadsPerBlock: *pi = p.maxThreadsPerBlock; break;
        case hipDeviceAttributeMaxThreadsPerMultiProcessor: *pi = p.maxThreadsPerMultiProcessor; break;
        case hipDeviceAttributeMaxPitch: *pi = static_cast<int>(p.memPitch); break;
        case hipDeviceAttributeMemoryBusWidth: *pi = p.memoryBusWidth; break;
        case hipDeviceAttributeMemoryClockRate: *pi = p.memoryClockRate; break;
        case hipDeviceAttributeMultiGpuBoardGroupID: *pi = 0; break;
        case hipDeviceAttributeMultiprocessorCount: *pi = p.multiProcessorCount; break;
        case hipDeviceAttributePageableMemoryAccess: *pi = 0; break;
        case hipDeviceAttributePageableMemoryAccessUsesHostPageTables: *pi = 0; break;
        case hipDeviceAttributePciBusId: *pi = p.pciBusID; break;
        case hipDeviceAttributePciDeviceId: *pi = p.pciDeviceID; break;
        case hipDeviceAttributePciDomainId: *pi = p.pciDomainID; break;
        case hipDeviceAttributePersistingL2CacheMaxSize: *pi = 0; break;
        case hipDeviceAttributeMaxRegistersPerBlock: *pi = p.regsPerBlock; break;
        case hipDeviceAttributeMaxRegistersPerMultiprocessor: *pi = p.regsPerMultiprocessor; break;
        case hipDeviceAttributeReservedSharedMemPerBlock: *pi = 0; break;
        case hipDeviceAttributeMaxSharedMemoryPerBlock: *pi = static_cast<int>(p.sharedMemPerBlock); break;
        case hipDeviceAttributeSharedMemPerBlockOptin: *pi = static_cast<int>(p.sharedMemPerBlockOptin); break;
        case hipDeviceAttributeSharedMemPerMultiprocessor: *pi = static_cast<int>(p.sharedMemPerMultiprocessor); break;
        case hipDeviceAttributeStreamPrioritiesSupported: *pi = p.streamPrioritiesSupported; break;
        case hipDeviceAttributeSurfaceAlignment: *pi = static_cast<int>(p.surfaceAlignment); break;
        case hipDeviceAttributeTccDriver: *pi = 0; break;
        case hipDeviceAttributeTextureAlignment: *pi = static_cast<int>(p.textureAlignment); break;
        case hipDeviceAttributeTexturePitchAlignment: *pi = static_cast<int>(p.texturePitchAlignment); break;
        case hipDeviceAttributeTotalConstantMemory: *pi = static_cast<int>(p.totalConstMem); break;
        case hipDeviceAttributeTotalGlobalMem:
            *pi = p.totalGlobalMem > static_cast<size_t>(INT_MAX) ? INT_MAX
                                                                   : static_cast<int>(p.totalGlobalMem);
            break;
        case hipDeviceAttributeUnifiedAddressing: *pi = p.unifiedAddressing; break;
        case hipDeviceAttributeWarpSize: *pi = p.warpSize; break;
        case hipDeviceAttributeMemoryPoolsSupported: *pi = p.memoryPoolsSupported; break;
        case hipDeviceAttributeVirtualMemoryManagementSupported: *pi = 0; break;
        case hipDeviceAttributeHostRegisterSupported: *pi = p.hostRegisterSupported; break;
        case hipDeviceAttributeMemoryPoolSupportedHandleTypes: *pi = 0; break;
        case hipDeviceAttributeHostNumaId: *pi = -1; break;
        case hipDeviceAttributeDmaBufSupported: *pi = 0; break;
        case hipDeviceAttributeGPUDirectRDMAWithHipVMMSupported: *pi = 0; break;
        case hipDeviceAttributeHandleTypeFabricSupported: *pi = 0; break;
        case hipDeviceAttributeHostAllocDmaBufSupported: *pi = 0; break;
        case hipDeviceAttributeGPUDirectRDMASupported: *pi = 0; break;
        case hipDeviceAttributeGPUDirectRDMAFlushWritesOptions: *pi = 0; break;
        case hipDeviceAttributeGPUDirectRDMAWritesOrdering: *pi = 0; break;
        case hipDeviceAttributeClockInstructionRate: *pi = p.clockInstructionRate; break;
        case hipDeviceAttributeMaxSharedMemoryPerMultiprocessor:
            *pi = static_cast<int>(p.maxSharedMemoryPerMultiProcessor);
            break;
        case hipDeviceAttributeCooperativeMultiDeviceUnmatchedFunc: *pi = 0; break;
        case hipDeviceAttributeCooperativeMultiDeviceUnmatchedGridDim: *pi = 0; break;
        case hipDeviceAttributeCooperativeMultiDeviceUnmatchedBlockDim: *pi = 0; break;
        case hipDeviceAttributeCooperativeMultiDeviceUnmatchedSharedMem: *pi = 0; break;
        case hipDeviceAttributeIsLargeBar: *pi = p.isLargeBar; break;
        case hipDeviceAttributeAsicRevision: *pi = p.asicRevision; break;
        case hipDeviceAttributeCanUseStreamWaitValue: *pi = 0; break;
        case hipDeviceAttributeImageSupport: *pi = 0; break;
        case hipDeviceAttributePhysicalMultiProcessorCount: *pi = p.multiProcessorCount; break;
        case hipDeviceAttributeFineGrainSupport: *pi = 0; break;
        case hipDeviceAttributeNumberOfXccs: *pi = 1; break;
        default:
            // Not known here, or an address (the HDP registers), or a clock this build has not
            // measured (the wall clock). A guessed zero would be read as an answer.
            return invalid();
    }
    return hipSuccess;
}

hipError_t hipDeviceGet(hipDevice_t* device, int ordinal) {
    if (device == nullptr) {
        return invalid();
    }
    if (ordinal != 0) {
        return ret(bc250face::fail(hipErrorInvalidDevice));
    }
    *device = 0;
    return hipSuccess;
}

hipError_t hipDeviceTotalMem(size_t* bytes, hipDevice_t device) {
    if (bytes == nullptr) {
        return invalid();
    }
    if (device != 0) {
        return ret(bc250face::fail(hipErrorInvalidDevice));
    }
    bc250face::DeviceInfo info;
    const int err = bc250face::device_info(&info);
    if (err != hipSuccess) {
        return ret(err);
    }
    *bytes = static_cast<size_t>(info.total_bytes);
    return hipSuccess;
}

hipError_t hipDeviceGetStreamPriorityRange(int* leastPriority, int* greatestPriority) {
    // One hardware queue: every stream has the one priority there is.
    if (leastPriority != nullptr) {
        *leastPriority = 0;
    }
    if (greatestPriority != nullptr) {
        *greatestPriority = 0;
    }
    return hipSuccess;
}

hipError_t hipDeviceDisablePeerAccess(int peerDeviceId) {
    (void)peerDeviceId;
    return ret(bc250face::fail(hipErrorInvalidDevice));   // one device, no peer (hip_device.cpp)
}

hipError_t hipCtxGetCurrent(hipCtx_t* ctx) {
    if (ctx == nullptr) {
        return invalid();
    }
    const int err = bc250face::device_ready();
    *ctx = err == hipSuccess ? the_context() : nullptr;
    return ret(err);
}

hipError_t hipCtxSetCurrent(hipCtx_t ctx) {
    if (ctx != nullptr && ctx != the_context()) {
        return ret(bc250face::fail(hipErrorInvalidContext));
    }
    return hipSuccess;
}

hipError_t hipDevicePrimaryCtxRetain(hipCtx_t* pctx, hipDevice_t dev) {
    if (pctx == nullptr) {
        return invalid();
    }
    if (dev != 0) {
        return ret(bc250face::fail(hipErrorInvalidDevice));
    }
    return hipCtxGetCurrent(pctx);
}

hipError_t hipDevicePrimaryCtxGetState(hipDevice_t dev, unsigned int* flags, int* active) {
    if (flags == nullptr || active == nullptr) {
        return invalid();
    }
    if (dev != 0) {
        return ret(bc250face::fail(hipErrorInvalidDevice));
    }
    *flags = 0;
    *active = bc250face::device_ready() == hipSuccess ? 1 : 0;
    return hipSuccess;
}

// The version of the interface this file implements: the HIP of the headers it compiles against.
hipError_t hipRuntimeGetVersion(int* runtimeVersion) {
    if (runtimeVersion == nullptr) {
        return invalid();
    }
    *runtimeVersion = HIP_VERSION;
    return hipSuccess;
}

hipError_t hipDriverGetVersion(int* driverVersion) {
    if (driverVersion == nullptr) {
        return invalid();
    }
    *driverVersion = HIP_VERSION;
    return hipSuccess;
}

hipError_t hipDrvGetErrorString(hipError_t hipError, const char** errorString) {
    if (errorString == nullptr) {
        return invalid();
    }
    *errorString = hipGetErrorString(hipError);
    return hipSuccess;
}

hipError_t hipExtGetLastError(void) { return hipGetLastError(); }

// ---------------------------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------------------------

hipError_t hipExtMallocWithFlags(void** ptr, size_t sizeBytes, unsigned int flags) {
    if (flags == hipDeviceMallocDefault) {
        return hipMalloc(ptr, sizeBytes);
    }
    // Fine-grained, uncached and contiguous memory are promises about coherence and placement
    // that this build has not measured.
    return not_supported("hipExtMallocWithFlags", "fine-grained, uncached or contiguous memory");
}

// Stream-ordered allocation without a pool: the allocation is ready at once, and a free waits
// for the device as hipFree does, which is later than the stream asks for and never earlier.
hipError_t hipMallocAsync(void** dev_ptr, size_t size, hipStream_t stream) {
    if (bc250face::stream_valid(stream) != hipSuccess) {
        return ret(hipErrorInvalidHandle);
    }
    return hipMalloc(dev_ptr, size);
}

hipError_t hipFreeAsync(void* dev_ptr, hipStream_t stream) {
    if (bc250face::stream_valid(stream) != hipSuccess) {
        return ret(hipErrorInvalidHandle);
    }
    return hipFree(dev_ptr);
}

hipError_t hipMemPtrGetInfo(void* ptr, size_t* size) {
    if (ptr == nullptr || size == nullptr) {
        return invalid();
    }
    bc250face::PointerInfo info;
    const hipError_t err = pointer_kind(ptr, &info);
    if (err != hipSuccess) {
        return err;
    }
    if (info.kind == 0) {
        return invalid();
    }
    *size = static_cast<size_t>(info.bytes);
    return hipSuccess;
}

// CUDA 11 semantics: a pointer this runtime does not know is unregistered host memory, and
// that is an answer, not an error.
hipError_t hipPointerGetAttributes(hipPointerAttribute_t* attributes, const void* ptr) {
    if (attributes == nullptr) {
        return invalid();
    }
    bc250face::PointerInfo info;
    const hipError_t err = pointer_kind(ptr, &info);
    if (err != hipSuccess) {
        return err;
    }
    std::memset(attributes, 0, sizeof(*attributes));
    attributes->type = info.kind == 2   ? hipMemoryTypeDevice
                       : info.kind == 1 ? hipMemoryTypeHost
                                        : hipMemoryTypeUnregistered;
    attributes->device = 0;
    attributes->devicePointer = info.device_pointer;
    attributes->hostPointer = info.host_pointer;
    attributes->isManaged = 0;
    attributes->allocationFlags = 0;
    return hipSuccess;
}

hipError_t hipPointerGetAttribute(void* data, hipPointer_attribute attribute, hipDeviceptr_t ptr) {
    if (data == nullptr) {
        return invalid();
    }
    bc250face::PointerInfo info;
    const hipError_t err = pointer_kind(ptr, &info);
    if (err != hipSuccess) {
        return err;
    }
    if (info.kind == 0) {
        return invalid();
    }
    switch (attribute) {
        case HIP_POINTER_ATTRIBUTE_MEMORY_TYPE:
            *static_cast<unsigned int*>(data) =
                static_cast<unsigned int>(info.kind == 2 ? hipMemoryTypeDevice : hipMemoryTypeHost);
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_DEVICE_POINTER:
            *static_cast<void**>(data) = info.device_pointer;
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_HOST_POINTER:
            *static_cast<void**>(data) = info.host_pointer;
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_IS_MANAGED:
            *static_cast<unsigned int*>(data) = 0;
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_DEVICE_ORDINAL:
            *static_cast<int*>(data) = 0;
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_RANGE_START_ADDR:
            *static_cast<void**>(data) = reinterpret_cast<void*>(static_cast<uintptr_t>(info.base));
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_RANGE_SIZE:
            *static_cast<size_t*>(data) = static_cast<size_t>(info.bytes);
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_MAPPED:
            *static_cast<unsigned int*>(data) = 1;
            return hipSuccess;
        case HIP_POINTER_ATTRIBUTE_BUFFER_ID:
            *static_cast<unsigned long long*>(data) = info.base;
            return hipSuccess;
        default:
            return not_supported("hipPointerGetAttribute", "an attribute this build does not track");
    }
}

hipError_t hipGetSymbolAddress(void** devPtr, const void* symbol) {
    return ret(bc250face::symbol_address(devPtr, nullptr, symbol));
}

hipError_t hipMemcpyToSymbol(const void* symbol, const void* src, size_t sizeBytes, size_t offset,
                             hipMemcpyKind kind) {
    void* base = nullptr;
    size_t bytes = 0;
    const hipError_t err = ret(bc250face::symbol_address(&base, &bytes, symbol));
    if (err != hipSuccess) {
        return err;
    }
    if (offset > bytes || sizeBytes > bytes - offset) {
        return invalid();
    }
    return hipMemcpy(static_cast<unsigned char*>(base) + offset, src, sizeBytes, kind);
}

hipError_t hipMemcpyFromSymbol(void* dst, const void* symbol, size_t sizeBytes, size_t offset,
                               hipMemcpyKind kind) {
    void* base = nullptr;
    size_t bytes = 0;
    const hipError_t err = ret(bc250face::symbol_address(&base, &bytes, symbol));
    if (err != hipSuccess) {
        return err;
    }
    if (offset > bytes || sizeBytes > bytes - offset) {
        return invalid();
    }
    return hipMemcpy(dst, static_cast<const unsigned char*>(base) + offset, sizeBytes, kind);
}

hipError_t hipMemcpy2D(void* dst, size_t dpitch, const void* src, size_t spitch, size_t width,
                       size_t height, hipMemcpyKind kind) {
    const hipError_t err = hipMemcpy2DAsync(dst, dpitch, src, spitch, width, height, kind, nullptr);
    return err != hipSuccess ? err : hipStreamSynchronize(nullptr);
}

hipError_t hipMemcpyWithStream(void* dst, const void* src, size_t sizeBytes, hipMemcpyKind kind,
                               hipStream_t stream) {
    const hipError_t err = hipMemcpyAsync(dst, src, sizeBytes, kind, stream);
    return err != hipSuccess ? err : hipStreamSynchronize(stream);
}

// ---------------------------------------------------------------------------------------------
// Streams and events
// ---------------------------------------------------------------------------------------------

hipError_t hipStreamCreateWithPriority(hipStream_t* stream, unsigned int flags, int priority) {
    (void)priority;   // one hardware queue: hipDeviceGetStreamPriorityRange answers 0 and 0
    return hipStreamCreateWithFlags(stream, flags);
}

hipError_t hipStreamQuery(hipStream_t stream) { return ret(bc250face::stream_query(stream)); }

hipError_t hipStreamGetFlags(hipStream_t stream, unsigned int* flags) {
    return ret(bc250face::stream_flags(stream, flags));
}

hipError_t hipStreamGetPriority(hipStream_t stream, int* priority) {
    if (priority == nullptr) {
        return invalid();
    }
    const int err = bc250face::stream_valid(stream);
    if (err != hipSuccess) {
        return ret(err);
    }
    *priority = 0;
    return hipSuccess;
}

hipError_t hipStreamGetDevice(hipStream_t stream, hipDevice_t* device) {
    if (device == nullptr) {
        return invalid();
    }
    const int err = bc250face::stream_valid(stream);
    if (err != hipSuccess) {
        return ret(err);
    }
    *device = 0;
    return hipSuccess;
}

int hipGetStreamDeviceId(hipStream_t stream) {
    return bc250face::stream_valid(stream) == hipSuccess ? 0 : -1;
}

// A host function runs when the work before it on the stream is done. Here it runs at once,
// after the stream is synchronised, on the calling thread. That is later than HIP asks for and
// never earlier, and the function sees every result it may depend on.
hipError_t hipLaunchHostFunc(hipStream_t stream, hipHostFn_t fn, void* userData) {
    if (fn == nullptr) {
        return invalid();
    }
    const hipError_t err = hipStreamSynchronize(stream);
    if (err != hipSuccess) {
        return err;
    }
    fn(userData);
    return hipSuccess;
}

hipError_t bc250amd_hipEventCreateWithFlags(hipEvent_t* event, unsigned flags) {
    // A blocking wait and the release scopes change nothing here: every wait of this runtime
    // sleeps in the kernel, and every release is a full one.
    const unsigned no_effect = hipEventBlockingSync | hipEventDisableSystemFence |
                               hipEventReleaseToDevice | hipEventReleaseToSystem;
    if ((flags & hipEventInterprocess) != 0) {
        return not_supported("hipEventCreateWithFlags", "interprocess events");
    }
    return hipEventCreateWithFlags(event, flags & ~no_effect);
}

hipError_t hipEventQuery(hipEvent_t event) { return ret(bc250face::event_query(event)); }

hipError_t hipEventRecordWithFlags(hipEvent_t event, hipStream_t stream, unsigned int flags) {
    if (flags != hipEventRecordDefault) {
        return not_supported("hipEventRecordWithFlags", "external event records");
    }
    return hipEventRecord(event, stream);
}

// No capture: every stream answers that it is not capturing, which is true.
hipError_t hipStreamIsCapturing(hipStream_t stream, hipStreamCaptureStatus* pCaptureStatus) {
    if (pCaptureStatus == nullptr) {
        return invalid();
    }
    const int err = bc250face::stream_valid(stream);
    if (err != hipSuccess) {
        return ret(err);
    }
    *pCaptureStatus = hipStreamCaptureStatusNone;
    return hipSuccess;
}

hipError_t hipStreamGetCaptureInfo(hipStream_t stream, hipStreamCaptureStatus* pCaptureStatus,
                                   unsigned long long* pId) {
    const hipError_t err = hipStreamIsCapturing(stream, pCaptureStatus);
    if (err == hipSuccess && pId != nullptr) {
        *pId = 0;
    }
    return err;
}

hipError_t hipStreamGetCaptureInfo_v2(hipStream_t stream, hipStreamCaptureStatus* captureStatus_out,
                                      unsigned long long* id_out, hipGraph_t* graph_out,
                                      const hipGraphNode_t** dependencies_out,
                                      size_t* numDependencies_out) {
    const hipError_t err = hipStreamIsCapturing(stream, captureStatus_out);
    if (err != hipSuccess) {
        return err;
    }
    if (id_out != nullptr) {
        *id_out = 0;
    }
    if (graph_out != nullptr) {
        *graph_out = nullptr;
    }
    if (dependencies_out != nullptr) {
        *dependencies_out = nullptr;
    }
    if (numDependencies_out != nullptr) {
        *numDependencies_out = 0;
    }
    return hipSuccess;
}

hipError_t hipStreamEndCapture(hipStream_t stream, hipGraph_t* pGraph) {
    (void)stream;
    if (pGraph != nullptr) {
        *pGraph = nullptr;
    }
    // hipStreamBeginCapture refuses, so no stream is ever capturing.
    return ret(bc250face::fail(hipErrorIllegalState));
}

hipError_t hipThreadExchangeStreamCaptureMode(hipStreamCaptureMode* mode) {
    if (mode == nullptr) {
        return invalid();
    }
    const hipStreamCaptureMode previous = g_capture_mode;
    g_capture_mode = *mode;
    *mode = previous;
    return hipSuccess;
}

// ---------------------------------------------------------------------------------------------
// Modules and kernels
// ---------------------------------------------------------------------------------------------

hipError_t hipModuleLoadData(hipModule_t* module, const void* image) {
    return ret(bc250face::module_load(image, 0, reinterpret_cast<void**>(module)));
}

hipError_t hipModuleLoadDataEx(hipModule_t* module, const void* image, unsigned int numOptions,
                               hipJitOption* options, void** optionValues) {
    // The options steer a JIT compiler, and a code object needs none.
    (void)numOptions;
    (void)options;
    (void)optionValues;
    return hipModuleLoadData(module, image);
}

hipError_t hipModuleLoad(hipModule_t* module, const char* fname) {
    return ret(bc250face::module_load_file(fname, reinterpret_cast<void**>(module)));
}

hipError_t hipModuleUnload(hipModule_t module) { return ret(bc250face::module_unload(module)); }

hipError_t hipModuleGetFunction(hipFunction_t* function, hipModule_t module, const char* kname) {
    return ret(bc250face::module_get_function(reinterpret_cast<void**>(function), module, kname));
}

hipError_t hipModuleLaunchKernel(hipFunction_t f, unsigned int gridDimX, unsigned int gridDimY,
                                 unsigned int gridDimZ, unsigned int blockDimX,
                                 unsigned int blockDimY, unsigned int blockDimZ,
                                 unsigned int sharedMemBytes, hipStream_t stream,
                                 void** kernelParams, void** extra) {
    const uint32_t grid[3] = {gridDimX, gridDimY, gridDimZ};
    const uint32_t block[3] = {blockDimX, blockDimY, blockDimZ};
    return module_launch(f, grid, block, sharedMemBytes, stream, kernelParams, extra);
}

// The sizes of this entry point are in work items, and a grid is in workgroups. A size that is
// not a multiple of the workgroup would need a partial last workgroup, which the dispatch of
// this build does not set up, so it is refused rather than rounded.
hipError_t hipExtModuleLaunchKernel(hipFunction_t f, uint32_t globalWorkSizeX,
                                    uint32_t globalWorkSizeY, uint32_t globalWorkSizeZ,
                                    uint32_t localWorkSizeX, uint32_t localWorkSizeY,
                                    uint32_t localWorkSizeZ, size_t sharedMemBytes,
                                    hipStream_t hStream, void** kernelParams, void** extra,
                                    hipEvent_t startEvent, hipEvent_t stopEvent, uint32_t flags) {
    (void)flags;   // hipExtAnyOrderLaunch: one queue keeps every order anyway
    const uint32_t local[3] = {localWorkSizeX, localWorkSizeY, localWorkSizeZ};
    const uint32_t global[3] = {globalWorkSizeX, globalWorkSizeY, globalWorkSizeZ};
    uint32_t grid[3];
    for (int d = 0; d < 3; ++d) {
        if (local[d] == 0 || global[d] % local[d] != 0) {
            return not_supported("hipExtModuleLaunchKernel",
                                 "a global size that is not a multiple of the workgroup size");
        }
        grid[d] = global[d] / local[d];
    }
    if (sharedMemBytes > UINT_MAX) {
        return invalid();
    }
    if (startEvent != nullptr) {
        const hipError_t err = hipEventRecord(startEvent, hStream);
        if (err != hipSuccess) {
            return err;
        }
    }
    const hipError_t err = module_launch(f, grid, local, static_cast<unsigned>(sharedMemBytes),
                                         hStream, kernelParams, extra);
    if (err != hipSuccess || stopEvent == nullptr) {
        return err;
    }
    return hipEventRecord(stopEvent, hStream);
}

hipError_t hipModuleOccupancyMaxActiveBlocksPerMultiprocessor(int* numBlocks, hipFunction_t f,
                                                              int blockSize,
                                                              size_t dynSharedMemPerBlk) {
    return ret(bc250face::occupancy(numBlocks, nullptr, f, blockSize, dynSharedMemPerBlk));
}

// The block size with the most work items resident per compute unit, from the same model as
// hipOccupancyMaxActiveBlocksPerMultiprocessor. A tie takes the larger block.
hipError_t hipOccupancyMaxPotentialBlockSize(int* gridSize, int* blockSize, const void* f,
                                             size_t dynSharedMemPerBlk, int blockSizeLimit) {
    if (gridSize == nullptr || blockSize == nullptr || f == nullptr || blockSizeLimit < 0) {
        return invalid();
    }
    bc250face::KernelInfo kernel;
    int err = bc250face::kernel_info(f, nullptr, &kernel);
    if (err != hipSuccess) {
        return ret(err);
    }
    bc250face::DeviceInfo device;
    err = bc250face::device_info(&device);
    if (err != hipSuccess) {
        return ret(err);
    }
    int limit = static_cast<int>(kernel.max_threads_per_block);
    if (blockSizeLimit > 0 && blockSizeLimit < limit) {
        limit = blockSizeLimit;
    }
    const int step = static_cast<int>(device.wave_size);
    int best_block = 0;
    int best_blocks = 0;
    for (int size = step; size <= limit; size += step) {
        int blocks = 0;
        err = bc250face::occupancy(&blocks, f, nullptr, size, dynSharedMemPerBlk);
        if (err != hipSuccess) {
            return ret(err);
        }
        if (blocks > 0 && blocks * size >= best_blocks * best_block) {
            best_block = size;
            best_blocks = blocks;
        }
    }
    if (best_block == 0) {
        return invalid();   // the kernel does not fit at any block size
    }
    *blockSize = best_block;
    *gridSize = best_blocks * static_cast<int>(device.cu_count);
    return hipSuccess;
}

hipError_t hipFuncGetAttributes(struct hipFuncAttributes* attr, const void* func) {
    if (attr == nullptr || func == nullptr) {
        return invalid();
    }
    return kernel_attributes(func, nullptr, attr);
}

hipError_t hipFuncGetAttribute(int* value, hipFunction_attribute attrib, hipFunction_t hfunc) {
    if (value == nullptr || hfunc == nullptr) {
        return invalid();
    }
    hipFuncAttributes a;
    const hipError_t err = kernel_attributes(nullptr, hfunc, &a);
    if (err != hipSuccess) {
        return err;
    }
    switch (attrib) {
        case HIP_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK: *value = a.maxThreadsPerBlock; break;
        case HIP_FUNC_ATTRIBUTE_SHARED_SIZE_BYTES: *value = static_cast<int>(a.sharedSizeBytes); break;
        case HIP_FUNC_ATTRIBUTE_CONST_SIZE_BYTES: *value = static_cast<int>(a.constSizeBytes); break;
        case HIP_FUNC_ATTRIBUTE_LOCAL_SIZE_BYTES: *value = static_cast<int>(a.localSizeBytes); break;
        case HIP_FUNC_ATTRIBUTE_NUM_REGS: *value = a.numRegs; break;
        case HIP_FUNC_ATTRIBUTE_PTX_VERSION: *value = a.ptxVersion; break;
        case HIP_FUNC_ATTRIBUTE_BINARY_VERSION: *value = a.binaryVersion; break;
        case HIP_FUNC_ATTRIBUTE_CACHE_MODE_CA: *value = 0; break;
        case HIP_FUNC_ATTRIBUTE_MAX_DYNAMIC_SHARED_SIZE_BYTES: *value = a.maxDynamicSharedSizeBytes; break;
        case HIP_FUNC_ATTRIBUTE_PREFERRED_SHARED_MEMORY_CARVEOUT: *value = 0; break;
        default:
            return invalid();
    }
    return hipSuccess;
}

// This part has no cache that shared memory is carved out of (hipFuncSetAttribute says the
// same of the carve-out preference): the request is legal and has no effect.
hipError_t hipFuncSetCacheConfig(const void* func, hipFuncCache_t config) {
    (void)config;
    return func != nullptr ? hipSuccess : ret(bc250face::fail(hipErrorInvalidDeviceFunction));
}

hipError_t hipDrvLaunchKernelEx(const HIP_LAUNCH_CONFIG* config, hipFunction_t f, void** params,
                                void** extra) {
    (void)config;
    (void)f;
    (void)params;
    (void)extra;
    return not_supported("hipDrvLaunchKernelEx", "launch attributes (clusters, priorities)");
}

hipError_t bc250amd_hipLaunchCooperativeKernel(const void* f, dim3 gridDim, dim3 blockDim,
                                               void** kernelParams, unsigned int sharedMemBytes,
                                               hipStream_t stream) {
    const uint32_t grid[3] = {gridDim.x, gridDim.y, gridDim.z};
    const uint32_t block[3] = {blockDim.x, blockDim.y, blockDim.z};
    return static_cast<hipError_t>(
        bc250face::launch_cooperative(f, grid, block, kernelParams, sharedMemBytes, stream));
}

hipError_t hipModuleLaunchCooperativeKernel(hipFunction_t f, unsigned int gridDimX,
                                            unsigned int gridDimY, unsigned int gridDimZ,
                                            unsigned int blockDimX, unsigned int blockDimY,
                                            unsigned int blockDimZ, unsigned int sharedMemBytes,
                                            hipStream_t stream, void** kernelParams) {
    (void)f;
    (void)gridDimX;
    (void)gridDimY;
    (void)gridDimZ;
    (void)blockDimX;
    (void)blockDimY;
    (void)blockDimZ;
    (void)sharedMemBytes;
    (void)stream;
    (void)kernelParams;
    return not_supported("hipModuleLaunchCooperativeKernel",
                         "one hardware queue, no cooperative dispatch packet and no reserved"
                         " occupancy");
}

// ---------------------------------------------------------------------------------------------
// What this build does not have. Each answers hipErrorNotSupported with its reason in the log.
// ---------------------------------------------------------------------------------------------

#define BC250_NOT_SUPPORTED(name, why, params) \
    hipError_t name params { return not_supported(#name, why); }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif

BC250_NOT_SUPPORTED(hipDeviceGetDefaultMemPool, "memory pools",
                    (hipMemPool_t* mem_pool, int device))
BC250_NOT_SUPPORTED(hipDeviceGetGraphMemAttribute, "graphs",
                    (int device, hipGraphMemAttributeType attr, void* value))
BC250_NOT_SUPPORTED(hipDeviceSetGraphMemAttribute, "graphs",
                    (int device, hipGraphMemAttributeType attr, void* value))
BC250_NOT_SUPPORTED(hipExtStreamCreateWithCUMask, "compute unit masks",
                    (hipStream_t* stream, uint32_t cuMaskSize, const uint32_t* cuMask))
BC250_NOT_SUPPORTED(hipGraphDestroy, "graphs", (hipGraph_t graph))
BC250_NOT_SUPPORTED(hipGraphExecDestroy, "graphs", (hipGraphExec_t graphExec))
BC250_NOT_SUPPORTED(hipGraphGetNodes, "graphs",
                    (hipGraph_t graph, hipGraphNode_t* nodes, size_t* numNodes))
BC250_NOT_SUPPORTED(hipGraphInstantiateWithFlags, "graphs",
                    (hipGraphExec_t* pGraphExec, hipGraph_t graph, unsigned long long flags))
BC250_NOT_SUPPORTED(hipGraphLaunch, "graphs", (hipGraphExec_t graphExec, hipStream_t stream))
BC250_NOT_SUPPORTED(hipGraphNodeGetDependencies, "graphs",
                    (hipGraphNode_t node, hipGraphNode_t* pDependencies,
                     size_t* pNumDependencies))
BC250_NOT_SUPPORTED(hipIpcCloseMemHandle, "interprocess memory", (void* devPtr))
BC250_NOT_SUPPORTED(hipIpcGetEventHandle, "interprocess events",
                    (hipIpcEventHandle_t* handle, hipEvent_t event))
BC250_NOT_SUPPORTED(hipIpcGetMemHandle, "interprocess memory",
                    (hipIpcMemHandle_t* handle, void* devPtr))
BC250_NOT_SUPPORTED(hipIpcOpenEventHandle, "interprocess events",
                    (hipEvent_t* event, hipIpcEventHandle_t handle))
BC250_NOT_SUPPORTED(hipIpcOpenMemHandle, "interprocess memory",
                    (void** devPtr, hipIpcMemHandle_t handle, unsigned int flags))
BC250_NOT_SUPPORTED(hipLinkAddData, "the JIT linker",
                    (hipLinkState_t state, hipJitInputType type, void* data, size_t size,
                     const char* name, unsigned int numOptions, hipJitOption* options,
                     void** optionValues))
BC250_NOT_SUPPORTED(hipLinkComplete, "the JIT linker",
                    (hipLinkState_t state, void** hipBinOut, size_t* sizeOut))
BC250_NOT_SUPPORTED(hipLinkCreate, "the JIT linker",
                    (unsigned int numOptions, hipJitOption* options, void** optionValues,
                     hipLinkState_t* stateOut))
BC250_NOT_SUPPORTED(hipLinkDestroy, "the JIT linker", (hipLinkState_t state))
BC250_NOT_SUPPORTED(hipMemAddressFree, "virtual memory management", (void* devPtr, size_t size))
BC250_NOT_SUPPORTED(hipMemAddressReserve, "virtual memory management",
                    (void** ptr, size_t size, size_t alignment, void* addr,
                     unsigned long long flags))
BC250_NOT_SUPPORTED(hipMemCreate, "virtual memory management",
                    (hipMemGenericAllocationHandle_t* handle, size_t size,
                     const hipMemAllocationProp* prop, unsigned long long flags))
BC250_NOT_SUPPORTED(hipMemMap, "virtual memory management",
                    (void* ptr, size_t size, size_t offset,
                     hipMemGenericAllocationHandle_t handle, unsigned long long flags))
BC250_NOT_SUPPORTED(hipMemRelease, "virtual memory management",
                    (hipMemGenericAllocationHandle_t handle))
BC250_NOT_SUPPORTED(hipMemSetAccess, "virtual memory management",
                    (void* ptr, size_t size, const hipMemAccessDesc* desc, size_t count))
BC250_NOT_SUPPORTED(hipMemUnmap, "virtual memory management", (void* ptr, size_t size))
BC250_NOT_SUPPORTED(hipMemPoolGetAttribute, "memory pools",
                    (hipMemPool_t mem_pool, hipMemPoolAttr attr, void* value))
BC250_NOT_SUPPORTED(hipMemPoolSetAccess, "memory pools",
                    (hipMemPool_t mem_pool, const hipMemAccessDesc* desc_list, size_t count))
BC250_NOT_SUPPORTED(hipMemPoolSetAttribute, "memory pools",
                    (hipMemPool_t mem_pool, hipMemPoolAttr attr, void* value))
BC250_NOT_SUPPORTED(hipMemPoolTrimTo, "memory pools",
                    (hipMemPool_t mem_pool, size_t min_bytes_to_hold))
BC250_NOT_SUPPORTED(hipStreamWaitValue32, "stream wait values",
                    (hipStream_t stream, void* ptr, uint32_t value, unsigned int flags,
                     uint32_t mask))

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// ---------------------------------------------------------------------------------------------
// Ours: the counters of the face, for the gate and the lab kit.
// ---------------------------------------------------------------------------------------------

int bc250amdGetFaceCounters(void* out) {
    return bc250face::counters(static_cast<bc250face::Counters*>(out));
}

}  // extern "C"
