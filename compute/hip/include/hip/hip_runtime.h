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

#include <hip/hip_vector_types.h>
#include <hip/hip_version.h>

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
  hipErrorPeerAccessAlreadyEnabled = 704,
  hipErrorPeerAccessNotEnabled = 705,
  hipErrorHostMemoryAlreadyRegistered = 712,
  hipErrorHostMemoryNotRegistered = 713,
  hipErrorUnknown = 999
} hipError_t;

typedef enum hipMemcpyKind {
  hipMemcpyHostToHost = 0,
  hipMemcpyHostToDevice = 1,
  hipMemcpyDeviceToHost = 2,
  hipMemcpyDeviceToDevice = 3,
  hipMemcpyDefault = 4
} hipMemcpyKind;

// A default argument is a C++ idea, and this header is also read from C. Two entry points need
// one, because CUDA gives the last parameter a default and code written for CUDA leaves it out:
// hipStreamWaitEvent (ggml-cuda/allreduce.cu:393) and hipMallocManaged (ggml-cuda.cu:145).
#if defined(__cplusplus)
#define BC250_HIP_DEFAULT(value) = value
#else
#define BC250_HIP_DEFAULT(value)
#endif

typedef struct ihipStream_t *hipStream_t;
typedef struct ihipEvent_t *hipEvent_t;
typedef struct ihipModule_t *hipModule_t;
typedef struct ihipModuleSymbol_t *hipFunction_t;

#define hipStreamDefault 0x00
#define hipStreamNonBlocking 0x01
#define hipEventDefault 0x00
#define hipEventDisableTiming 0x02
#define hipHostMallocDefault 0x00
#define hipHostMallocPortable 0x01
#define hipHostMallocMapped 0x02
#define hipHostMallocWriteCombined 0x04
#define hipHostRegisterDefault 0x00
#define hipHostRegisterPortable 0x01
#define hipHostRegisterMapped 0x02
#define hipHostRegisterIoMemory 0x04
#define hipHostRegisterReadOnly 0x08

// The per-thread default stream. HIP gives it a handle value that no stream object can have,
// and a program may pass it where a stream is expected. This runtime has one process-wide
// default stream and no per-thread one, so both handles mean the same object (step 3 of design
// docs/design/m16-hip-route-b.md, section 4.9).
#define hipStreamLegacy ((hipStream_t)1)
#define hipStreamPerThread ((hipStream_t)2)

// The device attributes this runtime answers. The numbers are ours: a program of this route
// compiles against this header, and the header travels with the DLL, so the only requirement is
// that the two agree. hipGetDeviceProperties carries the same numbers as whole fields, and a
// program may use either.
typedef enum hipDeviceAttribute_t {
  hipDeviceAttributeWarpSize = 1,
  hipDeviceAttributeMaxThreadsPerBlock = 2,
  hipDeviceAttributeMaxSharedMemoryPerBlock = 3,
  hipDeviceAttributeMultiprocessorCount = 4,
  hipDeviceAttributeClockRate = 5,
  hipDeviceAttributeConcurrentKernels = 6,
  hipDeviceAttributeIntegrated = 7,
  hipDeviceAttributeCanMapHostMemory = 8,
  hipDeviceAttributeComputeCapabilityMajor = 9,
  hipDeviceAttributeComputeCapabilityMinor = 10,
  hipDeviceAttributeCooperativeLaunch = 11,
  hipDeviceAttributeVirtualMemoryManagementSupported = 12,
  hipDeviceAttributeManagedMemory = 13
} hipDeviceAttribute_t;

// The kernel attributes a program may set. The two numbers are the ones CUDA uses for the same
// two attributes, because a program written for CUDA reaches them through a rename.
typedef enum hipFuncAttribute {
  hipFuncAttributeMaxDynamicSharedMemorySize = 8,
  hipFuncAttributePreferredSharedMemoryCarveout = 9
} hipFuncAttribute;

// The capture modes of a stream capture. This runtime captures nothing: hipStreamBeginCapture
// is not among its entry points, and hipDeviceAttributeConcurrentKernels and the graph
// attributes answer that there is no graph support. The enumeration exists because ggml-cuda
// names hipStreamCaptureModeRelaxed inside the branch its own GGML_HIP_GRAPHS option guards
// (ggml-cuda.cu:4598), and a name inside a dead branch still has to parse.
typedef enum hipStreamCaptureMode {
  hipStreamCaptureModeGlobal = 0,
  hipStreamCaptureModeThreadLocal = 1,
  hipStreamCaptureModeRelaxed = 2
} hipStreamCaptureMode;

// The advice a program may give about managed memory. This runtime has no managed memory, so
// every value is accepted by the parser and refused by hipMemAdvise.
typedef enum hipMemoryAdvise {
  hipMemAdviseSetReadMostly = 1,
  hipMemAdviseUnsetReadMostly = 2,
  hipMemAdviseSetPreferredLocation = 3,
  hipMemAdviseUnsetPreferredLocation = 4,
  hipMemAdviseSetAccessedBy = 5,
  hipMemAdviseUnsetAccessedBy = 6,
  hipMemAdviseSetCoarseGrain = 100,
  hipMemAdviseUnsetCoarseGrain = 101
} hipMemoryAdvise;

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
                                             unsigned int flags BC250_HIP_DEFAULT(0));
HIP_PUBLIC_API hipError_t hipStreamBeginCapture(hipStream_t stream, hipStreamCaptureMode mode);

HIP_PUBLIC_API hipError_t hipEventCreate(hipEvent_t *event);
HIP_PUBLIC_API hipError_t hipEventCreateWithFlags(hipEvent_t *event, unsigned int flags);
HIP_PUBLIC_API hipError_t hipEventDestroy(hipEvent_t event);
HIP_PUBLIC_API hipError_t hipEventRecord(hipEvent_t event, hipStream_t stream);
HIP_PUBLIC_API hipError_t hipEventSynchronize(hipEvent_t event);
HIP_PUBLIC_API hipError_t hipEventElapsedTime(float *ms, hipEvent_t start, hipEvent_t stop);

// ---------------------------------------------------------------------------
// The step 3 host entry points
//
// llama.cpp's ggml-hip backend links against every name below. Five of them say no and mean it:
// this part has no managed memory, no page-locked registration of a program's own memory, no
// second device and no cooperative dispatch. ggml-hip handles each of those answers, and
// docs/design/m16-hip-route-b.md section 4.9 states where it does.
// ---------------------------------------------------------------------------

HIP_PUBLIC_API hipError_t hipDeviceGetAttribute(int *value, hipDeviceAttribute_t attr,
                                                int deviceId);
HIP_PUBLIC_API hipError_t hipDeviceGetPCIBusId(char *pciBusId, int len, int deviceId);
HIP_PUBLIC_API hipError_t hipDeviceCanAccessPeer(int *canAccessPeer, int deviceId,
                                                 int peerDeviceId);
HIP_PUBLIC_API hipError_t hipDeviceEnablePeerAccess(int peerDeviceId, unsigned int flags);

HIP_PUBLIC_API hipError_t hipFuncSetAttribute(const void *func, hipFuncAttribute attr, int value);
HIP_PUBLIC_API hipError_t hipLaunchCooperativeKernel(const void *function, dim3 gridDim,
                                                     dim3 blockDim, void **args,
                                                     size_t sharedMemBytes, hipStream_t stream);

HIP_PUBLIC_API hipError_t hipHostGetDevicePointer(void **devPtr, void *hstPtr,
                                                  unsigned int flags);
HIP_PUBLIC_API hipError_t hipHostRegister(void *hostPtr, size_t sizeBytes, unsigned int flags);
HIP_PUBLIC_API hipError_t hipHostUnregister(void *hostPtr);
HIP_PUBLIC_API hipError_t hipMallocManaged(void **ptr, size_t size,
                                           unsigned int flags BC250_HIP_DEFAULT(1));
HIP_PUBLIC_API hipError_t hipOccupancyMaxActiveBlocksPerMultiprocessor(int *numBlocks,
                                                                      const void *func,
                                                                      int blockSize,
                                                                      size_t dynamicSMemSize);
HIP_PUBLIC_API hipError_t hipMemAdvise(const void *devPtr, size_t count, hipMemoryAdvise advice,
                                       int deviceId);
HIP_PUBLIC_API hipError_t hipMemcpy2DAsync(void *dst, size_t dpitch, const void *src,
                                           size_t spitch, size_t width, size_t height,
                                           hipMemcpyKind kind, hipStream_t stream);
HIP_PUBLIC_API hipError_t hipMemcpyPeerAsync(void *dst, int dstDeviceId, const void *src,
                                             int srcDeviceId, size_t sizeBytes,
                                             hipStream_t stream);

HIP_PUBLIC_API hipError_t hipGetLastError(void);
HIP_PUBLIC_API hipError_t hipPeekAtLastError(void);
HIP_PUBLIC_API const char *hipGetErrorString(hipError_t error);
HIP_PUBLIC_API const char *hipGetErrorName(hipError_t error);

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
// MEASURED: the conversion operator has to be the only way out of this object. An extra
// `operator+(unsigned int)` makes `blockIdx.x + 1` ambiguous against the built-in addition over
// the converted value (mmq.cuh:1076).
#define BC250_HIP_COORD(name, builtin)                             \
  struct name {                                                    \
    __device__ operator unsigned int() const { return builtin(); } \
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

// MEASURED: `__builtin_amdgcn_mbcnt_hi(~0u, 0u)` alone is 0 on every lane of a wave32 wave,
// because the high half of EXEC is empty there. The lane index is the low count first and the
// high count on top of it, which is correct for wave32 and for wave64.
__device__ __forceinline__ int __lane_id() {
  return __builtin_amdgcn_mbcnt_hi(~0u, __builtin_amdgcn_mbcnt_lo(~0u, 0u));
}

// ---------------------------------------------------------------------------
// The wavefront. gfx1013 is a wave32 part, and the compiler states the width itself.
// ---------------------------------------------------------------------------

// MEASURED: it must be a variable, not a macro. As a macro it also replaces the `warpSize`
// field of hipDeviceProp_t, and `prop.warpSize` then stops compiling (compute/hip/samples/
// vadd.hip:206). The device pass learns the width from the compiler; the host pass has no wave
// of its own and reads the device's width from hipGetDeviceProperties.
#if defined(__AMDGCN_WAVEFRONT_SIZE__)
static constexpr int warpSize = __AMDGCN_WAVEFRONT_SIZE__;
#else
static constexpr int warpSize = 32;
#endif

// ---------------------------------------------------------------------------
// The C names a kernel is allowed to call
//
// A device function may call printf, abort, memcpy and memset. The host declarations of those
// four come from the C library and are __host__ only, so without these the compiler refuses the
// call (MEASURED: ggml-cuda/common.cuh:417, :424, :842).
// ---------------------------------------------------------------------------

// This route has no hostcall buffer, so a kernel cannot write to the host's console. The call is
// therefore accepted and discarded, and it returns 0 characters written, which is the truth.
// Every ggml-cuda use of it sits next to a __trap(), and the trap is not discarded.
__device__ __forceinline__ int printf(const char *, ...) { return 0; }

__device__ __forceinline__ void abort() { __builtin_trap(); }

__device__ __forceinline__ void *memcpy(void *dst, const void *src, size_t n) {
  __builtin_memcpy(dst, src, n);
  return dst;
}

__device__ __forceinline__ void *memset(void *dst, int value, size_t n) {
  __builtin_memset(dst, value, n);
  return dst;
}

// ---------------------------------------------------------------------------
// The high half of a product, which CUDA has as an intrinsic and ggml-cuda's fast division uses
// (common.cuh:937).
// ---------------------------------------------------------------------------

// The rounded float-to-integer conversions. CUDA names the rounding mode in the name, and
// ggml-cuda uses the nearest-even one (quantize.cu:114).
__device__ __host__ __forceinline__ int __float2int_rn(float x) {
  return (int)__builtin_rintf(x);
}
__device__ __host__ __forceinline__ int __float2int_rz(float x) { return (int)x; }
__device__ __host__ __forceinline__ int __float2int_rd(float x) {
  return (int)__builtin_floorf(x);
}
__device__ __host__ __forceinline__ int __float2int_ru(float x) {
  return (int)__builtin_ceilf(x);
}
__device__ __host__ __forceinline__ unsigned int __float2uint_rn(float x) {
  return (unsigned int)__builtin_rintf(x);
}

__device__ __host__ __forceinline__ unsigned int __umulhi(unsigned int a, unsigned int b) {
  return (unsigned int)(((unsigned long long)a * (unsigned long long)b) >> 32);
}
__device__ __host__ __forceinline__ int __mulhi(int a, int b) {
  return (int)(((long long)a * (long long)b) >> 32);
}
__device__ __host__ __forceinline__ unsigned long long __umul64hi(unsigned long long a,
                                                                  unsigned long long b) {
  return (unsigned long long)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
}
__device__ __host__ __forceinline__ long long __mul64hi(long long a, long long b) {
  return (long long)(((__int128)a * (__int128)b) >> 64);
}

__device__ __forceinline__ void __threadfence_block() {
  __builtin_amdgcn_fence(__ATOMIC_SEQ_CST, "workgroup");
}

__device__ __forceinline__ void __threadfence_system() {
  __builtin_amdgcn_fence(__ATOMIC_SEQ_CST, "");
}

__device__ __forceinline__ void __syncwarp(unsigned int = 0xffffffffu) {
  __builtin_amdgcn_fence(__ATOMIC_SEQ_CST, "wavefront");
  __builtin_amdgcn_wave_barrier();
}

// s_sleep counts in units of about 64 clocks, and MEASURED: its argument must be a constant
// integer, so a runtime nanosecond count cannot become one instruction. The wait is therefore
// the instruction's smallest unit, repeated: one unit is about 64 clocks, which at the lab's
// 1000 MHz is about 64 ns. A caller that wants a long wait gets a short one, which is the safe
// direction for a spin loop; the name exists so that code written for CUDA compiles.
__device__ __forceinline__ void __nanosleep(unsigned int ns) {
  unsigned int left = ns;
  while (left > 64u) {
    __builtin_amdgcn_s_sleep(1);
    left -= 64u;
  }
  __builtin_amdgcn_s_sleep(1);
}

__device__ __forceinline__ int __syncthreads_count(int predicate) {
  // The count over the whole workgroup needs a shared counter, which a header cannot give.
  // Over one wave it is the population count of the ballot, and ggml-cuda uses the form only
  // inside a single wave. A workgroup wider than one wave would need the LDS version.
  __syncthreads();
  return __builtin_popcountll(__builtin_amdgcn_uicmp(predicate != 0, 0, 33 /* ne */));
}

__device__ __forceinline__ bool __syncthreads_or(int predicate) {
  __syncthreads();
  return __builtin_amdgcn_uicmp(predicate != 0, 0, 33 /* ne */) != 0;
}

__device__ __forceinline__ bool __syncthreads_and(int predicate) {
  __syncthreads();
  return __builtin_amdgcn_uicmp(predicate == 0, 0, 33 /* ne */) == 0;
}

// The ballot of a wave32 part still has the HIP signature, which is 64 bits wide.
__device__ __forceinline__ unsigned long long __ballot(int predicate) {
  return __builtin_amdgcn_uicmp(predicate != 0, 0, 33 /* ne */);
}

__device__ __forceinline__ unsigned long long __ballot_sync(unsigned long long /*mask*/,
                                                            int predicate) {
  return __ballot(predicate);
}

__device__ __forceinline__ int __all(int predicate) {
  return __builtin_amdgcn_uicmp(predicate != 0, 0, 33 /* ne */) == __builtin_amdgcn_read_exec()
             ? 1
             : 0;
}

__device__ __forceinline__ int __any(int predicate) {
  return __builtin_amdgcn_uicmp(predicate != 0, 0, 33 /* ne */) != 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Bit counting and bit casts
// ---------------------------------------------------------------------------

__device__ __host__ __forceinline__ int __popc(unsigned int v) { return __builtin_popcount(v); }
__device__ __host__ __forceinline__ int __popcll(unsigned long long v) {
  return __builtin_popcountll(v);
}
__device__ __host__ __forceinline__ int __clz(int v) {
  return v == 0 ? 32 : __builtin_clz((unsigned int)v);
}
__device__ __host__ __forceinline__ int __clzll(long long v) {
  return v == 0 ? 64 : __builtin_clzll((unsigned long long)v);
}
__device__ __host__ __forceinline__ int __ffs(int v) {
  return v == 0 ? 0 : __builtin_ctz((unsigned int)v) + 1;
}
__device__ __host__ __forceinline__ int __ffsll(long long v) {
  return v == 0 ? 0 : __builtin_ctzll((unsigned long long)v) + 1;
}
__device__ __host__ __forceinline__ unsigned int __brev(unsigned int v) {
  return __builtin_bitreverse32(v);
}

#define BC250_HIP_BITCAST(name, FROM, TO)                      \
  __device__ __host__ __forceinline__ TO name(const FROM v) {  \
    union {                                                    \
      FROM f;                                                  \
      TO t;                                                    \
    } c;                                                       \
    c.f = v;                                                   \
    return c.t;                                                \
  }

BC250_HIP_BITCAST(__float_as_int, float, int)
BC250_HIP_BITCAST(__float_as_uint, float, unsigned int)
BC250_HIP_BITCAST(__int_as_float, int, float)
BC250_HIP_BITCAST(__uint_as_float, unsigned int, float)
BC250_HIP_BITCAST(__double_as_longlong, double, long long)
BC250_HIP_BITCAST(__longlong_as_double, long long, double)
#undef BC250_HIP_BITCAST

// The byte selector of CUDA's __byte_perm, which gfx10 has as one instruction.
__device__ __forceinline__ unsigned int __byte_perm(unsigned int x, unsigned int y,
                                                    unsigned int s) {
  return __builtin_amdgcn_perm(y, x, s);
}

// A read through the constant cache. This part has no separate non-coherent path a header can
// name, so the load is an ordinary one and the name exists for source compatibility.
template <typename T>
__device__ __forceinline__ T __ldg(const T *ptr) {
  return *ptr;
}

// The shared-memory window of a generic pointer, as a 32-bit offset. On amdgcn the address space
// cast is the whole operation.
__device__ __forceinline__ unsigned int __cvta_generic_to_shared(const void *p) {
  return (unsigned int)(size_t)(__attribute__((address_space(3))) void *)p;
}

// ---------------------------------------------------------------------------
// The wave shuffle set
//
// One generic implementation over any type whose size is a multiple of 4 bytes, plus the 1-, 2-
// and 8-byte cases, so that `__shfl_xor(x, 16, 32)` works for int, float, __half and __half2
// alike. ds_bpermute moves one dword per lane; a wider value moves dword by dword.
// ---------------------------------------------------------------------------

__device__ __forceinline__ int __bc250_shfl_dword(int var, int lane) {
  return __builtin_amdgcn_ds_bpermute(lane << 2, var);
}

template <typename T>
__device__ __forceinline__ T __bc250_shfl_index(T var, int lane) {
  const int words = (int)((sizeof(T) + 3) / 4);
  union {
    T value;
    int word[(sizeof(T) + 3) / 4];
  } c;
  c.value = var;
#pragma unroll
  for (int i = 0; i < words; ++i) {
    c.word[i] = __bc250_shfl_dword(c.word[i], lane);
  }
  return c.value;
}

template <typename T>
__device__ __forceinline__ T __shfl(T var, int src_lane, int width = 0) {
  const int self = __lane_id();
  const int w = width > 0 ? width : __builtin_amdgcn_wavefrontsize();
  const int lane = (src_lane & (w - 1)) + (self & ~(w - 1));
  return __bc250_shfl_index(var, lane);
}

template <typename T>
__device__ __forceinline__ T __shfl_up(T var, unsigned int delta, int width = 0) {
  const int self = __lane_id();
  const int w = width > 0 ? width : __builtin_amdgcn_wavefrontsize();
  int lane = self - (int)delta;
  lane = lane < (self & ~(w - 1)) ? self : lane;
  return __bc250_shfl_index(var, lane);
}

template <typename T>
__device__ __forceinline__ T __shfl_down(T var, unsigned int delta, int width = 0) {
  const int self = __lane_id();
  const int w = width > 0 ? width : __builtin_amdgcn_wavefrontsize();
  int lane = self + (int)delta;
  lane = lane >= ((self + w) & ~(w - 1)) ? self : lane;
  return __bc250_shfl_index(var, lane);
}

template <typename T>
__device__ __forceinline__ T __shfl_xor(T var, int lane_mask, int width = 0) {
  const int self = __lane_id();
  const int w = width > 0 ? width : __builtin_amdgcn_wavefrontsize();
  int lane = self ^ lane_mask;
  lane = lane >= ((self + w) & ~(w - 1)) ? self : lane;
  return __bc250_shfl_index(var, lane);
}

// ---------------------------------------------------------------------------
// Atomics
//
// Every one of these is the corresponding C++ atomic operation on the pointed-to object, at
// agent scope, which is what CUDA's device-wide atomic means. MEASURED on gfx1013 (step 3
// probe): an integer add becomes global_atomic_add, and a float add becomes a
// global_atomic_cmpswap loop, because this part has no floating-point atomic add.
// ---------------------------------------------------------------------------

#define BC250_HIP_ATOMIC_RMW(name, op)                                            \
  template <typename T>                                                           \
  __device__ __forceinline__ T name(T *address, T val) {                          \
    return __atomic_fetch_##op(address, val, __ATOMIC_RELAXED);                    \
  }

BC250_HIP_ATOMIC_RMW(atomicAnd, and)
BC250_HIP_ATOMIC_RMW(atomicOr, or)
BC250_HIP_ATOMIC_RMW(atomicXor, xor)
BC250_HIP_ATOMIC_RMW(atomicSub, sub)
#undef BC250_HIP_ATOMIC_RMW

template <typename T>
__device__ __forceinline__ T atomicExch(T *address, T val) {
  return __atomic_exchange_n(address, val, __ATOMIC_RELAXED);
}

template <typename T>
__device__ __forceinline__ T atomicCAS(T *address, T compare, T val) {
  __atomic_compare_exchange_n(address, &compare, val, false, __ATOMIC_RELAXED,
                              __ATOMIC_RELAXED);
  return compare;
}

__device__ __forceinline__ int atomicAdd(int *address, int val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}
__device__ __forceinline__ unsigned int atomicAdd(unsigned int *address, unsigned int val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}
__device__ __forceinline__ unsigned long long atomicAdd(unsigned long long *address,
                                                        unsigned long long val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}
__device__ __forceinline__ float atomicAdd(float *address, float val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}
__device__ __forceinline__ double atomicAdd(double *address, double val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}
__device__ __forceinline__ float atomicAdd_system(float *address, float val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}
__device__ __forceinline__ int atomicAdd_system(int *address, int val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}
__device__ __forceinline__ unsigned int atomicAdd_system(unsigned int *address,
                                                         unsigned int val) {
  return __atomic_fetch_add(address, val, __ATOMIC_RELAXED);
}

template <typename T>
__device__ __forceinline__ T atomicMax(T *address, T val) {
  return __atomic_fetch_max(address, val, __ATOMIC_RELAXED);
}
template <typename T>
__device__ __forceinline__ T atomicMin(T *address, T val) {
  return __atomic_fetch_min(address, val, __ATOMIC_RELAXED);
}

#endif  // __HIP__ && __cplusplus

// `__align__(n)` is the CUDA spelling of an alignment request, and HIP keeps it.
#if !defined(__align__)
#define __align__(n) __attribute__((aligned(n)))
#endif

#endif  // BC250_HIP_RUNTIME_H
