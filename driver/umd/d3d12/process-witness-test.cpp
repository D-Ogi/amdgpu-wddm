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

int count_lines_with(const char* needle) {
    char text[16384];
    read_log(text, sizeof(text));
    int count = 0;
    for (const char* p = text; (p = std::strstr(p, needle)) != nullptr; ++p) ++count;
    return count;
}

// "fault witness: code" is not a substring of "exception witness: code", so the two counts never mix.
int count_fault_lines() { return count_lines_with("fault witness: code"); }
int count_exception_lines() { return count_lines_with("exception witness: code"); }

bool log_contains(const char* needle) {
    char text[16384];
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

// BD-344: the class of a code decides whether it gets a line, before any budget.
void test_never_a_fault() {
    using amdgpu_wddm_witness::never_a_fault;
    const unsigned long never[] = {0x40010006ul, 0x4001000Aul, 0x40010005ul, 0x40010008ul, 0x406D1388ul,
                                   0x80000001ul, 0x80000002ul, 0x80000003ul, 0x80000004ul};
    for (unsigned long code : never) CHECK(never_a_fault(code));
    const unsigned long counted[] = {0xC0000005ul, 0xC0000409ul, 0xE06D7363ul, 0xE0434352ul};
    for (unsigned long code : counted) CHECK(!never_a_fault(code));
    // A warning code outside the explicit trap list is not dropped.
    CHECK(!never_a_fault(0x80000005ul));
}

void test_fault_shaped() {
    using amdgpu_wddm_witness::fault_shaped;
    const unsigned long faults[] = {0xC0000005ul, 0xC000001Dul, 0xC000001Eul, 0xC0000094ul, 0xC0000095ul, 0xC0000096ul,
                                    0xC00000FDul, 0xC0000374ul, 0xC0000409ul, 0xC0000420ul, 0xC0000602ul};
    for (unsigned long code : faults) CHECK(fault_shaped(code));
    CHECK(!fault_shaped(0xE06D7363ul));
    CHECK(!fault_shaped(0x40010006ul));
    CHECK(!fault_shaped(0xC0000135ul));
}

PVOID point(unsigned offset) {
    return reinterpret_cast<PVOID>(reinterpret_cast<char*>(&faulting_point) + offset);
}

LONG raise_at(unsigned long code, unsigned offset) {
    EXCEPTION_RECORD record{};
    record.ExceptionCode = code;
    record.ExceptionAddress = point(offset);
    EXCEPTION_POINTERS info{&record, nullptr};
    return detail::fault_handler(&info);
}

// The W3 case: DBG_PRINTEXCEPTION_C at ten addresses writes nothing at all.
void test_handler_noise() {
    amdgpu_wddm_witness::arm();
    CHECK(detail::recording().load());
    const int faults = count_fault_lines(), others = count_exception_lines();
    for (unsigned i = 0; i < 10; ++i) CHECK(raise_at(0x40010006ul, 100 + i) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == faults);
    CHECK(count_exception_lines() == others);
    CHECK(!log_contains("code 40010006"));
    amdgpu_wddm_witness::disarm();
}

// A C++ throw is an "other" exception: its own line and budget, never the fault budget.
void test_handler_exception_class() {
    amdgpu_wddm_witness::arm();
    const int faults = count_fault_lines(), others = count_exception_lines();
    CHECK(raise_at(0xE06D7363ul, 200) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == faults);
    CHECK(count_exception_lines() == others + 1);
    CHECK(log_contains("exception witness: code e06d7363 continuing"));
    CHECK(raise_at(0xE06D7363ul, 200) == EXCEPTION_CONTINUE_SEARCH);   // the same address: a repeat
    CHECK(count_exception_lines() == others + 1);
    CHECK(raise_at(0xE06D7363ul, 201) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_exception_lines() == others + 2);
    CHECK(raise_at(0xE06D7363ul, 202) == EXCEPTION_CONTINUE_SEARCH);   // the budget of two is used
    CHECK(count_exception_lines() == others + 2);
    // The fault budget is independent: eight distinct access violations still get eight lines.
    for (unsigned i = 0; i < 8; ++i) CHECK(raise_at(0xC0000005ul, 300 + i) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == faults + 8);
    CHECK(count_exception_lines() == others + 2);
    amdgpu_wddm_witness::disarm();
}

// A code without a line between two identical faults does not move the fault repeat state.
void test_handler_dedup_independence() {
    amdgpu_wddm_witness::arm();
    const int faults = count_fault_lines(), others = count_exception_lines();
    CHECK(raise_at(0xC0000005ul, 400) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(raise_at(0x40010006ul, 401) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(raise_at(0xC0000005ul, 400) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == faults + 1);
    // An "other" line between them does not move it either: the two classes keep separate repeat states.
    CHECK(raise_at(0xE0434352ul, 402) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(raise_at(0xC0000005ul, 400) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == faults + 1);
    CHECK(count_exception_lines() == others + 1);
    amdgpu_wddm_witness::disarm();
}

void test_exception_line() {
    char line[512];
    FaultRecord f;
    f.code = 0xE06D7363ul;
    f.module = "witcher3.exe";
    f.module_base = 0x7ff600000000ull;
    f.address = 0x7ff600012345ull;
    f.thread = 1234;
    f.parameter_count = 2;   // parameters are never part of this line
    f.parameters[0] = 0x19930520ull;
    f.parameters[1] = 0x1000ull;
    const std::size_t n = amdgpu_wddm_witness::format_exception(line, sizeof(line), f);
    const char* const want =
        "exception witness: code e06d7363 continuing, module witcher3.exe, base 7ff600000000, rva 12345, "
        "address 7ff600012345, thread 1234\n";
    CHECK(n == std::strlen(want));
    CHECK(std::strcmp(line, want) == 0);
    FaultRecord none;
    none.code = 0xC0000135ul;
    none.address = 0x1234;
    none.thread = 77;
    CHECK(amdgpu_wddm_witness::format_exception(line, sizeof(line), none) > 0);
    CHECK(std::strcmp(line,
                      "exception witness: code c0000135 continuing, module -, base 0, rva 0, address 1234, "
                      "thread 77\n") == 0);
    CHECK(amdgpu_wddm_witness::format_exception(line, 8, f) == 0);
    CHECK(amdgpu_wddm_witness::format_exception(nullptr, 0, f) == 0);
}

// Loader failures that the loader handles: two lines at most, and the fault budget stays whole.
void test_exception_budget() {
    amdgpu_wddm_witness::arm();
    const int faults = count_fault_lines(), others = count_exception_lines();
    CHECK(raise_at(0xC0000135ul, 500) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(raise_at(0xC0000142ul, 501) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_exception_lines() == others + 2);
    CHECK(raise_at(0xC000007Bul, 502) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_exception_lines() == others + 2);
    for (unsigned i = 0; i < 10; ++i) CHECK(raise_at(0xC0000409ul, 600 + i) == EXCEPTION_CONTINUE_SEARCH);
    CHECK(count_fault_lines() == faults + 8);
    amdgpu_wddm_witness::disarm();
}

// Negative control of the class rule: a handler without the filter writes the W3 line. Shown with the formatter of
// the fault class, which is what the handler used for every code before BD-344.
void test_unfiltered_control() {
    char line[512];
    FaultRecord f;
    f.code = 0x40010006ul;
    f.address = 0x1234;
    CHECK(amdgpu_wddm_witness::format_fault(line, sizeof(line), f) > 0);
    CHECK(std::strstr(line, "fault witness: code 40010006") != nullptr);
    CHECK(amdgpu_wddm_witness::never_a_fault(f.code));   // and the filter is what keeps it out
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
    test_never_a_fault();
    test_fault_shaped();
    test_exception_line();
    test_unfiltered_control();
    test_handler_noise();
    test_handler_exception_class();
    test_handler_dedup_independence();
    test_exception_budget();
    std::printf("process-witness: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
