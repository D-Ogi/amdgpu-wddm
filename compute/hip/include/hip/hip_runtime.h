// hip_runtime.h - the minimal HIP header of the BC-250 HIP runtime (M16, route B, layer 2).
//
// Why it exists: our clang compiles HIP with -nogpuinc, so no ROCm header is on the include
// path, and clang itself supplies no HIP declaration. This header holds exactly what clang's
// HIP host and device code generation needs, plus the host entry points of step 2. The design
// is docs/design/m16-hip-route-b.md, section 4.4.
//
// Every declaration here is matched against clang 22 behaviour. It is not copied from ROCm.
//   - clang looks up `hipLaunchKernel` by name in the translation unit when it writes a kernel
//     stub (clang/lib/CodeGen/CGCUDANV.cpp), so that name must be visible at global scope with
//     exactly six parameters.
//   - `kernel<<<grid, block, shared, stream>>>` becomes a call of
//     `__hipPushCallConfiguration` and then a call of the stub, and the stub calls
//     `__hipPopCallConfiguration`.
//   - `__hipRegisterFatBinary`, `__hipRegisterFunction`, `__hipRegisterVar` and
//     `__hipUnregisterFatBinary` come from the module constructor as runtime functions. They do
//     not need a declaration here, but they are declared so that a C++ program can see them.
//
// Step 3 adds the device-side math set, the atomic set, the warp shuffle set, __half and the
// vector types. They resolve through the ROCm device library, so they are header work.

#ifndef BC250_HIP_RUNTIME_H
#define BC250_HIP_RUNTIME_H

// __launch_bounds__ takes one or two arguments in CUDA and in HIP. The first is the largest
// block this kernel is written for, which is amdgpu_flat_work_group_size(1, max). The second is
// the smallest number of waves per execution unit, which only steers occupancy; this build
// accepts it and ignores it, because the right mapping of that number needs a measurement on
// this part. MEASURED: a plain __VA_ARGS__ in the attribute gives
// "'amdgpu_flat_work_group_size' attribute requires exactly 2 arguments" for the two-argument
// form, so the macro takes the first argument out.
#define BC250_HIP_FIRST_ARG(first, ...) first

#if defined(__HIP__)
#define __device__ __attribute__((device))
#define __host__ __attribute__((host))
#define __global__ __attribute__((global))
#define __shared__ __attribute__((shared))
#define __constant__ __attribute__((constant))
#define __managed__ __attribute__((managed))
#define __forceinline__ inline __attribute__((always_inline))
#define __launch_bounds__(...) \
  __attribute__((amdgpu_flat_work_group_size(1, BC250_HIP_FIRST_ARG(__VA_ARGS__, 0))))
#else
#define __device__
#define __host__
#define __global__
#define __shared__
#define __constant__
#define __managed__
#define __forceinline__ inline
#define __launch_bounds__(...)
#endif

#include <stddef.h>
#include <stdint.h>

// MEASURED: the attribute must disappear in the device pass. clang compiles the device side for
// amdgcn-amd-amdhsa, which has no dllimport, and every declaration raises -Wignored-attributes
// there. BC250_HIP_BUILD_DLL is defined by the build of amdhip64.dll itself, whose exports come
// from amdhip64.def.
#if defined(BC250_HIP_BUILD_DLL)
#define HIP_PUBLIC_API
#elif defined(_WIN32) && !defined(__HIP_DEVICE_COMPILE__)
#define HIP_PUBLIC_API __declspec(dllimport)
#else
#define HIP_PUBLIC_API
#endif

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

typedef struct dim3 {
  unsigned int x, y, z;
#if defined(__cplusplus)
  __host__ __device__ dim3(unsigned int x_ = 1, unsigned int y_ = 1, unsigned int z_ = 1)
      : x(x_), y(y_), z(z_) {}
#endif
} dim3;

typedef enum hipError_t {
  hipSuccess = 0,
  hipErrorInvalidValue = 1,
  hipErrorOutOfMemory = 2,
  hipErrorNotInitialized = 3,
  hipErrorDeinitialized = 4,
  hipErrorInvalidDevicePointer = 17,
  hipErrorInvalidMemcpyDirection = 21,
  hipErrorInvalidImage = 200,
  hipErrorContextIsDestroyed = 709,
  hipErrorNoDevice = 100,
  hipErrorInvalidDevice = 101,
  hipErrorInvalidDeviceFunction = 98,
  hipErrorIllegalAddress = 700,
  hipErrorLaunchTimeOut = 702,
  hipErrorNotReady = 600,
  hipErrorInvalidHandle = 400,
  hipErrorNotSupported = 801,
  hipErrorUnknown = 999
} hipError_t;

typedef enum hipMemcpyKind {
  hipMemcpyHostToHost = 0,
  hipMemcpyHostToDevice = 1,
  hipMemcpyDeviceToHost = 2,
  hipMemcpyDeviceToDevice = 3,
  hipMemcpyDefault = 4
} hipMemcpyKind;

typedef struct ihipStream_t *hipStream_t;
typedef struct ihipEvent_t *hipEvent_t;
typedef struct ihipModule_t *hipModule_t;
typedef struct ihipModuleSymbol_t *hipFunction_t;

#define hipStreamDefault 0x00
#define hipStreamNonBlocking 0x01
#define hipEventDefault 0x00
#define hipEventDisableTiming 0x02
#define hipHostMallocDefault 0x00

// The fields that llama.cpp and a normal HIP program read. The runtime fills only the fields it
// knows, and a program that reads one of the others gets 0. Every program of this route is
// compiled against this header, so the field order only has to agree with itself.
typedef struct hipDeviceProp_t {
  char name[256];
  char gcnArchName[256];
  size_t totalGlobalMem;
  size_t sharedMemPerBlock;
  int regsPerBlock;
  int warpSize;
  int maxThreadsPerBlock;
  int maxThreadsDim[3];
  int maxGridSize[3];
  int clockRate;
  int memoryClockRate;
  int memoryBusWidth;
  size_t totalConstMem;
  int major;
  int minor;
  int multiProcessorCount;
  int l2CacheSize;
  int maxThreadsPerMultiProcessor;
  int computeMode;
  int clockInstructionRate;
  int concurrentKernels;
  int pciBusID;
  int pciDeviceID;
  int pciDomainID;
  size_t maxSharedMemoryPerMultiProcessor;
  int isMultiGpuBoard;
  int canMapHostMemory;
  int integrated;
  int cooperativeLaunch;
  int cooperativeMultiDeviceLaunch;
  int managedMemory;
  int unifiedAddressing;
} hipDeviceProp_t;

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------
// What clang's code generation needs
// ---------------------------------------------------------------------------

HIP_PUBLIC_API hipError_t hipLaunchKernel(const void *function, dim3 gridDim, dim3 blockDim,
                                          void **args, size_t sharedMemBytes, hipStream_t stream);
HIP_PUBLIC_API hipError_t __hipPushCallConfiguration(dim3 gridDim, dim3 blockDim,
                                                     size_t sharedMemBytes, hipStream_t stream);
HIP_PUBLIC_API hipError_t __hipPopCallConfiguration(dim3 *gridDim, dim3 *blockDim,
                                                    size_t *sharedMemBytes, hipStream_t *stream);
HIP_PUBLIC_API void **__hipRegisterFatBinary(const void *data);
HIP_PUBLIC_API void __hipUnregisterFatBinary(void **modules);
HIP_PUBLIC_API int __hipRegisterFunction(void **modules, const void *hostFunction,
                                         char *deviceFunction, const char *deviceName,
                                         int threadLimit, void *tid, void *bid, void *blockDim,
                                         void *gridDim, int *workgroupSizeHint);
HIP_PUBLIC_API void __hipRegisterVar(void **modules, void *hostVar, char *deviceVar,
                                     const char *deviceName, int isExtern, size_t size,
                                     int constant, int global);
HIP_PUBLIC_API void __hipRegisterManagedVar(void **modules, void *pointer, void *initValue,
                                            const char *name, size_t size, unsigned align);

// ---------------------------------------------------------------------------
// The step 2 host entry points
// ---------------------------------------------------------------------------

HIP_PUBLIC_API hipError_t hipInit(unsigned int flags);
HIP_PUBLIC_API hipError_t hipGetDeviceCount(int *count);
HIP_PUBLIC_API hipError_t hipSetDevice(int deviceId);
HIP_PUBLIC_API hipError_t hipGetDevice(int *deviceId);
HIP_PUBLIC_API hipError_t hipGetDeviceProperties(hipDeviceProp_t *prop, int deviceId);
HIP_PUBLIC_API hipError_t hipDeviceSynchronize(void);

HIP_PUBLIC_API hipError_t hipMalloc(void **ptr, size_t size);
HIP_PUBLIC_API hipError_t hipFree(void *ptr);
HIP_PUBLIC_API hipError_t hipHostMalloc(void **ptr, size_t size, unsigned int flags);
HIP_PUBLIC_API hipError_t hipHostFree(void *ptr);
HIP_PUBLIC_API hipError_t hipMemcpy(void *dst, const void *src, size_t sizeBytes,
                                    hipMemcpyKind kind);
HIP_PUBLIC_API hipError_t hipMemcpyAsync(void *dst, const void *src, size_t sizeBytes,
                                         hipMemcpyKind kind, hipStream_t stream);
HIP_PUBLIC_API hipError_t hipMemset(void *dst, int value, size_t sizeBytes);
HIP_PUBLIC_API hipError_t hipMemsetAsync(void *dst, int value, size_t sizeBytes,
                                         hipStream_t stream);
HIP_PUBLIC_API hipError_t hipMemGetInfo(size_t *freeBytes, size_t *totalBytes);

HIP_PUBLIC_API hipError_t hipStreamCreate(hipStream_t *stream);
HIP_PUBLIC_API hipError_t hipStreamCreateWithFlags(hipStream_t *stream, unsigned int flags);
HIP_PUBLIC_API hipError_t hipStreamDestroy(hipStream_t stream);
HIP_PUBLIC_API hipError_t hipStreamSynchronize(hipStream_t stream);
HIP_PUBLIC_API hipError_t hipStreamWaitEvent(hipStream_t stream, hipEvent_t event,
                                             unsigned int flags);

HIP_PUBLIC_API hipError_t hipEventCreate(hipEvent_t *event);
HIP_PUBLIC_API hipError_t hipEventCreateWithFlags(hipEvent_t *event, unsigned int flags);
HIP_PUBLIC_API hipError_t hipEventDestroy(hipEvent_t event);
HIP_PUBLIC_API hipError_t hipEventRecord(hipEvent_t event, hipStream_t stream);
HIP_PUBLIC_API hipError_t hipEventSynchronize(hipEvent_t event);
HIP_PUBLIC_API hipError_t hipEventElapsedTime(float *ms, hipEvent_t start, hipEvent_t stop);

HIP_PUBLIC_API hipError_t hipGetLastError(void);
HIP_PUBLIC_API hipError_t hipPeekAtLastError(void);
HIP_PUBLIC_API const char *hipGetErrorString(hipError_t error);
HIP_PUBLIC_API const char *hipGetErrorName(hipError_t error);

// ---------------------------------------------------------------------------
// Our own extension: what the submission layer did
//
// These two names are not HIP. A measurement program needs the number this driver is
// judged by, which is how many times one kernel launch entered the kernel driver, and
// no HIP entry point answers it. The named user is compute/hip/samples/hipbench.hip and
// the lab plan beside it; the numbers come from bc250hsa_counters_read (section 3 of
// compute/hip/include/bc250hsa.h). A program that does not call them is unaffected.
// ---------------------------------------------------------------------------

typedef struct bc250hipCounters {
  unsigned int struct_bytes;
  unsigned long long dispatches;          // dispatch packets built
  unsigned long long submissions;         // calls into the kernel driver that carried work
  unsigned long long batches;             // submissions that carried more than one dispatch
  unsigned long long dispatches_batched;  // dispatches that rode in such a submission
  unsigned long long waits;               // waits for the device
  unsigned long long waits_fast;          // waits the fence mapping answered with no sleep
} bc250hipCounters;

HIP_PUBLIC_API hipError_t bc250hipGetCounters(bc250hipCounters *out);
HIP_PUBLIC_API hipError_t bc250hipResetCounters(void);

#if defined(__cplusplus)
}  // extern "C"
#endif

// ---------------------------------------------------------------------------
// Device side
// ---------------------------------------------------------------------------

#if defined(__HIP__) && defined(__cplusplus)

// threadIdx, blockIdx, blockDim and gridDim must exist in BOTH passes. MEASURED: clang parses
// the body of a __global__ function in the host pass too, so a device-only definition gives
// "use of undeclared identifier 'blockIdx'" while it compiles for the host. Each coordinate is
// therefore an empty object with a __device__ conversion operator, which is the shape the ROCm
// header uses.
#define BC250_HIP_COORD(name, builtin)                                     \
  struct name {                                                            \
    __device__ operator unsigned int() const { return builtin(); }         \
    __device__ unsigned int operator+(unsigned int v) const {              \
      return builtin() + v;                                               \
    }                                                                      \
  }

BC250_HIP_COORD(__hip_wi_x, __builtin_amdgcn_workitem_id_x);
BC250_HIP_COORD(__hip_wi_y, __builtin_amdgcn_workitem_id_y);
BC250_HIP_COORD(__hip_wi_z, __builtin_amdgcn_workitem_id_z);
BC250_HIP_COORD(__hip_wg_x, __builtin_amdgcn_workgroup_id_x);
BC250_HIP_COORD(__hip_wg_y, __builtin_amdgcn_workgroup_id_y);
BC250_HIP_COORD(__hip_wg_z, __builtin_amdgcn_workgroup_id_z);
BC250_HIP_COORD(__hip_ws_x, __builtin_amdgcn_workgroup_size_x);
BC250_HIP_COORD(__hip_ws_y, __builtin_amdgcn_workgroup_size_y);
BC250_HIP_COORD(__hip_ws_z, __builtin_amdgcn_workgroup_size_z);
BC250_HIP_COORD(__hip_gs_x, __builtin_amdgcn_grid_size_x);
BC250_HIP_COORD(__hip_gs_y, __builtin_amdgcn_grid_size_y);
BC250_HIP_COORD(__hip_gs_z, __builtin_amdgcn_grid_size_z);
#undef BC250_HIP_COORD

struct __hip_threadIdx_t { __hip_wi_x x; __hip_wi_y y; __hip_wi_z z; };
struct __hip_blockIdx_t { __hip_wg_x x; __hip_wg_y y; __hip_wg_z z; };
struct __hip_blockDim_t { __hip_ws_x x; __hip_ws_y y; __hip_ws_z z; };
struct __hip_gridDim_t { __hip_gs_x x; __hip_gs_y y; __hip_gs_z z; };

__attribute__((unused)) static const __hip_threadIdx_t threadIdx;
__attribute__((unused)) static const __hip_blockIdx_t blockIdx;
__attribute__((unused)) static const __hip_blockDim_t blockDim;
__attribute__((unused)) static const __hip_gridDim_t gridDim;

__device__ __forceinline__ void __syncthreads() {
  __builtin_amdgcn_fence(__ATOMIC_RELEASE, "workgroup");
  __builtin_amdgcn_s_barrier();
  __builtin_amdgcn_fence(__ATOMIC_ACQUIRE, "workgroup");
}

__device__ __forceinline__ void __threadfence() {
  __builtin_amdgcn_fence(__ATOMIC_SEQ_CST, "agent");
}

__device__ __forceinline__ int __lane_id() { return __builtin_amdgcn_mbcnt_hi(~0u, 0u); }

#endif  // __HIP__ && __cplusplus

#endif  // BC250_HIP_RUNTIME_H
