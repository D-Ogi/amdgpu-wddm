// bc250hipblas.hip - the ten hipBLAS entry points llama.cpp's ggml-hip backend links against.
//
// Milestone M16, route B, step 3. The interface is compute\hip\include\hipblas\hipblas.h, the
// route and the measurement behind it are compute\hipblas\README.md, and the matrix multiply
// itself is src\gemm_core.h, which the host test and the device test share with the kernel
// below.
//
// This is one translation unit on purpose. clang's HIP driver is used with -fno-gpu-rdc, as
// hip::device of our CMake package asks for, so the device code of a translation unit has to be
// self-contained: a kernel in one file and its launch in another would need device-side link
// time optimisation that our loader does not implement. Every kernel and every entry point is
// therefore here, and gemm_core.h holds what three readers share.
//
// What this library is not: a BLAS. It implements the element types and the two operations that
// one caller passes, and it answers HIPBLAS_STATUS_NOT_SUPPORTED for everything else, which
// ggml turns into a stated abort through its CUBLAS_CHECK macro. The accepted set is the table
// of accept_types() in gemm_core.h.
//
// Nie wszystko złoto, co się świeci - not all that glitters is gold: ten exported names are not
// a linear algebra library, they are a link that holds and an answer that is right.

#include "gemm_core.h"

#include <cstring>
#include <new>

using bc250blas::accept_types;
using bc250blas::Combo;
using bc250blas::GemmShape;
using bc250blas::kTileK;
using bc250blas::kTileM;
using bc250blas::kTileN;
using bc250blas::TileStore;
using bc250blas::validate_shape;

// ---------------------------------------------------------------------------------------------
// The handle. hipBLAS keeps the stream in it, and that is the whole of our state: there is no
// workspace, no math mode (ggml's hip.h answers cublasSetMathMode with a success macro of its
// own) and no pointer mode, because this library reads alpha and beta from host memory, which
// is cuBLAS's CUBLAS_POINTER_MODE_HOST and the only mode ggml uses.
// ---------------------------------------------------------------------------------------------
namespace {

constexpr uint32_t kHandleMagic = 0x42434842u;  // 'BCHB'

}  // namespace

struct bc250BlasHandle_t {
    uint32_t magic;
    hipStream_t stream;
    int device;
};

namespace {

hipblasStatus_t handle_stream(hipblasHandle_t handle, hipStream_t *stream) {
    if (handle == nullptr || handle->magic != kHandleMagic) {
        return HIPBLAS_STATUS_NOT_INITIALIZED;
    }
    *stream = handle->stream;
    return HIPBLAS_STATUS_SUCCESS;
}

// alpha and beta arrive behind a `const void *` in the Ex entry points, and hipBLAS reads them
// in the compute type: a call with HIPBLAS_R_16F as its compute type passes two __half
// (ggml-cuda.cu:1399-1402 does exactly that), and one with HIPBLAS_R_32F passes two float.
bool read_scalar(const void *p, const bool is_half, float *out) {
    if (p == nullptr) {
        return false;
    }
    if (is_half) {
        __half h;
        std::memcpy(&h, p, sizeof(h));
        *out = __half2float(h);
    } else {
        float f;
        std::memcpy(&f, p, sizeof(f));
        *out = f;
    }
    return true;
}

// HIPBLAS_GEMM_DEFAULT is the only algorithm this header names, and the only one the caller
// passes (ggml/src/ggml-cuda/vendors/hip.h:14-15 renames both cuBLAS spellings onto it). An
// unknown selector is refused rather than ignored: a caller that asks for a named algorithm and
// silently gets another one cannot tell what it measured.
hipblasStatus_t accept_algo(const hipblasGemmAlgo_t algo) {
    return algo == HIPBLAS_GEMM_DEFAULT ? HIPBLAS_STATUS_SUCCESS : HIPBLAS_STATUS_INVALID_ENUM;
}

hipblasStatus_t launch_status() {
    const hipError_t err = hipGetLastError();
    switch (err) {
        case hipSuccess: return HIPBLAS_STATUS_SUCCESS;
        case hipErrorOutOfMemory: return HIPBLAS_STATUS_ALLOC_FAILED;
        case hipErrorNotInitialized: return HIPBLAS_STATUS_NOT_INITIALIZED;
        case hipErrorInvalidValue:
        case hipErrorInvalidDevicePointer: return HIPBLAS_STATUS_INVALID_VALUE;
        default: return HIPBLAS_STATUS_EXECUTION_FAILED;
    }
}

// ---------------------------------------------------------------------------------------------
// The two kernels. Both walk the reduction in tiles through local memory, and both are the
// phases of gemm_core.h with the barriers put between them. The only difference is where the
// three pointers of one batch element come from:
//
//   gemm_strided    base pointer plus an element stride per batch element;
//   gemm_pointers   an array of pointers IN DEVICE MEMORY, one per batch element. ggml fills
//                   that array with a kernel of its own (ggml-cuda.cu:1588 and
//                   out-prod.cu:100), so the array cannot be read on the host.
// ---------------------------------------------------------------------------------------------
template <typename TA, typename TB, typename TC>
struct StridedArgs {
    GemmShape g;
    const TA *a;
    const TB *b;
    TC *c;
    long long sa;
    long long sb;
    long long sc;
};

template <typename TA, typename TB, typename TC>
struct PointerArgs {
    GemmShape g;
    const TA *const *a;
    const TB *const *b;
    TC *const *c;
};

}  // namespace

// 256 work items per workgroup: eight wave32 waves, which is what the tile shape asks for.
enum { kGemmBlock = kTileM * kTileN };

template <typename TA, typename TB, typename TC>
__global__ void __launch_bounds__(kGemmBlock) gemm_strided(StridedArgs<TA, TB, TC> args) {
    __shared__ TileStore sh;
    const int tx = (int)threadIdx.x;
    const int ty = (int)threadIdx.y;
    const long long z = (long long)blockIdx.z;
    const TA *A = args.a + z * args.sa;
    const TB *B = args.b + z * args.sb;
    TC *C = args.c + z * args.sc;
    const int tile_i = (int)blockIdx.x * kTileM;
    const int tile_j = (int)blockIdx.y * kTileN;
    float acc = 0.0f;
    for (int kt = 0; kt < args.g.k; kt += kTileK) {
        bc250blas::tile_load(args.g, A, B, tile_i, tile_j, kt, tx, ty, &sh);
        __syncthreads();
        bc250blas::tile_mac(&sh, tx, ty, &acc);
        __syncthreads();
    }
    bc250blas::tile_store(args.g, C, tile_i, tile_j, tx, ty, acc);
}

template <typename TA, typename TB, typename TC>
__global__ void __launch_bounds__(kGemmBlock) gemm_pointers(PointerArgs<TA, TB, TC> args) {
    __shared__ TileStore sh;
    const int tx = (int)threadIdx.x;
    const int ty = (int)threadIdx.y;
    const unsigned z = blockIdx.z;
    const TA *A = args.a[z];
    const TB *B = args.b[z];
    TC *C = args.c[z];
    const int tile_i = (int)blockIdx.x * kTileM;
    const int tile_j = (int)blockIdx.y * kTileN;
    float acc = 0.0f;
    for (int kt = 0; kt < args.g.k; kt += kTileK) {
        bc250blas::tile_load(args.g, A, B, tile_i, tile_j, kt, tx, ty, &sh);
        __syncthreads();
        bc250blas::tile_mac(&sh, tx, ty, &acc);
        __syncthreads();
    }
    bc250blas::tile_store(args.g, C, tile_i, tile_j, tx, ty, acc);
}

namespace {

dim3 gemm_grid(const GemmShape &g, const int batch) {
    return dim3((unsigned)bc250blas::tiles_m(g.m), (unsigned)bc250blas::tiles_n(g.n),
                (unsigned)batch);
}

template <typename TA, typename TB, typename TC>
hipblasStatus_t run_strided(const GemmShape &g, const void *A, const void *B, void *C,
                            const long long sa, const long long sb, const long long sc,
                            const int batch, hipStream_t stream) {
    StridedArgs<TA, TB, TC> args;
    args.g = g;
    args.a = static_cast<const TA *>(A);
    args.b = static_cast<const TB *>(B);
    args.c = static_cast<TC *>(C);
    args.sa = sa;
    args.sb = sb;
    args.sc = sc;
    gemm_strided<TA, TB, TC><<<gemm_grid(g, batch), dim3(kTileM, kTileN, 1), 0, stream>>>(args);
    return launch_status();
}

template <typename TA, typename TB, typename TC>
hipblasStatus_t run_pointers(const GemmShape &g, const void *const *A, const void *const *B,
                             void *const *C, const int batch, hipStream_t stream) {
    PointerArgs<TA, TB, TC> args;
    args.g = g;
    args.a = reinterpret_cast<const TA *const *>(A);
    args.b = reinterpret_cast<const TB *const *>(B);
    args.c = reinterpret_cast<TC *const *>(C);
    gemm_pointers<TA, TB, TC><<<gemm_grid(g, batch), dim3(kTileM, kTileN, 1), 0, stream>>>(args);
    return launch_status();
}

// A and B always carry one element type (accept_types refuses a mixed pair), so the accepted
// set is four (input, output) pairs and this key names them.
enum ComboKey { KEY_F32_F32 = 0, KEY_F16_F16, KEY_F16_F32, KEY_BF16_F32, KEY_NONE };

ComboKey combo_key(const Combo &combo) {
    if (combo.a == bc250blas::ELEM_F32 && combo.c == bc250blas::ELEM_F32) {
        return KEY_F32_F32;
    }
    if (combo.a == bc250blas::ELEM_F16 && combo.c == bc250blas::ELEM_F16) {
        return KEY_F16_F16;
    }
    if (combo.a == bc250blas::ELEM_F16 && combo.c == bc250blas::ELEM_F32) {
        return KEY_F16_F32;
    }
    if (combo.a == bc250blas::ELEM_BF16 && combo.c == bc250blas::ELEM_F32) {
        return KEY_BF16_F32;
    }
    return KEY_NONE;
}

hipblasStatus_t dispatch_strided(const Combo &combo, const GemmShape &g, const void *A,
                                 const void *B, void *C, const long long sa, const long long sb,
                                 const long long sc, const int batch, hipStream_t stream) {
    switch (combo_key(combo)) {
        case KEY_F32_F32:
            return run_strided<float, float, float>(g, A, B, C, sa, sb, sc, batch, stream);
        case KEY_F16_F16:
            return run_strided<__half, __half, __half>(g, A, B, C, sa, sb, sc, batch, stream);
        case KEY_F16_F32:
            return run_strided<__half, __half, float>(g, A, B, C, sa, sb, sc, batch, stream);
        case KEY_BF16_F32:
            return run_strided<__hip_bfloat16, __hip_bfloat16, float>(g, A, B, C, sa, sb, sc,
                                                                      batch, stream);
        default:
            return HIPBLAS_STATUS_NOT_SUPPORTED;
    }
}

hipblasStatus_t dispatch_pointers(const Combo &combo, const GemmShape &g, const void *const *A,
                                  const void *const *B, void *const *C, const int batch,
                                  hipStream_t stream) {
    switch (combo_key(combo)) {
        case KEY_F32_F32:
            return run_pointers<float, float, float>(g, A, B, C, batch, stream);
        case KEY_F16_F16:
            return run_pointers<__half, __half, __half>(g, A, B, C, batch, stream);
        case KEY_F16_F32:
            return run_pointers<__half, __half, float>(g, A, B, C, batch, stream);
        case KEY_BF16_F32:
            return run_pointers<__hip_bfloat16, __hip_bfloat16, float>(g, A, B, C, batch, stream);
        default:
            return HIPBLAS_STATUS_NOT_SUPPORTED;
    }
}

GemmShape shape_of(const hipblasOperation_t op_a, const hipblasOperation_t op_b, const int m,
                   const int n, const int k, const int lda, const int ldb, const int ldc) {
    GemmShape g;
    g.op_a = (int)op_a;
    g.op_b = (int)op_b;
    g.m = m;
    g.n = n;
    g.k = k;
    g.lda = lda;
    g.ldb = ldb;
    g.ldc = ldc;
    g.alpha = 1.0f;
    g.beta = 0.0f;
    return g;
}

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------------------------
// The handle, the stream and the error string.
// ---------------------------------------------------------------------------------------------
hipblasStatus_t hipblasCreate(hipblasHandle_t *handle) {
    if (handle == nullptr) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    *handle = nullptr;
    bc250BlasHandle_t *h = new (std::nothrow) bc250BlasHandle_t();
    if (h == nullptr) {
        return HIPBLAS_STATUS_ALLOC_FAILED;
    }
    h->magic = kHandleMagic;
    h->stream = nullptr;  // the null stream, as hipBLAS starts
    h->device = 0;
    // A handle belongs to the device that was current when it was created, which is what ggml
    // assumes: it keeps one handle per device (ggml-cuda.cu:708).
    if (hipGetDevice(&h->device) != hipSuccess) {
        h->device = 0;
    }
    *handle = h;
    return HIPBLAS_STATUS_SUCCESS;
}

// A null handle, or one that was never created here, is refused. A handle that WAS destroyed
// cannot be refused, and this library does not pretend otherwise: the memory is freed, so no
// marker survives for a second call to read, and a second destroy or a call on a destroyed
// handle is undefined. hipBLAS and cuBLAS state the same contract. ggml creates one handle per
// device and destroys it once, in ~ggml_backend_cuda_context.
hipblasStatus_t hipblasDestroy(hipblasHandle_t handle) {
    if (handle == nullptr || handle->magic != kHandleMagic) {
        return HIPBLAS_STATUS_NOT_INITIALIZED;
    }
    delete handle;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t hipblasSetStream(hipblasHandle_t handle, hipStream_t stream) {
    if (handle == nullptr || handle->magic != kHandleMagic) {
        return HIPBLAS_STATUS_NOT_INITIALIZED;
    }
    handle->stream = stream;
    return HIPBLAS_STATUS_SUCCESS;
}

const char *hipblasStatusToString(hipblasStatus_t status) {
    switch (status) {
        case HIPBLAS_STATUS_SUCCESS: return "HIPBLAS_STATUS_SUCCESS";
        case HIPBLAS_STATUS_NOT_INITIALIZED: return "HIPBLAS_STATUS_NOT_INITIALIZED";
        case HIPBLAS_STATUS_ALLOC_FAILED: return "HIPBLAS_STATUS_ALLOC_FAILED";
        case HIPBLAS_STATUS_INVALID_VALUE: return "HIPBLAS_STATUS_INVALID_VALUE";
        case HIPBLAS_STATUS_MAPPING_ERROR: return "HIPBLAS_STATUS_MAPPING_ERROR";
        case HIPBLAS_STATUS_EXECUTION_FAILED: return "HIPBLAS_STATUS_EXECUTION_FAILED";
        case HIPBLAS_STATUS_INTERNAL_ERROR: return "HIPBLAS_STATUS_INTERNAL_ERROR";
        case HIPBLAS_STATUS_NOT_SUPPORTED: return "HIPBLAS_STATUS_NOT_SUPPORTED";
        case HIPBLAS_STATUS_ARCH_MISMATCH: return "HIPBLAS_STATUS_ARCH_MISMATCH";
        case HIPBLAS_STATUS_HANDLE_IS_NULLPTR: return "HIPBLAS_STATUS_HANDLE_IS_NULLPTR";
        case HIPBLAS_STATUS_INVALID_ENUM: return "HIPBLAS_STATUS_INVALID_ENUM";
        default: return "HIPBLAS_STATUS_UNKNOWN";
    }
}

// ---------------------------------------------------------------------------------------------
// The f32 general matrix multiply, in its three forms.
// ---------------------------------------------------------------------------------------------
hipblasStatus_t hipblasSgemm(hipblasHandle_t handle, hipblasOperation_t transA,
                             hipblasOperation_t transB, int m, int n, int k, const float *alpha,
                             const float *A, int lda, const float *B, int ldb, const float *beta,
                             float *C, int ldc) {
    hipStream_t stream = nullptr;
    const hipblasStatus_t held = handle_stream(handle, &stream);
    if (held != HIPBLAS_STATUS_SUCCESS) {
        return held;
    }
    GemmShape g = shape_of(transA, transB, m, n, k, lda, ldb, ldc);
    const bool has_work = m > 0 && n > 0;
    const hipblasStatus_t ok =
        validate_shape(g, 1, alpha != nullptr && beta != nullptr,
                       A != nullptr && B != nullptr && C != nullptr, has_work);
    if (ok != HIPBLAS_STATUS_SUCCESS) {
        return ok;
    }
    if (!has_work) {
        return HIPBLAS_STATUS_SUCCESS;
    }
    g.alpha = *alpha;
    g.beta = *beta;
    return run_strided<float, float, float>(g, A, B, C, 0, 0, 0, 1, stream);
}

hipblasStatus_t hipblasSgemmBatched(hipblasHandle_t handle, hipblasOperation_t transA,
                                    hipblasOperation_t transB, int m, int n, int k,
                                    const float *alpha, const float *const A[], int lda,
                                    const float *const B[], int ldb, const float *beta,
                                    float *const C[], int ldc, int batchCount) {
    hipStream_t stream = nullptr;
    const hipblasStatus_t held = handle_stream(handle, &stream);
    if (held != HIPBLAS_STATUS_SUCCESS) {
        return held;
    }
    GemmShape g = shape_of(transA, transB, m, n, k, lda, ldb, ldc);
    const bool has_work = m > 0 && n > 0 && batchCount > 0;
    const hipblasStatus_t ok =
        validate_shape(g, batchCount, alpha != nullptr && beta != nullptr,
                       A != nullptr && B != nullptr && C != nullptr, has_work);
    if (ok != HIPBLAS_STATUS_SUCCESS) {
        return ok;
    }
    if (!has_work) {
        return HIPBLAS_STATUS_SUCCESS;
    }
    g.alpha = *alpha;
    g.beta = *beta;
    return run_pointers<float, float, float>(g, reinterpret_cast<const void *const *>(A),
                                             reinterpret_cast<const void *const *>(B),
                                             reinterpret_cast<void *const *>(C), batchCount,
                                             stream);
}

hipblasStatus_t hipblasSgemmStridedBatched(hipblasHandle_t handle, hipblasOperation_t transA,
                                           hipblasOperation_t transB, int m, int n, int k,
                                           const float *alpha, const float *A, int lda,
                                           long long strideA, const float *B, int ldb,
                                           long long strideB, const float *beta, float *C,
                                           int ldc, long long strideC, int batchCount) {
    hipStream_t stream = nullptr;
    const hipblasStatus_t held = handle_stream(handle, &stream);
    if (held != HIPBLAS_STATUS_SUCCESS) {
        return held;
    }
    GemmShape g = shape_of(transA, transB, m, n, k, lda, ldb, ldc);
    const bool has_work = m > 0 && n > 0 && batchCount > 0;
    const hipblasStatus_t ok =
        validate_shape(g, batchCount, alpha != nullptr && beta != nullptr,
                       A != nullptr && B != nullptr && C != nullptr, has_work);
    if (ok != HIPBLAS_STATUS_SUCCESS) {
        return ok;
    }
    if (!has_work) {
        return HIPBLAS_STATUS_SUCCESS;
    }
    g.alpha = *alpha;
    g.beta = *beta;
    return run_strided<float, float, float>(g, A, B, C, strideA, strideB, strideC, batchCount,
                                            stream);
}

// ---------------------------------------------------------------------------------------------
// The typed general matrix multiply, in its three forms. Every type combination that
// accept_types() does not name answers HIPBLAS_STATUS_NOT_SUPPORTED.
// ---------------------------------------------------------------------------------------------
hipblasStatus_t hipblasGemmEx(hipblasHandle_t handle, hipblasOperation_t transA,
                              hipblasOperation_t transB, int m, int n, int k, const void *alpha,
                              const void *A, hipblasDatatype_t aType, int lda, const void *B,
                              hipblasDatatype_t bType, int ldb, const void *beta, void *C,
                              hipblasDatatype_t cType, int ldc, hipblasDatatype_t computeType,
                              hipblasGemmAlgo_t algo) {
    hipStream_t stream = nullptr;
    const hipblasStatus_t held = handle_stream(handle, &stream);
    if (held != HIPBLAS_STATUS_SUCCESS) {
        return held;
    }
    const hipblasStatus_t algo_ok = accept_algo(algo);
    if (algo_ok != HIPBLAS_STATUS_SUCCESS) {
        return algo_ok;
    }
    Combo combo;
    const hipblasStatus_t typed = accept_types(aType, bType, cType, computeType, &combo);
    if (typed != HIPBLAS_STATUS_SUCCESS) {
        return typed;
    }
    GemmShape g = shape_of(transA, transB, m, n, k, lda, ldb, ldc);
    const bool has_work = m > 0 && n > 0;
    const hipblasStatus_t ok = validate_shape(
        g, 1, alpha != nullptr && beta != nullptr,
        A != nullptr && B != nullptr && C != nullptr, has_work);
    if (ok != HIPBLAS_STATUS_SUCCESS) {
        return ok;
    }
    if (!has_work) {
        return HIPBLAS_STATUS_SUCCESS;
    }
    if (!read_scalar(alpha, combo.alpha_is_half, &g.alpha) ||
        !read_scalar(beta, combo.alpha_is_half, &g.beta)) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    return dispatch_strided(combo, g, A, B, C, 0, 0, 0, 1, stream);
}

hipblasStatus_t hipblasGemmBatchedEx(hipblasHandle_t handle, hipblasOperation_t transA,
                                     hipblasOperation_t transB, int m, int n, int k,
                                     const void *alpha, const void *A[], hipblasDatatype_t aType,
                                     int lda, const void *B[], hipblasDatatype_t bType, int ldb,
                                     const void *beta, void *C[], hipblasDatatype_t cType,
                                     int ldc, int batchCount, hipblasDatatype_t computeType,
                                     hipblasGemmAlgo_t algo) {
    hipStream_t stream = nullptr;
    const hipblasStatus_t held = handle_stream(handle, &stream);
    if (held != HIPBLAS_STATUS_SUCCESS) {
        return held;
    }
    const hipblasStatus_t algo_ok = accept_algo(algo);
    if (algo_ok != HIPBLAS_STATUS_SUCCESS) {
        return algo_ok;
    }
    Combo combo;
    const hipblasStatus_t typed = accept_types(aType, bType, cType, computeType, &combo);
    if (typed != HIPBLAS_STATUS_SUCCESS) {
        return typed;
    }
    GemmShape g = shape_of(transA, transB, m, n, k, lda, ldb, ldc);
    const bool has_work = m > 0 && n > 0 && batchCount > 0;
    const hipblasStatus_t ok = validate_shape(
        g, batchCount, alpha != nullptr && beta != nullptr,
        A != nullptr && B != nullptr && C != nullptr, has_work);
    if (ok != HIPBLAS_STATUS_SUCCESS) {
        return ok;
    }
    if (!has_work) {
        return HIPBLAS_STATUS_SUCCESS;
    }
    if (!read_scalar(alpha, combo.alpha_is_half, &g.alpha) ||
        !read_scalar(beta, combo.alpha_is_half, &g.beta)) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    return dispatch_pointers(combo, g, const_cast<const void *const *>(A),
                             const_cast<const void *const *>(B), C, batchCount, stream);
}

hipblasStatus_t hipblasGemmStridedBatchedEx(
    hipblasHandle_t handle, hipblasOperation_t transA, hipblasOperation_t transB, int m, int n,
    int k, const void *alpha, const void *A, hipblasDatatype_t aType, int lda, long long strideA,
    const void *B, hipblasDatatype_t bType, int ldb, long long strideB, const void *beta,
    void *C, hipblasDatatype_t cType, int ldc, long long strideC, int batchCount,
    hipblasDatatype_t computeType, hipblasGemmAlgo_t algo) {
    hipStream_t stream = nullptr;
    const hipblasStatus_t held = handle_stream(handle, &stream);
    if (held != HIPBLAS_STATUS_SUCCESS) {
        return held;
    }
    const hipblasStatus_t algo_ok = accept_algo(algo);
    if (algo_ok != HIPBLAS_STATUS_SUCCESS) {
        return algo_ok;
    }
    Combo combo;
    const hipblasStatus_t typed = accept_types(aType, bType, cType, computeType, &combo);
    if (typed != HIPBLAS_STATUS_SUCCESS) {
        return typed;
    }
    GemmShape g = shape_of(transA, transB, m, n, k, lda, ldb, ldc);
    const bool has_work = m > 0 && n > 0 && batchCount > 0;
    const hipblasStatus_t ok = validate_shape(
        g, batchCount, alpha != nullptr && beta != nullptr,
        A != nullptr && B != nullptr && C != nullptr, has_work);
    if (ok != HIPBLAS_STATUS_SUCCESS) {
        return ok;
    }
    if (!has_work) {
        return HIPBLAS_STATUS_SUCCESS;
    }
    if (!read_scalar(alpha, combo.alpha_is_half, &g.alpha) ||
        !read_scalar(beta, combo.alpha_is_half, &g.beta)) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    return dispatch_strided(combo, g, A, B, C, strideA, strideB, strideC, batchCount, stream);
}

// ---------------------------------------------------------------------------------------------
// The triangular solve, which this library refuses.
//
// It has one caller, ggml's solve_tri operation (ggml/src/ggml-cuda/solve_tri.cu:70), which
// inference never reaches: no model graph of llama.cpp contains GGML_OP_SOLVE_TRI. A refusal
// becomes a stated abort in ggml's CUBLAS_CHECK, and a wrong triangular solve would become a
// wrong number with no message. Honest refusal, therefore, and not a stub that returns success.
// ---------------------------------------------------------------------------------------------
hipblasStatus_t hipblasStrsmBatched(hipblasHandle_t handle, hipblasSideMode_t side,
                                    hipblasFillMode_t uplo, hipblasOperation_t transA,
                                    hipblasDiagType_t diag, int m, int n, const float *alpha,
                                    const float *const A[], int lda, float *const B[], int ldb,
                                    int batchCount) {
    hipStream_t stream = nullptr;
    const hipblasStatus_t held = handle_stream(handle, &stream);
    if (held != HIPBLAS_STATUS_SUCCESS) {
        return held;
    }
    (void)side;
    (void)uplo;
    (void)transA;
    (void)diag;
    (void)m;
    (void)n;
    (void)alpha;
    (void)A;
    (void)lda;
    (void)B;
    (void)ldb;
    (void)batchCount;
    return HIPBLAS_STATUS_NOT_SUPPORTED;
}

}  // extern "C"
