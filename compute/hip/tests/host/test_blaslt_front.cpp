// test_blaslt_front.cpp - the hipBLASLt front (compute\hip\blaslt) against a processor reference.
//
// argv[1] is a directory with the front (libhipblaslt.dll), the stand-in for AMD's library under its
// attach name (libhipblaslt_amd.dll, blaslt_fake_amd.cpp) and a processor rocblas.dll
// (blaslt_fake_rocblas.cpp). The test loads the front by full path and makes the calls of torch's
// int8_gemm (aten CUDABlas.cpp, CublasLtUtils.h): MatmulDescCreate(COMPUTE_32I, R_32I), TRANSA and
// TRANSB, the three layouts as CuBlasLtMatrixLayout creates them, a preference with
// MAX_WORKSPACE_BYTES, AlgoGetHeuristic(... C, C, 1 ...) and Matmul(alpha 1, beta 0, C = D).
//
// The numbers: X (m x k) and Y (k x n) are int8 matrices from a fixed generator. A and B hold them
// in the storage that the transpose flags name, with padding beyond each column that holds 0x7f.
// The reference R = X Y is computed from X and Y directly, so a wrong transpose, size or leading
// dimension in the front fails the comparison. D starts with 0x5a5a5a5a everywhere; its padding
// must stay unchanged.
//
// The stand-in answers GetAttribute as TheRock's hipBLASLt 1.5.0 does (it refuses a layout's type,
// order and sizes), so a front that reads the problem back from AMD's library fails step 3.
//
// It fails when:
//   1. a forwarded function or a forwarded data export does not reach the stand-in;
//   2. AMD's algo is not used when AMD's heuristic answers (real first);
//   3. a served case does not give R, for each transpose pair, or AMD is asked a second time for a
//      transpose pair it refused;
//   4. alpha 3, beta 2 with C apart from D does not give 3 R + 2 C;
//   5. a problem outside the served case does not get AMD's answer, or a marked algo of such a
//      problem is not refused (among them a layout made in AMD's library directly, of which the
//      front has no record); an attribute set and then set back to its default is served;
//   6. a call into the front from inside rocBLAS is served instead of forwarded;
//   7. hipblasLtDestroy does not destroy the rocBLAS handle;
//   8. the counters do not count the calls above;
//   9. with BC250_BLASLT_FRONT=0 (a child process) a served case is served.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "blaslt_abi.h"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

struct FakeCounts {
    long create, destroy, heuristic, matmul, desc_create, desc_destroy, layout_create, layout_destroy;
};

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

struct Counters {
    uint32_t version, bytes;
    uint64_t heuristic_calls, heuristic_amd_served, heuristic_amd_refused, heuristic_served, matmul_calls,
        matmul_served, matmul_refused, rocblas_errors, descs_live, handles_live, layouts_live;
};

struct Api {
    hipblasStatus_t (*create)(hipblasLtHandle_t*);
    hipblasStatus_t (*destroy)(hipblasLtHandle_t);
    hipblasStatus_t (*version)(hipblasLtHandle_t, int*);
    hipblasStatus_t (*desc_create)(hipblasLtMatmulDesc_t*, hipblasComputeType_t, hipDataType);
    hipblasStatus_t (*desc_destroy)(hipblasLtMatmulDesc_t);
    hipblasStatus_t (*desc_set)(hipblasLtMatmulDesc_t, hipblasLtMatmulDescAttributes_t, const void*, size_t);
    hipblasStatus_t (*layout_create)(hipblasLtMatrixLayout_t*, hipDataType, uint64_t, uint64_t, int64_t);
    hipblasStatus_t (*layout_destroy)(hipblasLtMatrixLayout_t);
    hipblasStatus_t (*layout_set)(hipblasLtMatrixLayout_t, hipblasLtMatrixLayoutAttribute_t, const void*, size_t);
    hipblasStatus_t (*pref_create)(hipblasLtMatmulPreference_t*);
    hipblasStatus_t (*pref_destroy)(hipblasLtMatmulPreference_t);
    hipblasStatus_t (*pref_set)(hipblasLtMatmulPreference_t, int, const void*, size_t);
    hipblasStatus_t (*heuristic)(hipblasLtHandle_t, hipblasLtMatmulDesc_t, hipblasLtMatrixLayout_t,
                                 hipblasLtMatrixLayout_t, hipblasLtMatrixLayout_t, hipblasLtMatrixLayout_t,
                                 hipblasLtMatmulPreference_t, int, hipblasLtMatmulHeuristicResult_t*, int*);
    hipblasStatus_t (*matmul)(hipblasLtHandle_t, hipblasLtMatmulDesc_t, const void*, const void*,
                              hipblasLtMatrixLayout_t, const void*, hipblasLtMatrixLayout_t, const void*, const void*,
                              hipblasLtMatrixLayout_t, void*, hipblasLtMatrixLayout_t, const hipblasLtMatmulAlgo_t*,
                              void*, size_t, hipStream_t);
    int (*counters)(Counters*, size_t);
    hipblasStatus_t (*amd_layout_create)(hipblasLtMatrixLayout_t*, hipDataType, uint64_t, uint64_t, int64_t);
    void (*amd_mode)(int);
    void (*amd_counts)(FakeCounts*);
    void (*rb_last)(FakeGemm*);
    void (*rb_callback)(int (*)(void*), void*);
};

template <typename T>
bool bind(HMODULE m, const char* name, T* out) {
    *out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    if (*out == nullptr) {
        std::printf("FAIL %s is not exported\n", name);
        ++g_failures;
        return false;
    }
    return true;
}

bool load(const std::wstring& dir, Api* api, HMODULE* front_out) {
    const std::wstring front_path = dir + L"\\libhipblaslt.dll";
    HMODULE front = LoadLibraryExW(front_path.c_str(), nullptr,
                                   LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (front == nullptr) {
        std::printf("FAIL the front does not load (error %lu)\n", GetLastError());
        ++g_failures;
        return false;
    }
    HMODULE amd = GetModuleHandleW(L"libhipblaslt_amd.dll");
    wchar_t amd_path[MAX_PATH] = {};
    if (amd != nullptr) {
        GetModuleFileNameW(amd, amd_path, MAX_PATH);
    }
    const std::wstring want = dir + L"\\libhipblaslt_amd.dll";
    check(amd != nullptr && _wcsicmp(amd_path, want.c_str()) == 0,
          "loading the front loads libhipblaslt_amd.dll from the directory of the front");
    const std::wstring rb_path = dir + L"\\rocblas.dll";
    HMODULE rb = LoadLibraryExW(rb_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (amd == nullptr || rb == nullptr) {
        return false;
    }
    bool ok = bind(front, "hipblasLtCreate", &api->create) && bind(front, "hipblasLtDestroy", &api->destroy) &&
              bind(front, "hipblasLtGetVersion", &api->version) &&
              bind(front, "hipblasLtMatmulDescCreate", &api->desc_create) &&
              bind(front, "hipblasLtMatmulDescDestroy", &api->desc_destroy) &&
              bind(front, "hipblasLtMatmulDescSetAttribute", &api->desc_set) &&
              bind(front, "hipblasLtMatrixLayoutCreate", &api->layout_create) &&
              bind(front, "hipblasLtMatrixLayoutDestroy", &api->layout_destroy) &&
              bind(front, "hipblasLtMatrixLayoutSetAttribute", &api->layout_set) &&
              bind(front, "hipblasLtMatmulPreferenceCreate", &api->pref_create) &&
              bind(front, "hipblasLtMatmulPreferenceDestroy", &api->pref_destroy) &&
              bind(front, "hipblasLtMatmulPreferenceSetAttribute", &api->pref_set) &&
              bind(front, "hipblasLtMatmulAlgoGetHeuristic", &api->heuristic) &&
              bind(front, "hipblasLtMatmul", &api->matmul) && bind(front, "bc250lt_get_counters", &api->counters) &&
              bind(amd, "hipblasLtMatrixLayoutCreate", &api->amd_layout_create) &&
              bind(amd, "fake_amd_set_mode", &api->amd_mode) && bind(amd, "fake_amd_get_counts", &api->amd_counts) &&
              bind(rb, "fake_rocblas_last", &api->rb_last) && bind(rb, "fake_rocblas_set_callback", &api->rb_callback);
    *front_out = front;
    return ok;
}

FakeCounts amd_counts(const Api& api) {
    FakeCounts c = {};
    api.amd_counts(&c);
    return c;
}

FakeGemm rb_last(const Api& api) {
    FakeGemm g = {};
    api.rb_last(&g);
    return g;
}

Counters counters(const Api& api) {
    Counters c = {};
    api.counters(&c, sizeof(c));
    return c;
}

// One problem in the storage of torch's int8_gemm.
struct Problem {
    int m, n, k;
    bool ta, tb;
    int pad_a, pad_b, pad_d;
    int32_t alpha = 1, beta = 0;
    bool separate_c = false;
    hipblasComputeType_t compute = HIPBLAS_COMPUTE_32I;
    hipDataType scale = HIP_R_32I;
    hipDataType ab_type = HIP_R_8I;
    hipDataType cd_type = HIP_R_32I;
};

struct Built {
    hipblasLtMatmulDesc_t desc = nullptr;
    hipblasLtMatrixLayout_t a = nullptr, b = nullptr, c = nullptr, d = nullptr;
    hipblasLtMatmulPreference_t pref = nullptr;
    std::vector<int8_t> A, B, X, Y;
    std::vector<int32_t> C, D;
    int lda = 0, ldb = 0, ldd = 0;
};

uint32_t g_seed = 0x2545f491u;
int8_t next_i8() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return static_cast<int8_t>(static_cast<int>((g_seed >> 24) & 0xff) - 128);
}

bool build(const Api& api, const Problem& p, Built* out) {
    const int rows_a = p.ta ? p.k : p.m, cols_a = p.ta ? p.m : p.k;
    const int rows_b = p.tb ? p.n : p.k, cols_b = p.tb ? p.k : p.n;
    out->lda = rows_a + p.pad_a;
    out->ldb = rows_b + p.pad_b;
    out->ldd = p.m + p.pad_d;
    out->X.resize(static_cast<size_t>(p.m) * p.k);
    out->Y.resize(static_cast<size_t>(p.k) * p.n);
    for (auto& v : out->X) v = next_i8();
    for (auto& v : out->Y) v = next_i8();
    out->A.assign(static_cast<size_t>(out->lda) * cols_a, 0x7f);
    out->B.assign(static_cast<size_t>(out->ldb) * cols_b, 0x7f);
    for (int i = 0; i < p.m; ++i)
        for (int l = 0; l < p.k; ++l) {
            const int8_t v = out->X[static_cast<size_t>(i) * p.k + l];
            if (p.ta) out->A[l + static_cast<size_t>(i) * out->lda] = v;
            else out->A[i + static_cast<size_t>(l) * out->lda] = v;
        }
    for (int l = 0; l < p.k; ++l)
        for (int j = 0; j < p.n; ++j) {
            const int8_t v = out->Y[static_cast<size_t>(l) * p.n + j];
            if (p.tb) out->B[j + static_cast<size_t>(l) * out->ldb] = v;
            else out->B[l + static_cast<size_t>(j) * out->ldb] = v;
        }
    out->D.assign(static_cast<size_t>(out->ldd) * p.n, 0x5a5a5a5a);
    if (p.separate_c) {
        out->C.resize(out->D.size());
        for (size_t i = 0; i < out->C.size(); ++i) out->C[i] = static_cast<int32_t>(i % 1000) - 500;
    }
    if (api.desc_create(&out->desc, p.compute, p.scale) != HIPBLAS_STATUS_SUCCESS) return false;
    const int32_t transa = p.ta ? HIPBLAS_OP_T : HIPBLAS_OP_N, transb = p.tb ? HIPBLAS_OP_T : HIPBLAS_OP_N;
    if (api.desc_set(out->desc, HIPBLASLT_MATMUL_DESC_TRANSA, &transa, sizeof(transa)) != HIPBLAS_STATUS_SUCCESS ||
        api.desc_set(out->desc, HIPBLASLT_MATMUL_DESC_TRANSB, &transb, sizeof(transb)) != HIPBLAS_STATUS_SUCCESS)
        return false;
    // CuBlasLtMatrixLayout(type, rows, cols, ld, t) creates (t ? cols : rows) x (t ? rows : cols).
    if (api.layout_create(&out->a, p.ab_type, rows_a, cols_a, out->lda) != HIPBLAS_STATUS_SUCCESS ||
        api.layout_create(&out->b, p.ab_type, rows_b, cols_b, out->ldb) != HIPBLAS_STATUS_SUCCESS ||
        api.layout_create(&out->c, p.cd_type, p.m, p.n, out->ldd) != HIPBLAS_STATUS_SUCCESS)
        return false;
    out->d = out->c;
    if (api.pref_create(&out->pref) != HIPBLAS_STATUS_SUCCESS) return false;
    const uint64_t ws = 1u << 20;
    return api.pref_set(out->pref, 1 /* HIPBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES */, &ws, sizeof(ws)) ==
           HIPBLAS_STATUS_SUCCESS;
}

void release(const Api& api, Built* b) {
    if (b->pref) api.pref_destroy(b->pref);
    if (b->a) api.layout_destroy(b->a);
    if (b->b) api.layout_destroy(b->b);
    if (b->c) api.layout_destroy(b->c);
    if (b->desc) api.desc_destroy(b->desc);
    *b = Built();
}

hipblasStatus_t heuristic(const Api& api, hipblasLtHandle_t h, const Built& b, hipblasLtMatmulHeuristicResult_t* r,
                          int* count) {
    std::memset(r, 0, sizeof(*r));
    *count = -1;
    return api.heuristic(h, b.desc, b.a, b.b, b.c, b.d, b.pref, 1, r, count);
}

std::vector<uint8_t> g_workspace(1u << 20);
void* const kStream = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234));

hipblasStatus_t matmul(const Api& api, hipblasLtHandle_t h, const Problem& p, Built* b, const hipblasLtMatmulAlgo_t* algo) {
    const void* c = p.separate_c ? static_cast<const void*>(b->C.data()) : static_cast<const void*>(b->D.data());
    return api.matmul(h, b->desc, &p.alpha, b->A.data(), b->a, b->B.data(), b->b, &p.beta, c, b->c, b->D.data(), b->d,
                      algo, g_workspace.data(), g_workspace.size(), kStream);
}

bool numbers_right(const Problem& p, const Built& b) {
    for (int j = 0; j < p.n; ++j) {
        for (int i = 0; i < b.ldd; ++i) {
            const int32_t got = b.D[i + static_cast<size_t>(j) * b.ldd];
            if (i >= p.m) {
                if (got != 0x5a5a5a5a) return false;
                continue;
            }
            int64_t sum = 0;
            for (int l = 0; l < p.k; ++l)
                sum += int64_t(b.X[static_cast<size_t>(i) * p.k + l]) * b.Y[static_cast<size_t>(l) * p.n + j];
            int64_t want = int64_t(p.alpha) * sum;
            if (p.separate_c) want += int64_t(p.beta) * b.C[i + static_cast<size_t>(j) * b.ldd];
            if (got != static_cast<int32_t>(want)) return false;
        }
    }
    return true;
}

bool is_front_algo(const hipblasLtMatmulAlgo_t& a) {
    static const uint8_t marker[8] = {'B', 'C', '2', '5', '0', 'I', '8', 0};
    return std::memcmp(a.data, marker, sizeof(marker)) == 0;
}

// The callback of the processor rocBLAS: the front's heuristic for a served problem, from inside a
// rocBLAS call of the front. Returns status * 100 + algo count.
struct Callback {
    const Api* api;
    hipblasLtHandle_t handle;
    Built* built;
};
int from_inside_rocblas(void* context) {
    Callback* cb = static_cast<Callback*>(context);
    hipblasLtMatmulHeuristicResult_t r;
    int count = 0;
    const hipblasStatus_t st = heuristic(*cb->api, cb->handle, *cb->built, &r, &count);
    return static_cast<int>(st) * 100 + count;
}

int child_disabled(const std::wstring& dir) {
    Api api = {};
    HMODULE front = nullptr;
    if (!load(dir, &api, &front)) return 1;
    api.amd_mode(0);
    hipblasLtHandle_t h = nullptr;
    api.create(&h);
    Problem p{32, 16, 24, false, false, 0, 0, 0};
    Built b;
    check(build(api, p, &b), "child: the served problem is built");
    hipblasLtMatmulHeuristicResult_t r;
    int count = 0;
    const hipblasStatus_t st = heuristic(api, h, b, &r, &count);
    const Counters c = counters(api);
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    check(st == HIPBLAS_STATUS_INVALID_VALUE && count == 0,
          "BC250_BLASLT_FRONT=0: the heuristic of a served problem is AMD's answer (invalid value, no algo)");
    check(c.heuristic_served == 0 && c.heuristic_amd_refused == 0, "BC250_BLASLT_FRONT=0: the front serves nothing");
#else
    // The release flavour reads no environment variable (BD-167): the front still serves.
    check(st == HIPBLAS_STATUS_SUCCESS && count == 1, "release: BC250_BLASLT_FRONT=0 is ignored, the heuristic is served");
    check(c.heuristic_served == 1, "release: BC250_BLASLT_FRONT=0 is ignored, the front serves");
#endif
    release(api, &b);
    api.destroy(h);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // The front writes its log lines to stderr; unbuffered, they cannot split a line of stdout.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 3 && std::wcscmp(argv[1], L"--child-disabled") == 0) {
        return child_disabled(argv[2]);
    }
    if (argc != 2) {
        std::printf("usage: test_blaslt_front <directory of the front and its stand-ins>\n");
        return 2;
    }
    const std::wstring dir = argv[1];
    SetEnvironmentVariableW(L"BC250_BLASLT_LOG", L"1");
    SetEnvironmentVariableW(L"BC250_BLASLT_FRONT", nullptr);
    Api api = {};
    HMODULE front = nullptr;
    if (!load(dir, &api, &front)) {
        std::printf("FAIL the front and its stand-ins do not load\n");
        return 1;
    }
    HMODULE amd = GetModuleHandleW(L"libhipblaslt_amd.dll");

    // 1. Forwarders.
    hipblasLtHandle_t h = nullptr;
    check(api.create(&h) == HIPBLAS_STATUS_SUCCESS && h != nullptr && amd_counts(api).create == 1,
          "hipblasLtCreate (a forwarder) reaches AMD's library");
    int version = 0;
    check(api.version(h, &version) == HIPBLAS_STATUS_SUCCESS && version == 4242,
          "hipblasLtGetVersion (a forwarder) returns AMD's value");
    const char* data_name = "??_7hipblaslt_internal_ostream@@6B@";
    check(GetProcAddress(front, data_name) != nullptr && GetProcAddress(front, data_name) == GetProcAddress(amd, data_name),
          "a data export of AMD's table resolves through the front to AMD's address");

    // 2. Real first: AMD answers, so AMD's algo and AMD's matmul are used.
    api.amd_mode(1);
    {
        Problem p{32, 16, 24, false, false, 0, 0, 0};
        Built b;
        check(build(api, p, &b), "the problem of step 2 is built");
        hipblasLtMatmulHeuristicResult_t r;
        int count = 0;
        const hipblasStatus_t st = heuristic(api, h, b, &r, &count);
        check(st == HIPBLAS_STATUS_SUCCESS && count == 1 && r.algo.data[0] == 0x77 && !is_front_algo(r.algo),
              "real first: AMD's heuristic answers, and its algo is returned");
        const long gemms = rb_last(api).gemms;
        const long amd_matmuls = amd_counts(api).matmul;
        check(matmul(api, h, p, &b, &r.algo) == HIPBLAS_STATUS_SUCCESS && b.D[0] == 0x7777 &&
                  amd_counts(api).matmul == amd_matmuls + 1 && rb_last(api).gemms == gemms,
              "real first: hipblasLtMatmul with AMD's algo runs in AMD's library, not in rocBLAS");
        release(api, &b);
    }

    // 3. AMD refuses: the front serves every transpose pair through rocblas_gemm_ex.
    api.amd_mode(0);
    const Problem shapes[] = {
        {17, 8, 8, false, false, 0, 0, 0},  {32, 16, 24, false, false, 3, 5, 2}, {64, 40, 48, false, true, 1, 0, 4},
        {33, 24, 16, true, false, 0, 7, 0}, {48, 32, 72, true, true, 2, 2, 1},   {256, 64, 128, false, false, 0, 0, 0},
        {40, 136, 32, true, true, 0, 0, 0}, {24, 8, 512, false, true, 0, 0, 0},
    };
    bool asked_pair[2][2] = {};
    for (const Problem& p : shapes) {
        Built b;
        char what[200];
        std::snprintf(what, sizeof(what), "m %d n %d k %d trans %c%c pads %d %d %d", p.m, p.n, p.k, p.ta ? 'T' : 'N',
                      p.tb ? 'T' : 'N', p.pad_a, p.pad_b, p.pad_d);
        if (!build(api, p, &b)) {
            check(false, what);
            continue;
        }
        const long amd_heuristics = amd_counts(api).heuristic;
        hipblasLtMatmulHeuristicResult_t r;
        int count = 0;
        const hipblasStatus_t st = heuristic(api, h, b, &r, &count);
        const bool asked = amd_counts(api).heuristic == amd_heuristics + 1;
        const bool first = !asked_pair[p.ta][p.tb];
        asked_pair[p.ta][p.tb] = true;
        std::string line = std::string("served heuristic, AMD asked ") + (first ? "once" : "not again") + ": " + what;
        check(st == HIPBLAS_STATUS_SUCCESS && count == 1 && is_front_algo(r.algo) && r.workspaceSize == 0 &&
                  r.algo.max_workspace_bytes == 0 && asked == first,
              line.c_str());
        const long amd_matmuls = amd_counts(api).matmul;
        const hipblasStatus_t ms = matmul(api, h, p, &b, &r.algo);
        const FakeGemm g = rb_last(api);
        line = std::string("served matmul gives X Y, padding unchanged: ") + what;
        check(ms == HIPBLAS_STATUS_SUCCESS && numbers_right(p, b) && amd_counts(api).matmul == amd_matmuls, line.c_str());
        line = std::string("rocblas_gemm_ex arguments (types, algo, flags, host pointers, stream, sizes): ") + what;
        check(g.a_type == rocblas_datatype_i8_r && g.b_type == rocblas_datatype_i8_r && g.c_type == rocblas_datatype_i32_r &&
                  g.d_type == rocblas_datatype_i32_r && g.compute_type == rocblas_datatype_i32_r && g.algo == 0 &&
                  g.flags == 0 && g.pointer_mode == 0 && g.stream == kStream && g.m == p.m && g.n == p.n && g.k == p.k &&
                  g.lda == b.lda && g.ldb == b.ldb && g.ldd == b.ldd && g.ldc == b.ldd,
              line.c_str());
        release(api, &b);
    }

    // 4. alpha 3, beta 2, C apart from D.
    {
        Problem p{40, 24, 32, true, false, 1, 3, 2};
        p.alpha = 3;
        p.beta = 2;
        p.separate_c = true;
        Built b;
        check(build(api, p, &b), "the problem of step 4 is built");
        hipblasLtMatmulHeuristicResult_t r;
        int count = 0;
        heuristic(api, h, b, &r, &count);
        check(matmul(api, h, p, &b, &r.algo) == HIPBLAS_STATUS_SUCCESS && numbers_right(p, b),
              "alpha 3 and beta 2 with C apart from D give 3 X Y + 2 C");
        release(api, &b);
    }

    // 5. Outside the served case: AMD's answer, and a marked algo is refused.
    struct Outside {
        const char* what;
        int kind;
    };
    const Outside outside[] = {
        {"compute 32F with scale 32F and float layouts", 0}, {"compute 32I_PEDANTIC", 1},
        {"scale type 32F with compute 32I", 2},              {"a bias pointer", 3},
        {"a non-default epilogue", 4},                        {"pointer mode DEVICE", 5},
        {"row order of A", 6},                                {"batch count 2 of C and D", 7},
        {"C and D of type 32F", 8},                            {"B with another k than A", 9},
        {"an A scale pointer", 10},                           {"a batch mode set on B", 11},
        {"a layout of A made in AMD's library directly", 12}, {"TRANSA C (conjugate)", 13},
        {"an SM count target of 8", 14},
    };
    hipblasLtMatmulHeuristicResult_t marked = {};
    {
        Problem p{32, 16, 24, false, false, 0, 0, 0};
        Built b;
        build(api, p, &b);
        int count = 0;
        heuristic(api, h, b, &marked, &count);
        release(api, &b);
    }
    for (const Outside& o : outside) {
        Problem p{32, 16, 24, false, false, 0, 0, 0};
        if (o.kind == 0) { p.compute = HIPBLAS_COMPUTE_32F; p.scale = HIP_R_32F; p.ab_type = HIP_R_32F; p.cd_type = HIP_R_32F; }
        if (o.kind == 1) p.compute = HIPBLAS_COMPUTE_32I_PEDANTIC;
        if (o.kind == 2) p.scale = HIP_R_32F;
        if (o.kind == 8) p.cd_type = HIP_R_32F;
        Built b;
        build(api, p, &b);
        void* some = &g_workspace[0];
        const uint32_t relu = 2;
        const int32_t device = 1, row = HIPBLASLT_ORDER_ROW, two = 2;
        if (o.kind == 3) api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_BIAS_POINTER, &some, sizeof(some));
        if (o.kind == 4) api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_EPILOGUE, &relu, sizeof(relu));
        if (o.kind == 5) api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_POINTER_MODE, &device, sizeof(device));
        if (o.kind == 6) api.layout_set(b.a, HIPBLASLT_MATRIX_LAYOUT_ORDER, &row, sizeof(row));
        if (o.kind == 7) api.layout_set(b.c, HIPBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &two, sizeof(two));
        if (o.kind == 9) {
            api.layout_destroy(b.b);
            api.layout_create(&b.b, HIP_R_8I, 16, 16, b.ldb + 16);
        }
        if (o.kind == 10) api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_A_SCALE_POINTER, &some, sizeof(some));
        const int32_t one = 1, conjugate = HIPBLAS_OP_C, eight = 8;
        if (o.kind == 11) api.layout_set(b.b, HIPBLASLT_MATRIX_LAYOUT_BATCH_MODE, &one, sizeof(one));
        if (o.kind == 12) {
            api.layout_destroy(b.a);
            api.amd_layout_create(&b.a, HIP_R_8I, p.m, p.k, b.lda);
        }
        if (o.kind == 13) api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_TRANSA, &conjugate, sizeof(conjugate));
        if (o.kind == 14) api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_SM_COUNT_TARGET, &eight, sizeof(eight));
        const long amd_heuristics = amd_counts(api).heuristic;
        hipblasLtMatmulHeuristicResult_t r;
        int count = 0;
        const hipblasStatus_t st = heuristic(api, h, b, &r, &count);
        std::string line = std::string("outside the served case, AMD's answer: ") + o.what;
        check(st == HIPBLAS_STATUS_INVALID_VALUE && count == 0 && amd_counts(api).heuristic == amd_heuristics + 1,
              line.c_str());
        const long amd_matmuls = amd_counts(api).matmul;
        const long gemms = rb_last(api).gemms;
        const hipblasStatus_t ms = matmul(api, h, p, &b, &marked.algo);
        line = std::string("outside the served case, a marked algo is refused: ") + o.what;
        check(ms == HIPBLAS_STATUS_INVALID_VALUE && amd_counts(api).matmul == amd_matmuls && rb_last(api).gemms == gemms,
              line.c_str());
        hipblasLtMatmulAlgo_t zero = {};
        const hipblasStatus_t fs = matmul(api, h, p, &b, &zero);
        line = std::string("outside the served case, an algo of AMD goes to AMD: ") + o.what;
        check(fs == HIPBLAS_STATUS_NOT_SUPPORTED && amd_counts(api).matmul == amd_matmuls + 1, line.c_str());
        release(api, &b);
    }
    // 5b. A bias pointer set, then set back to NULL, and a batch count of 2 set back to 1: served.
    {
        Problem p{24, 16, 32, false, true, 2, 0, 1};
        Built b;
        build(api, p, &b);
        void* some = &g_workspace[0];
        void* none = nullptr;
        const int32_t two = 2, one = 1;
        api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_BIAS_POINTER, &some, sizeof(some));
        api.desc_set(b.desc, HIPBLASLT_MATMUL_DESC_BIAS_POINTER, &none, sizeof(none));
        api.layout_set(b.a, HIPBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &two, sizeof(two));
        api.layout_set(b.a, HIPBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &one, sizeof(one));
        hipblasLtMatmulHeuristicResult_t r;
        int count = 0;
        const hipblasStatus_t st = heuristic(api, h, b, &r, &count);
        check(st == HIPBLAS_STATUS_SUCCESS && count == 1 && is_front_algo(r.algo) &&
                  matmul(api, h, p, &b, &r.algo) == HIPBLAS_STATUS_SUCCESS && numbers_right(p, b),
              "attributes set back to their defaults: the problem is served and gives X Y");
        release(api, &b);
    }

    // 6. From inside rocBLAS, the front forwards: AMD's refusal, not an algo of the front.
    {
        Problem p{32, 16, 24, false, false, 0, 0, 0};
        Built b, inner;
        build(api, p, &b);
        build(api, p, &inner);
        hipblasLtMatmulHeuristicResult_t r;
        int count = 0;
        heuristic(api, h, b, &r, &count);
        Callback cb{&api, h, &inner};
        api.rb_callback(from_inside_rocblas, &cb);
        const long amd_heuristics = amd_counts(api).heuristic;
        matmul(api, h, p, &b, &r.algo);
        const FakeGemm g = rb_last(api);
        check(g.callback_result == HIPBLAS_STATUS_INVALID_VALUE * 100 + 0 && amd_counts(api).heuristic == amd_heuristics + 1,
              "a heuristic call from inside rocBLAS goes to AMD's library (invalid value, no algo)");
        check(numbers_right(p, b), "the served matmul around that call still gives X Y");
        release(api, &b);
        release(api, &inner);
    }

    // 7 and 8. Destroy, then the counters.
    const FakeGemm before = rb_last(api);
    const long amd_destroys = amd_counts(api).destroy;
    check(api.destroy(h) == HIPBLAS_STATUS_SUCCESS && rb_last(api).destroys == before.destroys + 1 &&
              amd_counts(api).destroy == amd_destroys + 1 && before.creates == 1,
          "hipblasLtDestroy destroys the one rocBLAS handle of the handle and AMD's handle");
    const Counters c = counters(api);
    std::printf("counters: heuristic %llu (AMD served %llu, AMD refused %llu, front %llu), matmul %llu (served %llu, "
                "refused %llu), rocBLAS errors %llu, descriptors %llu, handles %llu, layouts %llu\n",
                (unsigned long long)c.heuristic_calls, (unsigned long long)c.heuristic_amd_served,
                (unsigned long long)c.heuristic_amd_refused, (unsigned long long)c.heuristic_served,
                (unsigned long long)c.matmul_calls, (unsigned long long)c.matmul_served,
                (unsigned long long)c.matmul_refused, (unsigned long long)c.rocblas_errors,
                (unsigned long long)c.descs_live, (unsigned long long)c.handles_live,
                (unsigned long long)c.layouts_live);
    const uint64_t served = sizeof(shapes) / sizeof(shapes[0]) + 3;  // steps 3, 4, 5b and 6
    const uint64_t outside_count = sizeof(outside) / sizeof(outside[0]);
    check(c.version == 2 && c.bytes == sizeof(Counters) && c.heuristic_amd_served == 1 && c.heuristic_amd_refused == 4 &&
              c.matmul_served == served && c.matmul_refused == outside_count && c.rocblas_errors == 0 &&
              c.descs_live == 0 && c.handles_live == 0 && c.layouts_live == 0,
          "the counters count the calls of steps 2 to 7, and no record is left");

    // 9. BC250_BLASLT_FRONT=0, in a child process.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --child-disabled \"" + dir + L"\"";
    SetEnvironmentVariableW(L"BC250_BLASLT_FRONT", L"0");
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(cmd.begin(), cmd.end());
    line.push_back(0);
    DWORD code = 99;
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SetEnvironmentVariableW(L"BC250_BLASLT_FRONT", nullptr);
    check(code == 0, "the child with BC250_BLASLT_FRONT=0 passes");

    std::printf("%s test_blaslt_front: %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
