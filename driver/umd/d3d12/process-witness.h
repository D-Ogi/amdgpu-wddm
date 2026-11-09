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
inline constexpr unsigned kPathChars = 512;

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

// 1 when the recording's time bound has passed. A zero bound never expires. Both values are FILETIME units.
inline bool expired(unsigned long long bound, unsigned long long now) noexcept { return bound != 0 && now >= bound; }

inline bool same_as_last(unsigned long code, unsigned long long address) noexcept {
    if (last_code().load(std::memory_order_relaxed) == code &&
        last_address().load(std::memory_order_relaxed) == address)
        return true;
    last_code().store(code, std::memory_order_relaxed);
    last_address().store(address, std::memory_order_relaxed);
    return false;
}

inline LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) noexcept {
    // Re-entrancy: one writer at a time. A nested or concurrent fault is dropped and its search continues.
    if (busy().exchange(true, std::memory_order_acquire)) return EXCEPTION_CONTINUE_SEARCH;
    if (recording().load(std::memory_order_relaxed) && info && info->ExceptionRecord &&
        !expired(until().load(std::memory_order_relaxed), amdgpu_wddm_log::detail::now_filetime())) {
        EXCEPTION_RECORD* record = info->ExceptionRecord;
        const unsigned long long address = reinterpret_cast<unsigned long long>(record->ExceptionAddress);
        const unsigned lines = fault_lines().load(std::memory_order_relaxed);
        if (lines < kFaultLines && !same_as_last(record->ExceptionCode, address)) {
            fault_lines().store(lines + 1, std::memory_order_relaxed);
            FaultRecord f;
            f.code = record->ExceptionCode;
            f.address = address;
            f.thread = GetCurrentThreadId();
            f.parameter_count = record->NumberParameters > 2 ? 2 : record->NumberParameters;
            for (unsigned long i = 0; i < f.parameter_count; ++i) f.parameters[i] = record->ExceptionInformation[i];
            // The file name lives in this frame until format_fault has copied it into the line.
            char module_name[128];
            HMODULE module = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(record->ExceptionAddress), &module) &&
                module) {
                f.module_base = reinterpret_cast<unsigned long long>(module);
                char path[kPathChars];
                const DWORD n = GetModuleFileNameA(module, path, kPathChars);
                if (n && n < kPathChars && file_name_of(path, module_name, sizeof(module_name)))
                    f.module = module_name;
            }
            char line[2048];
            const std::size_t length = format_fault(line, sizeof(line), f);
            // No flush: the sink opens its file unbuffered and writes with WriteFile, so the line is already with
            // the operating system, and fflush from inside a handler can wait for a stream lock that the faulting
            // thread holds (review finding F3).
            if (length) amdgpu_wddm_log::print("%s", line);
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
