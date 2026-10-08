// test_hip_mock.cpp - layer 2 over the mock bc250hsa backend (M16 route B).
//
// It checks what design docs/design/m16-hip-route-b.md section 5.4 asks of this test:
//   1. registration finds a kernel from its host stub, and a CUDA fat binary is refused;
//   2. a launch packs the arguments at the offsets of the metadata, explicit and hidden;
//   3. stream order survives an event wait;
//   4. a second run of the same work in one process leaks no allocation;
//   5. the fixed properties of design section 4.1 reach the program.
//
// It needs no GPU and no AMDGPU compiler: the code object comes from the committed fixture
// compute/hip/tests/data/hip_test_kernels.gfx1013.fatbin.
//
// Usage: test_hip_mock.exe [<path of the fatbin fixture>]

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bc250hsa.h"
#include "hip/hip_runtime.h"
#include "hipmock_backend.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("  FAIL %s\n", what);
    }
}

void check_u64(uint64_t got, uint64_t want, const char* what) {
    g_checks++;
    if (got != want) {
        g_failures++;
        std::printf("  FAIL %s: got %llu, expected %llu\n", what, (unsigned long long)got,
                    (unsigned long long)want);
    }
}

// The 24 bytes of .hipFatBinSegment, as clang writes them (MEASURED, design section 4.2).
struct FatBinWrapper {
    int32_t     magic;
    int32_t     version;
    const void* gpu_binary;
    const void* unused;
};

// Unique host addresses that stand for the kernel stubs of a compiled program.
const char g_stub_vadd = 0;
const char g_stub_scale = 0;
const char g_stub_unknown = 0;

unsigned char* read_file(const char* path, size_t* bytes) {
    FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path, "rb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path, "rb");
#endif
    if (file == nullptr) {
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(file);
        return nullptr;
    }
    unsigned char* buffer = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(size)));
    if (buffer == nullptr) {
        std::fclose(file);
        return nullptr;
    }
    const size_t got = std::fread(buffer, 1u, static_cast<size_t>(size), file);
    std::fclose(file);
    if (got != static_cast<size_t>(size)) {
        std::free(buffer);
        return nullptr;
    }
    *bytes = got;
    return buffer;
}

uint32_t arg_offset(const bc250hsa_kernel* kernel, uint16_t kind, uint32_t which) {
    uint32_t seen = 0;
    for (uint32_t i = 0; i < kernel->arg_count; ++i) {
        if (kernel->args[i].kind != kind) {
            continue;
        }
        if (seen == which) {
            return kernel->args[i].offset;
        }
        seen++;
    }
    return UINT32_MAX;
}

const bc250hsa_mock_record* first_of_kind(uint32_t kind, uint32_t* index) {
    for (uint32_t i = 0; i < bc250hsa_mock_record_count(); ++i) {
        const bc250hsa_mock_record* record = bc250hsa_mock_record_at(i);
        if (record != nullptr && record->kind == kind) {
            if (index != nullptr) {
                *index = i;
            }
            return record;
        }
    }
    return nullptr;
}

int index_of_dispatch(const char* kernel) {
    for (uint32_t i = 0; i < bc250hsa_mock_record_count(); ++i) {
        const bc250hsa_mock_record* record = bc250hsa_mock_record_at(i);
        if (record != nullptr && record->kind == BC250HSA_MOCK_DISPATCH &&
            std::strcmp(record->kernel, kernel) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint64_t rd_u64(const unsigned char* p) {
    uint64_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint32_t rd_u32(const unsigned char* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

uint16_t rd_u16(const unsigned char* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// One full run of the same work, for the leak test.
void run_sequence(int n) {
    float* a = nullptr;
    float* b = nullptr;
    float* c = nullptr;
    hipStream_t stream = nullptr;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    check(hipMalloc(reinterpret_cast<void**>(&a), bytes) == hipSuccess, "run hipMalloc a");
    check(hipMalloc(reinterpret_cast<void**>(&b), bytes) == hipSuccess, "run hipMalloc b");
    check(hipMalloc(reinterpret_cast<void**>(&c), bytes) == hipSuccess, "run hipMalloc c");
    check(hipStreamCreate(&stream) == hipSuccess, "run hipStreamCreate");
    void* args[4];
    args[0] = &a;
    args[1] = &b;
    args[2] = &c;
    args[3] = &n;
    check(hipLaunchKernel(&g_stub_vadd, dim3(static_cast<unsigned>(n) / 256u, 1u, 1u),
                          dim3(256u, 1u, 1u), args, 0, stream) == hipSuccess,
          "run hipLaunchKernel");
    check(hipStreamSynchronize(stream) == hipSuccess, "run hipStreamSynchronize");
    check(hipStreamDestroy(stream) == hipSuccess, "run hipStreamDestroy");
    check(hipFree(c) == hipSuccess, "run hipFree c");
    check(hipFree(b) == hipSuccess, "run hipFree b");
    check(hipFree(a) == hipSuccess, "run hipFree a");
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1]
                                : "compute/hip/tests/data/hip_test_kernels.gfx1013.fatbin";
    size_t fatbin_bytes = 0;
    unsigned char* fatbin = read_file(path, &fatbin_bytes);
    if (fatbin == nullptr) {
        std::printf("test_hip_mock: cannot read the fixture %s\n", path);
        return 2;
    }
    std::printf("test_hip_mock: fixture %s, %zu bytes\n", path, fatbin_bytes);

    // The same code object through the public interface, so that the test learns the argument
    // offsets from the metadata instead of repeating them.
    bc250hsa_device* probe_device = nullptr;
    check(bc250hsa_open(nullptr, &probe_device) == BC250HSA_OK, "bc250hsa_open");
    const void* image = nullptr;
    size_t image_bytes = 0;
    check(bc250hsa_unbundle(fatbin, fatbin_bytes, nullptr, &image, &image_bytes) == BC250HSA_OK,
          "bc250hsa_unbundle");
    bc250hsa_module* probe_module = nullptr;
    check(bc250hsa_module_load(probe_device, image, image_bytes, &probe_module) == BC250HSA_OK,
          "bc250hsa_module_load");
    if (probe_module == nullptr) {
        std::printf("test_hip_mock: the fixture does not load, nothing else can run\n");
        return 2;
    }
    const bc250hsa_kernel* probe_vadd = bc250hsa_module_kernel_by_name(probe_module, "vadd");
    check(probe_vadd != nullptr, "the fixture holds the kernel vadd");
    check(bc250hsa_module_kernel_by_name(probe_module, "scale") != nullptr,
          "the fixture holds the kernel scale");
    check(bc250hsa_module_kernel_count(probe_module) == 3u, "the fixture holds three kernels");
    if (probe_vadd == nullptr) {
        return 2;
    }
    // MEASURED values of this fixture, as a second opinion on the reader: four explicit
    // arguments, 288 bytes in all, and 16 bytes of alignment after max(8, 16).
    check_u64(probe_vadd->kernarg_bytes, 288u, "vadd kernarg bytes");
    check_u64(probe_vadd->kernarg_align, 16u, "vadd kernarg alignment");
    check_u64(probe_vadd->explicit_arg_count, 4u, "vadd explicit argument count");
    check_u64(arg_offset(probe_vadd, BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X, 0), 32u,
              "vadd hidden_block_count_x offset");
    check_u64(arg_offset(probe_vadd, BC250HSA_ARG_HIDDEN_GROUP_SIZE_X, 0), 44u,
              "vadd hidden_group_size_x offset");

    const uint32_t ptr0 = arg_offset(probe_vadd, BC250HSA_ARG_GLOBAL_BUFFER, 0);
    const uint32_t ptr1 = arg_offset(probe_vadd, BC250HSA_ARG_GLOBAL_BUFFER, 1);
    const uint32_t ptr2 = arg_offset(probe_vadd, BC250HSA_ARG_GLOBAL_BUFFER, 2);
    const uint32_t scalar = arg_offset(probe_vadd, BC250HSA_ARG_BY_VALUE, 0);
    const uint32_t count_x = arg_offset(probe_vadd, BC250HSA_ARG_HIDDEN_BLOCK_COUNT_X, 0);
    const uint32_t group_x = arg_offset(probe_vadd, BC250HSA_ARG_HIDDEN_GROUP_SIZE_X, 0);

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 1. registration\n");
    FatBinWrapper wrapper;
    wrapper.magic = static_cast<int32_t>(0x48495046);  // 'HIPF'
    wrapper.version = 1;
    wrapper.gpu_binary = fatbin;
    wrapper.unused = nullptr;
    void** handle = __hipRegisterFatBinary(&wrapper);
    check(handle != nullptr, "__hipRegisterFatBinary accepts a HIP wrapper");

    FatBinWrapper cuda_wrapper = wrapper;
    cuda_wrapper.magic = static_cast<int32_t>(0x466243B1);
    check(__hipRegisterFatBinary(&cuda_wrapper) == nullptr,
          "__hipRegisterFatBinary refuses a CUDA wrapper");

    check(__hipRegisterFunction(handle, &g_stub_vadd, const_cast<char*>("vadd"), "vadd", -1,
                                nullptr, nullptr, nullptr, nullptr, nullptr) == 0,
          "__hipRegisterFunction vadd");
    check(__hipRegisterFunction(handle, &g_stub_scale, const_cast<char*>("scale"), "scale", -1,
                                nullptr, nullptr, nullptr, nullptr, nullptr) == 0,
          "__hipRegisterFunction scale");

    hipDeviceProp_t prop;
    std::memset(&prop, 0, sizeof(prop));
    check(hipGetDeviceProperties(&prop, 0) == hipSuccess, "hipGetDeviceProperties");
    check(std::strcmp(prop.gcnArchName, "gfx1013") == 0, "gcnArchName is gfx1013");
    check_u64(static_cast<uint64_t>(prop.warpSize), 32u, "warpSize");
    check_u64(prop.sharedMemPerBlock, 65536u, "sharedMemPerBlock");

    // A stub that nothing registered must not reach the device.
    check(hipLaunchKernel(&g_stub_unknown, dim3(1u, 1u, 1u), dim3(1u, 1u, 1u), nullptr, 0,
                          nullptr) == hipErrorInvalidDeviceFunction,
          "an unregistered stub is refused");
    check(hipGetLastError() == hipErrorInvalidDeviceFunction, "the error state holds the refusal");
    check(hipGetLastError() == hipSuccess, "hipGetLastError clears the error state");

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 2. argument packing\n");
    float* device_a = nullptr;
    float* device_b = nullptr;
    float* device_c = nullptr;
    const int n = 4096;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    check(hipMalloc(reinterpret_cast<void**>(&device_a), bytes) == hipSuccess, "hipMalloc a");
    check(hipMalloc(reinterpret_cast<void**>(&device_b), bytes) == hipSuccess, "hipMalloc b");
    check(hipMalloc(reinterpret_cast<void**>(&device_c), bytes) == hipSuccess, "hipMalloc c");

    bc250hsa_mock_reset();
    void* args[4];
    args[0] = &device_a;
    args[1] = &device_b;
    args[2] = &device_c;
    args[3] = const_cast<int*>(&n);
    check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                          nullptr) == hipSuccess,
          "hipLaunchKernel vadd");
    uint32_t index = 0;
    const bc250hsa_mock_record* dispatch = first_of_kind(BC250HSA_MOCK_DISPATCH, &index);
    check(dispatch != nullptr, "the launch reached the device as one dispatch");
    if (dispatch != nullptr) {
        check(std::strcmp(dispatch->kernel, "vadd") == 0, "the dispatch names the kernel vadd");
        check_u64(dispatch->grid[0], 16u, "grid x");
        check_u64(dispatch->grid[1], 1u, "grid y");
        check_u64(dispatch->block[0], 256u, "block x");
        check_u64(dispatch->kernarg_bytes, 288u, "the packed kernel argument bytes");
        check_u64(rd_u64(dispatch->kernarg + ptr0),
                  static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device_a)), "argument a");
        check_u64(rd_u64(dispatch->kernarg + ptr1),
                  static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device_b)), "argument b");
        check_u64(rd_u64(dispatch->kernarg + ptr2),
                  static_cast<uint64_t>(reinterpret_cast<uintptr_t>(device_c)), "argument c");
        check_u64(rd_u32(dispatch->kernarg + scalar), static_cast<uint64_t>(n), "argument n");
        check_u64(rd_u32(dispatch->kernarg + count_x), 16u, "hidden block count x");
        check_u64(rd_u16(dispatch->kernarg + group_x), 256u, "hidden group size x");
        check(dispatch->va % probe_vadd->kernarg_align == 0u,
              "the kernel argument buffer is aligned");
    }

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 3. stream order across an event wait\n");
    hipStream_t first = nullptr;
    hipStream_t second = nullptr;
    hipEvent_t done = nullptr;
    check(hipStreamCreate(&first) == hipSuccess, "hipStreamCreate first");
    check(hipStreamCreate(&second) == hipSuccess, "hipStreamCreate second");
    check(hipEventCreate(&done) == hipSuccess, "hipEventCreate");

    bc250hsa_mock_reset();
    check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0, first) ==
              hipSuccess,
          "launch on the first stream");
    check(hipEventRecord(done, first) == hipSuccess, "hipEventRecord on the first stream");
    check(hipStreamWaitEvent(second, done, 0) == hipSuccess, "hipStreamWaitEvent on the second");
    void* scale_args[3];
    float factor = 1.0f;
    scale_args[0] = &device_c;
    scale_args[1] = &factor;
    scale_args[2] = const_cast<int*>(&n);
    check(hipLaunchKernel(&g_stub_scale, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), scale_args, 0,
                          second) == hipSuccess,
          "launch on the second stream");

    const int at_vadd = index_of_dispatch("vadd");
    const int at_scale = index_of_dispatch("scale");
    uint32_t at_wait = 0;
    const bc250hsa_mock_record* wait_record = first_of_kind(BC250HSA_MOCK_WAIT, &at_wait);
    check(at_vadd >= 0 && at_scale >= 0, "both dispatches reached the device");
    check(wait_record != nullptr, "the event wait reached the device as a wait");
    if (at_vadd >= 0 && at_scale >= 0 && wait_record != nullptr) {
        const bc250hsa_mock_record* vadd_record =
            bc250hsa_mock_record_at(static_cast<uint32_t>(at_vadd));
        check(at_vadd < static_cast<int>(at_wait) && static_cast<int>(at_wait) < at_scale,
              "the order is vadd, then the wait, then scale");
        check_u64(wait_record->value, vadd_record->value,
                  "the wait names the fence value of the first dispatch");
    }
    check(hipEventSynchronize(done) == hipSuccess, "hipEventSynchronize");
    float elapsed = -1.0f;
    hipEvent_t start_event = nullptr;
    check(hipEventCreate(&start_event) == hipSuccess, "hipEventCreate start");
    check(hipEventRecord(start_event, first) == hipSuccess, "hipEventRecord start");
    check(hipEventSynchronize(start_event) == hipSuccess, "hipEventSynchronize start");
    check(hipEventElapsedTime(&elapsed, start_event, done) == hipSuccess,
          "hipEventElapsedTime");
    check(elapsed <= 0.0f || elapsed > 0.0f, "hipEventElapsedTime answers a number");
    check(hipEventDestroy(start_event) == hipSuccess, "hipEventDestroy start");
    check(hipEventDestroy(done) == hipSuccess, "hipEventDestroy");
    check(hipStreamDestroy(second) == hipSuccess, "hipStreamDestroy second");
    check(hipStreamDestroy(first) == hipSuccess, "hipStreamDestroy first");
    check(hipFree(device_c) == hipSuccess, "hipFree c");
    check(hipFree(device_b) == hipSuccess, "hipFree b");
    check(hipFree(device_a) == hipSuccess, "hipFree a");

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 4. a second run leaks no allocation\n");
    run_sequence(4096);
    const uint32_t live_after_first = bc250hsa_mock_live_allocations();
    const uint64_t bytes_after_first = bc250hsa_mock_live_bytes();
    run_sequence(4096);
    const uint32_t live_after_second = bc250hsa_mock_live_allocations();
    const uint64_t bytes_after_second = bc250hsa_mock_live_bytes();
    std::printf("  live allocations after run 1: %u (%llu bytes), after run 2: %u (%llu bytes)\n",
                live_after_first, (unsigned long long)bytes_after_first, live_after_second,
                (unsigned long long)bytes_after_second);
    check_u64(live_after_second, live_after_first, "the live allocation count after run 2");
    check_u64(bytes_after_second, bytes_after_first, "the live bytes after run 2");

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 5. the module unregisters\n");
    const uint32_t live_before_unregister = bc250hsa_mock_live_allocations();
    __hipUnregisterFatBinary(handle);
    check(bc250hsa_mock_live_allocations() < live_before_unregister,
          "unregistering the fat binary frees the code object");
    check(hipLaunchKernel(&g_stub_vadd, dim3(1u, 1u, 1u), dim3(1u, 1u, 1u), args, 0, nullptr) ==
              hipErrorInvalidDeviceFunction,
          "a launch after the unregister is refused");
    (void)hipGetLastError();

    bc250hsa_module_unload(probe_module);
    bc250hsa_close(probe_device);
    std::free(fatbin);

    std::printf("test_hip_mock: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
