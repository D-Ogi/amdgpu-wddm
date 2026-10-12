// gemm_core.h - the matrix multiply of bc250hipblas, written once for three readers.
//
// Milestone M16, route B, step 3. The three readers are:
//
//   1. the device kernel of bc250hipblas.hip, which runs this code on gfx1013;
//   2. the host test tests\host\test_gemm_core.cpp, which runs the SAME tile phases serially on
//      the CPU and compares the result with the independent reference below. The mock backend of
//      layer 2 records a dispatch and executes no instruction (tests\host\hipmock_backend.h), so
//      a kernel cannot be run against it; this emulation is what replaces that;
//   3. the device test tests\gemm_check.hip, which uses the reference as its expected answer on
//      the hardware.
//
// Nothing here calls a HIP runtime entry point and nothing here uses __syncthreads, so every
// function compiles for the host pass as well. The kernel supplies the barriers, and the host
// emulation supplies them by running one phase for every thread before it starts the next.
//
// hipBLAS is column major, as cuBLAS is. C is m x n with leading dimension ldc; A is m x k when
// transA is N and k x m when it is T; B is k x n when transB is N and n x k when it is T. Every
// index of this file follows those three sentences and nothing else.

#ifndef BC250_GEMM_CORE_H
#define BC250_GEMM_CORE_H

#include <hip/hip_runtime.h>

#include <hip/hip_bf16.h>
#include <hip/hip_fp16.h>
#include <hipblas/hipblas.h>

// __host__ __device__ are empty in a host-only compilation (hip\hip_runtime.h), which is what
// lets the host test and the reference share this file with the kernel.
#define BC250_BLAS_HD __host__ __device__ __forceinline__

namespace bc250blas {

// ---------------------------------------------------------------------------------------------
// The element types this library computes with, and the table of combinations it accepts.
//
// Every combination that is not in accept_types() answers HIPBLAS_STATUS_NOT_SUPPORTED. That is
// the whole reason this enumeration is not just hipblasDatatype_t: an unknown type must never
// fall through to "assume f32", because with GGML_CUDA_FORCE_MMQ=ON a quantized type that MMQ
// does not cover still reaches hipblasGemmEx through ggml_cuda_mul_mat_cublas, and a wrong GEMM
// there is a wrong answer in the model's output. A refusal is a stated abort through ggml's own
// CUBLAS_CHECK macro instead.
// ---------------------------------------------------------------------------------------------
enum Elem {
    ELEM_F32 = 0,
    ELEM_F16 = 1,
    ELEM_BF16 = 2,
    ELEM_OTHER = 3
};

BC250_BLAS_HD Elem elem_of(const hipblasDatatype_t type) {
    switch (type) {
        case HIPBLAS_R_32F: return ELEM_F32;
        case HIPBLAS_R_16F: return ELEM_F16;
        case HIPBLAS_R_16B: return ELEM_BF16;
        default: return ELEM_OTHER;
    }
}

BC250_BLAS_HD int elem_bytes(const Elem e) {
    return e == ELEM_F32 ? 4 : (e == ELEM_OTHER ? 0 : 2);
}

// What one accepted call computes with. `alpha_is_half` says how the two scalars behind the
// `const void *` of hipblasGemmEx are read: hipBLAS reads them in the compute type, so a call
// with HIPBLAS_R_16F as its compute type passes two `__half` and not two `float`
// (ggml-cuda.cu:1399-1402 does exactly that).
struct Combo {
    Elem a;
    Elem b;
    Elem c;
    bool alpha_is_half;
};

// The accepted combinations, as a table and nothing cleverer. aType, bType and cType are the
// element types of the three matrices; computeType is what the caller asks the arithmetic to be
// done in.
//
// MEASURED against llama.cpp b86d2f07, the only caller this library has today:
//   (32F,32F,32F,32F)  ggml_cuda_mul_mat_cublas_impl<F32> through hipblasGemmEx and both
//                      batched forms, and every hipblasSgemm* call of out-prod.cu
//   (16F,16F,16F,16F)  the same function for a f16 or quantized weight on this part, because
//                      prefer_f32_output is false for RDNA1 (ggml-cuda.cu:1506-1512)
//   (16F,16F,32F,32F)  conv2d.cu:414 and conv3d.cu:330
//   (16B,16B,32F,32F)  the bf16 path, where prefer_f32_output IS true on this part
//
// (16F,16F,16F,32F) is accepted as well. It is not reached by this caller, it is a documented
// hipBLAS combination, and it is the same work: this library accumulates in f32 for every
// combination, so a f32 compute type over f16 data asks for exactly what the kernel already
// does.
//
// What a 16F compute type gets here, said plainly: the accumulator is f32, not f16. That is
// more accurate than the letter of the type, never less, and it is what the hardware wants -
// gfx1013 has no f16 dot instruction, so a f16 accumulator would be converted twice per
// multiply and add for nothing. The result is rounded to f16 one time, on the store, when cType
// is 16F.
BC250_BLAS_HD hipblasStatus_t accept_types(const hipblasDatatype_t a_type,
                                           const hipblasDatatype_t b_type,
                                           const hipblasDatatype_t c_type,
                                           const hipblasDatatype_t compute_type, Combo *out) {
    const Elem a = elem_of(a_type);
    const Elem b = elem_of(b_type);
    const Elem c = elem_of(c_type);
    const Elem comp = elem_of(compute_type);
    if (a == ELEM_OTHER || b == ELEM_OTHER || c == ELEM_OTHER || comp == ELEM_OTHER) {
        return HIPBLAS_STATUS_NOT_SUPPORTED;
    }
    if (comp == ELEM_BF16) {
        return HIPBLAS_STATUS_NOT_SUPPORTED;   // a bf16 accumulator is not what this kernel does
    }
    if (a != b) {
        return HIPBLAS_STATUS_NOT_SUPPORTED;   // one kernel reads A and B with one type each
    }
    const bool half_compute = comp == ELEM_F16;
    bool ok = false;
    if (a == ELEM_F32) {
        ok = c == ELEM_F32 && comp == ELEM_F32;
    } else if (a == ELEM_F16) {
        ok = (c == ELEM_F16) || (c == ELEM_F32 && comp == ELEM_F32);
    } else {  // ELEM_BF16
        ok = c == ELEM_F32 && comp == ELEM_F32;
    }
    if (!ok) {
        return HIPBLAS_STATUS_NOT_SUPPORTED;
    }
    out->a = a;
    out->b = b;
    out->c = c;
    out->alpha_is_half = half_compute;
    return HIPBLAS_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// The shape of one GEMM, after the entry point has read the scalars.
// ---------------------------------------------------------------------------------------------
struct GemmShape {
    int op_a;      // hipblasOperation_t, HIPBLAS_OP_N or HIPBLAS_OP_T
    int op_b;
    int m;
    int n;
    int k;
    int lda;
    int ldb;
    int ldc;
    float alpha;
    float beta;
};

// Whether an operation code is one this library implements. HIPBLAS_OP_C is a conjugate
// transpose: over real matrices it equals the transpose, but accepting it would mean answering
// for complex data this library has no type for, so it is refused. llama.cpp passes N and T
// only (every call site of ggml/src/ggml-cuda).
BC250_BLAS_HD hipblasStatus_t accept_op(const int op) {
    if (op == HIPBLAS_OP_N || op == HIPBLAS_OP_T) {
        return HIPBLAS_STATUS_SUCCESS;
    }
    if (op == HIPBLAS_OP_C) {
        return HIPBLAS_STATUS_NOT_SUPPORTED;
    }
    return HIPBLAS_STATUS_INVALID_ENUM;
}

BC250_BLAS_HD int max_int(const int a, const int b) { return a > b ? a : b; }

// The argument check of one call. It is the hipBLAS rule set and nothing of ours:
//
//   m, n, k and batchCount are not negative;
//   lda is at least max(1, transA == N ? m : k);
//   ldb is at least max(1, transB == N ? k : n);
//   ldc is at least max(1, m);
//   alpha and beta are not null pointers (this library reads scalars from host memory, which is
//   cuBLAS's CUBLAS_POINTER_MODE_HOST, the mode ggml uses);
//   A, B and C are not null when the call has work to do.
//
// `has_work` is the caller's answer to m > 0 && n > 0 && batchCount > 0. A call with no work is
// a success that launches nothing - including k == 0, which is NOT no work: k == 0 means
// C := beta * C, and the kernel does that correctly with an empty k loop.
BC250_BLAS_HD hipblasStatus_t validate_shape(const GemmShape &g, const int batch_count,
                                             const bool scalars_present,
                                             const bool matrices_present, const bool has_work) {
    hipblasStatus_t status = accept_op(g.op_a);
    if (status != HIPBLAS_STATUS_SUCCESS) {
        return status;
    }
    status = accept_op(g.op_b);
    if (status != HIPBLAS_STATUS_SUCCESS) {
        return status;
    }
    if (g.m < 0 || g.n < 0 || g.k < 0 || batch_count < 0) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    if (!scalars_present) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    const int need_lda = max_int(1, g.op_a == HIPBLAS_OP_N ? g.m : g.k);
    const int need_ldb = max_int(1, g.op_b == HIPBLAS_OP_N ? g.k : g.n);
    const int need_ldc = max_int(1, g.m);
    if (g.lda < need_lda || g.ldb < need_ldb || g.ldc < need_ldc) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    if (has_work && !matrices_present) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    return HIPBLAS_STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------------
// Element access. Four overloads each, so that one kernel body serves three element types with
// no template specialisation in a header that three translation units include.
// ---------------------------------------------------------------------------------------------
BC250_BLAS_HD float to_f32(const float v) { return v; }
BC250_BLAS_HD float to_f32(const __half v) { return __half2float(v); }
BC250_BLAS_HD float to_f32(const __hip_bfloat16 v) { return __bfloat162float(v); }

BC250_BLAS_HD void store_f32(float *p, const float v) { *p = v; }
BC250_BLAS_HD void store_f32(__half *p, const float v) { *p = __float2half(v); }
BC250_BLAS_HD void store_f32(__hip_bfloat16 *p, const float v) { *p = __float2bfloat16(v); }

// The offset of A(i, l), i the row of C and l the reduction index.
BC250_BLAS_HD long long a_offset(const int op, const int i, const int l, const int lda) {
    return op == HIPBLAS_OP_N ? (long long)i + (long long)l * lda
                              : (long long)l + (long long)i * lda;
}

// The offset of B(l, j), l the reduction index and j the column of C.
BC250_BLAS_HD long long b_offset(const int op, const int l, const int j, const int ldb) {
    return op == HIPBLAS_OP_N ? (long long)l + (long long)j * ldb
                              : (long long)j + (long long)l * ldb;
}

BC250_BLAS_HD long long c_offset(const int i, const int j, const int ldc) {
    return (long long)i + (long long)j * ldc;
}

// ---------------------------------------------------------------------------------------------
// The tile. One workgroup of kTileM x kTileN work items computes one tile of C, and walks the
// reduction in steps of kTileK through two tiles in local memory.
//
// 16 x 16 x 16: 256 work items, eight wave32 waves, 2 KB of local memory, one output per work
// item. It is the correctness-first shape, and it is deliberately the simple one: this library
// exists so that ggml-hip links and runs, and every quantized matrix multiply of a model goes
// to ggml's own MMQ kernels with GGML_CUDA_FORCE_MMQ=ON. The next shape, when the lab says the
// GEMM is worth it, is a 2x2 micro-tile per work item over a 32 x 32 tile, which quarters the
// local-memory traffic per multiply; compute\hipblas\README.md names that as open work.
// ---------------------------------------------------------------------------------------------
enum { kTileM = 16, kTileN = 16, kTileK = 16 };

struct TileStore {
    float a[kTileK][kTileM];
    float b[kTileK][kTileN];
};

// Phase 1: every work item of the workgroup reads one element of A and one of B into the tile
// store. An index outside the matrix reads as zero, which is what makes an odd shape correct
// with no separate boundary kernel.
template <typename TA, typename TB>
BC250_BLAS_HD void tile_load(const GemmShape &g, const TA *A, const TB *B, const int tile_i,
                             const int tile_j, const int kt, const int tx, const int ty,
                             TileStore *sh) {
    const int ai = tile_i + tx;
    const int al = kt + ty;
    sh->a[ty][tx] = (ai < g.m && al < g.k) ? to_f32(A[a_offset(g.op_a, ai, al, g.lda)]) : 0.0f;

    const int bl = kt + tx;
    const int bj = tile_j + ty;
    sh->b[tx][ty] = (bl < g.k && bj < g.n) ? to_f32(B[b_offset(g.op_b, bl, bj, g.ldb)]) : 0.0f;
}

// Phase 2: every work item multiplies its row of the A tile by its column of the B tile.
BC250_BLAS_HD void tile_mac(const TileStore *sh, const int tx, const int ty, float *acc) {
    float sum = *acc;
    for (int l = 0; l < kTileK; ++l) {
        sum += sh->a[l][tx] * sh->b[l][ty];
    }
    *acc = sum;
}

// Phase 3: C(i, j) := alpha * acc + beta * C(i, j), and beta == 0 does not read C. That is not
// an optimisation: cuBLAS and hipBLAS both state that C is not read when beta is zero, and
// ggml relies on it - ggml_cuda_mul_mat_cublas_impl writes into a pool allocation that holds
// the bytes of an earlier tensor, so reading it would multiply a NaN by zero and keep the NaN.
template <typename TC>
BC250_BLAS_HD void tile_store(const GemmShape &g, TC *C, const int tile_i, const int tile_j,
                              const int tx, const int ty, const float acc) {
    const int i = tile_i + tx;
    const int j = tile_j + ty;
    if (i >= g.m || j >= g.n) {
        return;
    }
    const long long off = c_offset(i, j, g.ldc);
    float v = g.alpha * acc;
    if (g.beta != 0.0f) {
        v += g.beta * to_f32(C[off]);
    }
    store_f32(C + off, v);
}

// The grid one call needs. Stated here so that the host test checks the same arithmetic the
// entry point uses.
BC250_BLAS_HD int tiles_m(const int m) { return (m + kTileM - 1) / kTileM; }
BC250_BLAS_HD int tiles_n(const int n) { return (n + kTileN - 1) / kTileN; }

// ---------------------------------------------------------------------------------------------
// The reference. It is NOT the code above: it materialises the two operands with explicit
// transposition and then sums in the plain order, so that a mistake in the tile indexing cannot
// hide in both. The device test uses it as the expected answer on the hardware, and the host
// test uses it against the emulated tile phases.
// ---------------------------------------------------------------------------------------------
template <typename TA, typename TB, typename TC>
void reference_gemm(const GemmShape &g, const TA *A, const TB *B, TC *C) {
    for (int j = 0; j < g.n; ++j) {
        for (int i = 0; i < g.m; ++i) {
            double sum = 0.0;
            for (int l = 0; l < g.k; ++l) {
                // A(i,l) and B(l,j), written out again rather than through a_offset/b_offset.
                const long long ai =
                    g.op_a == HIPBLAS_OP_N ? (long long)i + (long long)l * g.lda
                                           : (long long)i * g.lda + (long long)l;
                const long long bi =
                    g.op_b == HIPBLAS_OP_N ? (long long)l + (long long)j * g.ldb
                                           : (long long)l * g.ldb + (long long)j;
                sum += (double)to_f32(A[ai]) * (double)to_f32(B[bi]);
            }
            const long long ci = (long long)i + (long long)j * g.ldc;
            double v = (double)g.alpha * sum;
            if (g.beta != 0.0f) {
                v += (double)g.beta * (double)to_f32(C[ci]);
            }
            store_f32(C + ci, (float)v);
        }
    }
}

// The tolerance of a comparison against reference_gemm, written once because both tests use it
// and a tolerance that drifts between two tests is a tolerance that means nothing.
//
//   scale     |alpha| * k * max|A| * max|B| + |beta| * max|C|, the largest value a partial sum
//             of this call can reach;
//   want      the reference value of the element;
//   half_out  the destination is f16, which adds one rounding of the result.
//
// The two terms: the kernel and the reference sum the same products in a different order, so
// the f32 error is bounded by the number of terms times the machine epsilon of the largest
// partial sum (1e-5 is that bound with room, for k up to a few thousand); a f16 destination
// rounds once more, which is 2^-10 relative, and 0.001 is that with room.
inline float compare_tolerance(const float scale, const float want, const bool half_out) {
    float tol = 1e-5f * scale + 1e-6f;
    if (half_out) {
        tol += 0.001f * (want < 0.0f ? -want : want) + 0.001f * scale;
    }
    return tol;
}

}  // namespace bc250blas

#endif  // BC250_GEMM_CORE_H
