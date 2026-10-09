// test_gemm_core.cpp - the host test of bc250hipblas: the argument rules, and the kernel's own
// tile phases run on the CPU against an independent reference.
//
// Milestone M16, route B, step 3. Why it is shaped this way: the mock backend of layer 2
// records a dispatch and executes no instruction (compute\hip\tests\host\hipmock_backend.h), so
// no kernel can be run against it on the development PC. This test therefore runs the SAME
// phase functions that the kernel calls (bc250blas::tile_load, tile_mac, tile_store of
// src\gemm_core.h), one phase for every work item of a workgroup before the next phase starts,
// which is what the two __syncthreads of the kernel promise. What it cannot test is the
// hardware: the real barrier, the local memory and the dispatch are the device test
// (tests\gemm_check.hip), which runs on unit A.
//
// Build and run: compute\hipblas\build-hipblas.ps1. It is compiled by clang for the host only
// (no -x hip), because the shared header needs _Float16, which MSVC does not have.
//
// Exit code: the number of failed checks.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "gemm_core.h"

using bc250blas::Combo;
using bc250blas::GemmShape;
using bc250blas::kTileK;
using bc250blas::kTileM;
using bc250blas::kTileN;
using bc250blas::TileStore;

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

#define CHECK_STATUS(call, expected)                                             \
    do {                                                                         \
        const hipblasStatus_t s_ = (call);                                       \
        g_checks++;                                                              \
        if (s_ != (expected)) {                                                  \
            g_failures++;                                                        \
            printf("FAIL %s:%d  %s returned %d, expected %d\n", __FILE__,        \
                   __LINE__, #call, (int)s_, (int)(expected));                   \
        }                                                                        \
    } while (0)

// ---------------------------------------------------------------------------------------------
// A deterministic generator, so that a failure is reproducible from the seed printed with it.
// ---------------------------------------------------------------------------------------------
namespace {

struct Rng {
    unsigned int state;

    explicit Rng(unsigned int seed) : state(seed != 0 ? seed : 0x1234567u) {}

    unsigned int next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    // In [-1, 1), with 1024 steps, so that a f16 holds the value exactly and a conversion is
    // not part of what this test measures.
    float unit() { return (float)((int)(next() % 2048u) - 1024) / 1024.0f; }
};

int need_lda(const GemmShape &g) {
    return g.op_a == HIPBLAS_OP_N ? (g.m > 0 ? g.m : 1) : (g.k > 0 ? g.k : 1);
}
int need_ldb(const GemmShape &g) {
    return g.op_b == HIPBLAS_OP_N ? (g.k > 0 ? g.k : 1) : (g.n > 0 ? g.n : 1);
}

// The number of elements one batch element of each matrix occupies, as a column-major matrix
// with the leading dimension the caller chose.
long long a_elements(const GemmShape &g) {
    return (long long)g.lda * (g.op_a == HIPBLAS_OP_N ? g.k : g.m);
}
long long b_elements(const GemmShape &g) {
    return (long long)g.ldb * (g.op_b == HIPBLAS_OP_N ? g.n : g.k);
}
long long c_elements(const GemmShape &g) { return (long long)g.ldc * g.n; }

// ---------------------------------------------------------------------------------------------
// The emulation of one workgroup grid. It is the body of gemm_strided / gemm_pointers of
// bc250hipblas.hip, with the barriers made explicit by the loop structure: every work item
// finishes a phase before any work item starts the next one.
// ---------------------------------------------------------------------------------------------
template <typename TA, typename TB, typename TC>
void emulate_one(const GemmShape &g, const TA *A, const TB *B, TC *C) {
    for (int bx = 0; bx < bc250blas::tiles_m(g.m); ++bx) {
        for (int by = 0; by < bc250blas::tiles_n(g.n); ++by) {
            const int tile_i = bx * kTileM;
            const int tile_j = by * kTileN;
            TileStore sh;
            float acc[kTileM][kTileN];
            memset(&sh, 0, sizeof(sh));
            memset(acc, 0, sizeof(acc));
            for (int kt = 0; kt < g.k; kt += kTileK) {
                for (int tx = 0; tx < kTileM; ++tx) {
                    for (int ty = 0; ty < kTileN; ++ty) {
                        bc250blas::tile_load(g, A, B, tile_i, tile_j, kt, tx, ty, &sh);
                    }
                }
                // __syncthreads()
                for (int tx = 0; tx < kTileM; ++tx) {
                    for (int ty = 0; ty < kTileN; ++ty) {
                        bc250blas::tile_mac(&sh, tx, ty, &acc[tx][ty]);
                    }
                }
                // __syncthreads()
            }
            for (int tx = 0; tx < kTileM; ++tx) {
                for (int ty = 0; ty < kTileN; ++ty) {
                    bc250blas::tile_store(g, C, tile_i, tile_j, tx, ty, acc[tx][ty]);
                }
            }
        }
    }
}

template <typename TA, typename TB, typename TC>
void emulate_strided(const GemmShape &g, const TA *A, const TB *B, TC *C, const long long sa,
                     const long long sb, const long long sc, const int batch) {
    for (int z = 0; z < batch; ++z) {
        emulate_one(g, A + (long long)z * sa, B + (long long)z * sb, C + (long long)z * sc);
    }
}

template <typename TA, typename TB, typename TC>
void emulate_pointers(const GemmShape &g, const TA *const *A, const TB *const *B, TC *const *C,
                      const int batch) {
    for (int z = 0; z < batch; ++z) {
        emulate_one(g, A[z], B[z], C[z]);
    }
}

// ---------------------------------------------------------------------------------------------
// One numeric case: the reference against the emulated kernel, and the padding of C untouched.
// ---------------------------------------------------------------------------------------------
template <typename TA, typename TB, typename TC>
struct Case {
    static void run(const char *label, const int op_a, const int op_b, const int m, const int n,
                    const int k, const int pad, const float alpha, const float beta,
                    const int batch, const bool pointer_mode, const unsigned int seed) {
        GemmShape g;
        g.op_a = op_a;
        g.op_b = op_b;
        g.m = m;
        g.n = n;
        g.k = k;
        g.alpha = alpha;
        g.beta = beta;
        g.lda = 0;
        g.ldb = 0;
        g.ldc = 0;
        g.lda = need_lda(g) + pad;
        g.ldb = need_ldb(g) + pad;
        g.ldc = (m > 0 ? m : 1) + pad;

        const long long ea = a_elements(g);
        const long long eb = b_elements(g);
        const long long ec = c_elements(g);
        // k == 0 makes A and B empty matrices. One element keeps data() addressable; nothing
        // reads it, because the k loop of both sides runs zero times.
        std::vector<TA> a((size_t)(ea * batch > 0 ? ea * batch : 1));
        std::vector<TB> b((size_t)(eb * batch > 0 ? eb * batch : 1));
        std::vector<TC> c0((size_t)(ec * batch > 0 ? ec * batch : 1));

        Rng rng(seed);
        float max_a = 0.0f;
        float max_b = 0.0f;
        float max_c = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) {
            const float v = rng.unit();
            bc250blas::store_f32(&a[i], v);
            if (fabsf(v) > max_a) max_a = fabsf(v);
        }
        for (size_t i = 0; i < b.size(); ++i) {
            const float v = rng.unit();
            bc250blas::store_f32(&b[i], v);
            if (fabsf(v) > max_b) max_b = fabsf(v);
        }
        for (size_t i = 0; i < c0.size(); ++i) {
            const float v = rng.unit();
            bc250blas::store_f32(&c0[i], v);
            if (fabsf(v) > max_c) max_c = fabsf(v);
        }

        std::vector<TC> ref = c0;
        std::vector<TC> got = c0;

        for (int z = 0; z < batch; ++z) {
            bc250blas::reference_gemm(g, &a[(size_t)(ea * z)], &b[(size_t)(eb * z)],
                                      &ref[(size_t)(ec * z)]);
        }
        if (pointer_mode) {
            std::vector<const TA *> pa((size_t)batch);
            std::vector<const TB *> pb((size_t)batch);
            std::vector<TC *> pc((size_t)batch);
            for (int z = 0; z < batch; ++z) {
                pa[(size_t)z] = &a[(size_t)(ea * z)];
                pb[(size_t)z] = &b[(size_t)(eb * z)];
                pc[(size_t)z] = &got[(size_t)(ec * z)];
            }
            emulate_pointers(g, pa.data(), pb.data(), pc.data(), batch);
        } else {
            emulate_strided(g, a.data(), b.data(), got.data(), ea, eb, ec, batch);
        }

        // The tolerance is bc250blas::compare_tolerance, which the device test uses as well.
        const float scale =
            fabsf(alpha) * (float)(k > 0 ? k : 1) * max_a * max_b + fabsf(beta) * max_c;
        const bool half_out = sizeof(TC) == 2;
        int mismatches = 0;
        int padding_touched = 0;
        float worst = 0.0f;
        for (int z = 0; z < batch; ++z) {
            for (int j = 0; j < g.n; ++j) {
                for (int i = 0; i < g.ldc; ++i) {
                    const size_t at = (size_t)(ec * z + (long long)j * g.ldc + i);
                    const float want = bc250blas::to_f32(ref[at]);
                    const float have = bc250blas::to_f32(got[at]);
                    if (i >= g.m) {
                        // Outside the matrix. Both sides must have left the bytes alone.
                        if (memcmp(&got[at], &c0[at], sizeof(TC)) != 0 ||
                            memcmp(&ref[at], &c0[at], sizeof(TC)) != 0) {
                            padding_touched++;
                        }
                        continue;
                    }
                    const float tol = bc250blas::compare_tolerance(scale, want, half_out);
                    const float diff = fabsf(want - have);
                    if (diff > worst) worst = diff;
                    if (diff > tol) {
                        if (mismatches < 3) {
                            printf("     %s: C[%d,%d,batch %d] is %g, reference %g (tol %g)\n",
                                   label, i, j, z, (double)have, (double)want, (double)tol);
                        }
                        mismatches++;
                    }
                }
            }
        }
        g_checks++;
        if (mismatches != 0) {
            g_failures++;
            printf("FAIL %s: %d of %lld elements differ from the reference (seed %u)\n", label,
                   mismatches, (long long)g.m * g.n * batch, seed);
        }
        g_checks++;
        if (padding_touched != 0) {
            g_failures++;
            printf("FAIL %s: %d elements outside the matrix were written (ldc %d, m %d)\n", label,
                   padding_touched, g.ldc, g.m);
        }

        // The negative control of this case: one element of A moved by one, which must take at
        // least one element of C outside the tolerance. Without it a tolerance that is too wide
        // would make every case above pass for the wrong reason.
        if (m > 0 && n > 0 && k > 0) {
            std::vector<TA> a2 = a;
            bc250blas::store_f32(&a2[0], bc250blas::to_f32(a2[0]) + 1.0f);
            std::vector<TC> perturbed = c0;
            emulate_strided(g, a2.data(), b.data(), perturbed.data(), ea, eb, ec, batch);
            int noticed = 0;
            for (int j = 0; j < g.n; ++j) {
                for (int i = 0; i < g.m; ++i) {
                    const size_t at = (size_t)((long long)j * g.ldc + i);
                    const float want = bc250blas::to_f32(ref[at]);
                    const float have = bc250blas::to_f32(perturbed[at]);
                    const float tol = bc250blas::compare_tolerance(scale, want, half_out);
                    if (fabsf(want - have) > tol) {
                        noticed++;
                    }
                }
            }
            g_checks++;
            if (noticed == 0) {
                g_failures++;
                printf("FAIL %s: the negative control changed nothing the comparison noticed\n",
                       label);
            }
        }
    }
};

// ---------------------------------------------------------------------------------------------
// The type table, which decides what is computed and what is refused.
// ---------------------------------------------------------------------------------------------
void test_type_table() {
    struct Row {
        hipblasDatatype_t a;
        hipblasDatatype_t b;
        hipblasDatatype_t c;
        hipblasDatatype_t compute;
        hipblasStatus_t expected;
        bool alpha_is_half;
        const char *why;
    };
    static const Row rows[] = {
        // The four combinations llama.cpp b86d2f07 passes on this part.
        {HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_STATUS_SUCCESS,
         false, "the f32 path of ggml_cuda_mul_mat_cublas_impl<F32>"},
        {HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_STATUS_SUCCESS,
         true, "the f16 path on RDNA1, where prefer_f32_output is false"},
        {HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_STATUS_SUCCESS,
         false, "conv2d.cu:414 and conv3d.cu:330"},
        {HIPBLAS_R_16B, HIPBLAS_R_16B, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_STATUS_SUCCESS,
         false, "the bf16 path, which does prefer a f32 output on this part"},
        // Accepted as well, and not reached by this caller.
        {HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_R_32F, HIPBLAS_STATUS_SUCCESS,
         false, "f16 data with a f32 compute type is what the kernel already does"},
        // Refused, each for its own reason.
        {HIPBLAS_R_64F, HIPBLAS_R_64F, HIPBLAS_R_64F, HIPBLAS_R_64F, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "no f64 on this part"},
        {HIPBLAS_R_8I, HIPBLAS_R_8I, HIPBLAS_R_32I, HIPBLAS_R_32I, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "no integer GEMM"},
        {HIPBLAS_C_32F, HIPBLAS_C_32F, HIPBLAS_C_32F, HIPBLAS_C_32F, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "no complex GEMM"},
        {HIPBLAS_R_16F, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "A and B must carry one type"},
        {HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_16F, HIPBLAS_R_32F, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "a f16 destination for f32 data is not implemented"},
        {HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_16F, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "a f16 compute type over f32 data would be a lie about the accumulator"},
        {HIPBLAS_R_16B, HIPBLAS_R_16B, HIPBLAS_R_16B, HIPBLAS_R_32F, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "a bf16 destination is not implemented"},
        {HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_R_16F, HIPBLAS_R_16B, HIPBLAS_STATUS_NOT_SUPPORTED,
         false, "a bf16 accumulator is not what this kernel does"},
        {(hipblasDatatype_t)9999, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_32F,
         HIPBLAS_STATUS_NOT_SUPPORTED, false,
         "an unknown type code is refused and never read as f32"},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const Row &r = rows[i];
        Combo combo;
        memset(&combo, 0, sizeof(combo));
        const hipblasStatus_t got = bc250blas::accept_types(r.a, r.b, r.c, r.compute, &combo);
        g_checks++;
        if (got != r.expected) {
            g_failures++;
            printf("FAIL type row %u (%s): status %d, expected %d\n", i, r.why, (int)got,
                   (int)r.expected);
        }
        if (r.expected == HIPBLAS_STATUS_SUCCESS) {
            g_checks++;
            if (combo.alpha_is_half != r.alpha_is_half) {
                g_failures++;
                printf("FAIL type row %u (%s): alpha_is_half %d, expected %d\n", i, r.why,
                       (int)combo.alpha_is_half, (int)r.alpha_is_half);
            }
        }
    }
}

void test_operations() {
    CHECK_STATUS(bc250blas::accept_op(HIPBLAS_OP_N), HIPBLAS_STATUS_SUCCESS);
    CHECK_STATUS(bc250blas::accept_op(HIPBLAS_OP_T), HIPBLAS_STATUS_SUCCESS);
    // A conjugate transpose equals a transpose over real data, but accepting it would answer
    // for complex data this library has no type for.
    CHECK_STATUS(bc250blas::accept_op(HIPBLAS_OP_C), HIPBLAS_STATUS_NOT_SUPPORTED);
    CHECK_STATUS(bc250blas::accept_op(0), HIPBLAS_STATUS_INVALID_ENUM);
    CHECK_STATUS(bc250blas::accept_op(999), HIPBLAS_STATUS_INVALID_ENUM);
}

GemmShape good_shape() {
    GemmShape g;
    g.op_a = HIPBLAS_OP_T;
    g.op_b = HIPBLAS_OP_N;
    g.m = 7;
    g.n = 5;
    g.k = 3;
    g.lda = 3;  // transA == T, so lda >= k
    g.ldb = 3;  // transB == N, so ldb >= k
    g.ldc = 7;  // ldc >= m
    g.alpha = 1.0f;
    g.beta = 0.0f;
    return g;
}

void test_argument_rules() {
    const GemmShape ok = good_shape();
    CHECK_STATUS(bc250blas::validate_shape(ok, 1, true, true, true), HIPBLAS_STATUS_SUCCESS);

    // The three leading dimensions, each one short by one.
    GemmShape g = ok;
    g.lda = 2;
    CHECK_STATUS(bc250blas::validate_shape(g, 1, true, true, true),
                 HIPBLAS_STATUS_INVALID_VALUE);
    g = ok;
    g.ldb = 2;
    CHECK_STATUS(bc250blas::validate_shape(g, 1, true, true, true),
                 HIPBLAS_STATUS_INVALID_VALUE);
    g = ok;
    g.ldc = 6;
    CHECK_STATUS(bc250blas::validate_shape(g, 1, true, true, true),
                 HIPBLAS_STATUS_INVALID_VALUE);

    // The same shape with the operations the other way round needs other leading dimensions:
    // lda >= m and ldb >= n instead of k and k.
    g = ok;
    g.op_a = HIPBLAS_OP_N;
    g.op_b = HIPBLAS_OP_T;
    CHECK_STATUS(bc250blas::validate_shape(g, 1, true, true, true),
                 HIPBLAS_STATUS_INVALID_VALUE);
    g.lda = 7;
    g.ldb = 5;
    CHECK_STATUS(bc250blas::validate_shape(g, 1, true, true, true), HIPBLAS_STATUS_SUCCESS);

    // Negative sizes and a negative batch count.
    for (int which = 0; which < 4; ++which) {
        g = ok;
        if (which == 0) g.m = -1;
        if (which == 1) g.n = -1;
        if (which == 2) g.k = -1;
        const int batch = which == 3 ? -1 : 1;
        CHECK_STATUS(bc250blas::validate_shape(g, batch, true, true, true),
                     HIPBLAS_STATUS_INVALID_VALUE);
    }

    // A null alpha or beta, and a null matrix when there is work to do.
    CHECK_STATUS(bc250blas::validate_shape(ok, 1, false, true, true),
                 HIPBLAS_STATUS_INVALID_VALUE);
    CHECK_STATUS(bc250blas::validate_shape(ok, 1, true, false, true),
                 HIPBLAS_STATUS_INVALID_VALUE);
    // A null matrix with no work to do is not an error: hipBLAS answers success and launches
    // nothing. ggml reaches this with an empty tensor.
    CHECK_STATUS(bc250blas::validate_shape(ok, 1, true, false, false), HIPBLAS_STATUS_SUCCESS);

    // An unknown operation beats every other check, because the shape rules depend on it.
    g = ok;
    g.op_a = 77;
    CHECK_STATUS(bc250blas::validate_shape(g, 1, true, true, true), HIPBLAS_STATUS_INVALID_ENUM);

    // k == 0 is a legal call that scales C, so lda and ldb of 1 are enough and the call is not
    // refused. The numeric cases below check that it really scales.
    g = ok;
    g.k = 0;
    g.lda = 1;
    g.ldb = 1;
    CHECK_STATUS(bc250blas::validate_shape(g, 1, true, true, true), HIPBLAS_STATUS_SUCCESS);
}

void test_index_math() {
    // A(i,l) and B(l,j) against the two sentences of the column-major contract, written here a
    // third time so that a change of gemm_core.h has to be meant.
    const int lda = 11;
    const int ldb = 9;
    CHECK(bc250blas::a_offset(HIPBLAS_OP_N, 3, 4, lda) == 3 + 4 * 11);
    CHECK(bc250blas::a_offset(HIPBLAS_OP_T, 3, 4, lda) == 4 + 3 * 11);
    CHECK(bc250blas::b_offset(HIPBLAS_OP_N, 2, 5, ldb) == 2 + 5 * 9);
    CHECK(bc250blas::b_offset(HIPBLAS_OP_T, 2, 5, ldb) == 5 + 2 * 9);
    CHECK(bc250blas::c_offset(6, 7, 13) == 6 + 7 * 13);

    // The grid covers every row and column exactly once, and never fewer.
    CHECK(bc250blas::tiles_m(1) == 1);
    CHECK(bc250blas::tiles_m(16) == 1);
    CHECK(bc250blas::tiles_m(17) == 2);
    CHECK(bc250blas::tiles_n(0) == 0);
    CHECK(bc250blas::tiles_n(33) == 3);
    for (int m = 1; m <= 80; ++m) {
        CHECK(bc250blas::tiles_m(m) * kTileM >= m);
        CHECK((bc250blas::tiles_m(m) - 1) * kTileM < m);
    }
}

// ---------------------------------------------------------------------------------------------
// The numeric cases. Odd sizes on purpose: 1, 7, 17 and 33 are the shapes where a tile is
// partly outside the matrix, which is where a boundary mistake lives.
// ---------------------------------------------------------------------------------------------
struct Shape {
    int m;
    int n;
    int k;
    int pad;
};

void test_numeric() {
    static const Shape shapes[] = {
        {1, 1, 1, 0},     {1, 1, 17, 3},   {16, 16, 16, 0}, {17, 33, 7, 0},
        {7, 5, 129, 2},   {64, 48, 32, 5}, {33, 1, 16, 1},  {1, 64, 48, 0},
    };
    static const int ops[4][2] = {{HIPBLAS_OP_N, HIPBLAS_OP_N},
                                  {HIPBLAS_OP_N, HIPBLAS_OP_T},
                                  {HIPBLAS_OP_T, HIPBLAS_OP_N},
                                  {HIPBLAS_OP_T, HIPBLAS_OP_T}};
    unsigned int seed = 0x51ab3c7du;
    char label[160];
    for (unsigned s = 0; s < sizeof(shapes) / sizeof(shapes[0]); ++s) {
        const Shape &sh = shapes[s];
        for (int o = 0; o < 4; ++o) {
            for (int b = 0; b < 2; ++b) {
                const float beta = b == 0 ? 0.0f : 1.0f;
                const float alpha = b == 0 ? 1.0f : 0.5f;
                seed = seed * 1664525u + 1013904223u;
                snprintf(label, sizeof(label), "f32 %dx%dx%d op %c%c beta %g", sh.m, sh.n, sh.k,
                         ops[o][0] == HIPBLAS_OP_N ? 'N' : 'T',
                         ops[o][1] == HIPBLAS_OP_N ? 'N' : 'T', (double)beta);
                Case<float, float, float>::run(label, ops[o][0], ops[o][1], sh.m, sh.n, sh.k,
                                               sh.pad, alpha, beta, 1, false, seed);
            }
        }
    }

    // The three element combinations beside f32, on the shapes where they matter.
    static const Shape typed[] = {{17, 33, 7, 1}, {16, 16, 64, 0}, {5, 7, 129, 2}};
    for (unsigned s = 0; s < sizeof(typed) / sizeof(typed[0]); ++s) {
        const Shape &sh = typed[s];
        for (int o = 0; o < 4; ++o) {
            seed = seed * 1664525u + 1013904223u;
            snprintf(label, sizeof(label), "f16->f16 %dx%dx%d op %d", sh.m, sh.n, sh.k, o);
            Case<__half, __half, __half>::run(label, ops[o][0], ops[o][1], sh.m, sh.n, sh.k,
                                              sh.pad, 1.0f, 0.0f, 1, false, seed);
            seed = seed * 1664525u + 1013904223u;
            snprintf(label, sizeof(label), "f16->f32 %dx%dx%d op %d beta 1", sh.m, sh.n, sh.k, o);
            Case<__half, __half, float>::run(label, ops[o][0], ops[o][1], sh.m, sh.n, sh.k,
                                             sh.pad, 0.5f, 1.0f, 1, false, seed);
            seed = seed * 1664525u + 1013904223u;
            snprintf(label, sizeof(label), "bf16->f32 %dx%dx%d op %d", sh.m, sh.n, sh.k, o);
            Case<__hip_bfloat16, __hip_bfloat16, float>::run(label, ops[o][0], ops[o][1], sh.m,
                                                             sh.n, sh.k, sh.pad, 1.0f, 0.0f, 1,
                                                             false, seed);
        }
    }

    // The batched forms, strided and pointer array, with a batch count that is not a power of
    // two and a destination that has padding between its columns.
    static const int batches[] = {1, 3, 5};
    for (unsigned i = 0; i < sizeof(batches) / sizeof(batches[0]); ++i) {
        const int batch = batches[i];
        for (int mode = 0; mode < 2; ++mode) {
            seed = seed * 1664525u + 1013904223u;
            snprintf(label, sizeof(label), "f32 batch %d %s", batch,
                     mode == 0 ? "strided" : "pointers");
            Case<float, float, float>::run(label, HIPBLAS_OP_T, HIPBLAS_OP_N, 17, 9, 33, 2, 0.75f,
                                           0.25f, batch, mode == 1, seed);
            seed = seed * 1664525u + 1013904223u;
            snprintf(label, sizeof(label), "f16 batch %d %s", batch,
                     mode == 0 ? "strided" : "pointers");
            Case<__half, __half, __half>::run(label, HIPBLAS_OP_T, HIPBLAS_OP_N, 16, 7, 16, 0,
                                              1.0f, 0.0f, batch, mode == 1, seed);
        }
    }

    // k == 0: C := beta * C, and with beta == 0 that is a zero fill. hipBLAS states it and
    // ggml reaches it with an empty reduction.
    seed = seed * 1664525u + 1013904223u;
    Case<float, float, float>::run("f32 k=0 beta 1", HIPBLAS_OP_N, HIPBLAS_OP_N, 17, 5, 0, 1,
                                   1.0f, 1.0f, 1, false, seed);
    seed = seed * 1664525u + 1013904223u;
    Case<float, float, float>::run("f32 k=0 beta 0", HIPBLAS_OP_N, HIPBLAS_OP_N, 17, 5, 0, 1,
                                   1.0f, 0.0f, 1, false, seed);
}

// beta == 0 must not read C. A destination full of NaN is the test that says so: a read would
// make every result a NaN, because NaN * 0 is NaN and not zero. ggml depends on this, because
// its pool allocation holds the bytes of an earlier tensor.
void test_beta_zero_does_not_read_c() {
    GemmShape g;
    g.op_a = HIPBLAS_OP_N;
    g.op_b = HIPBLAS_OP_N;
    g.m = 17;
    g.n = 9;
    g.k = 5;
    g.lda = 17;
    g.ldb = 5;
    g.ldc = 20;
    g.alpha = 1.0f;
    g.beta = 0.0f;
    std::vector<float> a((size_t)(g.lda * g.k), 1.0f);
    std::vector<float> b((size_t)(g.ldb * g.n), 2.0f);
    std::vector<float> c((size_t)(g.ldc * g.n));
    // A quiet NaN from its bit pattern, so that no compiler option about fast mathematics can
    // turn the constant into something else.
    union {
        unsigned int u;
        float f;
    } quiet_nan;
    quiet_nan.u = 0x7fc00000u;
    for (size_t i = 0; i < c.size(); ++i) {
        c[i] = quiet_nan.f;
    }
    emulate_one(g, a.data(), b.data(), c.data());
    int nans = 0;
    for (int j = 0; j < g.n; ++j) {
        for (int i = 0; i < g.m; ++i) {
            const float v = c[(size_t)(j * g.ldc + i)];
            if (v != 10.0f) {
                nans++;
            }
        }
    }
    g_checks++;
    if (nans != 0) {
        g_failures++;
        printf("FAIL beta == 0 read the destination: %d of %d elements are not 10\n", nans,
               g.m * g.n);
    }
}

}  // namespace

int main(void) {
    printf("test_gemm_core: tile %dx%dx%d\n", (int)kTileM, (int)kTileN, (int)kTileK);
    test_type_table();
    test_operations();
    test_argument_rules();
    test_index_math();
    test_numeric();
    test_beta_zero_does_not_read_c();
    printf("test_gemm_core: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}
