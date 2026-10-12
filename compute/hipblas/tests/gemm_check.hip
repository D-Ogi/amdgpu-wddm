// gemm_check.hip - the program that checks bc250hipblas through its own interface.
//
// Milestone M16, route B, step 3. It is one program with two jobs, because the two machines can
// check different things:
//
//   on the development PC, against the mock build of amdhip64.dll, it checks the argument
//   handling and every refusal, and that a refused call dispatches nothing. The mock records a
//   dispatch and executes no instruction, so it runs with --no-numeric and the numbers are not
//   compared;
//
//   on unit A, against the product runtime, it runs with the numbers: random shapes with odd
//   sizes, both transpositions, several batch counts, beta 0 and 1, f16 and bf16 inputs, each
//   compared with bc250blas::reference_gemm on the host, and a negative control that moves one
//   element of A and must be noticed.
//
// Build: compute\hipblas\build-hipblas.ps1. Lab arm: scratch\m16-hip\lab\step3-README.md part
// 3B, arm (a).
//
//   gemm_check.exe [--no-numeric] [--wait-total <ms>] [--quiet]
//
// Exit code: the number of failed checks. It prints the number of dispatches it expects the
// runtime to have built, which is what the build script compares with the mock's record.

#include "gemm_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

using bc250blas::GemmShape;

static int g_checks;
static int g_failures;
static int g_expected_dispatches;
static int g_numeric = 1;
static int g_quiet = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                        \
    } while (0)

static void check_status(const char *what, const hipblasStatus_t got,
                         const hipblasStatus_t expected) {
    g_checks++;
    if (got != expected) {
        g_failures++;
        printf("FAIL %s: %s, expected %s\n", what, hipblasStatusToString(got),
               hipblasStatusToString(expected));
    }
}

static void check_hip(const char *what, const hipError_t err) {
    g_checks++;
    if (err != hipSuccess) {
        g_failures++;
        printf("FAIL %s: %s (%s)\n", what, hipGetErrorName(err), hipGetErrorString(err));
    }
}

namespace {

struct Rng {
    unsigned int state;
    explicit Rng(unsigned int seed) : state(seed != 0 ? seed : 0xa5a5a5a5u) {}
    unsigned int next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    // In [-1, 1) in steps of 1/1024, so that a f16 holds every input exactly and the test
    // measures the multiply and not a rounding of the operands.
    float unit() { return (float)((int)(next() % 2048u) - 1024) / 1024.0f; }
};

enum CallKind {
    CALL_SGEMM = 0,
    CALL_SGEMM_BATCHED,
    CALL_SGEMM_STRIDED,
    CALL_GEMM_EX,
    CALL_GEMM_BATCHED_EX,
    CALL_GEMM_STRIDED_EX
};

const char *kind_name(const CallKind kind) {
    switch (kind) {
        case CALL_SGEMM: return "hipblasSgemm";
        case CALL_SGEMM_BATCHED: return "hipblasSgemmBatched";
        case CALL_SGEMM_STRIDED: return "hipblasSgemmStridedBatched";
        case CALL_GEMM_EX: return "hipblasGemmEx";
        case CALL_GEMM_BATCHED_EX: return "hipblasGemmBatchedEx";
        default: return "hipblasGemmStridedBatchedEx";
    }
}

bool kind_is_pointers(const CallKind kind) {
    return kind == CALL_SGEMM_BATCHED || kind == CALL_GEMM_BATCHED_EX;
}

int need_lda(const GemmShape &g) {
    return g.op_a == HIPBLAS_OP_N ? (g.m > 0 ? g.m : 1) : (g.k > 0 ? g.k : 1);
}
int need_ldb(const GemmShape &g) {
    return g.op_b == HIPBLAS_OP_N ? (g.k > 0 ? g.k : 1) : (g.n > 0 ? g.n : 1);
}

// One case: the same shape through one entry point, on the device, against the reference.
template <typename TA, typename TB, typename TC>
struct Case {
    static void run(hipblasHandle_t handle, const CallKind kind, const hipblasDatatype_t dt_ab,
                    const hipblasDatatype_t dt_c, const hipblasDatatype_t dt_compute,
                    const int op_a, const int op_b, const int m, const int n, const int k,
                    const int pad, const float alpha, const float beta, const int batch,
                    const unsigned int seed, const bool perturb) {
        GemmShape g;
        g.op_a = op_a;
        g.op_b = op_b;
        g.m = m;
        g.n = n;
        g.k = k;
        g.alpha = alpha;
        g.beta = beta;
        g.lda = need_lda(g) + pad;
        g.ldb = need_ldb(g) + pad;
        g.ldc = (m > 0 ? m : 1) + pad;

        const long long ea = (long long)g.lda * (op_a == HIPBLAS_OP_N ? k : m);
        const long long eb = (long long)g.ldb * (op_b == HIPBLAS_OP_N ? n : k);
        const long long ec = (long long)g.ldc * n;

        char label[192];
        snprintf(label, sizeof(label), "%s %dx%dx%d op %c%c beta %g batch %d%s",
                 kind_name(kind), m, n, k, op_a == HIPBLAS_OP_N ? 'N' : 'T',
                 op_b == HIPBLAS_OP_N ? 'N' : 'T', (double)beta, batch,
                 perturb ? " [negative control]" : "");

        std::vector<TA> ha((size_t)(ea * batch > 0 ? ea * batch : 1));
        std::vector<TB> hb((size_t)(eb * batch > 0 ? eb * batch : 1));
        std::vector<TC> hc((size_t)(ec * batch > 0 ? ec * batch : 1));
        Rng rng(seed);
        float max_a = 0.0f;
        float max_b = 0.0f;
        float max_c = 0.0f;
        for (size_t i = 0; i < ha.size(); ++i) {
            const float v = rng.unit();
            bc250blas::store_f32(&ha[i], v);
            if (fabsf(v) > max_a) max_a = fabsf(v);
        }
        for (size_t i = 0; i < hb.size(); ++i) {
            const float v = rng.unit();
            bc250blas::store_f32(&hb[i], v);
            if (fabsf(v) > max_b) max_b = fabsf(v);
        }
        for (size_t i = 0; i < hc.size(); ++i) {
            const float v = rng.unit();
            bc250blas::store_f32(&hc[i], v);
            if (fabsf(v) > max_c) max_c = fabsf(v);
        }

        std::vector<TC> reference = hc;
        for (int z = 0; z < batch; ++z) {
            bc250blas::reference_gemm(g, &ha[(size_t)(ea * z)], &hb[(size_t)(eb * z)],
                                      &reference[(size_t)(ec * z)]);
        }
        // The negative control moves one element of A on the device only, after the reference
        // has been computed from the unchanged data. A comparison that still passes would mean
        // the comparison proves nothing.
        if (perturb) {
            bc250blas::store_f32(&ha[0], bc250blas::to_f32(ha[0]) + 1.0f);
        }

        void *da = nullptr;
        void *db = nullptr;
        void *dc = nullptr;
        check_hip("hipMalloc A", hipMalloc(&da, ha.size() * sizeof(TA)));
        check_hip("hipMalloc B", hipMalloc(&db, hb.size() * sizeof(TB)));
        check_hip("hipMalloc C", hipMalloc(&dc, hc.size() * sizeof(TC)));
        if (da == nullptr || db == nullptr || dc == nullptr) {
            printf("FAIL %s: no device memory\n", label);
            g_failures++;
            return;
        }
        check_hip("hipMemcpy A", hipMemcpy(da, ha.data(), ha.size() * sizeof(TA),
                                           hipMemcpyHostToDevice));
        check_hip("hipMemcpy B", hipMemcpy(db, hb.data(), hb.size() * sizeof(TB),
                                           hipMemcpyHostToDevice));
        check_hip("hipMemcpy C", hipMemcpy(dc, hc.data(), hc.size() * sizeof(TC),
                                           hipMemcpyHostToDevice));

        // The batched forms take arrays of pointers in DEVICE memory: ggml fills them with a
        // kernel of its own, so they cannot be read on the host and this test must put them
        // there the same way.
        void **dpa = nullptr;
        void **dpb = nullptr;
        void **dpc = nullptr;
        if (kind_is_pointers(kind)) {
            std::vector<void *> pa((size_t)batch);
            std::vector<void *> pb((size_t)batch);
            std::vector<void *> pc((size_t)batch);
            for (int z = 0; z < batch; ++z) {
                pa[(size_t)z] = (char *)da + (size_t)(ea * z) * sizeof(TA);
                pb[(size_t)z] = (char *)db + (size_t)(eb * z) * sizeof(TB);
                pc[(size_t)z] = (char *)dc + (size_t)(ec * z) * sizeof(TC);
            }
            check_hip("hipMalloc A pointers",
                      hipMalloc((void **)&dpa, pa.size() * sizeof(void *)));
            check_hip("hipMalloc B pointers",
                      hipMalloc((void **)&dpb, pb.size() * sizeof(void *)));
            check_hip("hipMalloc C pointers",
                      hipMalloc((void **)&dpc, pc.size() * sizeof(void *)));
            if (dpa == nullptr || dpb == nullptr || dpc == nullptr) {
                printf("FAIL %s: no device memory for the pointer arrays\n", label);
                g_failures++;
                return;
            }
            check_hip("hipMemcpy A pointers", hipMemcpy(dpa, pa.data(),
                                                        pa.size() * sizeof(void *),
                                                        hipMemcpyHostToDevice));
            check_hip("hipMemcpy B pointers", hipMemcpy(dpb, pb.data(),
                                                        pb.size() * sizeof(void *),
                                                        hipMemcpyHostToDevice));
            check_hip("hipMemcpy C pointers", hipMemcpy(dpc, pc.data(),
                                                        pc.size() * sizeof(void *),
                                                        hipMemcpyHostToDevice));
        }

        // alpha and beta are read in the compute type, as hipBLAS states.
        const bool half_scalars = dt_compute == HIPBLAS_R_16F;
        const __half h_alpha = __float2half(alpha);
        const __half h_beta = __float2half(beta);
        const void *p_alpha = half_scalars ? (const void *)&h_alpha : (const void *)&alpha;
        const void *p_beta = half_scalars ? (const void *)&h_beta : (const void *)&beta;

        hipblasStatus_t status = HIPBLAS_STATUS_UNKNOWN;
        switch (kind) {
            case CALL_SGEMM:
                status = hipblasSgemm(handle, (hipblasOperation_t)op_a, (hipblasOperation_t)op_b,
                                      m, n, k, &alpha, (const float *)da, g.lda,
                                      (const float *)db, g.ldb, &beta, (float *)dc, g.ldc);
                break;
            case CALL_SGEMM_BATCHED:
                status = hipblasSgemmBatched(
                    handle, (hipblasOperation_t)op_a, (hipblasOperation_t)op_b, m, n, k, &alpha,
                    (const float *const *)dpa, g.lda, (const float *const *)dpb, g.ldb, &beta,
                    (float *const *)dpc, g.ldc, batch);
                break;
            case CALL_SGEMM_STRIDED:
                status = hipblasSgemmStridedBatched(
                    handle, (hipblasOperation_t)op_a, (hipblasOperation_t)op_b, m, n, k, &alpha,
                    (const float *)da, g.lda, ea, (const float *)db, g.ldb, eb, &beta,
                    (float *)dc, g.ldc, ec, batch);
                break;
            case CALL_GEMM_EX:
                status = hipblasGemmEx(handle, (hipblasOperation_t)op_a, (hipblasOperation_t)op_b,
                                       m, n, k, p_alpha, da, dt_ab, g.lda, db, dt_ab, g.ldb,
                                       p_beta, dc, dt_c, g.ldc, dt_compute, HIPBLAS_GEMM_DEFAULT);
                break;
            case CALL_GEMM_BATCHED_EX:
                status = hipblasGemmBatchedEx(
                    handle, (hipblasOperation_t)op_a, (hipblasOperation_t)op_b, m, n, k, p_alpha,
                    (const void **)dpa, dt_ab, g.lda, (const void **)dpb, dt_ab, g.ldb, p_beta,
                    (void **)dpc, dt_c, g.ldc, batch, dt_compute, HIPBLAS_GEMM_DEFAULT);
                break;
            default:
                status = hipblasGemmStridedBatchedEx(
                    handle, (hipblasOperation_t)op_a, (hipblasOperation_t)op_b, m, n, k, p_alpha,
                    da, dt_ab, g.lda, ea, db, dt_ab, g.ldb, eb, p_beta, dc, dt_c, g.ldc, ec,
                    batch, dt_compute, HIPBLAS_GEMM_DEFAULT);
                break;
        }
        check_status(label, status, HIPBLAS_STATUS_SUCCESS);
        if (status == HIPBLAS_STATUS_SUCCESS) {
            g_expected_dispatches++;
        }
        check_hip("hipDeviceSynchronize", hipDeviceSynchronize());

        std::vector<TC> got = hc;
        check_hip("hipMemcpy C back", hipMemcpy(got.data(), dc, got.size() * sizeof(TC),
                                                 hipMemcpyDeviceToHost));

        if (g_numeric) {
            const float scale =
                fabsf(alpha) * (float)(k > 0 ? k : 1) * max_a * max_b + fabsf(beta) * max_c;
            const bool half_out = dt_c == HIPBLAS_R_16F;
            int mismatches = 0;
            int outside = 0;
            for (int z = 0; z < batch; ++z) {
                for (int j = 0; j < n; ++j) {
                    for (int i = 0; i < g.ldc; ++i) {
                        const size_t at = (size_t)(ec * z + (long long)j * g.ldc + i);
                        if (i >= m) {
                            if (memcmp(&got[at], &hc[at], sizeof(TC)) != 0) {
                                outside++;
                            }
                            continue;
                        }
                        const float want = bc250blas::to_f32(reference[at]);
                        const float have = bc250blas::to_f32(got[at]);
                        const float tol = bc250blas::compare_tolerance(scale, want, half_out);
                        if (fabsf(want - have) > tol) {
                            if (mismatches < 3 && !g_quiet) {
                                printf("     %s: C[%d,%d,%d] is %g, reference %g (tol %g)\n",
                                       label, i, j, z, (double)have, (double)want, (double)tol);
                            }
                            mismatches++;
                        }
                    }
                }
            }
            g_checks++;
            if (perturb) {
                // The control: the device computed from changed data, so the comparison has to
                // fail. A pass here would mean every other case passed for no reason.
                if (mismatches == 0) {
                    g_failures++;
                    printf("FAIL %s: the comparison noticed nothing\n", label);
                } else if (!g_quiet) {
                    printf("  ok  %s: %d elements differ, as they must\n", label, mismatches);
                }
            } else if (mismatches != 0) {
                g_failures++;
                printf("FAIL %s: %d of %lld elements differ from the reference (seed %u)\n",
                       label, mismatches, (long long)m * n * batch, seed);
            } else if (!g_quiet) {
                printf("  ok  %s\n", label);
            }
            g_checks++;
            if (outside != 0) {
                g_failures++;
                printf("FAIL %s: %d elements outside the matrix were written (ldc %d, m %d)\n",
                       label, outside, g.ldc, m);
            }
        } else if (!g_quiet) {
            printf("  ok  %s (status only)\n", label);
        }

        hipFree(da);
        hipFree(db);
        hipFree(dc);
        if (dpa != nullptr) hipFree(dpa);
        if (dpb != nullptr) hipFree(dpb);
        if (dpc != nullptr) hipFree(dpc);
    }
};

// ---------------------------------------------------------------------------------------------
// The refusals. None of these may reach the device, which the dispatch count proves: this
// section raises no expected dispatch, so a dispatch recorded here is a defect.
// ---------------------------------------------------------------------------------------------
void test_refusals(hipblasHandle_t handle) {
    float alpha = 1.0f;
    float beta = 0.0f;
    void *d = nullptr;
    check_hip("hipMalloc for the refusals", hipMalloc(&d, 4096));
    float *a = (float *)d;

    // No handle.
    check_status("hipblasSgemm with no handle",
                 hipblasSgemm(nullptr, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a, 4, a, 4,
                              &beta, a, 4),
                 HIPBLAS_STATUS_NOT_INITIALIZED);
    check_status("hipblasGemmEx with no handle",
                 hipblasGemmEx(nullptr, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a,
                               HIPBLAS_R_32F, 4, a, HIPBLAS_R_32F, 4, &beta, a, HIPBLAS_R_32F, 4,
                               HIPBLAS_R_32F, HIPBLAS_GEMM_DEFAULT),
                 HIPBLAS_STATUS_NOT_INITIALIZED);

    // The conjugate transpose, which this library does not answer for.
    check_status("hipblasSgemm with HIPBLAS_OP_C",
                 hipblasSgemm(handle, HIPBLAS_OP_C, HIPBLAS_OP_N, 4, 4, 4, &alpha, a, 4, a, 4,
                              &beta, a, 4),
                 HIPBLAS_STATUS_NOT_SUPPORTED);

    // Leading dimensions that are too small, one at a time.
    check_status("hipblasSgemm with lda too small",
                 hipblasSgemm(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a, 3, a, 4,
                              &beta, a, 4),
                 HIPBLAS_STATUS_INVALID_VALUE);
    check_status("hipblasSgemm with ldb too small",
                 hipblasSgemm(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a, 4, a, 3,
                              &beta, a, 4),
                 HIPBLAS_STATUS_INVALID_VALUE);
    check_status("hipblasSgemm with ldc too small",
                 hipblasSgemm(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a, 4, a, 4,
                              &beta, a, 3),
                 HIPBLAS_STATUS_INVALID_VALUE);

    // Null scalars, and a null matrix with work to do.
    check_status("hipblasSgemm with no alpha",
                 hipblasSgemm(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, nullptr, a, 4, a, 4,
                              &beta, a, 4),
                 HIPBLAS_STATUS_INVALID_VALUE);
    check_status("hipblasSgemm with no A",
                 hipblasSgemm(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, nullptr, 4, a,
                              4, &beta, a, 4),
                 HIPBLAS_STATUS_INVALID_VALUE);

    // A negative size and a negative batch count.
    check_status("hipblasSgemm with m < 0",
                 hipblasSgemm(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, -1, 4, 4, &alpha, a, 4, a, 4,
                              &beta, a, 4),
                 HIPBLAS_STATUS_INVALID_VALUE);
    check_status("hipblasSgemmStridedBatched with batchCount < 0",
                 hipblasSgemmStridedBatched(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha,
                                            a, 4, 16, a, 4, 16, &beta, a, 4, 16, -1),
                 HIPBLAS_STATUS_INVALID_VALUE);

    // An empty call is a success that computes nothing.
    check_status("hipblasSgemm with n == 0",
                 hipblasSgemm(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 0, 4, &alpha, a, 4, a, 4,
                              &beta, a, 4),
                 HIPBLAS_STATUS_SUCCESS);
    check_status("hipblasSgemmStridedBatched with batchCount == 0",
                 hipblasSgemmStridedBatched(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha,
                                            a, 4, 16, a, 4, 16, &beta, a, 4, 16, 0),
                 HIPBLAS_STATUS_SUCCESS);

    // The type combinations this library does not implement. Each one must be refused and not
    // computed as f32: with GGML_CUDA_FORCE_MMQ=ON a quantized type that MMQ does not cover
    // still reaches hipblasGemmEx, and a silently wrong GEMM there is a wrong answer.
    struct TypeRow {
        hipblasDatatype_t a;
        hipblasDatatype_t c;
        hipblasDatatype_t compute;
        const char *why;
    };
    static const TypeRow refused[] = {
        {HIPBLAS_R_64F, HIPBLAS_R_64F, HIPBLAS_R_64F, "f64"},
        {HIPBLAS_R_8I, HIPBLAS_R_32I, HIPBLAS_R_32I, "integer"},
        {HIPBLAS_C_32F, HIPBLAS_C_32F, HIPBLAS_C_32F, "complex"},
        {HIPBLAS_R_32F, HIPBLAS_R_16F, HIPBLAS_R_32F, "f32 in, f16 out"},
        {HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_16F, "f32 data with a f16 accumulator"},
        {HIPBLAS_R_16B, HIPBLAS_R_16B, HIPBLAS_R_32F, "bf16 out"},
        {(hipblasDatatype_t)9999, HIPBLAS_R_32F, HIPBLAS_R_32F, "an unknown type code"},
    };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        char what[160];
        snprintf(what, sizeof(what), "hipblasGemmEx refuses %s", refused[i].why);
        check_status(what,
                     hipblasGemmEx(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a,
                                   refused[i].a, 4, a, refused[i].a, 4, &beta, a, refused[i].c, 4,
                                   refused[i].compute, HIPBLAS_GEMM_DEFAULT),
                     HIPBLAS_STATUS_NOT_SUPPORTED);
        snprintf(what, sizeof(what), "hipblasGemmStridedBatchedEx refuses %s", refused[i].why);
        check_status(what,
                     hipblasGemmStridedBatchedEx(
                         handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a, refused[i].a, 4,
                         16, a, refused[i].a, 4, 16, &beta, a, refused[i].c, 4, 16, 2,
                         refused[i].compute, HIPBLAS_GEMM_DEFAULT),
                     HIPBLAS_STATUS_NOT_SUPPORTED);
    }

    // An algorithm selector this header does not name.
    check_status("hipblasGemmEx refuses an unknown algorithm",
                 hipblasGemmEx(handle, HIPBLAS_OP_N, HIPBLAS_OP_N, 4, 4, 4, &alpha, a,
                               HIPBLAS_R_32F, 4, a, HIPBLAS_R_32F, 4, &beta, a, HIPBLAS_R_32F, 4,
                               HIPBLAS_R_32F, (hipblasGemmAlgo_t)4242),
                 HIPBLAS_STATUS_INVALID_ENUM);

    // The triangular solve, which serves ggml's solve_tri operation only and is refused rather
    // than answered wrongly.
    const float *A_ptrs[1] = {a};
    float *B_ptrs[1] = {a};
    check_status("hipblasStrsmBatched",
                 hipblasStrsmBatched(handle, HIPBLAS_SIDE_RIGHT, HIPBLAS_FILL_MODE_UPPER,
                                     HIPBLAS_OP_N, HIPBLAS_DIAG_NON_UNIT, 4, 4, &alpha, A_ptrs, 4,
                                     B_ptrs, 4, 1),
                 HIPBLAS_STATUS_NOT_SUPPORTED);
    check_status("hipblasStrsmBatched with no handle",
                 hipblasStrsmBatched(nullptr, HIPBLAS_SIDE_RIGHT, HIPBLAS_FILL_MODE_UPPER,
                                     HIPBLAS_OP_N, HIPBLAS_DIAG_NON_UNIT, 4, 4, &alpha, A_ptrs, 4,
                                     B_ptrs, 4, 1),
                 HIPBLAS_STATUS_NOT_INITIALIZED);

    hipFree(d);
}

void test_handle() {
    hipblasHandle_t handle = nullptr;
    check_status("hipblasCreate", hipblasCreate(&handle), HIPBLAS_STATUS_SUCCESS);
    CHECK(handle != nullptr);
    check_status("hipblasCreate with no output", hipblasCreate(nullptr),
                 HIPBLAS_STATUS_INVALID_VALUE);

    hipStream_t stream = nullptr;
    check_hip("hipStreamCreate", hipStreamCreate(&stream));
    check_status("hipblasSetStream", hipblasSetStream(handle, stream), HIPBLAS_STATUS_SUCCESS);
    check_status("hipblasSetStream with no handle", hipblasSetStream(nullptr, stream),
                 HIPBLAS_STATUS_NOT_INITIALIZED);
    // Back to the null stream, which is where every other test of this program runs.
    check_status("hipblasSetStream back to the default", hipblasSetStream(handle, nullptr),
                 HIPBLAS_STATUS_SUCCESS);
    check_hip("hipStreamDestroy", hipStreamDestroy(stream));

    check_status("hipblasDestroy", hipblasDestroy(handle), HIPBLAS_STATUS_SUCCESS);
    check_status("hipblasDestroy with no handle", hipblasDestroy(nullptr),
                 HIPBLAS_STATUS_NOT_INITIALIZED);
    // A second destroy of the same handle is NOT checked, and that is deliberate: the first one
    // frees the memory, so no marker survives for the second one to read, and the answer would
    // depend on what the heap did with the block. hipBLAS and cuBLAS state the same contract,
    // and ggml destroys each handle once.
}

struct Shape {
    int m;
    int n;
    int k;
    int pad;
};

void test_numbers(hipblasHandle_t handle) {
    static const Shape shapes[] = {{1, 1, 1, 0}, {17, 33, 7, 1}, {64, 48, 32, 0}};
    static const int ops[4][2] = {{HIPBLAS_OP_N, HIPBLAS_OP_N},
                                  {HIPBLAS_OP_N, HIPBLAS_OP_T},
                                  {HIPBLAS_OP_T, HIPBLAS_OP_N},
                                  {HIPBLAS_OP_T, HIPBLAS_OP_T}};
    unsigned int seed = 0x2f6b91c3u;

    // hipblasSgemm: every transposition, and beta 0 and 1.
    for (unsigned s = 0; s < sizeof(shapes) / sizeof(shapes[0]); ++s) {
        for (int o = 0; o < 4; ++o) {
            for (int b = 0; b < 2; ++b) {
                seed = seed * 1664525u + 1013904223u;
                Case<float, float, float>::run(
                    handle, CALL_SGEMM, HIPBLAS_R_32F, HIPBLAS_R_32F, HIPBLAS_R_32F, ops[o][0],
                    ops[o][1], shapes[s].m, shapes[s].n, shapes[s].k, shapes[s].pad,
                    b == 0 ? 1.0f : 0.5f, b == 0 ? 0.0f : 1.0f, 1, seed, false);
            }
        }
    }

    // The batched f32 forms, with a batch count that is not a power of two.
    seed = seed * 1664525u + 1013904223u;
    Case<float, float, float>::run(handle, CALL_SGEMM_STRIDED, HIPBLAS_R_32F, HIPBLAS_R_32F,
                                   HIPBLAS_R_32F, HIPBLAS_OP_N, HIPBLAS_OP_T, 17, 9, 33, 2, 0.75f,
                                   0.25f, 3, seed, false);
    seed = seed * 1664525u + 1013904223u;
    Case<float, float, float>::run(handle, CALL_SGEMM_BATCHED, HIPBLAS_R_32F, HIPBLAS_R_32F,
                                   HIPBLAS_R_32F, HIPBLAS_OP_N, HIPBLAS_OP_T, 17, 9, 33, 2, 0.75f,
                                   0.25f, 5, seed, false);

    // The typed forms. The f16 one is the combination ggml passes on this part, with alpha and
    // beta as __half, and it is the one on the token path when FlashAttention is off.
    for (int o = 0; o < 4; ++o) {
        seed = seed * 1664525u + 1013904223u;
        Case<__half, __half, __half>::run(handle, CALL_GEMM_EX, HIPBLAS_R_16F, HIPBLAS_R_16F,
                                          HIPBLAS_R_16F, ops[o][0], ops[o][1], 17, 33, 7, 1, 1.0f,
                                          0.0f, 1, seed, false);
        seed = seed * 1664525u + 1013904223u;
        Case<__half, __half, float>::run(handle, CALL_GEMM_EX, HIPBLAS_R_16F, HIPBLAS_R_32F,
                                         HIPBLAS_R_32F, ops[o][0], ops[o][1], 16, 16, 64, 0, 0.5f,
                                         1.0f, 1, seed, false);
        seed = seed * 1664525u + 1013904223u;
        Case<__hip_bfloat16, __hip_bfloat16, float>::run(
            handle, CALL_GEMM_EX, HIPBLAS_R_16B, HIPBLAS_R_32F, HIPBLAS_R_32F, ops[o][0],
            ops[o][1], 5, 7, 129, 2, 1.0f, 0.0f, 1, seed, false);
    }

    // The batched typed forms. hipblasGemmStridedBatchedEx with f16 is what every attention
    // matrix multiply of a token goes through with GGML_CUDA_FA=OFF.
    seed = seed * 1664525u + 1013904223u;
    Case<__half, __half, __half>::run(handle, CALL_GEMM_STRIDED_EX, HIPBLAS_R_16F, HIPBLAS_R_16F,
                                      HIPBLAS_R_16F, HIPBLAS_OP_T, HIPBLAS_OP_N, 33, 17, 64, 0,
                                      1.0f, 0.0f, 3, seed, false);
    seed = seed * 1664525u + 1013904223u;
    Case<__half, __half, __half>::run(handle, CALL_GEMM_BATCHED_EX, HIPBLAS_R_16F, HIPBLAS_R_16F,
                                      HIPBLAS_R_16F, HIPBLAS_OP_T, HIPBLAS_OP_N, 16, 7, 16, 0,
                                      1.0f, 0.0f, 3, seed, false);
    seed = seed * 1664525u + 1013904223u;
    Case<float, float, float>::run(handle, CALL_GEMM_BATCHED_EX, HIPBLAS_R_32F, HIPBLAS_R_32F,
                                   HIPBLAS_R_32F, HIPBLAS_OP_T, HIPBLAS_OP_N, 17, 5, 31, 1, 1.0f,
                                   0.0f, 3, seed, false);

    // k == 0 is C := beta * C, which hipBLAS states and ggml reaches with an empty reduction.
    seed = seed * 1664525u + 1013904223u;
    Case<float, float, float>::run(handle, CALL_SGEMM, HIPBLAS_R_32F, HIPBLAS_R_32F,
                                   HIPBLAS_R_32F, HIPBLAS_OP_N, HIPBLAS_OP_N, 17, 5, 0, 1, 1.0f,
                                   1.0f, 1, seed, false);

    // The negative control, last, so that a reader sees it after the cases it guards.
    seed = seed * 1664525u + 1013904223u;
    Case<float, float, float>::run(handle, CALL_SGEMM, HIPBLAS_R_32F, HIPBLAS_R_32F,
                                   HIPBLAS_R_32F, HIPBLAS_OP_T, HIPBLAS_OP_N, 17, 33, 7, 0, 1.0f,
                                   0.0f, 1, seed, true);
}

// The wait bound of the runtime, which it reads one time from the environment at its first
// call. A lab trial stays inside three minutes, and the library default is 120 seconds for one
// wait.
int set_wait_total(const char *ms) {
    if (ms == nullptr || ms[0] == '\0') {
        return 0;
    }
#if defined(_WIN32)
    return _putenv_s("BC250_HIP_WAIT_TOTAL_MS", ms) == 0;
#else
    return setenv("BC250_HIP_WAIT_TOTAL_MS", ms, 1) == 0;
#endif
}

}  // namespace

int main(int argc, char **argv) {
    // Unbuffered output on purpose: this program runs on the lab, where a hang or a fault is
    // one of the things it is looking for, and a lost buffer would take the last line with it.
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-numeric") == 0) {
            g_numeric = 0;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            g_quiet = 1;
        } else if (strcmp(argv[i], "--wait-total") == 0 && i + 1 < argc) {
            set_wait_total(argv[++i]);
        } else {
            printf("gemm_check: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    printf("gemm_check: numeric %s\n", g_numeric ? "on" : "off (status and refusals only)");

    test_handle();

    hipblasHandle_t handle = nullptr;
    if (hipblasCreate(&handle) != HIPBLAS_STATUS_SUCCESS || handle == nullptr) {
        printf("gemm_check: hipblasCreate failed, nothing else can run\n");
        return 1;
    }
    test_refusals(handle);
    test_numbers(handle);
    check_status("hipblasDestroy at the end", hipblasDestroy(handle), HIPBLAS_STATUS_SUCCESS);

    printf("gemm_check: expected dispatches %d\n", g_expected_dispatches);
    printf("gemm_check: %d checks, %d failed\n", g_checks, g_failures);
    printf("gemm_check: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures;
}
