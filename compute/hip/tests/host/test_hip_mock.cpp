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
//      code that llama.cpp acts on;
//  12. the GPU copy route of scratch/hip-gpu-copy/CONTRACT.md picks the route the destination
//      asks for, counts every copy exactly once, and answers a failed coherence control or a
//      refused copy with the host-processor path and no new error code;
//  13. every duration counter moves when its operation runs and on no other copy line, a
//      maximum is never above its total, a reset clears them, and bc250hipGetCounters fills
//      exactly the layout of each of the three sizes the structure has had.
//  14. a GPU copy or fill that fails after its submission is never replaced by the host
//      processor and never reported as done, a failure before it is, a hipFree waits for the
//      fence value of a failed wait, the coherence gate follows the destination allocation
//      (the GPU address of a host allocation included), a failed host-processor copy is not
//      counted, and BC250_HIP_GPU_COPY_MIN takes decimal digits only.
//  15. while a GPU copy whose wait ended without retirement has not retired, no memory entry
//      point moves a byte with the host processor, neither as the configured route nor as the
//      fallback after a refusal: each answers the copy's error, and after the late retirement
//      the same calls succeed with the copy's bytes. On a device marked lost, none does either.
//
// It needs no GPU and no AMDGPU compiler: the code object comes from the committed fixture
// compute/hip/tests/data/hip_test_kernels.gfx1013.fatbin.
//
// Usage: test_hip_mock.exe [<path of the fatbin fixture>]

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "bc250hsa.h"
#include "hip/hip_runtime.h"
#include "hipmock_backend.h"
#include "runtime_internal.h"
#include "../../../../driver/shim/include/bc250_hip_journal.h"

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

// The process counters of bc250hsa.h section 3, which the GPU copy route of section 5.1 is
// judged by. The struct is sized by the caller, so a build whose header and backend disagree
// is a refusal here and not a wrong number.
bool read_counters(bc250hsa_counters* out) {
    std::memset(out, 0, sizeof(*out));
    out->struct_bytes = static_cast<uint32_t>(sizeof(*out));
    return bc250hsa_counters_read(out) == BC250HSA_OK;
}

// The five copy duration lines, in the order of the five copy counts of bc250hsa.h section 3.
enum CopyLine { kLineD2h = 0, kLineStaged, kLineD2d, kLineFill, kLineFallback, kLineCount };
const char* const kLineNames[kLineCount] = { "gpu_copy_d2h_ticks", "gpu_copy_d2h_staged_ticks",
                                             "gpu_copy_d2d_ticks", "gpu_fill_ticks",
                                             "cpu_copy_fallback_ticks" };

uint64_t copy_ticks(const bc250hsa_counters& c, int line) {
    switch (line) {
    case kLineD2h: return c.gpu_copy_d2h_ticks;
    case kLineStaged: return c.gpu_copy_d2h_staged_ticks;
    case kLineD2d: return c.gpu_copy_d2d_ticks;
    case kLineFill: return c.gpu_fill_ticks;
    default: return c.cpu_copy_fallback_ticks;
    }
}

// One copy or fill ran between the two reads: its own duration line moved and the four others
// stayed where they were, so a copy cannot add its time to a route it did not take.
void check_one_copy_line(const bc250hsa_counters& before, const bc250hsa_counters& after,
                         int line, const char* what) {
    for (int i = 0; i < kLineCount; ++i) {
        char text[200];
        const bool moved = copy_ticks(after, i) > copy_ticks(before, i);
        std::snprintf(text, sizeof(text), "%s: %s %s", what, kLineNames[i],
                      i == line ? "moves" : "stays");
        check(i == line ? moved : !moved, text);
    }
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
    const uint32_t records_before = bc250hsa_mock_record_count();
    bc250hsa_mock_journal_fail(1);
    check(hipLaunchKernel(&g_stub_vadd, dim3(static_cast<unsigned>(n) / 256u, 1u, 1u),
                          dim3(256u, 1u, 1u), args, 0, stream) == hipErrorOutOfMemory,
          "journal refusal reaches caller before dispatch");
    check(bc250hsa_mock_record_count() == records_before, "journal failure publishes no dispatch");
    bc250hsa_mock_journal_fail(0);
    check(hipLaunchKernel(&g_stub_vadd, dim3(static_cast<unsigned>(n) / 256u, 1u, 1u),
                          dim3(256u, 1u, 1u), args, 0, stream) == hipSuccess,
          "run hipLaunchKernel");
    uint32_t journal_bytes = 0;
    const void* journal = bc250hsa_mock_journal(&journal_bytes);
    check(journal_bytes >= sizeof(BC250_HIP_DISPATCH_RECORD), "successful runtime launch retained journal");
    if (journal_bytes >= sizeof(BC250_HIP_DISPATCH_RECORD)) {
        BC250_HIP_DISPATCH_RECORD record{};
        std::memcpy(&record, journal, sizeof(record));
        check(record.BindingCount == 3, "three pointer arguments, scalar n excluded");
        check(record.KernargBytes >= 28, "entire vadd argument segment retained");
        const auto* data = static_cast<const unsigned char*>(journal);
        BC250_HIP_POINTER_BINDING bindings[3]{};
        if (record.BindingCount == 3 && record.BindingsOffset <= journal_bytes &&
            sizeof(bindings) <= journal_bytes - record.BindingsOffset) {
            std::memcpy(bindings, data + record.BindingsOffset, sizeof(bindings));
            for (const auto& binding : bindings)
                check(binding.Flags == BC250_HIP_BINDING_INTERVAL_KNOWN && binding.Bytes >= bytes,
                      "runtime allocation interval accompanies each vadd pointer");
        } else check(false, "complete bounded bindings array");
    }
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
    // An event that nothing recorded is complete: it waits for nothing and holds nothing back.
    // MEASURED 2026-10-09: a refusal here stopped llama-bench at its first HIP call, because
    // ggml_backend_cuda_device_event_synchronize synchronizes an event before any record.
    hipEvent_t fresh = nullptr;
    float never = -1.0f;
    check(hipEventCreateWithFlags(&fresh, hipEventDisableTiming) == hipSuccess,
          "hipEventCreateWithFlags with timing off");
    check(hipEventSynchronize(fresh) == hipSuccess, "an unrecorded event synchronizes at once");
    check(hipStreamWaitEvent(first, fresh, 0) == hipSuccess, "a stream waits for it and goes on");
    check(hipEventSynchronize(fresh) == hipSuccess, "and it is still complete afterwards");
    check(hipEventDestroy(fresh) == hipSuccess, "hipEventDestroy fresh");
    hipEvent_t untimed = nullptr;
    check(hipEventCreate(&untimed) == hipSuccess, "hipEventCreate untimed");
    check(hipEventElapsedTime(&never, t0, untimed) == hipErrorInvalidHandle,
          "the elapsed time of an unrecorded event is refused, not zero");
    check(hipEventDestroy(untimed) == hipSuccess, "hipEventDestroy untimed");

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
    // The error state across a refused optional call and a launch, which is the shape the lab
    // session of 2026-10-09 could not tell apart from a failed launch (defect BD-110): three
    // llama.cpp programs died at `CUDA_CHECK(cudaGetLastError())` right after a `<<<>>>` call,
    // and from the message alone a sticky error of an earlier call looks the same.
    //
    // HIP says hipGetLastError returns the last error of any call of the thread, so a refused
    // optional call does leave its code there until something reads it. That is the behaviour
    // of ROCm as well, and llama.cpp is written for it: every optional call it makes is
    // followed by its own `(void)cudaGetLastError()`. These checks state both halves, so that
    // the next reader of a "ROCm error at hipGetLastError()" line knows which half to test.
    std::printf("test_hip_mock: 11. the error state across a refusal and a launch\n");
    {
        void* managed = nullptr;
        check(hipGetLastError() == hipSuccess, "the error state starts clear");
        check(hipMallocManaged(&managed, 4096, 0) == hipErrorNotSupported,
              "the optional call is refused");
        // The launch itself succeeds, and the refusal of the call before it is still there.
        check(hipLaunchKernel(&g_stub_vadd, dim3(1u, 1u, 1u), dim3(1u, 1u, 1u), args, 0,
                              nullptr) == hipSuccess,
              "the launch after the refused call succeeds");
        check(hipPeekAtLastError() == hipErrorNotSupported,
              "the refusal of the earlier call is what a launch-site hipGetLastError reports");
        check(hipGetLastError() == hipErrorNotSupported, "and reading it reports the same");
        check(hipGetLastError() == hipSuccess, "one read clears it");
        // The same sequence with the clear llama.cpp writes: the launch site sees nothing.
        check(hipMallocManaged(&managed, 4096, 0) == hipErrorNotSupported,
              "the optional call is refused again");
        (void)hipGetLastError();
        check(hipLaunchKernel(&g_stub_vadd, dim3(1u, 1u, 1u), dim3(1u, 1u, 1u), args, 0,
                              nullptr) == hipSuccess,
              "the launch after the cleared refusal succeeds");
        check(hipGetLastError() == hipSuccess,
              "a program that clears the error after the optional call sees a clean launch");
        check(hipDeviceSynchronize() == hipSuccess, "the two launches retire");
        (void)hipGetLastError();
    }

    // ---------------------------------------------------------------------------------------
    // The GPU copy route of scratch/hip-gpu-copy/CONTRACT.md, from the side layer 2 chooses
    // it. The dwords of the packet are test_pm4's business; this section checks the choice:
    // which of the three routes a copy takes, that every copy lands on exactly one counter,
    // and that the two failure paths of layer 1 leave a correct and slow runtime behind.
    std::printf("test_hip_mock: 12. the GPU copy route\n");
    {
        const size_t big = 8192;      // above the 4096-byte threshold
        const size_t small = 1024;    // below it
        unsigned char* device_src = nullptr;
        unsigned char* device_dst = nullptr;
        unsigned char* pinned_dst = nullptr;
        std::vector<unsigned char> pageable(big, 0u);
        std::vector<unsigned char> seed(big, 0u);
        for (size_t i = 0; i < big; ++i) {
            seed[i] = static_cast<unsigned char>(i * 7u + 1u);
        }
        check(hipMalloc(reinterpret_cast<void**>(&device_src), big) == hipSuccess,
              "hipMalloc the GPU copy source");
        check(hipMalloc(reinterpret_cast<void**>(&device_dst), big) == hipSuccess,
              "hipMalloc the GPU copy destination");
        check(hipHostMalloc(reinterpret_cast<void**>(&pinned_dst), big, hipHostMallocDefault) ==
                  hipSuccess,
              "hipHostMalloc the GPU copy destination");
        check(hipMemcpy(device_src, seed.data(), big, hipMemcpyHostToDevice) == hipSuccess,
              "the source holds the pattern");

        // Route 5a: the destination is one of our own host-visible allocations, so the copy
        // is one packet into it and nothing is staged. This is the llama.cpp logits case.
        bc250hsa_counters counters;
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        std::memset(pinned_dst, 0, big);
        check(hipMemcpy(pinned_dst, device_src, big, hipMemcpyDeviceToHost) == hipSuccess,
              "a device-to-host copy into our own host allocation");
        check(std::memcmp(pinned_dst, seed.data(), big) == 0, "route 5a moved the bytes");
        check(read_counters(&counters), "the counters are readable");
        check_u64(counters.gpu_copy_d2h, 1u, "route 5a counts one GPU device-to-host copy");
        check_u64(counters.gpu_copy_d2h_bytes, big, "route 5a counts its bytes");
        check_u64(counters.gpu_copy_d2h_staged, 0u, "route 5a stages nothing");
        check_u64(counters.cpu_copy_fallback, 0u, "route 5a takes no fallback");
        check_u64(counters.gpu_copy_packets, 1u, "8192 bytes are one packet");
        uint32_t record_index = 0;
        check(first_of_kind(BC250HSA_MOCK_GPU_COPY, &record_index) != nullptr,
              "the backend saw the direct GPU copy");
        record_index = 0;
        check(first_of_kind(BC250HSA_MOCK_COPY_FROM_DEVICE, &record_index) == nullptr,
              "and no host-processor copy beside it");

        // Route 5b: the destination is a program's own memory, so the bytes go through the
        // staging ring of layer 1.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        check(hipMemcpy(pageable.data(), device_src, big, hipMemcpyDeviceToHost) == hipSuccess,
              "a device-to-host copy into pageable memory");
        check(std::memcmp(pageable.data(), seed.data(), big) == 0, "route 5b moved the bytes");
        check(read_counters(&counters), "the counters are readable after route 5b");
        check_u64(counters.gpu_copy_d2h_staged, 1u, "route 5b counts one staged copy");
        check_u64(counters.gpu_copy_d2h_staged_bytes, big, "route 5b counts its bytes");
        check_u64(counters.gpu_copy_d2h, 0u, "route 5b is not counted as a direct copy");
        check_u64(counters.cpu_copy_fallback, 0u, "route 5b takes no fallback");
        record_index = 0;
        check(first_of_kind(BC250HSA_MOCK_GPU_COPY_STAGED, &record_index) != nullptr,
              "the backend saw the staged GPU copy");

        // Below the threshold a submission and a fence cost more than the bytes, so the copy
        // stays on the host processor and says so on the fallback counter.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        check(hipMemcpy(pageable.data(), device_src, small, hipMemcpyDeviceToHost) == hipSuccess,
              "a copy below the threshold");
        check(read_counters(&counters), "the counters are readable after the small copy");
        check_u64(counters.cpu_copy_fallback, 1u, "a small copy counts one fallback");
        check_u64(counters.cpu_copy_fallback_bytes, small, "and its bytes");
        check_u64(counters.gpu_copy_d2h + counters.gpu_copy_d2h_staged, 0u,
                  "a small copy builds no packet");

        // The coherence control of CONTRACT.md section 8 did not pass. Every device-to-host
        // copy must then be correct and slow, and no new error code may reach the program.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        bc250hsa_mock_set_gpu_copy(0, 0);
        std::memset(pageable.data(), 0, big);
        check(hipMemcpy(pageable.data(), device_src, big, hipMemcpyDeviceToHost) == hipSuccess,
              "a device-to-host copy with the coherence control failed");
        check(std::memcmp(pageable.data(), seed.data(), big) == 0,
              "the host-processor path moved the bytes");
        check(read_counters(&counters), "the counters are readable after the control failed");
        check_u64(counters.cpu_copy_fallback, 1u, "the failed control counts one fallback");
        check_u64(counters.gpu_copy_control_failed, 1u, "and raises the control counter");
        check_u64(counters.gpu_copy_d2h + counters.gpu_copy_d2h_staged, 0u,
                  "and no device-to-host packet is built");

        // Layer 1 refusing one copy is the same answer: the runtime falls back and stays
        // correct.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        bc250hsa_mock_set_gpu_copy(1, 1);
        std::memset(pinned_dst, 0, big);
        check(hipMemcpy(pinned_dst, device_src, big, hipMemcpyDeviceToHost) == hipSuccess,
              "a device-to-host copy that layer 1 refuses");
        check(std::memcmp(pinned_dst, seed.data(), big) == 0,
              "the refused copy still moved the bytes");
        check(read_counters(&counters), "the counters are readable after the refusal");
        check_u64(counters.cpu_copy_fallback, 1u, "a refused copy counts one fallback");
        bc250hsa_mock_set_gpu_copy(1, 0);

        // A device-to-device copy, which the old build refused whenever either side had no
        // host mapping. It is one packet now.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        check(hipMemcpy(device_dst, device_src, big, hipMemcpyDeviceToDevice) == hipSuccess,
              "a device-to-device copy");
        check(read_counters(&counters), "the counters are readable after the device copy");
        check_u64(counters.gpu_copy_d2d, 1u, "it counts one GPU device-to-device copy");
        check_u64(counters.gpu_copy_d2d_bytes, big, "and its bytes");
        std::memset(pageable.data(), 0, big);
        check(hipMemcpy(pageable.data(), device_dst, big, hipMemcpyDeviceToHost) == hipSuccess,
              "read the device-to-device destination back");
        check(std::memcmp(pageable.data(), seed.data(), big) == 0,
              "the device-to-device copy moved the bytes");

        // Two ranges of one allocation that overlap are never a copy packet: HIP leaves such
        // a copy undefined, and the host-processor path at least gives the memcpy answer.
        void* pinned_device_pointer = nullptr;
        check(hipHostGetDevicePointer(&pinned_device_pointer, pinned_dst, 0) == hipSuccess,
              "the device address of the host allocation");
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        check(hipMemcpy(static_cast<unsigned char*>(pinned_device_pointer) + 16,
                        pinned_device_pointer, big - 16, hipMemcpyDeviceToDevice) == hipSuccess,
              "an overlapping device-to-device copy is answered and not refused");
        check(read_counters(&counters), "the counters are readable after the overlap");
        check_u64(counters.gpu_copy_d2d, 0u, "an overlap builds no packet");
        check_u64(counters.cpu_copy_fallback, 1u, "and counts one fallback");

        // hipMemset of an allocation that has a host mapping keeps the host-processor fill,
        // because a write of that mapping is MEASURED at 15.6 GB/s. The switch turns the GPU
        // fill on, and then the same call is one packet.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        check(hipMemset(device_dst, 0x5A, big) == hipSuccess, "hipMemset of device memory");
        check(read_counters(&counters), "the counters are readable after the fill");
        check_u64(counters.gpu_fill, 0u, "the default keeps the host-processor fill");
        std::memset(pageable.data(), 0, big);
        check(hipMemcpy(pageable.data(), device_dst, big, hipMemcpyDeviceToHost) == hipSuccess,
              "read the filled memory back");
        check(pageable[0] == 0x5A && pageable[big - 1] == 0x5A, "the fill reached the memory");

        check(hipHostFree(pinned_dst) == hipSuccess, "hipHostFree of the GPU copy test");
        check(hipFree(device_dst) == hipSuccess, "hipFree of the GPU copy destination");
        check(hipFree(device_src) == hipSuccess, "hipFree of the GPU copy source");
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
    }

    // ---------------------------------------------------------------------------------------
    // The duration counters of bc250hsa.h section 3, from the side a reader sees them. Each
    // operation runs alone between two reads. The mock counts at least one tick for every
    // operation it times, the host-processor copy below is a mebibyte, and the slow wait waits
    // for a hold time of the mock, so no check here depends on how fast this machine is.
    std::printf("test_hip_mock: 13. the duration counters and the counter sizes\n");
    {
        const size_t big = 8192;
        const size_t mebibyte = 1024u * 1024u;
        unsigned char* device_src = nullptr;
        unsigned char* device_dst = nullptr;
        unsigned char* pinned_dst = nullptr;
        std::vector<unsigned char> pageable(mebibyte, 0u);
        check(hipMalloc(reinterpret_cast<void**>(&device_src), mebibyte) == hipSuccess,
              "hipMalloc the duration source");
        check(hipMalloc(reinterpret_cast<void**>(&device_dst), mebibyte) == hipSuccess,
              "hipMalloc the duration destination");
        check(hipHostMalloc(reinterpret_cast<void**>(&pinned_dst), big, hipHostMallocDefault) ==
                  hipSuccess,
              "hipHostMalloc the duration destination");
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();

        bc250hsa_counters before;
        bc250hsa_counters after;
        check(read_counters(&before), "the counters are readable before the durations");
        check(before.ticks_per_second != 0u, "the counters carry the tick frequency");
        check_u64(before.waits_fast_ticks + before.waits_fast_ticks_max +
                      before.waits_other_ticks + before.waits_other_ticks_max +
                      before.submit_calls + before.submit_ticks,
                  0u, "a reset clears the wait and submission durations");
        for (int i = 0; i < kLineCount; ++i) {
            check_u64(copy_ticks(before, i), 0u, "a reset clears every copy duration");
        }

        // The four GPU routes and the fallback, one at a time.
        check(hipMemcpy(pinned_dst, device_src, big, hipMemcpyDeviceToHost) == hipSuccess,
              "route 5a for the durations");
        check(read_counters(&after), "the counters are readable after route 5a");
        check_one_copy_line(before, after, kLineD2h, "route 5a");
        before = after;

        check(hipMemcpy(pageable.data(), device_src, big, hipMemcpyDeviceToHost) == hipSuccess,
              "route 5b for the durations");
        check(read_counters(&after), "the counters are readable after route 5b");
        check_one_copy_line(before, after, kLineStaged, "route 5b");
        before = after;

        check(hipMemcpy(device_dst, device_src, big, hipMemcpyDeviceToDevice) == hipSuccess,
              "a device-to-device copy for the durations");
        check(read_counters(&after), "the counters are readable after the device copy");
        check_one_copy_line(before, after, kLineD2d, "the device-to-device copy");
        before = after;

        // The GPU fill of a mapped allocation is what BC250_HIP_GPU_FILL=1 turns on; the test
        // sets the same field the switch sets, for this one call.
        bc250hip::state().gpu_fill_mapped = true;
        check(hipMemset(device_dst, 0x33, big) == hipSuccess, "a GPU fill for the durations");
        bc250hip::state().gpu_fill_mapped = false;
        check(read_counters(&after), "the counters are readable after the GPU fill");
        check_u64(after.gpu_fill - before.gpu_fill, 1u, "the fill took the GPU route");
        check_one_copy_line(before, after, kLineFill, "the GPU fill");
        before = after;

        // A copy layer 1 refuses: the refused attempt and the host-processor copy that replaces
        // it are one fallback, and its time is on the fallback line only.
        bc250hsa_mock_set_gpu_copy(1, 1);
        check(hipMemcpy(pageable.data(), device_src, mebibyte, hipMemcpyDeviceToHost) ==
                  hipSuccess,
              "a refused mebibyte for the durations");
        bc250hsa_mock_set_gpu_copy(1, 0);
        check(read_counters(&after), "the counters are readable after the fallback");
        check_u64(after.cpu_copy_fallback - before.cpu_copy_fallback, 1u,
                  "the refused copy is one fallback");
        check_one_copy_line(before, after, kLineFallback, "the fallback");
        before = after;

        // A launch and a synchronisation with nothing held: the submission moves its two lines
        // and the wait ends on the fence mapping, so it is a fast wait.
        check(hipLaunchKernel(&g_stub_vadd, dim3(4u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              nullptr) == hipSuccess,
              "a launch for the durations");
        check(hipDeviceSynchronize() == hipSuccess, "its synchronisation");
        check(read_counters(&after), "the counters are readable after the launch");
        check(after.submit_calls > before.submit_calls, "the launch adds to submit_calls");
        check_u64(after.submit_calls - before.submit_calls,
                  after.submissions - before.submissions,
                  "the mock refuses no submission, so submit_calls follows submissions");
        check(after.submit_ticks > before.submit_ticks, "the launch adds to submit_ticks");
        check(after.waits_fast_ticks > before.waits_fast_ticks,
              "a wait on retired work adds to waits_fast_ticks");
        check(after.waits_fast_ticks_max != 0u, "and raises waits_fast_ticks_max");
        before = after;

        // A dispatch the mock holds for 40 ms. The wait sleeps until it retires, so it is not a
        // fast wait, and it is long: GetTickCount64, the clock of the hold, ticks every 15.6 ms,
        // so the wait is at least 24 ms, and 10 ms of ticks is a bound with room on both sides.
        bc250hsa_mock_set_hold_ms(40u);
        check(hipLaunchKernel(&g_stub_vadd, dim3(4u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                              nullptr) == hipSuccess,
              "a held launch for the durations");
        check(hipDeviceSynchronize() == hipSuccess, "its synchronisation");
        bc250hsa_mock_set_hold_ms(0u);
        check(read_counters(&after), "the counters are readable after the held launch");
        const uint64_t ten_ms = after.ticks_per_second / 100u;
        check(after.waits_other_ticks - before.waits_other_ticks >= ten_ms,
              "a wait for a held dispatch adds at least 10 ms to waits_other_ticks");
        check(after.waits_other_ticks_max >= ten_ms, "and raises waits_other_ticks_max to it");
        check(after.waits_fast_ticks_max <= after.waits_fast_ticks,
              "waits_fast_ticks_max is not above its total");
        check(after.waits_other_ticks_max <= after.waits_other_ticks,
              "waits_other_ticks_max is not above its total");
        check(after.ticks_per_second == before.ticks_per_second,
              "the tick frequency does not change inside a process");

        // The sizes bc250hipCounters has had (hip_runtime.h). A caller passes its own size and
        // gets exactly its own fields: nothing past them is written, and every other size is
        // refused with nothing written at all.
        const unsigned sizes[3] = {
            static_cast<unsigned>(offsetof(bc250hipCounters, gpu_copy_d2h)),
            static_cast<unsigned>(offsetof(bc250hipCounters, ticks_per_second)),
            static_cast<unsigned>(sizeof(bc250hipCounters)) };
        check_u64(sizes[0], 56u, "the first bc250hipCounters layout is 56 bytes");
        check_u64(sizes[1], 160u, "the GPU copy layout is 160 bytes");
        check_u64(sizes[2], 256u, "the duration layout is 256 bytes");
        for (int k = 0; k < 3; ++k) {
            alignas(bc250hipCounters) unsigned char raw[sizeof(bc250hipCounters) + 32];
            std::memset(raw, 0xCD, sizeof(raw));
            std::memcpy(raw, &sizes[k], sizeof(sizes[k]));
            check(bc250hipGetCounters(reinterpret_cast<bc250hipCounters*>(raw)) == hipSuccess,
                  "bc250hipGetCounters takes a size it has had");
            check(read_counters(&after), "the counters are readable beside the sized read");
            size_t untouched = 0;
            for (size_t i = sizes[k]; i < sizeof(raw); ++i) {
                untouched += raw[i] == 0xCD ? 1u : 0u;
            }
            check_u64(untouched, sizeof(raw) - sizes[k], "nothing past the caller's size");
            bc250hipCounters got;
            std::memset(&got, 0, sizeof(got));
            std::memcpy(&got, raw, sizes[k]);
            check_u64(got.struct_bytes, sizes[k], "struct_bytes keeps the caller's size");
            check_u64(got.dispatches, after.dispatches_built, "the first layout has dispatches");
            check_u64(got.waits, after.waits, "the first layout has the waits");
            if (sizes[k] >= 160u) {
                check_u64(got.gpu_copy_d2h, after.gpu_copy_d2h, "the copy layout has its counts");
                check_u64(got.cpu_copy_fallback, after.cpu_copy_fallback,
                          "the copy layout has the fallback count");
            }
            if (sizes[k] >= 256u) {
                check_u64(got.ticks_per_second, after.ticks_per_second,
                          "the duration layout has the tick frequency");
                check_u64(got.waits_other_ticks_max, after.waits_other_ticks_max,
                          "the duration layout has the wait maximum");
                check_u64(got.cpu_copy_fallback_ticks, after.cpu_copy_fallback_ticks,
                          "the duration layout ends with the fallback ticks");
            }
        }
        const unsigned wrong[6] = { 0u, 8u, 55u, 64u, 248u, 264u };
        for (int k = 0; k < 6; ++k) {
            alignas(bc250hipCounters) unsigned char raw[sizeof(bc250hipCounters) + 32];
            std::memset(raw, 0xCD, sizeof(raw));
            std::memcpy(raw, &wrong[k], sizeof(wrong[k]));
            check(bc250hipGetCounters(reinterpret_cast<bc250hipCounters*>(raw)) ==
                      hipErrorInvalidValue,
                  "bc250hipGetCounters refuses a size it never had");
            size_t untouched = 0;
            for (size_t i = sizeof(wrong[k]); i < sizeof(raw); ++i) {
                untouched += raw[i] == 0xCD ? 1u : 0u;
            }
            check_u64(untouched, sizeof(raw) - sizeof(wrong[k]), "and writes nothing");
            (void)hipGetLastError();
        }

        // A reset clears the durations with the rest, and the frequency is not a counter.
        bc250hsa_counters_reset();
        check(read_counters(&after), "the counters are readable after the reset");
        check_u64(after.waits_fast_ticks + after.waits_fast_ticks_max + after.waits_other_ticks +
                      after.waits_other_ticks_max + after.submit_calls + after.submit_ticks,
                  0u, "the reset clears the wait and submission durations");
        for (int i = 0; i < kLineCount; ++i) {
            check_u64(copy_ticks(after, i), 0u, "the reset clears every copy duration");
        }
        check(after.ticks_per_second != 0u, "the reset leaves the tick frequency");

        check(hipHostFree(pinned_dst) == hipSuccess, "hipHostFree of the duration test");
        check(hipFree(device_dst) == hipSuccess, "hipFree of the duration destination");
        check(hipFree(device_src) == hipSuccess, "hipFree of the duration source");
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
    }

    // ---------------------------------------------------------------------------------------
    // A GPU copy or fill that fails, from the side layer 2 answers it (the review of
    // 2026-10-11, and its two non-blocking corrections). The mock injects the failure
    // before or after the submission; after it, the bytes never land, so a host-processor copy
    // that layer 2 must not do would show in the destination.
    std::printf("test_hip_mock: 14. a GPU copy or fill that fails, and the coherence gate\n");
    {
        const size_t big = 8192;
        unsigned char* device_src = nullptr;
        unsigned char* device_dst = nullptr;
        unsigned char* pinned_dst = nullptr;
        std::vector<unsigned char> pageable(big, 0u);
        std::vector<unsigned char> seed(big, 0u);
        for (size_t i = 0; i < big; ++i) {
            seed[i] = static_cast<unsigned char>(i * 13u + 5u);
        }
        check(hipMalloc(reinterpret_cast<void**>(&device_src), big) == hipSuccess,
              "hipMalloc the failure source");
        check(hipMalloc(reinterpret_cast<void**>(&device_dst), big) == hipSuccess,
              "hipMalloc the failure destination");
        check(hipHostMalloc(reinterpret_cast<void**>(&pinned_dst), big, hipHostMallocDefault) ==
                  hipSuccess,
              "hipHostMalloc the failure destination");
        check(hipMemcpy(device_src, seed.data(), big, hipMemcpyHostToDevice) == hipSuccess,
              "the failure source holds the pattern");
        const uint32_t violations_before = bc250hsa_mock_free_violations();
        bc250hsa_counters before;
        bc250hsa_counters after;

        // (a) Each caller, with a failure after the submission. EOS and ENOMEM are statuses the
        //     old rule let fall back; ETIMEOUT is one it already refused. None of them may be
        //     followed by a host-processor copy or fill, a success counter or hipSuccess.
        const bc250hsa_status after_submit[3] = { BC250HSA_EOS, BC250HSA_ENOMEM,
                                                  BC250HSA_ETIMEOUT };
        const char* const callers[4] = { "direct device to host", "staged device to host",
                                         "device to device", "GPU fill" };
        for (int k = 0; k < 3; ++k) {
            for (int caller = 0; caller < 4; ++caller) {
                char what[160];
                std::snprintf(what, sizeof(what), "%s, %s after the submission", callers[caller],
                              bc250hsa_status_string(after_submit[k]));
                bc250hsa_mock_reset();
                // The destination is dirty before the call, with a pattern no path writes.
                std::memset(pinned_dst, 0xEE, big);
                std::memset(pageable.data(), 0xEE, big);
                check(hipMemset(device_dst, 0xEE, big) == hipSuccess, "a dirty destination");
                bc250hsa_counters_reset();
                check(read_counters(&before), "the counters before the failure");
                bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_AFTER, after_submit[k]);
                hipError_t err = hipSuccess;
                if (caller == 0) {
                    err = hipMemcpy(pinned_dst, device_src, big, hipMemcpyDeviceToHost);
                } else if (caller == 1) {
                    err = hipMemcpy(pageable.data(), device_src, big, hipMemcpyDeviceToHost);
                } else if (caller == 2) {
                    err = hipMemcpy(device_dst, device_src, big, hipMemcpyDeviceToDevice);
                } else {
                    bc250hip::state().gpu_fill_mapped = true;
                    err = hipMemset(device_dst, 0x33, big);
                    bc250hip::state().gpu_fill_mapped = false;
                }
                bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_NONE, BC250HSA_OK);
                check(err != hipSuccess, what);
                check(hipGetLastError() == err, "and the thread's error is that error");
                check(read_counters(&after), "the counters after the failure");
                check_u64(after.cpu_copy_fallback - before.cpu_copy_fallback, 0u,
                          "no host-processor copy follows work the device accepted");
                check_u64(after.gpu_copy_d2h + after.gpu_copy_d2h_staged + after.gpu_copy_d2d +
                              after.gpu_fill,
                          0u, "and no GPU success counter moves");
                uint32_t record_index = 0;
                check(first_of_kind(BC250HSA_MOCK_COPY_FROM_DEVICE, &record_index) == nullptr,
                      "and the backend saw no host-processor copy");
                // The destination is read back with the fault cleared.
                const unsigned char* got = nullptr;
                std::vector<unsigned char> readback(big, 0u);
                if (caller == 0) {
                    got = pinned_dst;
                } else if (caller == 1) {
                    got = pageable.data();
                } else {
                    check(hipMemcpy(readback.data(), device_dst, big, hipMemcpyDeviceToHost) ==
                              hipSuccess,
                          "read the device destination back");
                    got = readback.data();
                }
                size_t dirty = 0;
                for (size_t i = 0; i < big; ++i) {
                    dirty += got[i] == 0xEE ? 1u : 0u;
                }
                check_u64(dirty, big, "the destination holds no byte of a replacement copy");
            }
        }

        // (b) A failure before the submission on a device that still runs: nothing of the
        //     attempt is outstanding, so the host processor does the copy and it is counted.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        std::memset(pinned_dst, 0xEE, big);
        bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_BEFORE, BC250HSA_EOS);
        check(hipMemcpy(pinned_dst, device_src, big, hipMemcpyDeviceToHost) == hipSuccess,
              "a refusal before the submission falls back");
        check(std::memcmp(pinned_dst, seed.data(), big) == 0,
              "and the host processor moved the bytes");
        check(read_counters(&after), "the counters after the early refusal");
        check_u64(after.cpu_copy_fallback, 1u, "the early refusal is one fallback");
        // The same refusal on a lost device is not a fallback: a lost device owns its memory.
        bc250hsa_counters_reset();
        std::memset(pinned_dst, 0xEE, big);
        bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_BEFORE, BC250HSA_EDEVICELOST);
        check(hipMemcpy(pinned_dst, device_src, big, hipMemcpyDeviceToHost) != hipSuccess,
              "a lost device before the submission is an error");
        (void)hipGetLastError();
        check(read_counters(&after), "the counters after the lost device");
        check_u64(after.cpu_copy_fallback, 0u, "and no fallback");
        check(pinned_dst[0] == 0xEE && pinned_dst[big - 1] == 0xEE,
              "and the destination is untouched");
        bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_NONE, BC250HSA_OK);

        // (c) A failed wait leaves the fence value with the device. hipFree must wait for it
        //     before the memory goes back. The control first: the mock's own count sees a free
        //     of memory whose fence value has not retired, so the check below can fail.
        {
            bc250hsa_mem raw_a;
            bc250hsa_mem raw_b;
            std::memset(&raw_a, 0, sizeof(raw_a));
            std::memset(&raw_b, 0, sizeof(raw_b));
            check(bc250hsa_alloc(probe_device, 4096, 0, BC250HSA_MEM_HOST, &raw_a) ==
                      BC250HSA_OK,
                  "a raw allocation for the free control");
            check(bc250hsa_alloc(probe_device, 4096, 0, BC250HSA_MEM_HOST, &raw_b) ==
                      BC250HSA_OK,
                  "a second raw allocation for the free control");
            bc250hsa_mock_reset();
            bc250hsa_mock_set_hold_ms(200u);
            bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_AFTER, BC250HSA_ETIMEOUT);
            bc250hsa_op_receipt receipt;
            std::memset(&receipt, 0, sizeof(receipt));
            receipt.struct_bytes = static_cast<uint32_t>(sizeof(receipt));
            check(bc250hsa_copy_device_to_device(probe_device, &raw_a, 0, &raw_b, 0, 4096, 0u,
                                                 &receipt) == BC250HSA_ETIMEOUT,
                  "the control copy times out after its submission");
            check_u64(receipt.state, BC250HSA_OP_SUBMITTED, "and its receipt says so");
            check(receipt.fence_value != 0u, "and names a fence value");
            const uint32_t control_before = bc250hsa_mock_free_violations();
            check(bc250hsa_free(probe_device, &raw_a) == BC250HSA_OK, "the control frees early");
            check_u64(bc250hsa_mock_free_violations() - control_before, 1u,
                      "the mock counts a free before the fence value retired");
            check(bc250hsa_wait(probe_device, receipt.fence_value, 0, 7000) == BC250HSA_OK,
                  "the fence value retires");
            check(bc250hsa_free(probe_device, &raw_b) == BC250HSA_OK, "the second free");
            check_u64(bc250hsa_mock_free_violations() - control_before, 1u,
                      "a free after the fence value retired is not counted");
            bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_NONE, BC250HSA_OK);
            bc250hsa_mock_set_hold_ms(0u);
        }
        {
            unsigned char* doomed = nullptr;
            check(hipMalloc(reinterpret_cast<void**>(&doomed), big) == hipSuccess,
                  "hipMalloc a destination to free after a failed wait");
            bc250hsa_mock_reset();
            bc250hsa_mock_set_hold_ms(200u);
            bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_AFTER, BC250HSA_ETIMEOUT);
            const uint32_t runtime_before = bc250hsa_mock_free_violations();
            check(hipMemcpy(doomed, device_src, big, hipMemcpyDeviceToDevice) != hipSuccess,
                  "a device-to-device copy whose wait fails");
            (void)hipGetLastError();
            bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_NONE, BC250HSA_OK);
            check(hipFree(doomed) == hipSuccess, "hipFree of its destination");
            check_u64(bc250hsa_mock_free_violations() - runtime_before, 0u,
                      "hipFree waited for the fence value the failed copy left");
            bc250hsa_mock_set_hold_ms(0u);
        }

        // (d) The coherence gate follows the destination (R4). The GPU address of a host
        //     allocation is a device-to-device destination the host processor reads through
        //     its own mapping. Its host lines are dirty before the copy, and the copy lands at
        //     an interior offset, so the bytes around it show a copy that went too far.
        void* alias = nullptr;
        check(hipHostGetDevicePointer(&alias, pinned_dst, 0) == hipSuccess,
              "the GPU address of the host allocation");
        const size_t inner = 256;
        const size_t span = 4096;
        for (int coherent = 0; coherent < 2; ++coherent) {
            bc250hsa_mock_reset();
            bc250hsa_counters_reset();
            bc250hsa_mock_set_gpu_copy(coherent, 0);
            std::memset(pinned_dst, 0xEE, big);
            check(hipMemcpy(static_cast<unsigned char*>(alias) + inner, device_src, span,
                            hipMemcpyDeviceToDevice) == hipSuccess,
                  coherent ? "a copy into the alias with the control passed"
                           : "a copy into the alias with the control failed");
            check(std::memcmp(pinned_dst + inner, seed.data(), span) == 0,
                  "the host pointer reads the copied bytes at the interior offset");
            size_t outside = 0;
            for (size_t i = 0; i < big; ++i) {
                if (i < inner || i >= inner + span) {
                    outside += pinned_dst[i] == 0xEE ? 1u : 0u;
                }
            }
            check_u64(outside, big - span, "and no byte around them changed");
            check(read_counters(&after), "the counters after the alias copy");
            uint32_t record_index = 0;
            const bool dma = first_of_kind(BC250HSA_MOCK_GPU_COPY, &record_index) != nullptr;
            if (coherent) {
                check(dma, "with the control passed the copy is one packet");
                check_u64(after.gpu_copy_d2d, 1u, "and counts as a device-to-device copy");
                check_u64(after.cpu_copy_fallback, 0u, "and takes no fallback");
            } else {
                check(!dma, "with the control failed no packet is emitted for the alias");
                check_u64(after.gpu_copy_d2d, 0u, "and no GPU copy is counted");
                check_u64(after.cpu_copy_fallback, 1u, "the host processor did the copy");
                check_u64(after.gpu_copy_control_failed, 1u, "and the control counter says why");
            }
        }
        // A hipMalloc buffer has a host mapping too, so it is gated as well.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        bc250hsa_mock_set_gpu_copy(0, 0);
        check(hipMemcpy(device_dst, device_src, big, hipMemcpyDeviceToDevice) == hipSuccess,
              "a device-to-device copy into a mapped device buffer, control failed");
        check(read_counters(&after), "the counters after the mapped copy");
        check_u64(after.gpu_copy_d2d, 0u, "a mapped device destination is gated too");
        check_u64(after.cpu_copy_fallback, 1u, "and the host processor copied it");
        bc250hsa_mock_set_gpu_copy(1, 0);

        // (e) A host-processor copy that fails is not a fallback (non-blocking correction 1).
        //     The source is device memory with no host mapping, which hipMalloc never makes on
        //     this part, so the test enters it into the table itself, and layer 1 refuses the
        //     GPU route: the replacement read then has nothing to read.
        {
            bc250hsa_mem unmapped;
            std::memset(&unmapped, 0, sizeof(unmapped));
            check(bc250hsa_alloc(probe_device, big, 0, BC250HSA_MEM_DEVICE, &unmapped) ==
                      BC250HSA_OK,
                  "an allocation with no host mapping");
            check(unmapped.host == nullptr, "and it has none");
            {
                bc250hip::Guard guard;
                bc250hip::Allocation entry;
                entry.mem = unmapped;
                bc250hip::state().by_va[unmapped.va] = entry;
            }
            bc250hsa_mock_reset();
            bc250hsa_counters_reset();
            bc250hsa_mock_set_gpu_copy(1, 1);
            check(hipMemcpy(pageable.data(),
                            reinterpret_cast<void*>(static_cast<uintptr_t>(unmapped.va)), big,
                            hipMemcpyDeviceToHost) == hipErrorNotSupported,
                  "a refused copy out of unmapped memory has no replacement");
            (void)hipGetLastError();
            bc250hsa_mock_set_gpu_copy(1, 0);
            check(read_counters(&after), "the counters after the failed replacement");
            check_u64(after.cpu_copy_fallback, 0u, "a failed host-processor copy is not counted");
            check_u64(after.cpu_copy_fallback_bytes, 0u, "and neither are its bytes");
            check(hipFree(reinterpret_cast<void*>(static_cast<uintptr_t>(unmapped.va))) ==
                      hipSuccess,
                  "hipFree of the unmapped allocation");
        }

        // (f) BC250_HIP_GPU_COPY_MIN takes decimal digits only (non-blocking correction 2).
        {
            struct ParseCase {
                const char* text;
                bool        ok;
                uint32_t    value;
            };
            const ParseCase cases[] = {
                { "4096", true, 4096u },        { "0", true, 0u },
                { "4294967295", true, 4294967295u },
                { "4294967296", false, 0u },    { "-1", false, 0u },
                { "+5", false, 0u },            { " 5", false, 0u },
                { "12abc", false, 0u },         { "", false, 0u },
                { "99999999999999999999999", false, 0u },
            };
            for (const ParseCase& c : cases) {
                uint32_t value = 77u;
                char what[120];
                std::snprintf(what, sizeof(what), "BC250_HIP_GPU_COPY_MIN '%s' is %s", c.text,
                              c.ok ? "taken" : "refused");
                check(bc250hip::parse_copy_min(c.text, &value) == c.ok, what);
                check_u64(value, c.ok ? c.value : 77u,
                          c.ok ? "and reads as its number" : "and leaves the value alone");
            }
        }

        check_u64(bc250hsa_mock_free_violations() - violations_before, 1u,
                  "the only early free of this section is the mock's own control");
        check(hipHostFree(pinned_dst) == hipSuccess, "hipHostFree of the failure test");
        check(hipFree(device_dst) == hipSuccess, "hipFree of the failure destination");
        check(hipFree(device_src) == hipSuccess, "hipFree of the failure source");
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
    }

    // ---------------------------------------------------------------------------------------
    // A copy whose wait ended without retirement, from the side of the host processor
    // (bc250hsa.h section 5.1, CONTRACT.md deviation 13.23). The mock keeps the copy's fence
    // value unretired and lands its bytes only when the test retires it late. Until then every
    // host-processor path of layer 2 must answer the copy's error and leave every byte as it
    // was: a device-to-host copy that reads the copy's destination, the GPU route that layer 1
    // refuses with nothing submitted, a small device-to-device copy, a write over the copy's
    // source, a fill of its destination, a host-to-host, a two-dimensional and a peer copy, and
    // a GPU fill that layer 1 refuses. The calls run on a stream of their own, which waits for
    // nothing, as llama.cpp's do. Afterwards the same calls move the copy's bytes. Last, a device
    // that layer 1 marked lost refuses the same paths with no copy unretired at all.
    std::printf("test_hip_mock: 15. no host-processor copy while a copy has not retired\n");
    {
        const size_t big = 8192;
        const size_t small = 256;
        const size_t at = 64;                  // inside the copy's destination
        unsigned char* src = nullptr;          // the unretired copy's source
        unsigned char* dst = nullptr;          // and its destination
        unsigned char* other = nullptr;        // a device buffer the copy does not name
        hipStream_t stream = nullptr;
        std::vector<unsigned char> seed(big, 0u);
        std::vector<unsigned char> old(big, 0x11u);
        std::vector<unsigned char> host(big, 0u);
        std::vector<unsigned char> payload(small, 0x77u);
        for (size_t i = 0; i < big; ++i) {
            seed[i] = static_cast<unsigned char>(i * 7u + 3u);
        }
        check(hipMalloc(reinterpret_cast<void**>(&src), big) == hipSuccess,
              "hipMalloc the unretired copy's source");
        check(hipMalloc(reinterpret_cast<void**>(&dst), big) == hipSuccess,
              "hipMalloc the unretired copy's destination");
        check(hipMalloc(reinterpret_cast<void**>(&other), big) == hipSuccess,
              "hipMalloc a buffer the copy does not name");
        check(hipStreamCreate(&stream) == hipSuccess, "hipStreamCreate for the unretired copy");
        const bc250hsa_status kinds[2] = { BC250HSA_ETIMEOUT, BC250HSA_EOS };
        const hipError_t errors[2] = { hipErrorLaunchTimeOut, hipErrorUnknown };
        for (int k = 0; k < 2; ++k) {
            const hipError_t want = errors[k];
            char what[200];
            std::printf("  the copy's wait: %s\n", bc250hsa_status_string(kinds[k]));
            check(hipMemcpy(src, seed.data(), big, hipMemcpyHostToDevice) == hipSuccess,
                  "the source holds the pattern");
            check(hipMemcpy(dst, old.data(), big, hipMemcpyHostToDevice) == hipSuccess,
                  "the destination holds the old bytes");
            bc250hsa_mock_reset();
            bc250hsa_counters_reset();
            bc250hsa_mock_set_gpu_fault(BC250HSA_MOCK_FAULT_UNRETIRED, kinds[k]);
            check(hipMemcpy(dst, src, big, hipMemcpyDeviceToDevice) == want,
                  "the copy fails with its wait's error and stays with the device");
            (void)hipGetLastError();

            // The case of the review: a device-to-host copy whose source lies inside the copy's
            // destination, with the host-processor route configured (BC250_HIP_GPU_COPY=0).
            bc250hip::state().gpu_copy_enabled = false;
            std::memset(host.data(), 0xEE, big);
            hipError_t err =
                hipMemcpyAsync(host.data(), dst + at, small, hipMemcpyDeviceToHost, stream);
            if (err == hipSuccess || host[0] != 0xEE) {
                std::printf("  a device-to-host copy on the host-processor route returned %s"
                            " while a copy was unretired (its wait: %s)\n",
                            hipGetErrorName(err), bc250hsa_status_string(kinds[k]));
            }
            std::snprintf(what, sizeof(what), "the host-processor route answers %s",
                          hipGetErrorName(want));
            check(err == want, what);
            check(hipGetLastError() == want, "and the thread's error is that error");
            bc250hip::state().gpu_copy_enabled = true;

            // The GPU route into the program's memory: layer 1 refuses it with nothing
            // submitted, and the host processor must not stand in.
            std::memset(host.data(), 0xEE, big);
            err = hipMemcpyAsync(host.data(), dst, big, hipMemcpyDeviceToHost, stream);
            if (err == hipSuccess || host[0] != 0xEE) {
                std::printf("  a device-to-host copy that layer 1 refused returned %s while a"
                            " copy was unretired (its wait: %s)\n",
                            hipGetErrorName(err), bc250hsa_status_string(kinds[k]));
            }
            check(err == want, "the refused GPU route is not replaced by the host processor");
            (void)hipGetLastError();
            size_t untouched = 0;
            for (size_t i = 0; i < big; ++i) {
                untouched += host[i] == 0xEE ? 1u : 0u;
            }
            check_u64(untouched, big, "no byte reached the program's buffer");

            // Below the threshold, a device-to-device copy is a memcpy of the two mappings.
            check(hipMemcpyAsync(other, dst + at, small, hipMemcpyDeviceToDevice, stream) ==
                      want,
                  "a host-processor device-to-device copy out of the destination is refused");
            (void)hipGetLastError();
            // A write over the copy's source, which the copy still reads.
            check(hipMemcpyAsync(src + at, payload.data(), small, hipMemcpyHostToDevice,
                                 stream) == want,
                  "a host-processor write over the copy's source is refused");
            (void)hipGetLastError();
            // A host-processor fill of the destination, the default for a mapped allocation.
            check(hipMemsetAsync(dst + at, 0x33, small, stream) == want,
                  "a host-processor fill of the copy's destination is refused");
            (void)hipGetLastError();
            // The synchronous call waits for the whole device first, and that wait fails.
            check(hipMemcpy(host.data(), dst + at, small, hipMemcpyDeviceToHost) != hipSuccess,
                  "a synchronous copy fails at its wait");
            (void)hipGetLastError();
            check(host[0] == 0xEE && host[small - 1] == 0xEE, "and moves no byte either");

            // The other exported routes that reach the host processor (review R3 of bde0ff88).
            // A host-to-host copy, which names no device memory, still waits for the copy.
            check(hipMemcpyAsync(host.data(), payload.data(), small, hipMemcpyHostToHost,
                                 stream) == want,
                  "a host-to-host copy is refused");
            (void)hipGetLastError();
            check(host[0] == 0xEE && host[small - 1] == 0xEE, "and moves no byte");
            // A two-dimensional copy goes row by row, and its first row is refused.
            check(hipMemcpy2DAsync(host.data(), 128, dst, 128, 64, 2, hipMemcpyDeviceToHost,
                                   stream) == want,
                  "a two-dimensional copy is refused at its first row");
            (void)hipGetLastError();
            check(host[0] == 0xEE && host[128] == 0xEE, "and neither row changed");
            // A peer copy is a device-to-device copy of device 0.
            check(hipMemcpyPeerAsync(other, 0, dst, 0, small, stream) == want,
                  "a peer copy is refused");
            (void)hipGetLastError();
            // A GPU fill of mapped memory (BC250_HIP_GPU_FILL=1) that layer 1 refuses does not
            // fall back to the host processor.
            bc250hip::state().gpu_fill_mapped = true;
            check(hipMemsetAsync(dst, 0x31, big, stream) == want,
                  "a GPU fill that layer 1 refused is not done by the host processor");
            (void)hipGetLastError();
            bc250hip::state().gpu_fill_mapped = false;
            // The synchronous fill waits for the whole device first, and that wait fails.
            check(hipMemset(dst, 0x31, small) != hipSuccess, "a synchronous fill fails at its wait");
            (void)hipGetLastError();

            bc250hsa_counters c;
            check(read_counters(&c), "the counters while the copy is unretired");
            check(first_of_kind(BC250HSA_MOCK_COPY_FROM_DEVICE, nullptr) == nullptr &&
                      first_of_kind(BC250HSA_MOCK_COPY_TO_DEVICE, nullptr) == nullptr,
                  "the backend saw no host-processor copy");
            check_u64(c.cpu_copy_fallback, 0u, "and no fallback was counted");
            check_u64(c.gpu_copy_unretired, 1u, "one copy is unretired");
            check_u64(c.gpu_copy_exclusion_refusals, 2u,
                      "layer 1 refused the two GPU attempts, a copy and a fill");
#if BC250HSA_ABI_VERSION_MINOR >= 8
            // A timeout of a GPU attempt is refused by the old rule already (no_fallback); every
            // other refusal is the new one, and counted: the host-processor device-to-host,
            // device-to-device, host-to-device, fill, host-to-host, two-dimensional and peer
            // calls, and with BC250HSA_EOS also the fallbacks of the GPU copy and the GPU fill.
            check_u64(c.cpu_copy_exclusion_refusals, k == 0 ? 7u : 9u,
                      "each host-processor refusal is counted");
#endif

            // The late retirement: the copy's bytes land and its value retires.
            bc250hsa_mock_retire_unretired();
            bc250hip::state().gpu_copy_enabled = false;
            err = hipMemcpyAsync(host.data(), dst + at, small, hipMemcpyDeviceToHost, stream);
            bc250hip::state().gpu_copy_enabled = true;
            check(err == hipSuccess, "after the late retirement the same call succeeds");
            check(std::memcmp(host.data(), seed.data() + at, small) == 0,
                  "with the bytes of the copy");
            check(hipMemcpyAsync(host.data(), dst, big, hipMemcpyDeviceToHost, stream) ==
                      hipSuccess,
                  "and the GPU route goes again");
            check(std::memcmp(host.data(), seed.data(), big) == 0,
                  "with every byte of the copy, the source unchanged");
            check(hipStreamSynchronize(stream) == hipSuccess, "the stream synchronises");
            check(read_counters(&c), "the counters after the late retirement");
            check_u64(c.gpu_copy_late_retirements, 1u, "one late retirement");
        }

        // A device that layer 1 marked lost (review R3 of bde0ff88), with no copy unretired.
        // A call on a stream with nothing of its own outstanding waits for nothing, so the read
        // of layer 1 is the one place that sees the loss, and it must refuse every
        // host-processor path with the loss.
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
        check(hipMemcpy(dst, seed.data(), big, hipMemcpyHostToDevice) == hipSuccess,
              "the destination holds the pattern before the loss");
        bc250hsa_mock_set_lost(1);
        std::memset(host.data(), 0xEE, big);
        bc250hip::state().gpu_copy_enabled = false;
        hipError_t lost = hipMemcpyAsync(host.data(), dst + at, small, hipMemcpyDeviceToHost,
                                         stream);
        bc250hip::state().gpu_copy_enabled = true;
        if (lost == hipSuccess || host[0] != 0xEE) {
            std::printf("  a host-processor copy on a lost device returned %s\n",
                        hipGetErrorName(lost));
        }
        check(lost == hipErrorContextIsDestroyed, "a host-processor copy on a lost device is refused");
        (void)hipGetLastError();
        check(host[0] == 0xEE && host[small - 1] == 0xEE, "and moves no byte");
        check(hipMemcpyAsync(other, dst + at, small, hipMemcpyDeviceToDevice, stream) ==
                  hipErrorContextIsDestroyed,
              "a host-processor device-to-device copy on a lost device is refused");
        (void)hipGetLastError();
        check(hipMemsetAsync(other, 0x44, small, stream) == hipErrorContextIsDestroyed,
              "a host-processor fill on a lost device is refused");
        (void)hipGetLastError();
#if BC250HSA_ABI_VERSION_MINOR >= 8
        {
            bc250hsa_counters c;
            check(read_counters(&c), "the counters on the lost device");
            check_u64(c.cpu_copy_exclusion_refusals, 3u, "each refusal on the lost device is counted");
        }
#endif
        bc250hsa_mock_set_lost(0);
        check(hipStreamDestroy(stream) == hipSuccess, "hipStreamDestroy of the unretired test");
        check(hipFree(other) == hipSuccess, "hipFree of the third buffer");
        check(hipFree(dst) == hipSuccess, "hipFree of the copy's destination");
        check(hipFree(src) == hipSuccess, "hipFree of the copy's source");
        bc250hsa_mock_reset();
        bc250hsa_counters_reset();
    }

    // ---------------------------------------------------------------------------------------
    std::printf("test_hip_mock: 16. the module unregisters\n");
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
