// amd_abi_probe.inc - what both HIP headers must agree on, for test_amd_abi_agreement.
//
// amdhip64_7.dll exports these entry points of our runtime unchanged under AMD's names, so a
// program compiled against AMD's header calls our definitions. That is only correct when both
// headers give each one the same type and give each value the same number. The test includes
// this list twice, once under each header, and compares what the two translation units print.
// A name or value missing from either header is a compile error, which is a failure too.

// The entry points amdhip64_7.def exports unchanged (all but the renamed and the dropped ones).
// The five __hipRegister and __hipUnregister calls are not here: no header declares them for
// the caller, because clang's HIP code generation emits those calls itself, and test_hip_mock
// and the HIP samples of build-runtime.ps1 run that code against this runtime.
#define BC250_ABI_FUNCTIONS(X) \
    X(__hipPopCallConfiguration) X(__hipPushCallConfiguration) \
    X(hipDeviceCanAccessPeer) X(hipDeviceEnablePeerAccess) \
    X(hipDeviceGetPCIBusId) X(hipDeviceSynchronize) X(hipEventCreate) X(hipEventDestroy) \
    X(hipEventElapsedTime) X(hipEventRecord) X(hipEventSynchronize) X(hipFree) \
    X(hipFuncSetAttribute) X(hipGetDevice) X(hipGetDeviceCount) X(hipGetErrorName) \
    X(hipGetErrorString) X(hipGetLastError) X(hipHostFree) X(hipHostGetDevicePointer) \
    X(hipHostRegister) X(hipHostUnregister) X(hipInit) X(hipLaunchKernel) \
    X(hipMemAdvise) X(hipMemGetInfo) X(hipMemcpy) X(hipMemcpy2DAsync) X(hipMemcpyAsync) \
    X(hipMemcpyPeerAsync) X(hipMemset) X(hipMemsetAsync) X(hipPeekAtLastError) X(hipSetDevice) \
    X(hipStreamBeginCapture) X(hipStreamCreate) X(hipStreamCreateWithFlags) \
    X(hipStreamDestroy) X(hipStreamSynchronize) X(hipStreamWaitEvent)

// The exported names that AMD's header also overloads with a C++ template. The C declaration is
// named by its type, written here once: a header that does not declare exactly this function
// does not compile.
#define BC250_ABI_OVERLOADED(X) \
    X(hipHostMalloc, (void**, size_t, unsigned int)) \
    X(hipMalloc, (void**, size_t)) \
    X(hipMallocManaged, (void**, size_t, unsigned int)) \
    X(hipOccupancyMaxActiveBlocksPerMultiprocessor, (int*, const void*, int, size_t))

// Every enumerator and flag of our header that a caller passes or receives through those.
#define BC250_ABI_VALUES(X) \
    X(hipSuccess) X(hipErrorInvalidValue) X(hipErrorOutOfMemory) X(hipErrorNotInitialized) \
    X(hipErrorDeinitialized) X(hipErrorInvalidDevicePointer) X(hipErrorInvalidMemcpyDirection) \
    X(hipErrorInvalidImage) X(hipErrorContextIsDestroyed) X(hipErrorNoDevice) \
    X(hipErrorInvalidDevice) X(hipErrorInvalidDeviceFunction) X(hipErrorIllegalAddress) \
    X(hipErrorLaunchTimeOut) X(hipErrorNotReady) X(hipErrorInvalidHandle) \
    X(hipErrorNotSupported) X(hipErrorPeerAccessAlreadyEnabled) \
    X(hipErrorPeerAccessNotEnabled) X(hipErrorHostMemoryAlreadyRegistered) \
    X(hipErrorHostMemoryNotRegistered) X(hipErrorUnknown) \
    X(hipMemcpyHostToHost) X(hipMemcpyHostToDevice) X(hipMemcpyDeviceToHost) \
    X(hipMemcpyDeviceToDevice) X(hipMemcpyDefault) \
    X(hipStreamDefault) X(hipStreamNonBlocking) X(hipEventDefault) X(hipEventDisableTiming) \
    X(hipHostMallocDefault) X(hipHostMallocPortable) X(hipHostMallocMapped) \
    X(hipHostMallocWriteCombined) X(hipHostRegisterDefault) X(hipHostRegisterPortable) \
    X(hipHostRegisterMapped) X(hipHostRegisterIoMemory) X(hipHostRegisterReadOnly) \
    X(hipFuncAttributeMaxDynamicSharedMemorySize) \
    X(hipFuncAttributePreferredSharedMemoryCarveout) \
    X(hipStreamCaptureModeGlobal) X(hipStreamCaptureModeThreadLocal) \
    X(hipStreamCaptureModeRelaxed) \
    X(hipMemAdviseSetReadMostly) X(hipMemAdviseUnsetReadMostly) \
    X(hipMemAdviseSetPreferredLocation) X(hipMemAdviseUnsetPreferredLocation) \
    X(hipMemAdviseSetAccessedBy) X(hipMemAdviseUnsetAccessedBy) \
    X(hipMemAdviseSetCoarseGrain) X(hipMemAdviseUnsetCoarseGrain)

// The types whose size and layout cross the interface by value or behind a pointer.
#define BC250_ABI_TYPES(X) \
    X(dim3) X(hipError_t) X(hipMemcpyKind) X(hipFuncAttribute) X(hipStreamCaptureMode) \
    X(hipMemoryAdvise) X(hipStream_t) X(hipEvent_t)

#include <cstdio>
#include <typeinfo>

static void BC250_ABI_DUMP(std::FILE* out) {
#define BC250_ABI_F(sym) std::fprintf(out, "fn %s %s\n", #sym, typeid(decltype(&sym)).name());
    BC250_ABI_FUNCTIONS(BC250_ABI_F)
#undef BC250_ABI_F
#define BC250_ABI_O(sym, params) \
    std::fprintf(out, "fn %s %s\n", #sym, \
                 typeid(static_cast<hipError_t(*) params>(&sym)).name());
    BC250_ABI_OVERLOADED(BC250_ABI_O)
#undef BC250_ABI_O
#define BC250_ABI_V(sym) std::fprintf(out, "value %s %lld\n", #sym, static_cast<long long>(sym));
    BC250_ABI_VALUES(BC250_ABI_V)
#undef BC250_ABI_V
#define BC250_ABI_T(sym) \
    std::fprintf(out, "type %s %zu %zu\n", #sym, sizeof(sym), alignof(sym));
    BC250_ABI_TYPES(BC250_ABI_T)
#undef BC250_ABI_T
    std::fprintf(out, "value hipStreamLegacy %lld\n",
                 static_cast<long long>(reinterpret_cast<uintptr_t>(hipStreamLegacy)));
    std::fprintf(out, "value hipStreamPerThread %lld\n",
                 static_cast<long long>(reinterpret_cast<uintptr_t>(hipStreamPerThread)));
    std::fprintf(out, "layout dim3 %zu %zu %zu\n", offsetof(dim3, x), offsetof(dim3, y),
                 offsetof(dim3, z));
}
