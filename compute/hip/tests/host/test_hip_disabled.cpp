// Actual runtime over the host mock. No KMT or GPU calls.
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "hip/hip_runtime.h"
#include "hipmock_backend.h"
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL CHECK %s\n", #x); } } while (0)
int main(int argc, char** argv) {
    const bool disabled = argc == 2 && std::strcmp(argv[1], "disabled") == 0;
    SetEnvironmentVariableA("BC250_HIP_GPU_DISABLED", "0");
    SetEnvironmentVariableA("BC250_HIP_BATCH", "1");
    const hipError_t wanted = disabled ? hipErrorNotSupported : hipSuccess;
    CHECK(bc250hsa_mock_open_calls() == 0);
    CHECK(hipInit(0) == wanted);
    int count = -1;
    CHECK(hipGetDeviceCount(&count) == wanted);
    CHECK(count == (disabled ? 0 : 1));
    void* pointer = nullptr;
    CHECK(hipMalloc(&pointer, 4096) == wanted);
    if (!disabled) CHECK(hipFree(pointer) == hipSuccess);
    static const char unregisteredStub = 0;
    CHECK(hipLaunchKernel(&unregisteredStub, dim3(1), dim3(1), nullptr, 0, nullptr) ==
          (disabled ? hipErrorNotSupported : hipErrorInvalidDeviceFunction));
    CHECK(hipDeviceSynchronize() == wanted);
    CHECK(hipInit(0) == wanted);
    CHECK(bc250hsa_mock_open_calls() == (disabled ? 0u : 1u));
    if (disabled) CHECK(bc250hsa_mock_record_count() == 0);
    std::printf("containment %s: %u checks, %u failures\n", disabled ? "disabled" : "enabled", checks, failures);
    return failures ? 1 : 0;
}
