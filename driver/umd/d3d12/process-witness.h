// SPDX-License-Identifier: MIT
// What else is in the application's process (BD-332): one line per device that names the graphics modules this
// driver cares about, and a fault witness that writes the first few exceptions of the process into the same log.
//
// Why: a game that ends without a failing driver call and without a device removal (BD-332, The Witcher 3 on a
// tester PC) leaves the driver log with no name of the module that faulted. The log sink (stdio-log.h) already
// follows the control app's recording switch; this header writes through the same sink and is silent when the sink
// is silent.
//
// Life cycle (review findings F1, F6): arm() registers the handler at a device creation, when a log file exists,
// and keeps the handle. disarm() removes it and clears the latch. The shell calls disarm() when the last outer
// device is destroyed and from DLL_PROCESS_DETACH, so the handler never outlives the recording or the module.
// A later device arms again.
//
// Bounds and safety:
//   - the module list is a fixed watch list, asked with GetModuleHandleW and GetModuleFileNameW: no allocation,
//     no enumeration, at most one line per module that is present;
//   - the handler is armed only while the sink records, writes at most kFaultLines lines, skips a repeat of the
//     last fault, and writes nothing once the recording's time bound has passed (a clock read, no syscall);
//   - it sorts each first-chance exception by its code before any budget (BD-344): a code that is never a fault
//     gets no line, a fault-shaped code gets a fault line, and every other code gets at most kExceptionLines
//     "exception witness: ... continuing" lines with a budget and a repeat state of their own;
//   - it makes no allocation, calls no flush and starts no thread-local initialisation (review findings F3, F4);
//   - it writes the line and returns EXCEPTION_CONTINUE_SEARCH, so the process behaves exactly as it would without
//     it;
//   - it writes a module name, a base and an offset, and the exception code. No memory contents (BD-167).
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdint>
#include "stdio-log.h"

namespace amdgpu_wddm_witness {

// The modules whose presence and path answer the questions a game report leaves open: which upscaling file the
// application loaded and from where, and whether a game folder carries a proxy for a system DLL (a path outside
// System32 names one). The order is the order of the lines.
inline constexpr const char* kWatch[] = {
    "dxgi.dll",
    "d3d12.dll",
    "d3d12core.dll",
    "amd_fidelityfx_dx12.dll",
    "amd_fidelityfx_upscaler_dx12.dll",
    "amd_fidelityfx_loader_dx12.dll",
    "amd_fidelityfx_framegeneration_dx12.dll",
    "amdxc64.dll",
    "amdxcffx64.dll",
    "amdihk64.dll",
    "amd_ags_x64.dll",
    "winmm.dll",
    "version.dll",
    "winhttp.dll",
    "wininet.dll",
    "dbghelp.dll",
};
inline constexpr unsigned kWatchCount = static_cast<unsigned>(sizeof(kWatch) / sizeof(kWatch[0]));
inline constexpr unsigned kFaultLines = 8;
inline constexpr unsigned kExceptionLines = 2;
inline constexpr unsigned kPathChars = 512;

// BD-344: the handler is a first-chance handler, so it sees every exception that is raised, also the ones that the
// process handles itself and that are not errors. The Witcher 3 raises one DBG_PRINTEXCEPTION_C (40010006, the
// OutputDebugStringA signal) per process; before this filter it took a line of the fault budget with the "fault
// witness:" prefix and read as a crash. The class is decided by the code alone, before the budget and the repeat
// check. The values are those of ntstatus.h (Windows SDK 10.0.26100).

// Never a fault, no line at all. NTSTATUS severity success (0) or informational (1): these codes carry information,
// by their own severity they are not errors (DBG_PRINTEXCEPTION_C 40010006, DBG_PRINTEXCEPTION_WIDE_C 4001000a,
// DBG_CONTROL_C 40010005, DBG_CONTROL_BREAK 40010008, and 406d1388, the MSVC thread-name exception, which is not in
// ntstatus.h but has the informational severity bits). Also the four debugger traps of severity warning, as an
// explicit list (STATUS_GUARD_PAGE_VIOLATION 80000001, STATUS_DATATYPE_MISALIGNMENT 80000002, STATUS_BREAKPOINT
// 80000003, STATUS_SINGLE_STEP 80000004), so that a future warning code goes to the "other" class and is not dropped
// without a word.
inline bool never_a_fault(unsigned long code) noexcept {
    const unsigned long severity = (code >> 30) & 3ul;
    if (severity == 0ul || severity == 1ul) return true;
    switch (code) {
    case 0x80000001ul: case 0x80000002ul: case 0x80000003ul: case 0x80000004ul: return true;
    default: return false;
    }
}

// A fault-shaped first-chance event: the "fault witness:" line and the kFaultLines budget, as before BD-344. A
// handled access violation still gets its line: it is rare in a healthy process, and it is the signal BD-332 asks
// for. STATUS_ACCESS_VIOLATION c0000005, STATUS_ILLEGAL_INSTRUCTION c000001d, STATUS_INVALID_LOCK_SEQUENCE c000001e,
// STATUS_INTEGER_DIVIDE_BY_ZERO c0000094, STATUS_INTEGER_OVERFLOW c0000095, STATUS_PRIVILEGED_INSTRUCTION c0000096,
// STATUS_STACK_OVERFLOW c00000fd, STATUS_HEAP_CORRUPTION c0000374, STATUS_STACK_BUFFER_OVERRUN c0000409,
// STATUS_ASSERTION_FAILURE c0000420, STATUS_FAIL_FAST_EXCEPTION c0000602.
inline bool fault_shaped(unsigned long code) noexcept {
    switch (code) {
    case 0xC0000005ul: case 0xC000001Dul: case 0xC000001Eul: case 0xC0000094ul: case 0xC0000095ul:
    case 0xC0000096ul: case 0xC00000FDul: case 0xC0000374ul: case 0xC0000409ul: case 0xC0000420ul:
    case 0xC0000602ul: return true;
    default: return false;
    }
}
// Every other code (a C++ throw e06d7363, a .NET exception e0434352, a loader failure that the loader handles, such
// as c0000135) is "other": visible once per code and address, in a line that a reader cannot take for a crash. An
// exception that is really unhandled ends the process with its own exit code and WER record; that is the death
// signal, not this line.

// The last component of a path: the file name a report reads. Returns its length; an empty string for a null or
// empty path.
inline std::size_t file_name_of(const char* path, char* out, std::size_t size) noexcept {
    if (!out || !size) return 0;
    out[0] = '\0';
    if (!path || !*path) return 0;
    const char* name = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') name = p + 1;
    std::size_t n = 0;
    for (; name[n] && n + 1 < size; ++n) out[n] = name[n];
    out[n] = '\0';
    return n;
}

// One module line: "d3d12-module: <name> <path>" (the name as watched, the full path as the loader reports it).
inline std::size_t format_module_line(char* out, std::size_t size, const char* name, const char* path) noexcept {
    if (!out || !size) return 0;
    const int n = _snprintf_s(out, size, _TRUNCATE, "d3d12-module: %s %s\n", name ? name : "-", path ? path : "-");
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

// What the handler knows about one fault.
struct FaultRecord {
    unsigned long code = 0;
    const char* module = "-";           // the file name of the module that holds the address, or "-"
    unsigned long long module_base = 0; // 0: no module (the address is not in a loaded image)
    unsigned long long address = 0;
    unsigned long thread = 0;
    unsigned long parameter_count = 0;
    unsigned long long parameters[2] = {0, 0};
};

// One fault line: "fault witness: code <c>, module <name>, base <b>, rva <r>, address <a>, thread <t>, info <i>".
// rva is 0 when the address is in no module. info holds the two exception parameters when there are any (for an
// access violation the first is 0 read, 1 write or 8 execute).
inline std::size_t format_fault(char* out, std::size_t size, const FaultRecord& f) noexcept {
    if (!out || !size) return 0;
    const unsigned long long rva = f.module_base ? f.address - f.module_base : 0;
    int n;
    if (f.parameter_count >= 2)
        n = _snprintf_s(out, size, _TRUNCATE,
                        "fault witness: code %08lx, module %s, base %llx, rva %llx, address %llx, thread %lu, "
                        "info %llx %llx\n",
                        f.code, f.module ? f.module : "-", f.module_base, rva, f.address, f.thread, f.parameters[0],
                        f.parameters[1]);
    else if (f.parameter_count == 1)
        n = _snprintf_s(out, size, _TRUNCATE,
                        "fault witness: code %08lx, module %s, base %llx, rva %llx, address %llx, thread %lu, "
                        "info %llx\n",
                        f.code, f.module ? f.module : "-", f.module_base, rva, f.address, f.thread, f.parameters[0]);
    else
        n = _snprintf_s(out, size, _TRUNCATE,
                        "fault witness: code %08lx, module %s, base %llx, rva %llx, address %llx, thread %lu\n", f.code,
                        f.module ? f.module : "-", f.module_base, rva, f.address, f.thread);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

// One line for an exception of the "other" class: "exception witness: code <c> continuing, module <name>, base <b>,
// rva <r>, address <a>, thread <t>". The word "continuing" says that the handler let the search go on and that the
// process may handle it; no exception parameters (a C++ throw's parameters are pointers into the process).
inline std::size_t format_exception(char* out, std::size_t size, const FaultRecord& f) noexcept {
    if (!out || !size) return 0;
    const unsigned long long rva = f.module_base ? f.address - f.module_base : 0;
    const int n = _snprintf_s(out, size, _TRUNCATE,
                              "exception witness: code %08lx continuing, module %s, base %llx, rva %llx, address %llx, "
                              "thread %lu\n",
                              f.code, f.module ? f.module : "-", f.module_base, rva, f.address, f.thread);
    return n > 0 ? static_cast<std::size_t>(n) : 0;
}

// The name of a watched module as UTF-16, for GetModuleHandleW. Returns 0 when the name does not fit.
inline int wide_name(const char* name, wchar_t* out, std::size_t size) noexcept {
    if (!name || !out || !size) return 0;
    std::size_t n = 0;
    for (; name[n] && n + 1 < size; ++n) out[n] = static_cast<wchar_t>(static_cast<unsigned char>(name[n]));
    out[n] = L'\0';
    return name[n] ? 0 : 1;
}

// One line per watched module that is present in this process. Silent when the sink does not record. Safe to call
// at every device creation: a game opens several probe devices before its real one, and the list is short.
inline void log_modules() noexcept {
    if (!amdgpu_wddm_log::enabled()) return;
    amdgpu_wddm_log::print("d3d12-module-watch: %u names; a module that is absent has no line\n", kWatchCount);
    for (unsigned i = 0; i < kWatchCount; ++i) {
        wchar_t wide[96];
        if (!wide_name(kWatch[i], wide, sizeof(wide) / sizeof(wide[0]))) continue;
        const HMODULE module = GetModuleHandleW(wide);
        if (!module) continue;
        wchar_t path[kPathChars];
        const DWORD n = GetModuleFileNameW(module, path, kPathChars);
        if (!n || n >= kPathChars) continue;
        char narrow[kPathChars * 3];
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, path, static_cast<int>(n), narrow,
                                              static_cast<int>(sizeof(narrow) - 1), nullptr, nullptr);
        if (bytes <= 0) continue;
        narrow[bytes] = '\0';
        char line[2048];
        const std::size_t length = format_module_line(line, sizeof(line), kWatch[i], narrow);
        if (length) amdgpu_wddm_log::print("%s", line);
    }
    amdgpu_wddm_log::flush();
}

namespace detail {

// The handler is armed and its line budget is open. Set by arm(), cleared by disarm().
inline std::atomic<bool>& recording() noexcept {
    static std::atomic<bool> on{false};
    return on;
}

// One fault writer at a time. A fault that arrives while another line is written is dropped and its search
// continues. This is a plain atomic and not a thread_local: a thread_local's first use runs the CRT's thread-local
// initialisation, which can take a lock and allocate, and this code runs inside an exception handler.
inline std::atomic<bool>& busy() noexcept {
    static std::atomic<bool> busy{false};
    return busy;
}

// The handle that AddVectoredExceptionHandler returned, null when not armed.
inline std::atomic<PVOID>& handler_handle() noexcept {
    static std::atomic<PVOID> handle{nullptr};
    return handle;
}

// The instant at which the recording ends (FILETIME, 0 = no bound).
inline std::atomic<unsigned long long>& until() noexcept {
    static std::atomic<unsigned long long> bound{0};
    return bound;
}

inline std::atomic<unsigned>& fault_lines() noexcept {
    static std::atomic<unsigned> lines{0};
    return lines;
}

// The fault written last, so that a probe or a handled exception that repeats adds one line and not eight.
inline std::atomic<unsigned long>& last_code() noexcept {
    static std::atomic<unsigned long> code{0};
    return code;
}
inline std::atomic<unsigned long long>& last_address() noexcept {
    static std::atomic<unsigned long long> address{0};
    return address;
}

// The budget and the last written pair of the "other" class (BD-344). Separate from the fault state, so that a line
// of this class between two identical faults does not reset the suppression of the second fault.
inline std::atomic<unsigned>& exception_lines() noexcept {
    static std::atomic<unsigned> lines{0};
    return lines;
}
inline std::atomic<unsigned long>& last_exception_code() noexcept {
    static std::atomic<unsigned long> code{0};
    return code;
}
inline std::atomic<unsigned long long>& last_exception_address() noexcept {
    static std::atomic<unsigned long long> address{0};
    return address;
}

// 1 when the recording's time bound has passed. A zero bound never expires. Both values are FILETIME units.
inline bool expired(unsigned long long bound, unsigned long long now) noexcept { return bound != 0 && now >= bound; }

// True when code and address are the pair written last in this state; otherwise records them as the last pair.
inline bool repeats(std::atomic<unsigned long>& last_c, std::atomic<unsigned long long>& last_a, unsigned long code,
                    unsigned long long address) noexcept {
    if (last_c.load(std::memory_order_relaxed) == code && last_a.load(std::memory_order_relaxed) == address)
        return true;
    last_c.store(code, std::memory_order_relaxed);
    last_a.store(address, std::memory_order_relaxed);
    return false;
}

inline bool same_as_last(unsigned long code, unsigned long long address) noexcept {
    return repeats(last_code(), last_address(), code, address);
}

// Fills the module, base, thread and parameters of f from the record. module_name receives the file name and must
// stay alive until the line is formatted.
inline void describe(const EXCEPTION_RECORD* record, FaultRecord& f, char* module_name, std::size_t size) noexcept {
    f.code = record->ExceptionCode;
    f.address = reinterpret_cast<unsigned long long>(record->ExceptionAddress);
    f.thread = GetCurrentThreadId();
    f.parameter_count = record->NumberParameters > 2 ? 2 : record->NumberParameters;
    for (unsigned long i = 0; i < f.parameter_count; ++i) f.parameters[i] = record->ExceptionInformation[i];
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(record->ExceptionAddress), &module) &&
        module) {
        f.module_base = reinterpret_cast<unsigned long long>(module);
        char path[kPathChars];
        const DWORD n = GetModuleFileNameA(module, path, kPathChars);
        if (n && n < kPathChars && file_name_of(path, module_name, size)) f.module = module_name;
    }
}

// Writes one line of a class when its budget is open and the pair is not a repeat of the class's last line.
inline void witness(const EXCEPTION_RECORD* record, std::atomic<unsigned>& budget, unsigned limit,
                    std::atomic<unsigned long>& last_c, std::atomic<unsigned long long>& last_a, bool fault) noexcept {
    const unsigned long long address = reinterpret_cast<unsigned long long>(record->ExceptionAddress);
    const unsigned lines = budget.load(std::memory_order_relaxed);
    if (lines >= limit || repeats(last_c, last_a, record->ExceptionCode, address)) return;
    budget.store(lines + 1, std::memory_order_relaxed);
    FaultRecord f;
    char module_name[128];
    describe(record, f, module_name, sizeof(module_name));
    char line[2048];
    const std::size_t length = fault ? format_fault(line, sizeof(line), f) : format_exception(line, sizeof(line), f);
    // No flush: the sink opens its file unbuffered and writes with WriteFile, so the line is already with the
    // operating system, and fflush from inside a handler can wait for a stream lock that the faulting thread holds
    // (review finding F3).
    if (length) amdgpu_wddm_log::print("%s", line);
}

inline LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) noexcept {
    // Re-entrancy: one writer at a time. A nested or concurrent fault is dropped and its search continues.
    if (busy().exchange(true, std::memory_order_acquire)) return EXCEPTION_CONTINUE_SEARCH;
    if (recording().load(std::memory_order_relaxed) && info && info->ExceptionRecord &&
        !expired(until().load(std::memory_order_relaxed), amdgpu_wddm_log::detail::now_filetime())) {
        const EXCEPTION_RECORD* record = info->ExceptionRecord;
        // BD-344: the class first, before any budget or repeat state is touched.
        if (never_a_fault(record->ExceptionCode)) {
            // No line: an informational code or a debugger trap (the W3 40010006 case).
        } else if (fault_shaped(record->ExceptionCode)) {
            witness(record, fault_lines(), kFaultLines, last_code(), last_address(), true);
        } else {
            witness(record, exception_lines(), kExceptionLines, last_exception_code(), last_exception_address(),
                    false);
        }
    }
    busy().store(false, std::memory_order_release);
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace detail

// Arm the fault witness: register the handler once, when the process has a log file now, and open a fresh line
// budget. A process that asked for no recording gets no handler. A failed registration writes one line and leaves
// the latch clear, so the next device creation tries again (review finding F6).
inline void arm() noexcept {
    if (detail::recording().load(std::memory_order_relaxed)) return;
    const amdgpu_wddm_log::detail::Sink* const sink = amdgpu_wddm_log::detail::active();
    if (!sink || !sink->file) return;
    // One armer at a time; the loser does not wait and a later call tries again.
    static std::atomic<bool> arming{false};
    bool expected = false;
    if (!arming.compare_exchange_strong(expected, true, std::memory_order_acquire)) return;
    detail::until().store(sink->until, std::memory_order_relaxed);
    detail::fault_lines().store(0, std::memory_order_relaxed);
    detail::last_code().store(0, std::memory_order_relaxed);
    detail::last_address().store(0, std::memory_order_relaxed);
    detail::exception_lines().store(0, std::memory_order_relaxed);
    detail::last_exception_code().store(0, std::memory_order_relaxed);
    detail::last_exception_address().store(0, std::memory_order_relaxed);
    PVOID const handle = AddVectoredExceptionHandler(0, &detail::fault_handler);
    if (!handle) {
        amdgpu_wddm_log::print("fault witness: the handler could not be added (%lu)\n", GetLastError());
        detail::until().store(0, std::memory_order_relaxed);
    } else {
        detail::handler_handle().store(handle, std::memory_order_release);
        detail::recording().store(true, std::memory_order_release);
    }
    arming.store(false, std::memory_order_release);
}

// Remove the handler and close the line budget. Safe to call when not armed, and safe at DLL_PROCESS_DETACH:
// RemoveVectoredExceptionHandler does not block. A later arm() registers again.
inline void disarm() noexcept {
    if (!detail::recording().exchange(false, std::memory_order_acq_rel)) return;
    PVOID const handle = detail::handler_handle().exchange(nullptr, std::memory_order_acq_rel);
    detail::until().store(0, std::memory_order_relaxed);
    if (handle) RemoveVectoredExceptionHandler(handle);
}

} // namespace amdgpu_wddm_witness
