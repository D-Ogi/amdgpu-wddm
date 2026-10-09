// test_hip_mock.cpp - layer 2 over the mock bc250hsa backend (M16 route B).
//
// It checks what design docs/design/m16-hip-route-b.md section 5.4 asks of this test:
//   1. registration finds a kernel from its host stub, and a CUDA fat binary is refused;
//   2. a launch packs the arguments at the offsets of the metadata, explicit and hidden;
//   3. stream order survives an event wait;
//   4. a second run of the same work in one process leaks no allocation;
//   5. the fixed properties of design section 4.1 reach the program;
//   6. the memory entry points move the bytes they are given, with an explicit kind and with
//      hipMemcpyDefault, and a host pointer never becomes a device pointer;
//   7. the legacy null stream waits for the work of other streams, and so does a stream that
//      carries nothing but an event wait;
//   8. an event timestamp is the time its value retired, so two events report the gap between
//      them and not zero;
//   9. the error state belongs to the thread that made the error;
//  10. a second fat binary in the same process registers and launches on its own;
//  11. the 13 entry points of step 3 answer as design section 4.9 states: the attributes agree
//      with the device properties, the two-dimensional copy keeps the bytes between its rows,
//      the per-thread default stream resolves, and the five refusals return the exact error
//      code that llama.cpp acts on.
//
// It needs no GPU and no AMDGPU compiler: the code object comes from the committed fixture
// compute/hip/tests/data/hip_test_kernels.gfx1013.fatbin.
//
// Usage: test_hip_mock.exe [<path of the fatbin fixture>]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "bc250hsa.h"
#include "hip/hip_runtime.h"
#include "hipmock_backend.h"

namespace {

// The wait bound that this test gives the runtime, in milliseconds. Every wait that layer 2
// performs must carry it, which is what makes a short lab trial possible (design decision 10).
const char* const kWaitTotalMs = "7000";

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

// The wait record that names this value, or nullptr. The index tells a caller where it sits in
// the record, so that a test can state an order.
const bc250hsa_mock_record* wait_of_value(uint64_t value, uint32_t* index) {
    for (uint32_t i = 0; i < bc250hsa_mock_record_count(); ++i) {
        const bc250hsa_mock_record* record = bc250hsa_mock_record_at(i);
        if (record != nullptr && record->kind == BC250HSA_MOCK_WAIT && record->value == value) {
            if (index != nullptr) {
                *index = i;
            }
            return record;
        }
    }
    return nullptr;
}

// Every wait of the record must carry the wait policy of the process.
void check_wait_bounds(const char* what) {
    uint32_t waits = 0;
    uint32_t wrong = 0;
    const uint32_t want = static_cast<uint32_t>(std::strtoul(kWaitTotalMs, nullptr, 10));
    for (uint32_t i = 0; i < bc250hsa_mock_record_count(); ++i) {
        const bc250hsa_mock_record* record = bc250hsa_mock_record_at(i);
        if (record == nullptr || record->kind != BC250HSA_MOCK_WAIT) {
            continue;
        }
        waits++;
        if (record->wait_total_ms != want) {
            wrong++;
        }
    }
    check(waits > 0u, what);
    check_u64(wrong, 0u, "every wait carries the wait bound of the process");
}

// A host spin of about this many milliseconds. The mock retires a fence at once, so a real
// sleep is the only way to put a measurable gap between two events.
void spin_ms(double ms) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const std::chrono::duration<double, std::milli> gone =
            std::chrono::steady_clock::now() - start;
        if (gone.count() >= ms) {
            return;
        }
    }
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
    // Before the first HIP call: the runtime reads its wait policy one time, at its first
    // call, so a test that sets it later would measure nothing.
#if defined(_WIN32)
    _putenv_s("BC250_HIP_WAIT_TOTAL_MS", kWaitTotalMs);
#else
    setenv("BC250_HIP_WAIT_TOTAL_MS", kWaitTotalMs, 1);
#endif
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

    int device_count = 0;
    int current_device = -1;
    check(hipInit(0) == hipSuccess, "hipInit");
    check(hipInit(1) == hipErrorInvalidValue, "hipInit refuses an unknown flag");
    (void)hipGetLastError();
    check(hipGetDeviceCount(&device_count) == hipSuccess, "hipGetDeviceCount");
    check_u64(static_cast<uint64_t>(device_count), 1u, "one device");
    check(hipSetDevice(0) == hipSuccess, "hipSetDevice 0");
    check(hipSetDevice(1) == hipErrorInvalidDevice, "hipSetDevice refuses a second device");
    (void)hipGetLastError();
    check(hipGetDevice(&current_device) == hipSuccess, "hipGetDevice");
    check_u64(static_cast<uint64_t>(current_device), 0u, "the current device is 0");
    check(std::strcmp(hipGetErrorName(hipErrorNotSupported), "hipErrorNotSupported") == 0,
          "hipGetErrorName");
    check(std::strcmp(hipGetErrorString(hipSuccess), "no error") == 0, "hipGetErrorString");

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
    check_wait_bounds("the event wait reached the device with a bound");

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 4. event timing is the gap between the two records\n");
    // Two events around a measured host gap, asked about much later. A timestamp taken at the
    // question instead of at the moment the value retired answers 0 here.
    hipEvent_t t0 = nullptr;
    hipEvent_t t1 = nullptr;
    float elapsed = -1.0f;
    check(hipEventCreate(&t0) == hipSuccess, "hipEventCreate t0");
    check(hipEventCreate(&t1) == hipSuccess, "hipEventCreate t1");
    check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0, first) ==
              hipSuccess,
          "launch before t0");
    check(hipEventRecord(t0, first) == hipSuccess, "hipEventRecord t0");
    spin_ms(6.0);
    check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0, first) ==
              hipSuccess,
          "launch before t1");
    check(hipEventRecord(t1, first) == hipSuccess, "hipEventRecord t1");
    check(hipStreamSynchronize(first) == hipSuccess, "hipStreamSynchronize before the question");
    spin_ms(20.0);
    check(hipEventElapsedTime(&elapsed, t0, t1) == hipSuccess, "hipEventElapsedTime");
    check(elapsed >= 4.0f, "the elapsed time holds the gap between the two records");
    check(elapsed < 1000.0f, "the elapsed time is not the age of the question");
    check(hipEventDestroy(t1) == hipSuccess, "hipEventDestroy t1");
    check(hipEventDestroy(t0) == hipSuccess, "hipEventDestroy t0");
    check(hipEventDestroy(done) == hipSuccess, "hipEventDestroy");
    check(hipStreamDestroy(second) == hipSuccess, "hipStreamDestroy second");
    check(hipStreamDestroy(first) == hipSuccess, "hipStreamDestroy first");
    check(hipFree(device_c) == hipSuccess, "hipFree c");
    check(hipFree(device_b) == hipSuccess, "hipFree b");
    check(hipFree(device_a) == hipSuccess, "hipFree a");

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 5. a second run leaks no allocation\n");
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
    std::printf("test_hip_mock: 6. memory, with an explicit kind and with hipMemcpyDefault\n");
    {
        const int elements = 256;
        const size_t span = sizeof(float) * static_cast<size_t>(elements);
        float* device = nullptr;
        float* pinned = nullptr;
        float source[256];
        float back[256];
        int i = 0;
        for (i = 0; i < elements; ++i) {
            source[i] = static_cast<float>(i) + 0.5f;
            back[i] = -1.0f;
        }
        check(hipMalloc(reinterpret_cast<void**>(&device), span) == hipSuccess,
              "hipMalloc for the copy test");
        check(hipHostMalloc(reinterpret_cast<void**>(&pinned), span, hipHostMallocDefault) ==
                  hipSuccess,
              "hipHostMalloc");
        size_t free_bytes = 0;
        size_t total_bytes = 0;
        check(hipMemGetInfo(&free_bytes, &total_bytes) == hipSuccess, "hipMemGetInfo");
        check(total_bytes > 0u && free_bytes <= total_bytes, "hipMemGetInfo answers a range");

        // An explicit kind, in both directions, through the host mapping that hipMalloc kept
        // from bc250hsa_map. A runtime that dropped that mapping cannot copy at all.
        check(hipMemcpy(device, source, span, hipMemcpyHostToDevice) == hipSuccess,
              "hipMemcpy host to device");
        check(hipMemcpy(back, device, span, hipMemcpyDeviceToHost) == hipSuccess,
              "hipMemcpy device to host");
        check(std::memcmp(back, source, span) == 0, "the bytes came back unchanged");

        // hipMemset and hipMemsetAsync on device memory.
        check(hipMemset(device, 0, span) == hipSuccess, "hipMemset on device memory");
        check(hipMemcpy(back, device, span, hipMemcpyDeviceToHost) == hipSuccess,
              "hipMemcpy after the fill");
        check(back[0] == 0.0f && back[elements - 1] == 0.0f, "the fill reached the device memory");
        hipStream_t copy_stream = nullptr;
        check(hipStreamCreate(&copy_stream) == hipSuccess, "hipStreamCreate for the copies");
        check(hipMemsetAsync(device, 0, span, copy_stream) == hipSuccess, "hipMemsetAsync");
        check(hipMemcpyAsync(device, source, span, hipMemcpyHostToDevice, copy_stream) ==
                  hipSuccess,
              "hipMemcpyAsync host to device");
        check(hipMemcpyAsync(back, device, span, hipMemcpyDeviceToHost, copy_stream) ==
                  hipSuccess,
              "hipMemcpyAsync device to host");
        check(std::memcmp(back, source, span) == 0, "the asynchronous copies moved the bytes");

        // hipMemcpyDefault must not read a host pointer as a device pointer. A pointer of
        // hipHostMalloc is the hard case, because it belongs to an allocation of the table.
        check(hipMemcpy(pinned, source, span, hipMemcpyDefault) == hipSuccess,
              "hipMemcpyDefault into a host allocation");
        check(std::memcmp(pinned, source, span) == 0, "the host allocation holds the bytes");
        std::memset(back, 0, span);
        check(hipMemcpy(back, pinned, span, hipMemcpyDefault) == hipSuccess,
              "hipMemcpyDefault out of a host allocation");
        check(std::memcmp(back, source, span) == 0, "the bytes came back from host memory");
        check(hipMemset(pinned, 0, span) == hipSuccess, "hipMemset on a host allocation");
        check(pinned[0] == 0.0f && pinned[elements - 1] == 0.0f, "the fill reached the host memory");
        check(hipMemcpy(device, pinned, span, hipMemcpyDefault) == hipSuccess,
              "hipMemcpyDefault from host memory to device memory");

        // A pointer of neither table is not device memory.
        check(hipMemset(source, 0, span) == hipErrorInvalidDevicePointer,
              "hipMemset refuses a pointer of no allocation");
        (void)hipGetLastError();

        check(hipStreamDestroy(copy_stream) == hipSuccess, "hipStreamDestroy of the copies");
        check(hipHostFree(pinned) == hipSuccess, "hipHostFree");
        check(hipFree(device) == hipSuccess, "hipFree of the copy test");
        check(hipDeviceSynchronize() == hipSuccess, "hipDeviceSynchronize");
    }

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 7. the null stream and a stream that only carries a wait\n");
    {
        hipStream_t work = nullptr;
        hipEvent_t mark = nullptr;
        check(hipStreamCreate(&work) == hipSuccess, "hipStreamCreate for the null stream test");

        // A copy on the null stream must wait for work that another stream submitted. HIP says
        // the legacy null stream synchronises with every other stream.
        bc250hsa_mock_reset();
        check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              work) == hipSuccess,
              "launch on the other stream");
        const int at_work = index_of_dispatch("vadd");
        check(at_work >= 0, "the launch reached the device");
        uint64_t work_value = 0;
        if (at_work >= 0) {
            const bc250hsa_mock_record* record =
                bc250hsa_mock_record_at(static_cast<uint32_t>(at_work));
            work_value = record != nullptr ? record->value : 0;
        }
        check(work_value != 0u, "the dispatch has a fence value");
        check(hipStreamSynchronize(nullptr) == hipSuccess, "hipStreamSynchronize of the null stream");
        check(wait_of_value(work_value, nullptr) != nullptr,
              "the null stream waits for the work of the other stream");

        // A stream that carries nothing but an event wait owes that event.
        bc250hsa_mock_reset();
        check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              work) == hipSuccess,
              "a second launch on the other stream");
        const int at_second = index_of_dispatch("vadd");
        uint64_t second_value = 0;
        if (at_second >= 0) {
            const bc250hsa_mock_record* record =
                bc250hsa_mock_record_at(static_cast<uint32_t>(at_second));
            second_value = record != nullptr ? record->value : 0;
        }
        check(hipEventCreate(&mark) == hipSuccess, "hipEventCreate mark");
        check(hipEventRecord(mark, work) == hipSuccess, "hipEventRecord on the other stream");
        hipStream_t idle = nullptr;
        check(hipStreamCreate(&idle) == hipSuccess, "hipStreamCreate idle");
        check(hipStreamWaitEvent(idle, mark, 0) == hipSuccess, "hipStreamWaitEvent on the idle stream");
        check(hipStreamSynchronize(idle) == hipSuccess, "hipStreamSynchronize of the idle stream");
        check(wait_of_value(second_value, nullptr) != nullptr,
              "the idle stream waits for the event it carries");
        check_wait_bounds("the waits of the null stream test carry a bound");

        check(hipEventDestroy(mark) == hipSuccess, "hipEventDestroy mark");
        check(hipStreamDestroy(idle) == hipSuccess, "hipStreamDestroy idle");
        check(hipStreamDestroy(work) == hipSuccess, "hipStreamDestroy of the null stream test");
    }

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 8. the error state belongs to one thread\n");
    {
        (void)hipGetLastError();
        hipError_t inside = hipSuccess;
        std::thread worker([&inside]() {
            (void)hipLaunchKernel(&g_stub_unknown, dim3(1u, 1u, 1u), dim3(1u, 1u, 1u), nullptr, 0,
                                  nullptr);
            inside = hipPeekAtLastError();
            int count = 0;
            (void)hipGetDeviceCount(&count);
        });
        worker.join();
        check(inside == hipErrorInvalidDeviceFunction, "the worker thread sees its own error");
        check(hipPeekAtLastError() == hipSuccess, "the error of the worker does not leak here");
    }

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 9. a second fat binary of the same process\n");
    {
        FatBinWrapper other = wrapper;
        void** other_handle = __hipRegisterFatBinary(&other);
        check(other_handle != nullptr, "a second wrapper registers");
        check(other_handle != handle, "the second wrapper is its own module");
        static char stub_other = 0;
        check(__hipRegisterFunction(other_handle, &stub_other, const_cast<char*>("vadd"), "vadd",
                                    -1, nullptr, nullptr, nullptr, nullptr, nullptr) == 0,
              "__hipRegisterFunction of the second module");
        check(hipLaunchKernel(&stub_other, dim3(4u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              nullptr) == hipSuccess,
              "a launch through the second module");
        __hipUnregisterFatBinary(other_handle);
        check(hipLaunchKernel(&stub_other, dim3(4u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              nullptr) == hipErrorInvalidDeviceFunction,
              "the stub of the second module is gone with it");
        (void)hipGetLastError();
        check(hipLaunchKernel(&g_stub_vadd, dim3(4u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              nullptr) == hipSuccess,
              "the first module still launches");
    }

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 10. the step 3 entry points\n");
    {
        (void)hipGetLastError();

        // The attributes. Each one must agree with the field of hipGetDeviceProperties that
        // carries the same thing, because a program may read either.
        hipDeviceProp_t props10;
        check(hipGetDeviceProperties(&props10, 0) == hipSuccess, "hipGetDeviceProperties for 10");
        int value = -1;
        check(hipDeviceGetAttribute(&value, hipDeviceAttributeWarpSize, 0) == hipSuccess,
              "hipDeviceGetAttribute warp size");
        check_u64((uint64_t)value, (uint64_t)props10.warpSize, "the attribute warp size");
        check(hipDeviceGetAttribute(&value, hipDeviceAttributeMultiprocessorCount, 0) ==
                  hipSuccess,
              "hipDeviceGetAttribute compute unit count");
        check_u64((uint64_t)value, (uint64_t)props10.multiProcessorCount,
                  "the attribute compute unit count");
        check(hipDeviceGetAttribute(&value, hipDeviceAttributeMaxSharedMemoryPerBlock, 0) ==
                  hipSuccess,
              "hipDeviceGetAttribute group memory");
        check_u64((uint64_t)value, (uint64_t)props10.sharedMemPerBlock,
                  "the attribute group memory per workgroup");
        // The two answers llama.cpp acts on. Both must be 0, or it would take a path this
        // build cannot run.
        check(hipDeviceGetAttribute(&value, hipDeviceAttributeCooperativeLaunch, 0) == hipSuccess,
              "hipDeviceGetAttribute cooperative launch");
        check_u64((uint64_t)value, 0u, "cooperative launch is not supported");
        check(hipDeviceGetAttribute(&value, hipDeviceAttributeVirtualMemoryManagementSupported,
                                    0) == hipSuccess,
              "hipDeviceGetAttribute virtual memory management");
        check_u64((uint64_t)value, 0u, "virtual memory management is not supported");
        check(hipDeviceGetAttribute(&value, (hipDeviceAttribute_t)9999, 0) ==
                  hipErrorInvalidValue,
              "an unknown attribute is refused and not guessed");
        check(hipDeviceGetAttribute(&value, hipDeviceAttributeWarpSize, 1) ==
                  hipErrorInvalidDevice,
              "an attribute of a second device is refused");
        check(hipDeviceGetAttribute(nullptr, hipDeviceAttributeWarpSize, 0) ==
                  hipErrorInvalidValue,
              "hipDeviceGetAttribute checks its pointer");
        (void)hipGetLastError();

        // The bus identifier. The mock backend reports bus 3, device 0, function 0.
        char bus[32];
        std::memset(bus, 0x7F, sizeof(bus));
        check(hipDeviceGetPCIBusId(bus, (int)sizeof(bus), 0) == hipSuccess,
              "hipDeviceGetPCIBusId");
        check(std::strcmp(bus, "0000:03:00.0") == 0, "the bus identifier of the mock backend");
        char shortbus[6];
        check(hipDeviceGetPCIBusId(shortbus, (int)sizeof(shortbus), 0) == hipSuccess,
              "hipDeviceGetPCIBusId into a short buffer");
        check(std::strcmp(shortbus, "0000:") == 0,
              "a short buffer truncates and stays terminated");
        check(hipDeviceGetPCIBusId(bus, 0, 0) == hipErrorInvalidValue,
              "hipDeviceGetPCIBusId refuses a zero length");
        check(hipDeviceGetPCIBusId(bus, (int)sizeof(bus), 1) == hipErrorInvalidDevice,
              "hipDeviceGetPCIBusId refuses a second device");
        (void)hipGetLastError();

        // No peer, and both entry points say so.
        int can = -1;
        check(hipDeviceCanAccessPeer(&can, 0, 0) == hipSuccess, "hipDeviceCanAccessPeer");
        check_u64((uint64_t)can, 0u, "a device is not its own peer");
        check(hipDeviceCanAccessPeer(&can, 0, 1) == hipErrorInvalidDevice,
              "there is no second device to peer with");
        check(hipDeviceEnablePeerAccess(0, 0) == hipErrorInvalidDevice,
              "hipDeviceEnablePeerAccess has nothing to enable");
        (void)hipGetLastError();

        // The kernel attribute. A value the hardware can give is kept; one it cannot is
        // refused at the call that asks for it.
        check(hipFuncSetAttribute(&g_stub_vadd, hipFuncAttributeMaxDynamicSharedMemorySize,
                                  32768) == hipSuccess,
              "hipFuncSetAttribute 32768 bytes of group memory");
        check(hipFuncSetAttribute(&g_stub_vadd, hipFuncAttributeMaxDynamicSharedMemorySize,
                                  1 << 20) == hipErrorInvalidValue,
              "a group memory ceiling above the hardware is refused");
        check(hipFuncSetAttribute(&g_stub_vadd, hipFuncAttributeMaxDynamicSharedMemorySize, -1) ==
                  hipErrorInvalidValue,
              "a negative group memory ceiling is refused");
        check(hipFuncSetAttribute(&g_stub_vadd, hipFuncAttributePreferredSharedMemoryCarveout,
                                  50) == hipSuccess,
              "the carveout preference is accepted and has no effect");
        check(hipFuncSetAttribute(&g_stub_vadd, (hipFuncAttribute)4242, 1) ==
                  hipErrorInvalidValue,
              "an unknown kernel attribute is refused");
        check(hipFuncSetAttribute(&g_stub_unknown, hipFuncAttributeMaxDynamicSharedMemorySize,
                                  1024) == hipErrorInvalidDeviceFunction,
              "hipFuncSetAttribute of an unregistered stub is refused");
        check(hipFuncSetAttribute(nullptr, hipFuncAttributeMaxDynamicSharedMemorySize, 1024) ==
                  hipErrorInvalidDeviceFunction,
              "hipFuncSetAttribute checks its function");
        // The attribute must not change what a launch does: the ceiling is advice here.
        check(hipLaunchKernel(&g_stub_vadd, dim3(4u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              nullptr) == hipSuccess,
              "a launch after hipFuncSetAttribute still runs");
        (void)hipGetLastError();

        // A cooperative start is refused, and the attribute above already told the program so.
        check(hipLaunchCooperativeKernel(&g_stub_vadd, dim3(4u, 1u, 1u), dim3(256u, 1u, 1u),
                                         args, 0, nullptr) == hipErrorNotSupported,
              "hipLaunchCooperativeKernel is not supported");
        check(hipLaunchCooperativeKernel(nullptr, dim3(1u, 1u, 1u), dim3(1u, 1u, 1u), nullptr, 0,
                                         nullptr) == hipErrorInvalidDeviceFunction,
              "hipLaunchCooperativeKernel checks its function");
        (void)hipGetLastError();

        // The device address of host-visible memory, and of a pointer inside it.
        void* host = nullptr;
        check(hipHostMalloc(&host, 4096, hipHostMallocMapped) == hipSuccess,
              "hipHostMalloc for the device pointer test");
        void* dev_of_host = nullptr;
        check(hipHostGetDevicePointer(&dev_of_host, host, 0) == hipSuccess,
              "hipHostGetDevicePointer");
        check(dev_of_host != nullptr, "the device pointer is not null");
        void* dev_inside = nullptr;
        check(hipHostGetDevicePointer(&dev_inside, static_cast<char*>(host) + 64, 0) ==
                  hipSuccess,
              "hipHostGetDevicePointer of an interior pointer");
        check_u64((uint64_t)(uintptr_t)dev_inside, (uint64_t)(uintptr_t)dev_of_host + 64u,
                  "an interior pointer moves the device address by the same offset");
        check(hipHostGetDevicePointer(&dev_of_host, host, 1) == hipErrorInvalidValue,
              "hipHostGetDevicePointer takes no flags in this build");
        int on_the_stack = 0;
        check(hipHostGetDevicePointer(&dev_of_host, &on_the_stack, 0) == hipErrorInvalidValue,
              "memory this runtime did not allocate has no device address");
        (void)hipGetLastError();

        // The five honest refusals. Each code is the one llama.cpp acts on.
        check(hipHostRegister(&on_the_stack, sizeof(on_the_stack), hipHostRegisterReadOnly) ==
                  hipErrorNotSupported,
              "hipHostRegister refuses, because layer 1 cannot map a program's own memory");
        check(hipHostUnregister(&on_the_stack) == hipErrorHostMemoryNotRegistered,
              "hipHostUnregister has nothing registered to release");
        void* managed = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
        check(hipMallocManaged(&managed, 4096, 0) == hipErrorNotSupported,
              "hipMallocManaged refuses with the code llama.cpp falls back on");
        check(managed == nullptr, "a refused hipMallocManaged leaves no pointer behind");
        check(hipMemAdvise(host, 4096, hipMemAdviseSetCoarseGrain, 0) == hipErrorNotSupported,
              "hipMemAdvise has no managed memory to advise about");
        (void)hipGetLastError();

        // The two-dimensional copy, between two device allocations, which is where ggml-hip
        // uses it. The rows are 48 bytes inside a stride of 64, so a copy that ignored the
        // stride would be visible at once.
        const size_t width = 48;
        const size_t pitch = 64;
        const size_t rows = 8;
        const size_t span2d = pitch * rows;
        unsigned char expect[512];
        unsigned char readback[512];
        check(span2d == sizeof(expect), "the two-dimensional test buffers match its sizes");
        void* src2d = nullptr;
        void* dst2d = nullptr;
        check(hipMalloc(&src2d, span2d) == hipSuccess, "hipMalloc src2d");
        check(hipMalloc(&dst2d, span2d) == hipSuccess, "hipMalloc dst2d");
        for (size_t i = 0; i < span2d; ++i) {
            expect[i] = static_cast<unsigned char>(i & 0xFF);
            readback[i] = 0xEE;
        }
        check(hipMemcpy(src2d, expect, span2d, hipMemcpyHostToDevice) == hipSuccess,
              "the source of the two-dimensional copy is filled");
        check(hipMemcpy(dst2d, readback, span2d, hipMemcpyHostToDevice) == hipSuccess,
              "the destination of the two-dimensional copy is marked");

        check(hipMemcpy2DAsync(dst2d, pitch, src2d, pitch, width, rows,
                               hipMemcpyDeviceToDevice, nullptr) == hipSuccess,
              "hipMemcpy2DAsync");
        std::memset(readback, 0, sizeof(readback));
        check(hipMemcpy(readback, dst2d, span2d, hipMemcpyDeviceToHost) == hipSuccess,
              "the destination of the two-dimensional copy is read back");
        bool rows_match = true;
        bool gaps_kept = true;
        for (size_t r = 0; r < rows; ++r) {
            for (size_t c = 0; c < pitch; ++c) {
                const unsigned char got = readback[r * pitch + c];
                if (c < width) {
                    if (got != expect[r * pitch + c]) {
                        rows_match = false;
                    }
                } else if (got != 0xEE) {
                    gaps_kept = false;
                }
            }
        }
        check(rows_match, "every row of the two-dimensional copy holds the source bytes");
        check(gaps_kept, "the bytes between the rows are untouched");
        check(hipMemcpy2DAsync(dst2d, 16, src2d, pitch, width, rows, hipMemcpyDeviceToDevice,
                               nullptr) == hipErrorInvalidValue,
              "a row wider than its pitch is refused");
        check(hipMemcpy2DAsync(dst2d, pitch, src2d, pitch, width, 0, hipMemcpyDeviceToDevice,
                               nullptr) == hipSuccess,
              "a copy of no rows is legal and does nothing");
        (void)hipGetLastError();

        // The copy between devices, which this process can only make within device 0, and the
        // per-thread default stream that llama.cpp passes to it.
        check(hipMemset(dst2d, 0, span2d) == hipSuccess, "the destination is cleared");
        check(hipMemcpyPeerAsync(dst2d, 0, src2d, 0, span2d, hipStreamPerThread) == hipSuccess,
              "hipMemcpyPeerAsync within device 0 on the per-thread stream");
        std::memset(readback, 0, sizeof(readback));
        check(hipMemcpy(readback, dst2d, span2d, hipMemcpyDeviceToHost) == hipSuccess,
              "the result of hipMemcpyPeerAsync is read back");
        check(std::memcmp(readback, expect, span2d) == 0, "hipMemcpyPeerAsync moved every byte");
        check(hipMemcpyPeerAsync(dst2d, 1, src2d, 0, 16, nullptr) == hipErrorInvalidDevice,
              "a copy to a second device is refused");
        check(hipStreamSynchronize(hipStreamPerThread) == hipSuccess,
              "the per-thread default stream synchronises");
        (void)hipGetLastError();

        check(hipFree(src2d) == hipSuccess, "hipFree src2d");
        check(hipFree(dst2d) == hipSuccess, "hipFree dst2d");
        check(hipHostFree(host) == hipSuccess, "hipHostFree of the device pointer test");
    }

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 11. the module unregisters\n");
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
