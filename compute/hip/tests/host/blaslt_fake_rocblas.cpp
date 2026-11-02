// blaslt_fake_rocblas.cpp - a processor rocblas.dll for test_blaslt_front.
//
// rocblas_gemm_ex computes D = alpha op(A) op(B) + beta C on host memory, column major, as
// rocblas-functions.h (rocm-libraries db2d71e0) defines it: op(A) is m x k, op(B) k x n, element
// (i, j) of a matrix X with leading dimension ldx is X[i + j * ldx]. It accepts only the types of
// the served case (A and B rocblas_datatype_i8_r, C, D and compute rocblas_datatype_i32_r), the
// standard algorithm, flags 0 and host pointer mode; anything else is rocblas_status_invalid_value.
// fake_rocblas_last reads the arguments of the last gemm_ex and the call counts.
// fake_rocblas_set_callback names a function that gemm_ex calls once, before it computes, and keeps
// its return value: the test checks with it that a call from inside rocBLAS back into the front
// only forwards.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "blaslt_abi.h"

struct FakeGemm {
    long creates, destroys, gemms, streams_set;
    int pointer_mode;
    int ta, tb, m, n, k, lda, ldb, ldc, ldd;
    int a_type, b_type, c_type, d_type, compute_type, algo;
    uint32_t flags;
    void* handle;
    void* stream;
    int callback_result;
};

namespace {

struct Handle {
    hipStream_t stream = nullptr;
    int pointer_mode = -1;
};

FakeGemm g;
int (*g_callback)(void*) = nullptr;
void* g_callback_context = nullptr;

int64_t element_i8(const void* base, int ld, int row, int col) {
    return static_cast<const int8_t*>(base)[static_cast<int64_t>(row) + static_cast<int64_t>(col) * ld];
}

}  // namespace

extern "C" {

void fake_rocblas_last(FakeGemm* out) { *out = g; }

void fake_rocblas_set_callback(int (*callback)(void*), void* context) {
    g_callback = callback;
    g_callback_context = context;
}

rocblas_status fake_rocblas_create_handle(rocblas_handle* handle) {
    *handle = reinterpret_cast<rocblas_handle>(new Handle());
    ++g.creates;
    return rocblas_status_success;
}

rocblas_status fake_rocblas_destroy_handle(rocblas_handle handle) {
    delete reinterpret_cast<Handle*>(handle);
    ++g.destroys;
    return rocblas_status_success;
}

rocblas_status fake_rocblas_set_stream(rocblas_handle handle, hipStream_t stream) {
    reinterpret_cast<Handle*>(handle)->stream = stream;
    ++g.streams_set;
    return rocblas_status_success;
}

rocblas_status fake_rocblas_set_pointer_mode(rocblas_handle handle, rocblas_pointer_mode mode) {
    reinterpret_cast<Handle*>(handle)->pointer_mode = static_cast<int>(mode);
    return rocblas_status_success;
}

rocblas_status fake_rocblas_gemm_ex(rocblas_handle handle, rocblas_operation ta, rocblas_operation tb, rocblas_int m,
                                    rocblas_int n, rocblas_int k, const void* alpha, const void* a,
                                    rocblas_datatype a_type, rocblas_int lda, const void* b, rocblas_datatype b_type,
                                    rocblas_int ldb, const void* beta, const void* c, rocblas_datatype c_type,
                                    rocblas_int ldc, void* d, rocblas_datatype d_type, rocblas_int ldd,
                                    rocblas_datatype compute_type, rocblas_gemm_algo algo, int32_t, uint32_t flags) {
    Handle* h = reinterpret_cast<Handle*>(handle);
    ++g.gemms;
    g.handle = handle;
    g.stream = h->stream;
    g.pointer_mode = h->pointer_mode;
    g.ta = ta; g.tb = tb; g.m = m; g.n = n; g.k = k;
    g.lda = lda; g.ldb = ldb; g.ldc = ldc; g.ldd = ldd;
    g.a_type = a_type; g.b_type = b_type; g.c_type = c_type; g.d_type = d_type;
    g.compute_type = compute_type; g.algo = algo; g.flags = flags;
    if (g_callback != nullptr) {
        g.callback_result = g_callback(g_callback_context);
        g_callback = nullptr;
    }
    if (a_type != rocblas_datatype_i8_r || b_type != rocblas_datatype_i8_r || c_type != rocblas_datatype_i32_r ||
        d_type != rocblas_datatype_i32_r || compute_type != rocblas_datatype_i32_r || algo != rocblas_gemm_algo_standard ||
        flags != 0 || h->pointer_mode != 0 || m < 1 || n < 1 || k < 1) {
        return static_cast<rocblas_status>(11);
    }
    const bool at = ta == rocblas_operation_transpose;
    const bool bt = tb == rocblas_operation_transpose;
    if (lda < (at ? k : m) || ldb < (bt ? n : k) || ldc < m || ldd < m) {
        return static_cast<rocblas_status>(4);
    }
    const int64_t al = *static_cast<const int32_t*>(alpha);
    const int64_t be = *static_cast<const int32_t*>(beta);
    const int32_t* cc = static_cast<const int32_t*>(c);
    int32_t* dd = static_cast<int32_t*>(d);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < m; ++i) {
            int64_t sum = 0;
            for (int l = 0; l < k; ++l) {
                const int64_t x = at ? element_i8(a, lda, l, i) : element_i8(a, lda, i, l);
                const int64_t y = bt ? element_i8(b, ldb, j, l) : element_i8(b, ldb, l, j);
                sum += x * y;
            }
            const int64_t cv = be != 0 ? cc[static_cast<int64_t>(i) + static_cast<int64_t>(j) * ldc] : 0;
            dd[static_cast<int64_t>(i) + static_cast<int64_t>(j) * ldd] = static_cast<int32_t>(al * sum + be * cv);
        }
    }
    return rocblas_status_success;
}

}  // extern "C"
