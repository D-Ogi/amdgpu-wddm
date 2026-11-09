// SPDX-License-Identifier: MIT
// Host test of process-witness.h: the text of the module line and the fault line, and the handler's contract
// (BD-332, review findings F1-F6). The lines must be exact: a report is read by a person and by a grep, and neither
// may mistake a module name or an offset. The handler is called with a synthetic EXCEPTION_POINTERS, so the return
// value, the re-entrancy guard, the line bound, the repeat suppression and arming/removal are checked without
// raising anything.
//
// The test is built with AMDGPU_WDDM_LAB_FLAVOUR (tools/build/build-umd-d3d12.ps1), because only that flavour of
// the sink reads AMDGPU_WDDM_LOG directly; the test uses it to open a real log file in the current directory.
#include "process-witness.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s (line %d)\n", #c, __LINE__); ++failures; } } while (0)

using amdgpu_wddm_witness::FaultRecord;
namespace detail = amdgpu_wddm_witness::detail;

__declspec(noinline) void faulting_point() {
    // Only an address of this image is needed; the function is never entered.
    volatile unsigned sink = 0;
    (void)sink;
}

// The log file of this run, in the current directory.
char g_log_name[64];
char g_log_variable[96];

void prepare_log() {
    _snprintf_s(g_log_name, sizeof(g_log_name), _TRUNCATE, "process-witness-%lu.log", GetCurrentProcessId());
    DeleteFileA(g_log_name);
    _snprintf_s(g_log_variable, sizeof(g_log_variable), _TRUNCATE, "file:%s", g_log_name);
    SetEnvironmentVariableA("AMDGPU_WDDM_LOG", g_log_variable);
}

// The log's text. A Win32 reader, not the CRT: the sink holds the file with FILE_APPEND_DATA, and the CRT's
// fopen refuses that share mode on the same file in the same process (measured 2026-11-09, EACCES).
std::size_t read_log(char* out, std::size_t size) {
    if (!size) return 0;
    out[0] = '\0';
    HANDLE h = CreateFileA(g_log_name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD n = 0;
    ReadFile(h, out, static_cast<DWORD>(size - 1), &n, nullptr);
    CloseHandle(h);
    out[n] = '\0';
    return n;
}

int count_fault_lines() {
    char text[8192];
    read_log(text, sizeof(text));
    int count = 0;
    for (const char* p = text; (p = std::strstr(p, "fault witness: code")) != nullptr; ++p) ++count;
    return count;
}

bool log_contains(const char* needle) {
    char text[8192];
    read_log(text, sizeof(text));
    return std::strstr(text, needle) != nullptr;
}

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

void test_watch_list() {
    unsigned watch = amdgpu_wddm_witness::kWatchCount;
    unsigned lines = amdgpu_wddm_witness::kFaultLines;
    CHECK(watch == 16);
    CHECK(lines == 8);
    bool fsr4 = false, proxy = false, amdxc = false;
    for (unsigned i = 0; i < amdgpu_wddm_witness::kWatchCount; ++i) {
        if (!std::strcmp(amdgpu_wddm_witness::kWatch[i], "amd_fidelityfx_upscaler_dx12.dll")) fsr4 = true;
        if (!std::strcmp(amdgpu_wddm_witness::kWatch[i], "dxgi.dll")) proxy = true;
        if (!std::strcmp(amdgpu_wddm_witness::kWatch[i], "amdxc64.dll")) amdxc = true;
    }
    CHECK(fsr4 && proxy && amdxc);
}

void test_expiry() {
    CHECK(detail::expired(100, 100));
    CHECK(detail::expired(100, 101));
    CHECK(!detail::expired(100, 99));
    CHECK(!detail::expired(0, 999));   // no bound never expires
}

// The handler's contract, with a synthetic record whose address lies in this image.
void test_handler() {
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0xC0000005ul;
    record.ExceptionAddress = reinterpret_cast<PVOID>(&faulting_point);
    record.NumberParameters = 2;
    record.ExceptionInformation[0] = 1;   // write
    record.ExceptionInformation[1] = 0;
    EXCEPTION_POINTERS info{&record, nullptr};

    // Not armed: nothing happens, and the search continues.
    CHECK(!detail::recording().load());
    CHECK(detail::fault_handler(&info) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(detail::fault_handler(nullptr) == EXCEPTION_CONTINUE_SEARCH);

    // Armed with a real sink: the first fault writes one line with this module.
    amdgpu_wddm_witness::arm();
    CHECK(detail::recording().load());
    CHECK(detail::handler_handle().load() != nullptr);
    CHECK(detail::fault_handler(&info) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == 1);
    CHECK(log_contains("fault witness: code c0000005, module process-witness-test.exe"));
    CHECK(log_contains("info 1 0"));

    // A repeat of the last fault adds no line.
    CHECK(detail::fault_handler(&info) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == 1);

    // Distinct faults: eight lines in all, and not one more.
    for (unsigned i = 1; i < 12; ++i) {
        record.ExceptionAddress = reinterpret_cast<PVOID>(reinterpret_cast<char*>(&faulting_point) + i);
        CHECK(detail::fault_handler(&info) == EXCEPTION_CONTINUE_SEARCH);
    }
    CHECK(count_fault_lines() == 8);

    // A fault while another line is written is dropped and its search continues.
    detail::busy().store(true);
    record.ExceptionAddress = reinterpret_cast<PVOID>(reinterpret_cast<char*>(&faulting_point) + 64);
    CHECK(detail::fault_handler(&info) == EXCEPTION_CONTINUE_SEARCH);
    detail::busy().store(false);
    CHECK(count_fault_lines() == 8);

    // A line whose address is in no module still names the fault.
    amdgpu_wddm_witness::disarm();
    CHECK(!detail::recording().load());
    CHECK(detail::handler_handle().load() == nullptr);
    amdgpu_wddm_witness::arm();
    CHECK(detail::recording().load());
    record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(0x1234));
    CHECK(detail::fault_handler(&info) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == 9);   // a fresh budget after arm
    CHECK(log_contains("module -, base 0, rva 0"));

    amdgpu_wddm_witness::disarm();
    CHECK(!detail::recording().load());
    // Disarmed: a fault writes nothing (the handler is also unregistered).
    CHECK(detail::fault_handler(&info) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == 9);
    // A second disarm is a no-op: the last device destroy and a later DllMain may both reach it.
    amdgpu_wddm_witness::disarm();
    CHECK(!detail::recording().load());
    CHECK(detail::handler_handle().load() == nullptr);
}

} // namespace

int main() {
    prepare_log();
    test_file_name();
    test_module_line();
    test_fault_line();
    test_wide_name();
    test_watch_list();
    test_expiry();
    test_handler();
    std::printf("process-witness: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
