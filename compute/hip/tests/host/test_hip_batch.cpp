// test_hip_batch.cpp - batching, from the side layer 2 sees it (M16 route B, section 8.1).
//
// Two things can go wrong with batching, and this test is about the second one.
//
//   1. The dwords. A batched indirect buffer must hold one head, one completion write and the
//      right barrier between two dispatches. That is layer 1's business and
//      tests/host/test_pm4.c proves it against a golden stream, with a negative control.
//
//   2. The flush points. A dispatch that sits in an open buffer has a fence value that the
//      device cannot reach yet, so every path through which a program can observe the device
//      must submit that buffer first. A forgotten flush point is not a slow program: it is a
//      wait that never ends. This test drives those paths through the HIP entry points, with
//      the mock backend emulating a batch (hipmock_backend.c), so a forgotten flush shows up
//      here as a failed wait and not on the lab as a hang.
//
// The oracle of the correctness check is the dispatch record of the mock: the same 1000
// dependent launches are run with batching off and with batching on, and the two record
// streams must be equal dispatch for dispatch, kernel name, grid, block and packed kernel
// argument bytes included. The mock runs no instruction, so "exact results" means exactly
// that: the device is asked for the same work in the same order. The arithmetic of a
// dependent chain is checked on the hardware by compute/hip/samples/hipbench.hip
// --expect-compute, which is the lab arm of the plan beside it.
//
// It needs no GPU and no AMDGPU compiler: the code object is the committed fixture.
//
// Usage: test_hip_batch.exe [<path of the fatbin fixture>]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "bc250hsa.h"
#include "hip/hip_runtime.h"
#include "hipmock_backend.h"

namespace {

const char* const kWaitTotalMs = "7000";

// The chain of the correctness check. 1000 launches, as the brief asks, and the mock's record
// holds 4096 entries, so one arm fits with room for the waits.
const long long kChain = 1000;

// The dispatch cap of the batched arm, and a time cap so long that it never fires: the
// submission count of this test must come from the dispatch cap alone and not from how fast
// the machine runs it. 4000000000 microseconds is 4000 seconds.
const uint32_t kBatchMax = 32;
const uint32_t kNeverHold = 4000000000u;

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

struct FatBinWrapper {
    int32_t     magic;
    int32_t     version;
    const void* gpu_binary;
    const void* unused;
};

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

// What one dispatch of the record says, kept so that two arms can be compared.
struct Seen {
    char          kernel[64];
    uint32_t      grid[3];
    uint32_t      block[3];
    uint32_t      kernarg_bytes;
    unsigned char kernarg[BC250HSA_MOCK_KERNARG_MAX];
    uint64_t      value;
};

std::vector<Seen> collect_dispatches() {
    std::vector<Seen> out;
    const uint32_t count = bc250hsa_mock_record_count();
    for (uint32_t i = 0; i < count; ++i) {
        const bc250hsa_mock_record* record = bc250hsa_mock_record_at(i);
        if (record == nullptr || record->kind != BC250HSA_MOCK_DISPATCH) {
            continue;
        }
        Seen seen;
        std::memset(&seen, 0, sizeof(seen));
        std::memcpy(seen.kernel, record->kernel, sizeof(seen.kernel));
        std::memcpy(seen.grid, record->grid, sizeof(seen.grid));
        std::memcpy(seen.block, record->block, sizeof(seen.block));
        seen.kernarg_bytes = record->kernarg_bytes;
        std::memcpy(seen.kernarg, record->kernarg, sizeof(seen.kernarg));
        seen.value = record->value;
        out.push_back(seen);
    }
    return out;
}

bc250hsa_counters read_counters() {
    bc250hsa_counters counters;
    std::memset(&counters, 0, sizeof(counters));
    counters.struct_bytes = static_cast<uint32_t>(sizeof(counters));
    if (bc250hsa_counters_read(&counters) != BC250HSA_OK) {
        std::memset(&counters, 0, sizeof(counters));
    }
    return counters;
}

// One arm: kChain launches of the same kernel on one stream, then one synchronisation.
// It returns what the device was asked for and how many submissions it took.
std::vector<Seen> run_arm(const char* name, bc250hsa_device* dev,
                          const bc250hsa_batch_policy& policy, void** args, hipStream_t stream,
                          uint64_t* submissions_out, uint64_t* batches_out,
                          uint64_t* batched_out) {
    bc250hsa_batch_policy set = policy;
    set.struct_bytes = static_cast<uint32_t>(sizeof(set));
    check(bc250hsa_batch_policy_set(dev, &set) == BC250HSA_OK, "the backend takes the policy");
    bc250hsa_mock_reset();
    const bc250hsa_counters before = read_counters();

    bool launches_ok = true;
    for (long long i = 0; i < kChain; ++i) {
        if (hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                            stream) != hipSuccess) {
            launches_ok = false;
            break;
        }
    }
    check(launches_ok, "every launch of the arm answered hipSuccess");
    // The flush point that matters: a value the device has not been given yet cannot retire,
    // so this call must submit the open buffer. A missing flush is a failed wait here.
    check(hipStreamSynchronize(stream) == hipSuccess, "the synchronisation of the arm ended");

    const bc250hsa_counters after = read_counters();
    *submissions_out = after.submissions - before.submissions;
    *batches_out = after.batches_submitted - before.batches_submitted;
    *batched_out = after.dispatches_batched - before.dispatches_batched;
    std::vector<Seen> seen = collect_dispatches();
    std::printf("test_hip_batch: arm %-14s %zu dispatches, %llu submissions\n", name,
                seen.size(), (unsigned long long)*submissions_out);
    return seen;
}

bool same(const std::vector<Seen>& a, const std::vector<Seen>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::strcmp(a[i].kernel, b[i].kernel) != 0) {
            return false;
        }
        for (int k = 0; k < 3; ++k) {
            if (a[i].grid[k] != b[i].grid[k] || a[i].block[k] != b[i].block[k]) {
                return false;
            }
        }
        if (a[i].kernarg_bytes != b[i].kernarg_bytes) {
            return false;
        }
        if (std::memcmp(a[i].kernarg, b[i].kernarg, a[i].kernarg_bytes) != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // Before the first HIP call: the runtime reads its wait policy one time, at its first call.
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
        std::printf("test_hip_batch: cannot read the fixture '%s'\n", path);
        return 2;
    }

    FatBinWrapper wrapper;
    std::memset(&wrapper, 0, sizeof(wrapper));
    wrapper.magic = static_cast<int32_t>(0x48495046);   /* 'HIPF' */
    wrapper.version = 1;
    wrapper.gpu_binary = fatbin;
    void** handle = __hipRegisterFatBinary(&wrapper);
    check(handle != nullptr, "the fixture registers");
    check(__hipRegisterFunction(handle, &g_stub_vadd, const_cast<char*>("vadd"), "vadd", -1,
                                nullptr, nullptr, nullptr, nullptr, nullptr) == 0,
          "the kernel registers");

    check(hipInit(0) == hipSuccess, "hipInit");

    // The same device layer 2 opened. The mock holds one, and bc250hsa_open hands it out again
    // with a reference of its own, so this test can set the policy between two arms. A program
    // sets it one time through the environment (runtime/hip_device.cpp); a test needs both
    // values in one process.
    bc250hsa_device* dev = nullptr;
    check(bc250hsa_open(nullptr, &dev) == BC250HSA_OK, "the test holds the mock device");
    if (dev == nullptr) {
        std::printf("test_hip_batch: no device\n");
        return 2;
    }

    float* device_a = nullptr;
    float* device_b = nullptr;
    float* device_c = nullptr;
    const int n = 4096;
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    check(hipMalloc(reinterpret_cast<void**>(&device_a), bytes) == hipSuccess, "hipMalloc a");
    check(hipMalloc(reinterpret_cast<void**>(&device_b), bytes) == hipSuccess, "hipMalloc b");
    check(hipMalloc(reinterpret_cast<void**>(&device_c), bytes) == hipSuccess, "hipMalloc c");
    void* args[4];
    args[0] = &device_a;
    args[1] = &device_b;
    args[2] = &device_c;
    args[3] = const_cast<int*>(&n);

    hipStream_t stream = nullptr;
    check(hipStreamCreate(&stream) == hipSuccess, "hipStreamCreate");

    // -------------------------------------------------------------------------------------
    std::printf("test_hip_batch: 1. the same work with batching off and on\n");
    bc250hsa_batch_policy off;
    std::memset(&off, 0, sizeof(off));
    off.struct_bytes = static_cast<uint32_t>(sizeof(off));
    off.enabled = 0;
    bc250hsa_batch_policy on = off;
    on.enabled = 1;
    on.max_dispatches = kBatchMax;
    on.max_hold_us = kNeverHold;

    uint64_t off_submissions = 0, off_batches = 0, off_batched = 0;
    uint64_t on_submissions = 0, on_batches = 0, on_batched = 0;
    const std::vector<Seen> as_off =
        run_arm("batch off", dev, off, args, stream, &off_submissions, &off_batches,
                &off_batched);
    const std::vector<Seen> as_on =
        run_arm("batch on", dev, on, args, stream, &on_submissions, &on_batches, &on_batched);

    check_u64(as_off.size(), static_cast<uint64_t>(kChain), "batching off recorded every dispatch");
    check_u64(as_on.size(), static_cast<uint64_t>(kChain), "batching on recorded every dispatch");
    check(same(as_off, as_on), "the device was asked for the same work, dispatch for dispatch");

    // One submission per launch with batching off; one per batch with it on.
    check_u64(off_submissions, static_cast<uint64_t>(kChain), "batching off: one submission each");
    check_u64(off_batches, 0u, "batching off: no batch");
    const uint64_t expect_batches = (kChain + kBatchMax - 1) / kBatchMax;
    check_u64(on_submissions, expect_batches, "batching on: one submission per batch");
    check_u64(on_batches, expect_batches, "batching on: every submission carried a batch");
    check_u64(on_batched, static_cast<uint64_t>(kChain), "batching on: every dispatch rode in one");
    check(on_submissions * 4 < off_submissions, "batching on cut the submissions by more than 4x");

    // The fence values: one value per batch, shared by its dispatches, and never going back.
    {
        uint64_t distinct = 0;
        bool monotonic = true;
        for (size_t i = 0; i < as_on.size(); ++i) {
            if (i == 0 || as_on[i].value != as_on[i - 1].value) {
                distinct++;
            }
            if (i != 0 && as_on[i].value < as_on[i - 1].value) {
                monotonic = false;
            }
        }
        check_u64(distinct, expect_batches, "batching on: one fence value per batch");
        check(monotonic, "batching on: the fence values never go back");
    }

    // -------------------------------------------------------------------------------------
    std::printf("test_hip_batch: 2. a cap of one is one submission per launch\n");
    {
        bc250hsa_batch_policy one = on;
        one.max_dispatches = 1;
        uint64_t submissions = 0, batches = 0, batched = 0;
        const std::vector<Seen> as_one =
            run_arm("batch cap 1", dev, one, args, stream, &submissions, &batches, &batched);
        check_u64(as_one.size(), static_cast<uint64_t>(kChain), "every dispatch recorded");
        check_u64(submissions, static_cast<uint64_t>(kChain), "one submission per launch");
        check_u64(batches, 0u, "no submission carried more than one dispatch");
        check(same(as_off, as_one), "and the same work as batching off");
    }

    // -------------------------------------------------------------------------------------
    std::printf("test_hip_batch: 3. every flush point of section 8.1\n");
    {
        bc250hsa_batch_policy policy = on;
        check(bc250hsa_batch_policy_set(dev, &policy) == BC250HSA_OK, "batching on again");

        // (a) hipDeviceSynchronize, with fewer launches than the cap: nothing can retire
        //     unless the wait submitted the open buffer.
        bc250hsa_mock_reset();
        for (int i = 0; i < 5; ++i) {
            check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                                  stream) == hipSuccess,
                  "a launch below the cap");
        }
        check(hipDeviceSynchronize() == hipSuccess, "hipDeviceSynchronize submits the open batch");

        // (b) hipMemcpy. It waits for the device first, so it carries the same flush.
        bc250hsa_mock_reset();
        bc250hsa_counters before = read_counters();
        for (int i = 0; i < 5; ++i) {
            check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                                  stream) == hipSuccess,
                  "a launch before a copy");
        }
        float probe[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        check(hipMemcpy(probe, device_c, sizeof(probe), hipMemcpyDeviceToHost) == hipSuccess,
              "hipMemcpy after a launch");
        check(read_counters().submissions > before.submissions,
              "the copy submitted the work that was open");

        // (c) hipEventRecord. The event must carry the value of the work asked for by now, so
        //     the record submits the buffer and the wait for the event ends.
        bc250hsa_mock_reset();
        before = read_counters();
        hipEvent_t mark = nullptr;
        check(hipEventCreate(&mark) == hipSuccess, "hipEventCreate");
        for (int i = 0; i < 3; ++i) {
            check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                                  stream) == hipSuccess,
                  "a launch before an event record");
        }
        check(hipEventRecord(mark, stream) == hipSuccess, "hipEventRecord");
        check(read_counters().submissions == before.submissions + 1,
              "the record submitted exactly the one open buffer");
        check(hipEventSynchronize(mark) == hipSuccess, "the wait for the event ended");
        check(hipEventDestroy(mark) == hipSuccess, "hipEventDestroy");

        // (d) hipFree. The range may be named by a dispatch of the open buffer.
        bc250hsa_mock_reset();
        before = read_counters();
        float* scratch = nullptr;
        check(hipMalloc(reinterpret_cast<void**>(&scratch), bytes) == hipSuccess, "hipMalloc");
        for (int i = 0; i < 3; ++i) {
            check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                                  stream) == hipSuccess,
                  "a launch before a free");
        }
        check(hipFree(scratch) == hipSuccess, "hipFree after a launch");
        check(read_counters().submissions > before.submissions,
              "the free submitted the work that was open");

        // (e) the time cap. With a cap of one microsecond a launch is never held: the mock's
        //     clock has millisecond resolution, so this is the "submit at once" end of the
        //     policy and it must behave like batching off.
        policy.max_hold_us = 1;
        check(bc250hsa_batch_policy_set(dev, &policy) == BC250HSA_OK, "the time cap of 1 us");
        bc250hsa_mock_reset();
        before = read_counters();
        for (int i = 0; i < 4; ++i) {
            check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                                  stream) == hipSuccess,
                  "a launch under the time cap");
        }
        check_u64(read_counters().submissions - before.submissions, 4u,
                  "the time cap submitted every launch by itself");
        check(hipStreamSynchronize(stream) == hipSuccess, "and the stream is retired");
    }

    // -------------------------------------------------------------------------------------
    std::printf("test_hip_batch: 4. the policy is refused when it cannot be met\n");
    {
        // The policy in force before the refusals, so that the invariant below can be checked:
        // a refusal must leave it exactly as it was. A backend that applied half of a policy and
        // then returned the refusal would enable batching with a buffer it cannot fill, and
        // every later dispatch would fail while the caller believed its policy was rejected.
        bc250hsa_batch_policy in_force;
        std::memset(&in_force, 0, sizeof(in_force));
        in_force.struct_bytes = static_cast<uint32_t>(sizeof(in_force));
        check(bc250hsa_batch_policy_get(dev, &in_force) == BC250HSA_OK, "the policy in force");

        bc250hsa_batch_policy bad;
        std::memset(&bad, 0, sizeof(bad));
        bad.struct_bytes = static_cast<uint32_t>(sizeof(bad));
        bad.enabled = 1;
        bad.max_dispatches = BC250HSA_BATCH_DISPATCHES_MAX + 1u;
        check(bc250hsa_batch_policy_set(dev, &bad) == BC250HSA_EINVAL,
              "a cap above the maximum is refused");
        bad.max_dispatches = 0;
        bad.struct_bytes = 4u;
        check(bc250hsa_batch_policy_set(dev, &bad) == BC250HSA_EINVAL,
              "a structure size this build does not know is refused");
        check(bc250hsa_batch_policy_set(dev, nullptr) == BC250HSA_EINVAL, "no policy is refused");
        check(bc250hsa_flush(nullptr, nullptr) == BC250HSA_EINVAL, "a flush of no device");

        bc250hsa_batch_policy after_refusals;
        std::memset(&after_refusals, 0, sizeof(after_refusals));
        after_refusals.struct_bytes = static_cast<uint32_t>(sizeof(after_refusals));
        check(bc250hsa_batch_policy_get(dev, &after_refusals) == BC250HSA_OK,
              "the policy after the refusals");
        check(std::memcmp(&in_force, &after_refusals, sizeof(in_force)) == 0,
              "a refused policy changed nothing of the one in force");
        // And the device still batches as the policy in force says: a refusal that wedged it
        // would show up as a launch that fails.
        bc250hsa_mock_reset();
        const uint64_t before_submissions = read_counters().submissions;
        for (int i = 0; i < 4; ++i) {
            check(hipLaunchKernel(&g_stub_vadd, dim3(16u, 1u, 1u), dim3(256u, 1u, 1u), args, 0,
                                  stream) == hipSuccess,
                  "a launch after the refusals");
        }
        check(hipStreamSynchronize(stream) == hipSuccess, "and its synchronisation");
        check(read_counters().submissions > before_submissions,
              "the work after the refusals reached the device");
    }

    // Back to the default, so that the teardown below is the ordinary path.
    check(bc250hsa_batch_policy_set(dev, &off) == BC250HSA_OK, "batching off again");
    check(hipStreamDestroy(stream) == hipSuccess, "hipStreamDestroy");
    check(hipFree(device_c) == hipSuccess, "hipFree c");
    check(hipFree(device_b) == hipSuccess, "hipFree b");
    check(hipFree(device_a) == hipSuccess, "hipFree a");
    bc250hsa_close(dev);
    __hipUnregisterFatBinary(handle);
    std::free(fatbin);

    std::printf("test_hip_batch: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
