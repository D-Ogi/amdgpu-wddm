// test_hip_threads.cpp - layer 2 under several threads, over the mock bc250hsa backend.
//
// The owner's instruction of 2026-09-29: multithreading is tested with our own clients before a
// real application is asked to exercise it. The measurement itself lives in
// tests/host/hip_threads_client.h, which the lab program compute/hip/samples/threads.hip shares.
// This file gives that client a device, a registered code object and a launch, and it checks
// what only a build that links the runtime objects can check: the reference counts of the
// streams and the events.
//
// Two builds of this test exist and build-runtime.ps1 runs both:
//   * the product form, which does not hold the process lock over a wait. Every worker thread
//     must finish work while thread 0 waits for the device.
//   * the negative control, compiled with BC250_HIP_WAIT_UNDER_LOCK=1, which restores the
//     behaviour of the first build of layer 2. No worker may finish anything, and the test
//     refuses to run as a control unless the runtime it links really waits with the lock held.
//
// The mock backend holds each dispatch for a measured time (bc250hsa_mock_set_hold_ms), because
// a backend that retires a fence at once has no wait to measure.
//
// Usage: test_hip_threads.exe [<path of the fatbin fixture>] [--negative-control] [--hold <ms>]

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bc250hsa.h"
#include "hip/hip_runtime.h"
#include "hip_threads_client.h"
#include "hipmock_backend.h"
#include "runtime_internal.h"

namespace {

// The wait bound that this test gives the runtime. It must be longer than the hold time of the
// mock and shorter than the patience of a build gate.
const char* const kWaitTotalMs = "20000";

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("  FAIL %s\n", what);
    }
}

// The 24 bytes of .hipFatBinSegment, as clang writes them (design section 4.2).
struct FatBinWrapper {
    int32_t     magic;
    int32_t     version;
    const void* gpu_binary;
    const void* unused;
};

// The host stub of the kernel this test launches. Its address is the identity of the kernel, as
// it is in a compiled program.
const char g_stub_vadd = 0;

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

// One launch of the fixture's vadd kernel. Every argument sits on this thread's stack, because
// the client calls this from several threads at once.
hipError_t launch_vadd(hipStream_t stream, void* buffer, void* ctx) {
    (void)ctx;
    void* a = buffer;
    void* b = buffer;
    void* c = buffer;
    int   n = 64;
    void* args[4];
    args[0] = &a;
    args[1] = &b;
    args[2] = &c;
    args[3] = &n;
    return hipLaunchKernel(&g_stub_vadd, dim3(1u, 1u, 1u), dim3(64u, 1u, 1u), args, 0, stream);
}

}  // namespace

int main(int argc, char** argv) {
    // Before the first HIP call: the runtime reads its wait policy one time, at its first call.
#if defined(_WIN32)
    _putenv_s("BC250_HIP_WAIT_TOTAL_MS", kWaitTotalMs);
#else
    setenv("BC250_HIP_WAIT_TOTAL_MS", kWaitTotalMs, 1);
#endif

    const char* path = "compute/hip/tests/data/hip_test_kernels.gfx1013.fatbin";
    int         negative_control = 0;
    uint32_t    hold_ms = 200;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--negative-control") == 0) {
            negative_control = 1;
        } else if (std::strcmp(argv[i], "--hold") == 0 && i + 1 < argc) {
            ++i;
            hold_ms = static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 10));
        } else if (argv[i][0] != '-') {
            path = argv[i];
        } else {
            std::printf("test_hip_threads: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }

    std::printf("test_hip_threads: the runtime %s the process lock over a wait\n",
                bc250hip::waits_under_lock() ? "holds" : "opens");
    // The flag and the build must agree, or the measurement would prove the opposite of what it
    // says. This is why the negative control cannot pass by accident.
    check(bc250hip::waits_under_lock() == (negative_control != 0),
          "the build and the --negative-control flag agree");
    if (bc250hip::waits_under_lock() != (negative_control != 0)) {
        std::printf("test_hip_threads: %d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    size_t         fatbin_bytes = 0;
    unsigned char* fatbin = read_file(path, &fatbin_bytes);
    if (fatbin == nullptr) {
        std::printf("test_hip_threads: cannot read the fixture %s\n", path);
        return 2;
    }
    std::printf("test_hip_threads: fixture %s, %zu bytes\n", path, fatbin_bytes);

    bc250hsa_mock_set_hold_ms(hold_ms);
    std::printf("test_hip_threads: the mock holds every dispatch for %u ms\n",
                bc250hsa_mock_hold_ms());
    check(bc250hsa_mock_hold_ms() == hold_ms, "the mock took the hold time");

    FatBinWrapper wrapper;
    wrapper.magic = static_cast<int32_t>(0x48495046);  // 'HIPF'
    wrapper.version = 1;
    wrapper.gpu_binary = fatbin;
    wrapper.unused = nullptr;
    void** handle = __hipRegisterFatBinary(&wrapper);
    check(handle != nullptr, "__hipRegisterFatBinary accepts the fixture");
    check(__hipRegisterFunction(handle, &g_stub_vadd, const_cast<char*>("vadd"), "vadd", -1,
                                nullptr, nullptr, nullptr, nullptr, nullptr) == 0,
          "__hipRegisterFunction vadd");
    check(hipInit(0) == hipSuccess, "hipInit");

    // The kernel argument pool, before any thread runs. A buffer that one thread has taken and
    // not submitted yet must never be handed to a second thread: a launch may open the lock
    // between the packing and the submission (the event wait of its stream), and the second
    // thread would pack its own arguments over the first one's. The rule is "busy with no fence
    // value is not free", and this is its control.
    {
        bc250hip::Guard         guard;
        bc250hip::KernargBuffer* first = nullptr;
        bc250hip::KernargBuffer* second = nullptr;
        check(bc250hip::kernarg_acquire(guard, 64u, 16u, &first) == hipSuccess,
              "kernarg_acquire gives a buffer");
        check(bc250hip::kernarg_acquire(guard, 64u, 16u, &second) == hipSuccess,
              "kernarg_acquire gives a second buffer");
        check(first != nullptr && second != nullptr && first != second,
              "a kernel argument buffer that is taken and not submitted is not handed out twice");
        bc250hip::kernarg_release(first, 0);
        bc250hip::kernarg_release(second, 0);
    }

    const int streams_before = bc250hip::state().live_streams;
    const int events_before = bc250hip::state().live_events;

    bc250hipthreads::Options options = bc250hipthreads::default_options(launch_vadd, nullptr);
    options.negative_control = negative_control;
    bc250hipthreads::Result result;
    (void)bc250hipthreads::run(options, &result);
    check(result.api_failures == 0, "every HIP call of the client answered hipSuccess");
    check(result.verdict_failures == 0, "every verdict of the client held");

    // The reference counts: every stream and every event of the client is gone, the ones that
    // another thread destroyed inside a wait among them. A count that never returns to its
    // starting value is a leak, and a count that falls below it is a double free.
    check(bc250hip::state().live_streams == streams_before,
          "no stream of the client is left behind");
    check(bc250hip::state().live_events == events_before,
          "no event of the client is left behind");
    std::printf("test_hip_threads: live streams %d, live events %d\n",
                bc250hip::state().live_streams, bc250hip::state().live_events);

    // The wait policy of the process reached every wait, the ones inside the threads among them.
    {
        const uint32_t want = static_cast<uint32_t>(std::strtoul(kWaitTotalMs, nullptr, 10));
        uint32_t       waits = 0;
        uint32_t       wrong = 0;
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
        std::printf("test_hip_threads: %u waits recorded, %u of them without the bound\n", waits,
                    wrong);
        check(waits > 0u, "the client waited at least once");
        check(wrong == 0u, "every wait carries the wait bound of the process");
    }

    // The same rule, now for a buffer of the pool that a launch has already used once. The
    // client's launches left every buffer busy with a fence value the device has retired, which
    // is what makes such a buffer free again. Taking it must still mark it as taken, or two
    // threads pack their arguments into one buffer, as they would for a buffer that was never
    // used. This control runs after the client, because only then does the pool hold buffers
    // with a retired fence value.
    {
        bc250hip::Guard          guard;
        bc250hip::KernargBuffer* first = nullptr;
        bc250hip::KernargBuffer* second = nullptr;
        check(bc250hip::kernarg_acquire(guard, 64u, 16u, &first) == hipSuccess,
              "kernarg_acquire gives a buffer of the used pool");
        check(bc250hip::kernarg_acquire(guard, 64u, 16u, &second) == hipSuccess,
              "kernarg_acquire gives a second buffer of the used pool");
        check(first != nullptr && second != nullptr && first != second,
              "a reused kernel argument buffer that is taken and not submitted is not handed "
              "out twice");
        bc250hip::kernarg_release(first, 0);
        bc250hip::kernarg_release(second, 0);
    }

    // A counter of layer 1 for the same waits, which is the second witness of the measurement.
    {
        bc250hsa_counters counters;
        std::memset(&counters, 0, sizeof(counters));
        counters.struct_bytes = static_cast<uint32_t>(sizeof(counters));
        check(bc250hsa_counters_read(&counters) == BC250HSA_OK, "bc250hsa_counters_read");
        std::printf("test_hip_threads: submissions %llu, waits %llu, fast waits %llu, "
                    "timeouts %llu\n",
                    static_cast<unsigned long long>(counters.submissions),
                    static_cast<unsigned long long>(counters.waits),
                    static_cast<unsigned long long>(counters.waits_fast),
                    static_cast<unsigned long long>(counters.waits_timed_out));
        check(counters.waits_timed_out == 0u, "no wait of the client timed out");
    }

    __hipUnregisterFatBinary(handle);
    std::free(fatbin);

    std::printf("test_hip_threads: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
