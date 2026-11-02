// hipblaslt_front.cpp - libhipblaslt.dll, our front of AMD's hipBLASLt (renamed libhipblaslt_amd.dll).
//
// Why: TheRock ships libhipblaslt.dll for gfx1010 with no kernel library ("Cannot read
// TensileLibrary_lazy_gfx1010.dat"), so hipblasLtMatmulAlgoGetHeuristic fails for every problem.
// torch._int_mm (aten Blas.cpp _int_mm_out_cuda -> CUDABlas.cpp int8_gemm) asks hipBLASLt for an
// i8 x i8 -> i32 GEMM and has no other route, and ComfyUI asks for torch._int_mm on every AMD GPU
// (comfy-kitchen eager quantization). rocBLAS of the same wheel has gfx1010 kernels for that type
// (the 4xi8 HPA Tensile fallback libraries). This front serves that one case through rocblas_gemm_ex
// and passes everything else to AMD's library unchanged.
//
// Forward by default: every export of AMD's DLL is a PE forwarder to libhipblaslt_amd.dll
// (build-blaslt-front.ps1 writes the .def from AMD's export table, ordinals kept), except the nine
// names below, which this file implements and which call AMD's functions first.
//
// Served cases (the complete list; anything else goes to AMD's library, whose answer the caller gets):
//   I8I32: hipblasLtMatmulDescCreate with HIPBLAS_COMPUTE_32I and scale type HIP_R_32I; TRANSA and
//          TRANSB each N or T; every other attribute of the descriptor at its default (epilogue
//          DEFAULT, pointer mode HOST, no bias, scale, amax or aux pointer, scale modes SCALAR_32F, SM
//          count target 0) or never set; A and B HIP_R_8I, C and D HIP_R_32I, all column order, batch
//          count 1, no layout attribute set other than type, order, rows, cols, ld, batch count and
//          strided batch offset; sizes and leading dimensions that fit int32 and are consistent with
//          each other.
// The front decides from what it saw, not from what AMD's library reports: MEASURED on TheRock's
// hipBLASLt 1.5.0 (db2d71e0, kit3-testlog t33d), hipblasLtMatrixLayoutGetAttribute returns
// HIPBLAS_STATUS_INVALID_VALUE for TYPE, ORDER, ROWS, COLS and LD, and
// hipblasLtMatmulDescGetAttribute does the same for the C and D scale pointers and the A and B scale
// modes. So the front keeps a record of every descriptor and layout made through it (the create
// arguments and each attribute set with AMD's success), and a descriptor or layout that it has no
// record of is not a served case.
// Real first: for a served case the heuristic asks AMD's library once per transpose pair and
// process. If AMD returns HIPBLAS_STATUS_SUCCESS with at least one algo, its answer stands and the
// matmul goes to AMD. If AMD fails or returns no algo, the front returns one algo of its own (a
// marker in hipblasLtMatmulAlgo_t.data, no workspace), and hipblasLtMatmul with that algo runs
// rocblas_gemm_ex(i8, i8, i32, i32, compute i32) on the caller's stream.
//
// rocblas.dll imports hipBLASLt itself (it can use hipBLASLt as a backend). While this front is in a
// rocBLAS call on a thread, the wrapped names of that thread only forward, so that rocBLAS gets AMD's
// answers and no call can return to this front.
//
// Lab flavour only (AMDGPU_WDDM_LAB_FLAVOUR): BC250_BLASLT_FRONT=0 in the environment turns serving
// off (every call forwards), for comparison and rollback. BC250_BLASLT_LOG=1 writes one line to
// stderr at the first served heuristic per transpose pair, at AMD's first refusal, and at each
// refused or failed served matmul. The release flavour reads no environment variable: it serves,
// and it does not log (BD-167).
//
// Wrapped names: hipblasLtDestroy (frees the rocBLAS handle of a hipBLASLt handle);
// hipblasLtMatmulDescCreate, hipblasLtMatmulDescDestroy and hipblasLtMatmulDescSetAttribute, and
// hipblasLtMatrixLayoutCreate, hipblasLtMatrixLayoutDestroy and hipblasLtMatrixLayoutSetAttribute
// (the records); hipblasLtMatmulAlgoGetHeuristic and hipblasLtMatmul. Extra export:
// bc250lt_get_counters.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "blaslt_abi.h"

// AMD's functions, from libhipblaslt_amd.dll (import library of blaslt\libhipblaslt_amd.def).
extern "C" {
__declspec(dllimport) hipblasStatus_t hipblasLtDestroy(const hipblasLtHandle_t handle);
__declspec(dllimport) hipblasStatus_t hipblasLtMatmulDescCreate(hipblasLtMatmulDesc_t* desc,
                                                                hipblasComputeType_t computeType,
                                                                hipDataType scaleType);
__declspec(dllimport) hipblasStatus_t hipblasLtMatmulDescDestroy(const hipblasLtMatmulDesc_t desc);
__declspec(dllimport) hipblasStatus_t hipblasLtMatmulDescSetAttribute(hipblasLtMatmulDesc_t desc,
                                                                      hipblasLtMatmulDescAttributes_t attr,
                                                                      const void* buf, size_t sizeInBytes);
__declspec(dllimport) hipblasStatus_t hipblasLtMatrixLayoutCreate(hipblasLtMatrixLayout_t* layout, hipDataType type,
                                                                  uint64_t rows, uint64_t cols, int64_t ld);
__declspec(dllimport) hipblasStatus_t hipblasLtMatrixLayoutDestroy(const hipblasLtMatrixLayout_t layout);
__declspec(dllimport) hipblasStatus_t hipblasLtMatrixLayoutSetAttribute(hipblasLtMatrixLayout_t layout,
                                                                        hipblasLtMatrixLayoutAttribute_t attr,
                                                                        const void* buf, size_t sizeInBytes);
__declspec(dllimport) hipblasStatus_t hipblasLtMatmulAlgoGetHeuristic(
    hipblasLtHandle_t handle, hipblasLtMatmulDesc_t matmulDesc, hipblasLtMatrixLayout_t Adesc,
    hipblasLtMatrixLayout_t Bdesc, hipblasLtMatrixLayout_t Cdesc, hipblasLtMatrixLayout_t Ddesc,
    hipblasLtMatmulPreference_t pref, int requestedAlgoCount,
    hipblasLtMatmulHeuristicResult_t heuristicResultsArray[], int* returnAlgoCount);
__declspec(dllimport) hipblasStatus_t hipblasLtMatmul(
    hipblasLtHandle_t handle, hipblasLtMatmulDesc_t matmulDesc, const void* alpha, const void* A,
    hipblasLtMatrixLayout_t Adesc, const void* B, hipblasLtMatrixLayout_t Bdesc, const void* beta,
    const void* C, hipblasLtMatrixLayout_t Cdesc, void* D, hipblasLtMatrixLayout_t Ddesc,
    const hipblasLtMatmulAlgo_t* algo, void* workspace, size_t workspaceSizeInBytes, hipStream_t stream);
}

// The counters of this front, read through bc250lt_get_counters. Version 2 (version 1 had no
// layouts_live; it was never released).
struct bc250lt_counters {
    uint32_t version;
    uint32_t bytes;
    uint64_t heuristic_calls;      // every call of hipblasLtMatmulAlgoGetHeuristic
    uint64_t heuristic_amd_served; // a served case that AMD's library answered with an algo
    uint64_t heuristic_amd_refused;// a served case that AMD's library refused (asked once per pair)
    uint64_t heuristic_served;     // a served case answered with the algo of this front
    uint64_t matmul_calls;         // every call of hipblasLtMatmul
    uint64_t matmul_served;        // rocblas_gemm_ex returned success
    uint64_t matmul_refused;       // an algo of this front with a problem that is not a served case
    uint64_t rocblas_errors;       // rocblas.dll absent, a missing entry point, or a rocBLAS error
    uint64_t descs_live;           // matmul descriptors this front has a record of
    uint64_t handles_live;         // hipBLASLt handles that have a rocBLAS handle
    uint64_t layouts_live;         // matrix layouts this front has a record of
};

namespace {

// The marker of an algo of this front: "BC250I8" and a zero byte, then zero bytes.
const uint8_t kMarker[8] = {'B', 'C', '2', '5', '0', 'I', '8', 0};

enum Count {
    kHeuristicCalls, kHeuristicAmdServed, kHeuristicAmdRefused, kHeuristicServed, kMatmulCalls,
    kMatmulServed, kMatmulRefused, kRocblasErrors, kDescsLive, kHandlesLive, kLayoutsLive, kCountNumber
};
volatile LONG64 g_counts[kCountNumber];
void count(Count c, LONG64 delta = 1) { InterlockedAdd64(&g_counts[c], delta); }

// The settings read once from the environment.
INIT_ONCE g_settings_once = INIT_ONCE_STATIC_INIT;
bool g_serving = true;
bool g_log = false;

BOOL CALLBACK read_settings(PINIT_ONCE, PVOID, PVOID*) {
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    char value[8] = {};
    DWORD n = GetEnvironmentVariableA("BC250_BLASLT_FRONT", value, sizeof(value));
    if (n > 0 && n < sizeof(value) && std::strcmp(value, "0") == 0) {
        g_serving = false;
    }
    n = GetEnvironmentVariableA("BC250_BLASLT_LOG", value, sizeof(value));
    g_log = n > 0 && n < sizeof(value) && std::strcmp(value, "0") != 0;
#endif
    return TRUE;
}

void settings() { InitOnceExecuteOnce(&g_settings_once, read_settings, nullptr, nullptr); }

void log_line(const char* text) {
    if (g_log) {
        std::fprintf(stderr, "bc250 blaslt: %s\n", text);
        std::fflush(stderr);
    }
}

// A thread in a rocBLAS call of this front: its wrapped calls only forward.
thread_local int t_inside_rocblas = 0;

// The attributes set away from their defaults, one bit per attribute value below 128, and one flag
// for any higher value. An attribute set back to its default clears its bit.
struct Changed {
    uint64_t bits[2] = {0, 0};
    bool high = false;
    void mark(int attr, bool changed) {
        if (attr < 0 || attr >= 128) {
            high = high || changed;
            return;
        }
        const uint64_t bit = uint64_t(1) << (attr % 64);
        if (changed) bits[attr / 64] |= bit;
        else bits[attr / 64] &= ~bit;
    }
    bool any() const { return bits[0] != 0 || bits[1] != 0 || high; }
};

// The record of a matmul descriptor made through this front.
struct DescRecord {
    hipblasComputeType_t compute;
    hipDataType scale;
    int32_t transa = HIPBLAS_OP_N;
    int32_t transb = HIPBLAS_OP_N;
    Changed changed;
};

// The record of a matrix layout made through this front.
struct LayoutRecord {
    uint32_t type;
    uint64_t rows;
    uint64_t cols;
    int64_t ld;
    int32_t order = HIPBLASLT_ORDER_COL;
    int32_t batch = 1;
    Changed changed;
};

// The records of one kind of object, by handle. The map is made on first use and never freed, so
// that a call during process exit finds it.
template <typename K, typename V>
class Records {
public:
    // Returns true when the handle had no record yet.
    bool put(K key, const V& value) {
        AcquireSRWLockExclusive(&lock_);
        if (map_ == nullptr) map_ = new std::unordered_map<K, V>();
        const bool fresh = map_->find(key) == map_->end();
        (*map_)[key] = value;
        ReleaseSRWLockExclusive(&lock_);
        return fresh;
    }
    bool get(K key, V* out) {
        AcquireSRWLockShared(&lock_);
        bool found = false;
        if (map_ != nullptr) {
            auto it = map_->find(key);
            if (it != map_->end()) {
                *out = it->second;
                found = true;
            }
        }
        ReleaseSRWLockShared(&lock_);
        return found;
    }
    template <typename F>
    void update(K key, F change) {
        AcquireSRWLockExclusive(&lock_);
        if (map_ != nullptr) {
            auto it = map_->find(key);
            if (it != map_->end()) change(it->second);
        }
        ReleaseSRWLockExclusive(&lock_);
    }
    bool erase(K key) {
        AcquireSRWLockExclusive(&lock_);
        const bool erased = map_ != nullptr && map_->erase(key) != 0;
        ReleaseSRWLockExclusive(&lock_);
        return erased;
    }

private:
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::unordered_map<K, V>* map_ = nullptr;
};

Records<hipblasLtMatmulDesc_t, DescRecord> g_descs;
Records<hipblasLtMatrixLayout_t, LayoutRecord> g_layouts;

template <typename T>
bool value_of(const void* buf, size_t size, T* out) {
    if (buf == nullptr || size != sizeof(T)) return false;
    std::memcpy(out, buf, sizeof(T));
    return true;
}

// A descriptor attribute that AMD's library accepted, entered in the record.
void enter_desc_attr(DescRecord& r, int attr, const void* buf, size_t size) {
    int32_t i32 = 0;
    uint32_t u32 = 0;
    void* ptr = nullptr;
    switch (attr) {
    case HIPBLASLT_MATMUL_DESC_TRANSA:
        r.changed.mark(attr, !value_of(buf, size, &r.transa));
        return;
    case HIPBLASLT_MATMUL_DESC_TRANSB:
        r.changed.mark(attr, !value_of(buf, size, &r.transb));
        return;
    case HIPBLASLT_MATMUL_DESC_EPILOGUE:
        r.changed.mark(attr, !value_of(buf, size, &u32) || u32 != HIPBLASLT_EPILOGUE_DEFAULT);
        return;
    case HIPBLASLT_MATMUL_DESC_POINTER_MODE:
        r.changed.mark(attr, !value_of(buf, size, &i32) || i32 != HIPBLASLT_POINTER_MODE_HOST);
        return;
    case HIPBLASLT_MATMUL_DESC_A_SCALE_MODE:
    case HIPBLASLT_MATMUL_DESC_B_SCALE_MODE:
        r.changed.mark(attr, !value_of(buf, size, &i32) || i32 != HIPBLASLT_MATMUL_MATRIX_SCALE_SCALAR_32F);
        return;
    case HIPBLASLT_MATMUL_DESC_SM_COUNT_TARGET:
        r.changed.mark(attr, !value_of(buf, size, &i32) || i32 != 0);
        return;
    case HIPBLASLT_MATMUL_DESC_BIAS_POINTER:
    case HIPBLASLT_MATMUL_DESC_A_SCALE_POINTER:
    case HIPBLASLT_MATMUL_DESC_B_SCALE_POINTER:
    case HIPBLASLT_MATMUL_DESC_C_SCALE_POINTER:
    case HIPBLASLT_MATMUL_DESC_D_SCALE_POINTER:
    case HIPBLASLT_MATMUL_DESC_EPILOGUE_AUX_SCALE_POINTER:
    case HIPBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER:
    case HIPBLASLT_MATMUL_DESC_AMAX_D_POINTER:
    case HIPBLASLT_MATMUL_DESC_FUSED_EPILOGUE:
        r.changed.mark(attr, !value_of(buf, size, &ptr) || ptr != nullptr);
        return;
    default:
        // An attribute that the served case does not know: the descriptor is outside it.
        r.changed.mark(attr, true);
        return;
    }
}

// A layout attribute that AMD's library accepted, entered in the record.
void enter_layout_attr(LayoutRecord& r, int attr, const void* buf, size_t size) {
    int64_t i64 = 0;
    switch (attr) {
    case HIPBLASLT_MATRIX_LAYOUT_BATCH_COUNT:
        r.changed.mark(attr, !value_of(buf, size, &r.batch));
        return;
    case HIPBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET:
        // Unused at batch count 1, which the served case requires.
        r.changed.mark(attr, !value_of(buf, size, &i64));
        return;
    case HIPBLASLT_MATRIX_LAYOUT_TYPE:
        r.changed.mark(attr, !value_of(buf, size, &r.type));
        return;
    case HIPBLASLT_MATRIX_LAYOUT_ORDER:
        r.changed.mark(attr, !value_of(buf, size, &r.order));
        return;
    case HIPBLASLT_MATRIX_LAYOUT_ROWS:
        r.changed.mark(attr, !value_of(buf, size, &r.rows));
        return;
    case HIPBLASLT_MATRIX_LAYOUT_COLS:
        r.changed.mark(attr, !value_of(buf, size, &r.cols));
        return;
    case HIPBLASLT_MATRIX_LAYOUT_LD:
        r.changed.mark(attr, !value_of(buf, size, &r.ld));
        return;
    default:
        // Batch mode, offset, the grouped arrays and anything newer: outside the served case.
        r.changed.mark(attr, true);
        return;
    }
}

// rocblas.dll, loaded from the directory of this DLL on the first served matmul.
using rocblas_create_handle_fn = rocblas_status (*)(rocblas_handle*);
using rocblas_destroy_handle_fn = rocblas_status (*)(rocblas_handle);
using rocblas_set_stream_fn = rocblas_status (*)(rocblas_handle, hipStream_t);
using rocblas_set_pointer_mode_fn = rocblas_status (*)(rocblas_handle, rocblas_pointer_mode);
using rocblas_gemm_ex_fn = rocblas_status (*)(
    rocblas_handle, rocblas_operation, rocblas_operation, rocblas_int, rocblas_int, rocblas_int,
    const void*, const void*, rocblas_datatype, rocblas_int, const void*, rocblas_datatype, rocblas_int,
    const void*, const void*, rocblas_datatype, rocblas_int, void*, rocblas_datatype, rocblas_int,
    rocblas_datatype, rocblas_gemm_algo, int32_t, uint32_t);

struct Rocblas {
    rocblas_create_handle_fn create = nullptr;
    rocblas_destroy_handle_fn destroy = nullptr;
    rocblas_set_stream_fn set_stream = nullptr;
    rocblas_set_pointer_mode_fn set_pointer_mode = nullptr;
    rocblas_gemm_ex_fn gemm_ex = nullptr;
    bool ok = false;
};
INIT_ONCE g_rocblas_once = INIT_ONCE_STATIC_INIT;
Rocblas g_rocblas;

bool own_directory(wchar_t* out, DWORD out_chars) {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_rocblas), &self)) {
        return false;
    }
    const DWORD n = GetModuleFileNameW(self, out, out_chars);
    if (n == 0 || n >= out_chars) {
        return false;
    }
    for (DWORD i = n; i > 0; --i) {
        if (out[i - 1] == L'\\' || out[i - 1] == L'/') {
            out[i] = L'\0';
            return true;
        }
    }
    return false;
}

template <typename T>
T entry(HMODULE module, const char* name) {
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

BOOL CALLBACK load_rocblas(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t path[MAX_PATH + 16];
    if (!own_directory(path, MAX_PATH)) {
        log_line("the directory of libhipblaslt.dll is unknown; rocblas.dll is not loaded");
        return TRUE;
    }
    wcscat_s(path, L"rocblas.dll");
    // The rocblas.dll beside this DLL, and its own dependencies from its directory.
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) {
        char text[96];
        std::snprintf(text, sizeof(text), "rocblas.dll beside libhipblaslt.dll does not load (error %lu)",
                      GetLastError());
        log_line(text);
        return TRUE;
    }
    g_rocblas.create = entry<rocblas_create_handle_fn>(module, "rocblas_create_handle");
    g_rocblas.destroy = entry<rocblas_destroy_handle_fn>(module, "rocblas_destroy_handle");
    g_rocblas.set_stream = entry<rocblas_set_stream_fn>(module, "rocblas_set_stream");
    g_rocblas.set_pointer_mode = entry<rocblas_set_pointer_mode_fn>(module, "rocblas_set_pointer_mode");
    g_rocblas.gemm_ex = entry<rocblas_gemm_ex_fn>(module, "rocblas_gemm_ex");
    g_rocblas.ok = g_rocblas.create && g_rocblas.destroy && g_rocblas.set_stream && g_rocblas.set_pointer_mode &&
                   g_rocblas.gemm_ex;
    if (!g_rocblas.ok) {
        log_line("rocblas.dll lacks an entry point that this front calls");
    }
    return TRUE;
}

const Rocblas* rocblas() {
    InitOnceExecuteOnce(&g_rocblas_once, load_rocblas, nullptr, nullptr);
    return g_rocblas.ok ? &g_rocblas : nullptr;
}

// One rocBLAS handle per hipBLASLt handle. The lock of an entry covers set_stream and gemm_ex, so
// that two threads on one hipBLASLt handle cannot swap each other's stream.
struct HandleEntry {
    rocblas_handle handle = nullptr;
    SRWLOCK lock = SRWLOCK_INIT;
};
SRWLOCK g_handle_lock = SRWLOCK_INIT;
std::unordered_map<hipblasLtHandle_t, HandleEntry*>* g_handles = nullptr;

HandleEntry* handle_entry(hipblasLtHandle_t lt, const Rocblas* rb) {
    AcquireSRWLockExclusive(&g_handle_lock);
    if (g_handles == nullptr) {
        g_handles = new std::unordered_map<hipblasLtHandle_t, HandleEntry*>();
    }
    HandleEntry* entry_ptr = nullptr;
    auto it = g_handles->find(lt);
    if (it != g_handles->end()) {
        entry_ptr = it->second;
    } else {
        rocblas_handle h = nullptr;
        ++t_inside_rocblas;
        rocblas_status st = rb->create(&h);
        if (st == rocblas_status_success) {
            st = rb->set_pointer_mode(h, rocblas_pointer_mode_host);
            if (st != rocblas_status_success) {
                rb->destroy(h);
                h = nullptr;
            }
        }
        --t_inside_rocblas;
        if (h != nullptr) {
            entry_ptr = new HandleEntry();
            entry_ptr->handle = h;
            (*g_handles)[lt] = entry_ptr;
            count(kHandlesLive);
        }
    }
    ReleaseSRWLockExclusive(&g_handle_lock);
    return entry_ptr;
}

// The problem of a served case, in the arguments of rocblas_gemm_ex.
struct Plan {
    rocblas_operation ta;
    rocblas_operation tb;
    rocblas_int m, n, k, lda, ldb, ldc, ldd;
};

// A column-major matrix of rows x cols with leading dimension ld, each in [1, INT32_MAX] and
// ld >= rows (hipBLASLt's own rule for HIPBLASLT_ORDER_COL), with no other attribute changed.
bool fits(const LayoutRecord& l, uint32_t type) {
    const uint64_t kMax = 0x7fffffffu;
    return !l.changed.any() && l.type == type && l.order == HIPBLASLT_ORDER_COL && l.batch == 1 && l.rows >= 1 &&
           l.rows <= kMax && l.cols >= 1 && l.cols <= kMax && l.ld >= static_cast<int64_t>(l.rows) &&
           l.ld <= static_cast<int64_t>(kMax);
}

bool op_of(int32_t value, rocblas_operation* out) {
    if (value == HIPBLAS_OP_N) {
        *out = rocblas_operation_none;
        return true;
    }
    if (value == HIPBLAS_OP_T) {
        *out = rocblas_operation_transpose;
        return true;
    }
    return false;
}

// True when the problem is the served case I8I32; *plan then holds its rocblas_gemm_ex arguments.
bool plan_case(hipblasLtMatmulDesc_t desc, hipblasLtMatrixLayout_t a, hipblasLtMatrixLayout_t b,
               hipblasLtMatrixLayout_t c, hipblasLtMatrixLayout_t d, Plan* plan) {
    DescRecord r;
    if (desc == nullptr || !g_descs.get(desc, &r) || r.compute != HIPBLAS_COMPUTE_32I || r.scale != HIP_R_32I ||
        r.changed.any() || !op_of(r.transa, &plan->ta) || !op_of(r.transb, &plan->tb)) {
        return false;
    }
    LayoutRecord la, lb, lc, ld;
    if (a == nullptr || b == nullptr || c == nullptr || d == nullptr || !g_layouts.get(a, &la) ||
        !g_layouts.get(b, &lb) || !g_layouts.get(c, &lc) || !g_layouts.get(d, &ld)) {
        return false;
    }
    if (!fits(la, HIP_R_8I) || !fits(lb, HIP_R_8I) || !fits(lc, HIP_R_32I) || !fits(ld, HIP_R_32I)) {
        return false;
    }
    // D = op(A) op(B) + C, all column major: D and C are m x n, op(A) m x k, op(B) k x n.
    const uint64_t m = ld.rows, n = ld.cols;
    if (lc.rows != m || lc.cols != n) {
        return false;
    }
    const bool ta = plan->ta == rocblas_operation_transpose;
    const bool tb = plan->tb == rocblas_operation_transpose;
    const uint64_t a_m = ta ? la.cols : la.rows;
    const uint64_t k = ta ? la.rows : la.cols;
    const uint64_t b_k = tb ? lb.cols : lb.rows;
    const uint64_t b_n = tb ? lb.rows : lb.cols;
    if (a_m != m || b_k != k || b_n != n) {
        return false;
    }
    plan->m = static_cast<rocblas_int>(m);
    plan->n = static_cast<rocblas_int>(n);
    plan->k = static_cast<rocblas_int>(k);
    plan->lda = static_cast<rocblas_int>(la.ld);
    plan->ldb = static_cast<rocblas_int>(lb.ld);
    plan->ldc = static_cast<rocblas_int>(lc.ld);
    plan->ldd = static_cast<rocblas_int>(ld.ld);
    return true;
}

bool is_marker(const hipblasLtMatmulAlgo_t* algo) {
    if (algo == nullptr || std::memcmp(algo->data, kMarker, sizeof(kMarker)) != 0) {
        return false;
    }
    for (size_t i = sizeof(kMarker); i < sizeof(algo->data); ++i) {
        if (algo->data[i] != 0) {
            return false;
        }
    }
    return true;
}

// AMD's answer for a served case, asked once per transpose pair: 0 not asked, 1 AMD answers,
// 2 AMD refused.
volatile LONG g_amd_answer[2][2];

hipblasStatus_t status_of(rocblas_status st) {
    switch (static_cast<int>(st)) {
    case 0: return HIPBLAS_STATUS_SUCCESS;
    case 1: return HIPBLAS_STATUS_NOT_INITIALIZED;   // invalid_handle
    case 2: return HIPBLAS_STATUS_NOT_SUPPORTED;     // not_implemented
    case 3: return HIPBLAS_STATUS_INVALID_VALUE;     // invalid_pointer
    case 4: return HIPBLAS_STATUS_INVALID_VALUE;     // invalid_size
    case 5: return HIPBLAS_STATUS_ALLOC_FAILED;      // memory_error
    case 11: return HIPBLAS_STATUS_INVALID_VALUE;    // invalid_value
    case 14: return HIPBLAS_STATUS_NOT_SUPPORTED;    // excluded_from_build
    case 15: return HIPBLAS_STATUS_ARCH_MISMATCH;    // arch_mismatch
    default: return HIPBLAS_STATUS_EXECUTION_FAILED;
    }
}

}  // namespace

extern "C" {

hipblasStatus_t bc250lt_hipblasLtDestroy(const hipblasLtHandle_t handle) {
    HandleEntry* gone = nullptr;
    AcquireSRWLockExclusive(&g_handle_lock);
    if (g_handles != nullptr) {
        auto it = g_handles->find(handle);
        if (it != g_handles->end()) {
            gone = it->second;
            g_handles->erase(it);
        }
    }
    ReleaseSRWLockExclusive(&g_handle_lock);
    if (gone != nullptr) {
        const Rocblas* rb = rocblas();
        if (rb != nullptr) {
            ++t_inside_rocblas;
            rb->destroy(gone->handle);
            --t_inside_rocblas;
        }
        delete gone;
        count(kHandlesLive, -1);
    }
    return hipblasLtDestroy(handle);
}

hipblasStatus_t bc250lt_hipblasLtMatmulDescCreate(hipblasLtMatmulDesc_t* desc, hipblasComputeType_t computeType,
                                                  hipDataType scaleType) {
    const hipblasStatus_t st = hipblasLtMatmulDescCreate(desc, computeType, scaleType);
    if (st == HIPBLAS_STATUS_SUCCESS && desc != nullptr && *desc != nullptr) {
        DescRecord r;
        r.compute = computeType;
        r.scale = scaleType;
        if (g_descs.put(*desc, r)) count(kDescsLive);
    }
    return st;
}

// The record goes before AMD frees the descriptor: once it is freed, another thread can get the same
// address from a create, and its record must not be erased here.
hipblasStatus_t bc250lt_hipblasLtMatmulDescDestroy(const hipblasLtMatmulDesc_t desc) {
    if (g_descs.erase(desc)) count(kDescsLive, -1);
    return hipblasLtMatmulDescDestroy(desc);
}

hipblasStatus_t bc250lt_hipblasLtMatmulDescSetAttribute(hipblasLtMatmulDesc_t desc, hipblasLtMatmulDescAttributes_t attr,
                                                        const void* buf, size_t sizeInBytes) {
    const hipblasStatus_t st = hipblasLtMatmulDescSetAttribute(desc, attr, buf, sizeInBytes);
    if (st == HIPBLAS_STATUS_SUCCESS) {
        g_descs.update(desc, [&](DescRecord& r) { enter_desc_attr(r, static_cast<int>(attr), buf, sizeInBytes); });
    }
    return st;
}

hipblasStatus_t bc250lt_hipblasLtMatrixLayoutCreate(hipblasLtMatrixLayout_t* layout, hipDataType type, uint64_t rows,
                                                    uint64_t cols, int64_t ld) {
    const hipblasStatus_t st = hipblasLtMatrixLayoutCreate(layout, type, rows, cols, ld);
    if (st == HIPBLAS_STATUS_SUCCESS && layout != nullptr && *layout != nullptr) {
        LayoutRecord r;
        r.type = static_cast<uint32_t>(type);
        r.rows = rows;
        r.cols = cols;
        r.ld = ld;
        if (g_layouts.put(*layout, r)) count(kLayoutsLive);
    }
    return st;
}

hipblasStatus_t bc250lt_hipblasLtMatrixLayoutDestroy(const hipblasLtMatrixLayout_t layout) {
    if (g_layouts.erase(layout)) count(kLayoutsLive, -1);
    return hipblasLtMatrixLayoutDestroy(layout);
}

hipblasStatus_t bc250lt_hipblasLtMatrixLayoutSetAttribute(hipblasLtMatrixLayout_t layout,
                                                          hipblasLtMatrixLayoutAttribute_t attr, const void* buf,
                                                          size_t sizeInBytes) {
    const hipblasStatus_t st = hipblasLtMatrixLayoutSetAttribute(layout, attr, buf, sizeInBytes);
    if (st == HIPBLAS_STATUS_SUCCESS) {
        g_layouts.update(layout, [&](LayoutRecord& r) { enter_layout_attr(r, static_cast<int>(attr), buf, sizeInBytes); });
    }
    return st;
}

hipblasStatus_t bc250lt_hipblasLtMatmulAlgoGetHeuristic(
    hipblasLtHandle_t handle, hipblasLtMatmulDesc_t matmulDesc, hipblasLtMatrixLayout_t Adesc,
    hipblasLtMatrixLayout_t Bdesc, hipblasLtMatrixLayout_t Cdesc, hipblasLtMatrixLayout_t Ddesc,
    hipblasLtMatmulPreference_t pref, int requestedAlgoCount,
    hipblasLtMatmulHeuristicResult_t heuristicResultsArray[], int* returnAlgoCount) {
    count(kHeuristicCalls);
    settings();
    Plan plan;
    if (!g_serving || t_inside_rocblas != 0 || !plan_case(matmulDesc, Adesc, Bdesc, Cdesc, Ddesc, &plan)) {
        return hipblasLtMatmulAlgoGetHeuristic(handle, matmulDesc, Adesc, Bdesc, Cdesc, Ddesc, pref,
                                               requestedAlgoCount, heuristicResultsArray, returnAlgoCount);
    }
    const int ia = plan.ta == rocblas_operation_transpose ? 1 : 0;
    const int ib = plan.tb == rocblas_operation_transpose ? 1 : 0;
    if (g_amd_answer[ia][ib] != 2) {
        const hipblasStatus_t st = hipblasLtMatmulAlgoGetHeuristic(handle, matmulDesc, Adesc, Bdesc, Cdesc, Ddesc,
                                                                   pref, requestedAlgoCount, heuristicResultsArray,
                                                                   returnAlgoCount);
        if (st == HIPBLAS_STATUS_SUCCESS && returnAlgoCount != nullptr && *returnAlgoCount > 0) {
            InterlockedExchange(&g_amd_answer[ia][ib], 1);
            count(kHeuristicAmdServed);
            return st;
        }
        if (InterlockedExchange(&g_amd_answer[ia][ib], 2) != 2) {
            count(kHeuristicAmdRefused);
            char text[160];
            std::snprintf(text, sizeof(text),
                          "AMD's hipBLASLt has no algo for i8 x i8 -> i32 (trans %c%c): status %d, %d algos; "
                          "this front serves the case through rocblas_gemm_ex",
                          ia ? 'T' : 'N', ib ? 'T' : 'N', static_cast<int>(st),
                          returnAlgoCount != nullptr ? *returnAlgoCount : -1);
            log_line(text);
        }
    }
    if (requestedAlgoCount < 1 || heuristicResultsArray == nullptr || returnAlgoCount == nullptr) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    hipblasLtMatmulHeuristicResult_t& r = heuristicResultsArray[0];
    std::memset(&r, 0, sizeof(r));
    std::memcpy(r.algo.data, kMarker, sizeof(kMarker));
    r.algo.max_workspace_bytes = 0;
    r.workspaceSize = 0;
    r.state = HIPBLAS_STATUS_SUCCESS;
    r.wavesCount = 1.0f;
    *returnAlgoCount = 1;
    count(kHeuristicServed);
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t bc250lt_hipblasLtMatmul(hipblasLtHandle_t handle, hipblasLtMatmulDesc_t matmulDesc, const void* alpha,
                                        const void* A, hipblasLtMatrixLayout_t Adesc, const void* B,
                                        hipblasLtMatrixLayout_t Bdesc, const void* beta, const void* C,
                                        hipblasLtMatrixLayout_t Cdesc, void* D, hipblasLtMatrixLayout_t Ddesc,
                                        const hipblasLtMatmulAlgo_t* algo, void* workspace, size_t workspaceSizeInBytes,
                                        hipStream_t stream) {
    count(kMatmulCalls);
    if (!is_marker(algo)) {
        return hipblasLtMatmul(handle, matmulDesc, alpha, A, Adesc, B, Bdesc, beta, C, Cdesc, D, Ddesc, algo, workspace,
                               workspaceSizeInBytes, stream);
    }
    // An algo of this front never goes to AMD's library, which would not know it.
    settings();
    Plan plan;
    if (t_inside_rocblas != 0 || handle == nullptr || !plan_case(matmulDesc, Adesc, Bdesc, Cdesc, Ddesc, &plan) ||
        alpha == nullptr || beta == nullptr || A == nullptr || B == nullptr || D == nullptr ||
        (C == nullptr && *static_cast<const int32_t*>(beta) != 0) || (C == D && plan.ldc != plan.ldd)) {
        count(kMatmulRefused);
        log_line("hipblasLtMatmul with an algo of this front for a problem that is not the served case: refused");
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    const Rocblas* rb = rocblas();
    HandleEntry* he = rb != nullptr ? handle_entry(handle, rb) : nullptr;
    if (he == nullptr) {
        count(kRocblasErrors);
        log_line("no rocBLAS handle for the served matmul");
        return HIPBLAS_STATUS_NOT_INITIALIZED;
    }
    AcquireSRWLockExclusive(&he->lock);
    ++t_inside_rocblas;
    rocblas_status st = rb->set_stream(he->handle, stream);
    if (st == rocblas_status_success) {
        st = rb->gemm_ex(he->handle, plan.ta, plan.tb, plan.m, plan.n, plan.k, alpha, A, rocblas_datatype_i8_r,
                         plan.lda, B, rocblas_datatype_i8_r, plan.ldb, beta, C != nullptr ? C : D,
                         rocblas_datatype_i32_r, C != nullptr ? plan.ldc : plan.ldd, D, rocblas_datatype_i32_r,
                         plan.ldd, rocblas_datatype_i32_r, rocblas_gemm_algo_standard, 0, rocblas_gemm_flags_none);
    }
    --t_inside_rocblas;
    ReleaseSRWLockExclusive(&he->lock);
    if (st != rocblas_status_success) {
        count(kRocblasErrors);
        char text[96];
        std::snprintf(text, sizeof(text), "rocblas_gemm_ex for the served matmul returns %d", static_cast<int>(st));
        log_line(text);
        return status_of(st);
    }
    count(kMatmulServed);
    return HIPBLAS_STATUS_SUCCESS;
}

// The counters of this front. Returns 0, or -1 when out is null or bytes is smaller than the
// version 2 structure.
int bc250lt_get_counters(bc250lt_counters* out, size_t bytes) {
    static_assert(sizeof(bc250lt_counters) == 8 + 8 * kCountNumber, "one uint64_t per Count");
    if (out == nullptr || bytes < sizeof(bc250lt_counters)) {
        return -1;
    }
    std::memset(out, 0, sizeof(*out));
    out->version = 2;
    out->bytes = static_cast<uint32_t>(sizeof(*out));
    uint64_t* fields = &out->heuristic_calls;
    for (int i = 0; i < kCountNumber; ++i) {
        fields[i] = static_cast<uint64_t>(InterlockedCompareExchange64(&g_counts[i], 0, 0));
    }
    return 0;
}

}  // extern "C"
