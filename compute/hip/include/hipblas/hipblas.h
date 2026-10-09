// hipblas.h - the hipBLAS interface the BC-250 HIP runtime answers (M16, route B, step 3).
//
// Why it exists: llama.cpp's ggml-hip backend includes <hipblas/hipblas.h> unconditionally
// (ggml/src/ggml-cuda/vendors/hip.h:5) and renames every cuBLAS name onto a hipBLAS one. We have
// no hipBLAS and no rocBLAS, and this route does not want one: ggml's own MMQ and MMVQ kernels do
// the quantized matrix multiply, and GGML_CUDA_FORCE_MMQ sends every quantized multiply there.
//
// MEASURED (scratch\m16-hip\step3\out\inventory.md, llama.cpp b86d2f07): ten hipBLAS entry points
// remain after that, in four files. None of them is on the token-generation path of a quantized
// model; they are the f32 and f16 general matrix multiply of ggml_cuda_mul_mat for a non-quantized
// tensor, the batched multiply of out-prod, the 2-D convolution and the triangular solve.
//
// This header declares exactly those ten, plus the handle, the status and the enumerations the
// backend's renames need. The implementation is bc250hipblas.dll, in ..\..\hipblas. Each entry
// point that this route does not implement answers HIPBLAS_STATUS_NOT_SUPPORTED, which
// ggml-cuda turns into a stated abort through its CUBLAS_CHECK macro instead of a wrong result.
// Why that is the honest route, and what it costs, is in compute\hipblas\README.md.

#ifndef BC250_HIPBLAS_H
#define BC250_HIPBLAS_H

#include <hip/hip_runtime.h>

#if defined(BC250_HIPBLAS_BUILD_DLL)
#define HIPBLAS_PUBLIC_API
#elif defined(_WIN32) && !defined(__HIP_DEVICE_COMPILE__)
#define HIPBLAS_PUBLIC_API __declspec(dllimport)
#else
#define HIPBLAS_PUBLIC_API
#endif

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct bc250BlasHandle_t *hipblasHandle_t;

typedef enum hipblasStatus_t {
  HIPBLAS_STATUS_SUCCESS = 0,
  HIPBLAS_STATUS_NOT_INITIALIZED = 1,
  HIPBLAS_STATUS_ALLOC_FAILED = 2,
  HIPBLAS_STATUS_INVALID_VALUE = 3,
  HIPBLAS_STATUS_MAPPING_ERROR = 4,
  HIPBLAS_STATUS_EXECUTION_FAILED = 5,
  HIPBLAS_STATUS_INTERNAL_ERROR = 6,
  HIPBLAS_STATUS_NOT_SUPPORTED = 7,
  HIPBLAS_STATUS_ARCH_MISMATCH = 8,
  HIPBLAS_STATUS_HANDLE_IS_NULLPTR = 9,
  HIPBLAS_STATUS_INVALID_ENUM = 10,
  HIPBLAS_STATUS_UNKNOWN = 11
} hipblasStatus_t;

typedef enum hipblasOperation_t {
  HIPBLAS_OP_N = 111,
  HIPBLAS_OP_T = 112,
  HIPBLAS_OP_C = 113
} hipblasOperation_t;

typedef enum hipblasSideMode_t {
  HIPBLAS_SIDE_LEFT = 141,
  HIPBLAS_SIDE_RIGHT = 142,
  HIPBLAS_SIDE_BOTH = 143
} hipblasSideMode_t;

typedef enum hipblasFillMode_t {
  HIPBLAS_FILL_MODE_UPPER = 121,
  HIPBLAS_FILL_MODE_LOWER = 122,
  HIPBLAS_FILL_MODE_FULL = 123
} hipblasFillMode_t;

typedef enum hipblasDiagType_t {
  HIPBLAS_DIAG_NON_UNIT = 131,
  HIPBLAS_DIAG_UNIT = 132
} hipblasDiagType_t;

// The element types this interface names. The numbers are hipBLAS's own, so that a program built
// against a real hipBLAS header and this one agrees on the wire values.
typedef enum hipblasDatatype_t {
  HIPBLAS_R_16F = 150,
  HIPBLAS_R_32F = 151,
  HIPBLAS_R_64F = 152,
  HIPBLAS_C_16F = 153,
  HIPBLAS_C_32F = 154,
  HIPBLAS_C_64F = 155,
  HIPBLAS_R_8I = 160,
  HIPBLAS_R_8U = 161,
  HIPBLAS_R_32I = 162,
  HIPBLAS_R_32U = 163,
  HIPBLAS_R_16B = 168,
  HIPBLAS_C_16B = 169
} hipblasDatatype_t;

typedef enum hipblasGemmAlgo_t {
  HIPBLAS_GEMM_DEFAULT = 160
} hipblasGemmAlgo_t;

typedef __half hipblasHalf;

HIPBLAS_PUBLIC_API hipblasStatus_t hipblasCreate(hipblasHandle_t *handle);
HIPBLAS_PUBLIC_API hipblasStatus_t hipblasDestroy(hipblasHandle_t handle);
HIPBLAS_PUBLIC_API hipblasStatus_t hipblasSetStream(hipblasHandle_t handle, hipStream_t stream);
HIPBLAS_PUBLIC_API const char *hipblasStatusToString(hipblasStatus_t status);

HIPBLAS_PUBLIC_API hipblasStatus_t hipblasSgemm(hipblasHandle_t handle, hipblasOperation_t transA,
                                                hipblasOperation_t transB, int m, int n, int k,
                                                const float *alpha, const float *A, int lda,
                                                const float *B, int ldb, const float *beta,
                                                float *C, int ldc);

HIPBLAS_PUBLIC_API hipblasStatus_t hipblasSgemmBatched(
    hipblasHandle_t handle, hipblasOperation_t transA, hipblasOperation_t transB, int m, int n,
    int k, const float *alpha, const float *const A[], int lda, const float *const B[], int ldb,
    const float *beta, float *const C[], int ldc, int batchCount);

HIPBLAS_PUBLIC_API hipblasStatus_t hipblasSgemmStridedBatched(
    hipblasHandle_t handle, hipblasOperation_t transA, hipblasOperation_t transB, int m, int n,
    int k, const float *alpha, const float *A, int lda, long long strideA, const float *B,
    int ldb, long long strideB, const float *beta, float *C, int ldc, long long strideC,
    int batchCount);

HIPBLAS_PUBLIC_API hipblasStatus_t hipblasGemmEx(
    hipblasHandle_t handle, hipblasOperation_t transA, hipblasOperation_t transB, int m, int n,
    int k, const void *alpha, const void *A, hipblasDatatype_t aType, int lda, const void *B,
    hipblasDatatype_t bType, int ldb, const void *beta, void *C, hipblasDatatype_t cType, int ldc,
    hipblasDatatype_t computeType, hipblasGemmAlgo_t algo);

HIPBLAS_PUBLIC_API hipblasStatus_t hipblasGemmBatchedEx(
    hipblasHandle_t handle, hipblasOperation_t transA, hipblasOperation_t transB, int m, int n,
    int k, const void *alpha, const void *A[], hipblasDatatype_t aType, int lda, const void *B[],
    hipblasDatatype_t bType, int ldb, const void *beta, void *C[], hipblasDatatype_t cType,
    int ldc, int batchCount, hipblasDatatype_t computeType, hipblasGemmAlgo_t algo);

HIPBLAS_PUBLIC_API hipblasStatus_t hipblasGemmStridedBatchedEx(
    hipblasHandle_t handle, hipblasOperation_t transA, hipblasOperation_t transB, int m, int n,
    int k, const void *alpha, const void *A, hipblasDatatype_t aType, int lda, long long strideA,
    const void *B, hipblasDatatype_t bType, int ldb, long long strideB, const void *beta, void *C,
    hipblasDatatype_t cType, int ldc, long long strideC, int batchCount,
    hipblasDatatype_t computeType, hipblasGemmAlgo_t algo);

// A: the triangular matrices, which the caller holds as `const float **`
// (ggml-cuda/solve_tri.cu:62); B: the right-hand sides, which it overwrites with the solution.
HIPBLAS_PUBLIC_API hipblasStatus_t hipblasStrsmBatched(
    hipblasHandle_t handle, hipblasSideMode_t side, hipblasFillMode_t uplo,
    hipblasOperation_t transA, hipblasDiagType_t diag, int m, int n, const float *alpha,
    const float *const A[], int lda, float *const B[], int ldb, int batchCount);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // BC250_HIPBLAS_H
