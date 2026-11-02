// blaslt_abi.h - the part of the hipBLASLt and rocBLAS C interfaces that the hipBLASLt front calls.
//
// The values and the two structures come from these AMD headers (MIT), at ROCm/rocm-libraries
// db2d71e08153d4f53d67bf6b12f38a93e46dce47, the revision that libhipblaslt.dll of TheRock
// 10.3.0a20261030 reports through hipblasLtGetGitRevision ("db2d71e0", hipBLASLt 1.5.0):
//   projects/hipblaslt/library/include/hipblaslt/hipblaslt.h
//   projects/hipblas-common/library/include/hipblas-common/hipblas-common.h
//   projects/rocblas/library/include/internal/rocblas-types.h and rocblas-functions.h
// and hipDataType from hip/library_types.h of the same TheRock wheel. Only the names that the front
// reads or writes are here; the front passes every other value through without reading it.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// hipblas-common.h
typedef enum {
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

typedef enum { HIPBLAS_OP_N = 111, HIPBLAS_OP_T = 112, HIPBLAS_OP_C = 113 } hipblasOperation_t;

typedef enum {
    HIPBLAS_COMPUTE_32F = 2,
    HIPBLAS_COMPUTE_32I = 9,
    HIPBLAS_COMPUTE_32I_PEDANTIC = 10
} hipblasComputeType_t;

// hip/library_types.h
typedef enum { HIP_R_32F = 0, HIP_R_16F = 2, HIP_R_8I = 3, HIP_R_32I = 10, HIP_R_16BF = 14 } hipDataType;

// hipblaslt.h
typedef void* hipblasLtHandle_t;
typedef struct hipblasLtMatmulDescOpaque_st* hipblasLtMatmulDesc_t;
typedef struct hipblasLtMatrixLayoutOpaque_st* hipblasLtMatrixLayout_t;
typedef struct hipblasLtMatmulPreferenceOpaque_st* hipblasLtMatmulPreference_t;
typedef void* hipStream_t;

typedef enum {
    HIPBLASLT_MATRIX_LAYOUT_BATCH_COUNT = 0,           // int32_t
    HIPBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET = 1,  // int64_t
    HIPBLASLT_MATRIX_LAYOUT_TYPE = 2,                  // uint32_t (hipDataType)
    HIPBLASLT_MATRIX_LAYOUT_ORDER = 3,                 // int32_t (hipblasLtOrder_t)
    HIPBLASLT_MATRIX_LAYOUT_ROWS = 4,                  // uint64_t
    HIPBLASLT_MATRIX_LAYOUT_COLS = 5,                  // uint64_t
    HIPBLASLT_MATRIX_LAYOUT_LD = 6,                    // int64_t
    HIPBLASLT_MATRIX_LAYOUT_BATCH_MODE = 7,            // outside the served case when set
    HIPBLASLT_MATRIX_LAYOUT_OFFSET = 8                 // outside the served case when set
} hipblasLtMatrixLayoutAttribute_t;

typedef enum { HIPBLASLT_ORDER_COL = 0, HIPBLASLT_ORDER_ROW = 1 } hipblasLtOrder_t;

typedef enum {
    HIPBLASLT_MATMUL_DESC_TRANSA = 0,          // int32_t (hipblasOperation_t)
    HIPBLASLT_MATMUL_DESC_TRANSB = 1,          // int32_t (hipblasOperation_t)
    HIPBLASLT_MATMUL_DESC_EPILOGUE = 2,        // uint32_t (hipblasLtEpilogue_t)
    HIPBLASLT_MATMUL_DESC_BIAS_POINTER = 3,    // void*
    HIPBLASLT_MATMUL_DESC_A_SCALE_POINTER = 5, // void*
    HIPBLASLT_MATMUL_DESC_B_SCALE_POINTER = 6, // void*
    HIPBLASLT_MATMUL_DESC_C_SCALE_POINTER = 7, // void*
    HIPBLASLT_MATMUL_DESC_D_SCALE_POINTER = 8, // void*
    HIPBLASLT_MATMUL_DESC_EPILOGUE_AUX_SCALE_POINTER = 9, // void*
    HIPBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER = 10,      // void*
    HIPBLASLT_MATMUL_DESC_POINTER_MODE = 13,   // int32_t (hipblasLtPointerMode_t)
    HIPBLASLT_MATMUL_DESC_AMAX_D_POINTER = 14, // void*
    HIPBLASLT_MATMUL_DESC_A_SCALE_MODE = 31,   // int32_t (hipblasLtMatmulMatrixScale_t)
    HIPBLASLT_MATMUL_DESC_B_SCALE_MODE = 32,   // int32_t (hipblasLtMatmulMatrixScale_t)
    HIPBLASLT_MATMUL_DESC_SM_COUNT_TARGET = 33, // int32_t, default 0
    HIPBLASLT_MATMUL_DESC_FUSED_EPILOGUE = 106  // hipblasLtFusedEpilogueDescriptor_t (a pointer)
} hipblasLtMatmulDescAttributes_t;

enum {
    HIPBLASLT_EPILOGUE_DEFAULT = 1,
    HIPBLASLT_POINTER_MODE_HOST = 0,
    HIPBLASLT_MATMUL_MATRIX_SCALE_SCALAR_32F = 0
};

typedef struct _hipblasLtMatmulAlgo_t {
    uint8_t data[16];
    size_t max_workspace_bytes;
} hipblasLtMatmulAlgo_t;

typedef struct _hipblasLtMatmulHeuristicResult_t {
    hipblasLtMatmulAlgo_t algo;
    size_t workspaceSize;
    hipblasStatus_t state;
    float wavesCount;
    int reserved[4];
} hipblasLtMatmulHeuristicResult_t;

// rocblas-types.h. rocblas_int is int32_t in the LP64 build that TheRock ships.
typedef struct _rocblas_handle* rocblas_handle;
typedef int32_t rocblas_int;
typedef enum { rocblas_status_success = 0 } rocblas_status;
typedef enum { rocblas_operation_none = 111, rocblas_operation_transpose = 112 } rocblas_operation;
typedef enum { rocblas_datatype_i8_r = 160, rocblas_datatype_i32_r = 162 } rocblas_datatype;
typedef enum { rocblas_pointer_mode_host = 0 } rocblas_pointer_mode;
typedef enum { rocblas_gemm_algo_standard = 0 } rocblas_gemm_algo;
// rocblas_gemm_flags_none. The flag rocblas_gemm_flags_pack_int8x4 (0x1) was removed in ROCm 6.0:
// int8 inputs need no flag (rocblas-types.h, rocblas_gemm_flags_).
enum { rocblas_gemm_flags_none = 0 };

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(hipblasLtMatmulAlgo_t) == 24, "hipblasLtMatmulAlgo_t is uint8_t[16] and size_t");
static_assert(sizeof(hipblasLtMatmulHeuristicResult_t) == 56,
              "hipblasLtMatmulHeuristicResult_t is algo, size_t, an enum, float and int[4]");
static_assert(offsetof(hipblasLtMatmulHeuristicResult_t, state) == 32, "state follows workspaceSize");
#endif
