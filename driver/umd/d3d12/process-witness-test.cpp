// SPDX-License-Identifier: MIT
// Host test of process-witness.h: the text of the module line and the fault line (BD-332). The lines must be exact:
// a report is read by a person and by a grep, and neither may mistake a module name or an offset. The handler itself
// is exercised only for safety (arming twice, and a call without a recording does nothing).
#include "process-witness.h"
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s (line %d)\n", #c, __LINE__); ++failures; } } while (0)

using amdgpu_wddm_witness::FaultRecord;

void test_file_name() {
    char out[64];
    CHECK(amdgpu_wddm_witness::file_name_of("C:\\Games\\W3\\bin\\x64_dx12\\dxgi.dll", out, sizeof(out)) == 8);
    CHECK(std::strcmp(out, "dxgi.dll") == 0);
    CHECK(amdgpu_wddm_witness::file_name_of("dxgi.dll", out, sizeof(out)) == 8 && std::strcmp(out, "dxgi.dll") == 0);
    CHECK(amdgpu_wddm_witness::file_name_of("C:/Games/amd_fidelityfx_upscaler_dx12.dll", out, sizeof(out)) == 32);
    CHECK(std::strcmp(out, "amd_fidelityfx_upscaler_dx12.dll") == 0);
    CHECK(amdgpu_wddm_witness::file_name_of(nullptr, out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(amdgpu_wddm_witness::file_name_of("", out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(amdgpu_wddm_witness::file_name_of("C:\\Games\\", out, sizeof(out)) == 0 && out[0] == '\0');
    // A buffer that is too small holds what fits, terminated. ("small" is a macro of rpcndr.h.)
    char tiny[4];
    CHECK(amdgpu_wddm_witness::file_name_of("dxgi.dll", tiny, sizeof(tiny)) == 3 && std::strcmp(tiny, "dxg") == 0);
}

void test_module_line() {
    char line[512];
    const char* const path = "C:\\Games\\W3\\bin\\x64_dx12\\dxgi.dll";
    const std::size_t n = amdgpu_wddm_witness::format_module_line(line, sizeof(line), "dxgi.dll", path);
    const char* const want = "d3d12-module: dxgi.dll C:\\Games\\W3\\bin\\x64_dx12\\dxgi.dll\n";
    CHECK(n == std::strlen(want));
    CHECK(std::strcmp(line, want) == 0);
    CHECK(amdgpu_wddm_witness::format_module_line(line, sizeof(line), "dxgi.dll", nullptr) > 0);
    CHECK(std::strcmp(line, "d3d12-module: dxgi.dll -\n") == 0);
    // A line that does not fit is refused, so the caller writes nothing.
    CHECK(amdgpu_wddm_witness::format_module_line(line, 8, "dxgi.dll", path) == 0);
    CHECK(amdgpu_wddm_witness::format_module_line(nullptr, 0, "dxgi.dll", path) == 0);
}

void test_fault_line() {
    char line[512];
    FaultRecord f;
    f.code = 0xC0000005ul;
    f.module = "witcher3.exe";
    f.module_base = 0x7ff600000000ull;
    f.address = 0x7ff600012345ull;
    f.thread = 1234;
    f.parameter_count = 2;
    f.parameters[0] = 1;    // write
    f.parameters[1] = 0;
    const std::size_t n = amdgpu_wddm_witness::format_fault(line, sizeof(line), f);
    const char* const want =
        "fault witness: code c0000005, module witcher3.exe, base 7ff600000000, rva 12345, address 7ff600012345, "
        "thread 1234, info 1 0\n";
    CHECK(n == std::strlen(want));
    CHECK(std::strcmp(line, want) == 0);
    // One parameter, no module: the base and the rva are 0 and the line still names the address and the thread.
    FaultRecord one;
    one.code = 0xE06D7363ul;
    one.address = 0x1234;
    one.thread = 77;
    one.parameter_count = 1;
    one.parameters[0] = 0x19930520ull;
    CHECK(amdgpu_wddm_witness::format_fault(line, sizeof(line), one) > 0);
    CHECK(std::strcmp(line,
                      "fault witness: code e06d7363, module -, base 0, rva 0, address 1234, thread 77, "
                      "info 19930520\n") == 0);
    // No parameter at all.
    FaultRecord none;
    none.code = 0x80000003ul;
    none.address = 0x7ff600001000ull;
    none.module_base = 0x7ff600000000ull;
    none.module = "amd_fidelityfx_upscaler_dx12.dll";
    none.thread = 5;
    CHECK(amdgpu_wddm_witness::format_fault(line, sizeof(line), none) > 0);
    CHECK(std::strcmp(line,
                      "fault witness: code 80000003, module amd_fidelityfx_upscaler_dx12.dll, base 7ff600000000, "
                      "rva 1000, address 7ff600001000, thread 5\n") == 0);
    // A line that does not fit is refused.
    CHECK(amdgpu_wddm_witness::format_fault(line, 8, f) == 0);
    CHECK(amdgpu_wddm_witness::format_fault(nullptr, 0, f) == 0);
}

void test_wide_name() {
    wchar_t wide[32];
    CHECK(amdgpu_wddm_witness::wide_name("dxgi.dll", wide, 32) == 1);
    CHECK(std::wcscmp(wide, L"dxgi.dll") == 0);
    CHECK(amdgpu_wddm_witness::wide_name("dxgi.dll", wide, 4) == 0);   // needs 9 characters
    CHECK(amdgpu_wddm_witness::wide_name(nullptr, wide, 32) == 0);
}

// The counts are passed as values, so that the checks are not constant conditions (C4127 with /W4 /WX).
void check_counts(unsigned watch, unsigned lines) {
    CHECK(watch == 16);
    CHECK(lines == 8);
}

void test_watch_list() {
    check_counts(amdgpu_wddm_witness::kWatchCount, amdgpu_wddm_witness::kFaultLines);
    bool fsr4 = false, proxy = false, amdxc = false;
    for (unsigned i = 0; i < amdgpu_wddm_witness::kWatchCount; ++i) {
        if (!std::strcmp(amdgpu_wddm_witness::kWatch[i], "amd_fidelityfx_upscaler_dx12.dll")) fsr4 = true;
        if (!std::strcmp(amdgpu_wddm_witness::kWatch[i], "dxgi.dll")) proxy = true;
        if (!std::strcmp(amdgpu_wddm_witness::kWatch[i], "amdxc64.dll")) amdxc = true;
    }
    CHECK(fsr4 && proxy && amdxc);
}

void test_arming_is_safe() {
    // Without a recording the sink is silent and arm() registers nothing; either way both calls are safe and a
    // second arm does nothing. This is the state of every process the control app did not arm.
    amdgpu_wddm_witness::arm();
    amdgpu_wddm_witness::arm();
    amdgpu_wddm_witness::log_modules();
}

} // namespace

int main() {
    test_file_name();
    test_module_line();
    test_fault_line();
    test_wide_name();
    test_watch_list();
    test_arming_is_safe();
    std::printf("process-witness: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
