// blaslt_fake_amd.cpp - a stand-in for AMD's hipBLASLt (libhipblaslt_amd.dll) in test_blaslt_front.
//
// It keeps descriptors and layouts in memory with the defaults of hipblaslt.h (TRANSA and TRANSB
// N, epilogue DEFAULT, pointer mode HOST, no pointers, scale modes SCALAR_32F, column order, batch
// count 1). GetAttribute answers as TheRock's hipBLASLt 1.5.0 does (MEASURED, kit3-testlog t33d):
// HIPBLAS_STATUS_INVALID_VALUE for the layout's TYPE, ORDER, ROWS, COLS and LD and for the
// descriptor's C and D scale pointers and A and B scale modes, the stored bytes for the rest. A front
// that reads those back fails the served cases of the test. Its heuristic has two modes, set by
// fake_amd_set_mode: 0 answers as TheRock's library does for gfx1010 (HIPBLAS_STATUS_INVALID_VALUE
// and no algo), 1 returns one algo whose data[0] is 0x77. Its matmul with that algo writes 0x7777
// into the first element of D (host memory in the test). fake_amd_get_counts reads its call counts.
// One data export carries a name of AMD's table, so that the test can check a forwarded data export.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "blaslt_abi.h"

namespace {

using Bytes = std::vector<uint8_t>;

template <typename T>
Bytes bytes_of(T v) {
    Bytes b(sizeof(T));
    std::memcpy(b.data(), &v, sizeof(T));
    return b;
}

struct Attrs {
    std::map<int, Bytes> values;
};

struct FakeCounts {
    long create, destroy, heuristic, matmul, desc_create, desc_destroy, layout_create, layout_destroy;
};

FakeCounts g_counts;
int g_mode = 0;

hipblasStatus_t set_attr(Attrs* a, int attr, const void* buf, size_t size) {
    if (a == nullptr || buf == nullptr) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    auto it = a->values.find(attr);
    if (it == a->values.end() || it->second.size() != size) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    std::memcpy(it->second.data(), buf, size);
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t get_attr(Attrs* a, int attr, void* buf, size_t size, size_t* written) {
    if (a == nullptr) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    auto it = a->values.find(attr);
    if (it == a->values.end()) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    if (written != nullptr) {
        *written = it->second.size();
    }
    if (buf == nullptr || size < it->second.size()) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    std::memcpy(buf, it->second.data(), it->second.size());
    return HIPBLAS_STATUS_SUCCESS;
}

}  // namespace

extern "C" {

// Exported under "??_7hipblaslt_internal_ostream@@6B@" (blaslt_fake_amd.def).
const void* fake_vtable[2] = {nullptr, nullptr};

void fake_amd_set_mode(int mode) { g_mode = mode; }
void fake_amd_get_counts(FakeCounts* out) { *out = g_counts; }

hipblasStatus_t fake_hipblasLtCreate(hipblasLtHandle_t* handle) {
    if (handle == nullptr) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    *handle = new int(1);
    ++g_counts.create;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtDestroy(const hipblasLtHandle_t handle) {
    delete static_cast<int*>(handle);
    ++g_counts.destroy;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtGetVersion(hipblasLtHandle_t, int* version) {
    *version = 4242;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatmulDescCreate(hipblasLtMatmulDesc_t* desc, hipblasComputeType_t, hipDataType) {
    Attrs* a = new Attrs();
    a->values[HIPBLASLT_MATMUL_DESC_TRANSA] = bytes_of<int32_t>(HIPBLAS_OP_N);
    a->values[HIPBLASLT_MATMUL_DESC_TRANSB] = bytes_of<int32_t>(HIPBLAS_OP_N);
    a->values[HIPBLASLT_MATMUL_DESC_EPILOGUE] = bytes_of<uint32_t>(HIPBLASLT_EPILOGUE_DEFAULT);
    a->values[HIPBLASLT_MATMUL_DESC_POINTER_MODE] = bytes_of<int32_t>(HIPBLASLT_POINTER_MODE_HOST);
    a->values[HIPBLASLT_MATMUL_DESC_A_SCALE_MODE] = bytes_of<int32_t>(HIPBLASLT_MATMUL_MATRIX_SCALE_SCALAR_32F);
    a->values[HIPBLASLT_MATMUL_DESC_B_SCALE_MODE] = bytes_of<int32_t>(HIPBLASLT_MATMUL_MATRIX_SCALE_SCALAR_32F);
    a->values[HIPBLASLT_MATMUL_DESC_SM_COUNT_TARGET] = bytes_of<int32_t>(0);
    for (int attr : {HIPBLASLT_MATMUL_DESC_BIAS_POINTER, HIPBLASLT_MATMUL_DESC_A_SCALE_POINTER,
                     HIPBLASLT_MATMUL_DESC_B_SCALE_POINTER, HIPBLASLT_MATMUL_DESC_C_SCALE_POINTER,
                     HIPBLASLT_MATMUL_DESC_D_SCALE_POINTER, HIPBLASLT_MATMUL_DESC_AMAX_D_POINTER}) {
        a->values[attr] = bytes_of<void*>(nullptr);
    }
    *desc = reinterpret_cast<hipblasLtMatmulDesc_t>(a);
    ++g_counts.desc_create;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatmulDescDestroy(const hipblasLtMatmulDesc_t desc) {
    delete reinterpret_cast<Attrs*>(desc);
    ++g_counts.desc_destroy;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatmulDescSetAttribute(hipblasLtMatmulDesc_t desc, hipblasLtMatmulDescAttributes_t attr,
                                                     const void* buf, size_t size) {
    return set_attr(reinterpret_cast<Attrs*>(desc), attr, buf, size);
}

hipblasStatus_t fake_hipblasLtMatmulDescGetAttribute(hipblasLtMatmulDesc_t desc, hipblasLtMatmulDescAttributes_t attr,
                                                     void* buf, size_t size, size_t* written) {
    if (attr == HIPBLASLT_MATMUL_DESC_C_SCALE_POINTER || attr == HIPBLASLT_MATMUL_DESC_D_SCALE_POINTER) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    if (attr == HIPBLASLT_MATMUL_DESC_A_SCALE_MODE || attr == HIPBLASLT_MATMUL_DESC_B_SCALE_MODE) {
        if (written != nullptr) *written = 4;  // as measured: the size is written, the status is a refusal
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    return get_attr(reinterpret_cast<Attrs*>(desc), attr, buf, size, written);
}

hipblasStatus_t fake_hipblasLtMatrixLayoutCreate(hipblasLtMatrixLayout_t* layout, hipDataType type, uint64_t rows,
                                                 uint64_t cols, int64_t ld) {
    Attrs* a = new Attrs();
    a->values[HIPBLASLT_MATRIX_LAYOUT_TYPE] = bytes_of<uint32_t>(static_cast<uint32_t>(type));
    a->values[HIPBLASLT_MATRIX_LAYOUT_ORDER] = bytes_of<int32_t>(HIPBLASLT_ORDER_COL);
    a->values[HIPBLASLT_MATRIX_LAYOUT_BATCH_COUNT] = bytes_of<int32_t>(1);
    a->values[HIPBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET] = bytes_of<int64_t>(0);
    a->values[HIPBLASLT_MATRIX_LAYOUT_ROWS] = bytes_of<uint64_t>(rows);
    a->values[HIPBLASLT_MATRIX_LAYOUT_COLS] = bytes_of<uint64_t>(cols);
    a->values[HIPBLASLT_MATRIX_LAYOUT_LD] = bytes_of<int64_t>(ld);
    a->values[HIPBLASLT_MATRIX_LAYOUT_BATCH_MODE] = bytes_of<int32_t>(0);
    *layout = reinterpret_cast<hipblasLtMatrixLayout_t>(a);
    ++g_counts.layout_create;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatrixLayoutDestroy(const hipblasLtMatrixLayout_t layout) {
    delete reinterpret_cast<Attrs*>(layout);
    ++g_counts.layout_destroy;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatrixLayoutSetAttribute(hipblasLtMatrixLayout_t layout,
                                                       hipblasLtMatrixLayoutAttribute_t attr, const void* buf,
                                                       size_t size) {
    return set_attr(reinterpret_cast<Attrs*>(layout), attr, buf, size);
}

hipblasStatus_t fake_hipblasLtMatrixLayoutGetAttribute(hipblasLtMatrixLayout_t layout,
                                                       hipblasLtMatrixLayoutAttribute_t attr, void* buf, size_t size,
                                                       size_t* written) {
    if (attr != HIPBLASLT_MATRIX_LAYOUT_BATCH_COUNT && attr != HIPBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    return get_attr(reinterpret_cast<Attrs*>(layout), attr, buf, size, written);
}

hipblasStatus_t fake_hipblasLtMatmulPreferenceCreate(hipblasLtMatmulPreference_t* pref) {
    *pref = reinterpret_cast<hipblasLtMatmulPreference_t>(new uint64_t(0));
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatmulPreferenceDestroy(const hipblasLtMatmulPreference_t pref) {
    delete reinterpret_cast<uint64_t*>(pref);
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatmulPreferenceSetAttribute(hipblasLtMatmulPreference_t pref, int, const void* buf,
                                                           size_t size) {
    if (pref == nullptr || buf == nullptr || size != sizeof(uint64_t)) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    std::memcpy(reinterpret_cast<uint64_t*>(pref), buf, sizeof(uint64_t));
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatmulAlgoGetHeuristic(hipblasLtHandle_t, hipblasLtMatmulDesc_t, hipblasLtMatrixLayout_t,
                                                     hipblasLtMatrixLayout_t, hipblasLtMatrixLayout_t,
                                                     hipblasLtMatrixLayout_t, hipblasLtMatmulPreference_t,
                                                     int requested, hipblasLtMatmulHeuristicResult_t results[],
                                                     int* count) {
    ++g_counts.heuristic;
    if (g_mode == 0) {
        if (count != nullptr) {
            *count = 0;
        }
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    if (requested < 1 || results == nullptr || count == nullptr) {
        return HIPBLAS_STATUS_INVALID_VALUE;
    }
    std::memset(&results[0], 0, sizeof(results[0]));
    results[0].algo.data[0] = 0x77;
    results[0].state = HIPBLAS_STATUS_SUCCESS;
    results[0].wavesCount = 1.0f;
    *count = 1;
    return HIPBLAS_STATUS_SUCCESS;
}

hipblasStatus_t fake_hipblasLtMatmul(hipblasLtHandle_t, hipblasLtMatmulDesc_t, const void*, const void*,
                                     hipblasLtMatrixLayout_t, const void*, hipblasLtMatrixLayout_t, const void*,
                                     const void*, hipblasLtMatrixLayout_t, void* D, hipblasLtMatrixLayout_t,
                                     const hipblasLtMatmulAlgo_t* algo, void*, size_t, hipStream_t) {
    ++g_counts.matmul;
    if (g_mode == 1 && algo != nullptr && algo->data[0] == 0x77 && D != nullptr) {
        *static_cast<int32_t*>(D) = 0x7777;
        return HIPBLAS_STATUS_SUCCESS;
    }
    return HIPBLAS_STATUS_NOT_SUPPORTED;
}

}  // extern "C"
