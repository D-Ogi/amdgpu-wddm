// hip_log.cpp - the diagnostic log of amdhip64.dll, and the one place a refusal is named.
//
// Why this file exists. On 2026-10-09 every llama.cpp program of the ggml-hip backend stopped at
// its first kernel launch with hipErrorNotSupported, and the lab could not say which call refused
// what: layer 1 writes its reason through bc250hsa_log, this runtime installed no sink, and the
// refusal counters of layer 1 are not among the two counter exports. The session had to end with
// "the next step is a build whose refusals say which call and which kernel they refuse" (defect
// BD-110). This is that build.
//
// bc250hsa.h rule 6 keeps every environment variable out of layer 1, so the switch lives here:
//
//   BC250_HIP_LOG=0 or absent   off, and this runtime writes nothing of its own
//   BC250_HIP_LOG=1             the standard error stream
//   BC250_HIP_LOG=stderr        the same
//   BC250_HIP_LOG=<path>        append to that file, one line per message
//   BC250_HIP_LOG_LEVEL=0..3    error, warning, information, trace. The default is 2
//
// Every line carries the process and thread identifier, so two programs that append to one file
// stay apart, and the level as one word. The log holds call names, kernel names and status
// names, and no application data: no buffer contents, no device pointer of the program's own
// allocations and no environment of the machine.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "runtime_internal.h"

namespace bc250hip {

namespace {

std::mutex g_log_lock;
std::FILE* g_stream = nullptr;     // nullptr with g_on set means stderr
bool       g_on = false;
bool       g_own_stream = false;
uint32_t   g_level = BC250HSA_LOG_INFO;

// The operating system's environment and not getenv, for the reason hip_device.cpp states: a
// program with the static C runtime and this DLL with the dynamic one keep two copies.
bool env_text(const char* name, char* text, size_t bytes) {
#if defined(_WIN32)
    const DWORD got = GetEnvironmentVariableA(name, text, static_cast<DWORD>(bytes));
    return got != 0 && got < bytes;
#else
    const char* found = std::getenv(name);
    if (found == nullptr || std::strlen(found) + 1 > bytes) {
        return false;
    }
    std::strncpy(text, found, bytes - 1);
    text[bytes - 1] = '\0';
    return true;
#endif
}

const char* level_name(uint32_t level) {
    switch (level) {
        case BC250HSA_LOG_ERROR: return "error";
        case BC250HSA_LOG_WARN:  return "warn";
        case BC250HSA_LOG_INFO:  return "info";
        default:                 return "trace";
    }
}

unsigned long this_process() {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return 0ul;
#endif
}

unsigned long this_thread() {
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentThreadId());
#else
    return 0ul;
#endif
}

// The sink layer 1 writes through. It takes the same lock as our own lines, so a message of the
// library and a message of this runtime never interleave inside one line.
void sink(void* ctx, uint32_t level, const char* message) {
    (void)ctx;
    std::lock_guard<std::mutex> held(g_log_lock);
    if (!g_on || level > g_level) {
        return;
    }
    std::FILE* out = g_stream != nullptr ? g_stream : stderr;
    std::fprintf(out, "amdhip64 [%lu:%lu] %s bc250hsa: %s\n", this_process(), this_thread(),
                 level_name(level), message);
    std::fflush(out);
}

// Reads the switch once and installs the sink. A local static makes the first call the only one
// that runs the body, whichever thread gets there first, and registration happens before main().
void start() {
    char text[512];
    if (!env_text("BC250_HIP_LOG", text, sizeof(text)) || std::strcmp(text, "0") == 0 ||
        text[0] == '\0') {
        return;
    }
    char level_text[16];
    if (env_text("BC250_HIP_LOG_LEVEL", level_text, sizeof(level_text))) {
        const unsigned long value = std::strtoul(level_text, nullptr, 10);
        g_level = value > BC250HSA_LOG_TRACE ? BC250HSA_LOG_TRACE : static_cast<uint32_t>(value);
    }
    if (std::strcmp(text, "1") != 0 && std::strcmp(text, "stderr") != 0) {
        // A path. Append, so that several runs of one trial keep their order, and fall back to
        // the error stream with one line when the file cannot be opened: a diagnostic switch
        // that silently does nothing is worse than no switch.
        std::FILE* file = std::fopen(text, "a");
        if (file == nullptr) {
            std::fprintf(stderr, "amdhip64: BC250_HIP_LOG names '%s', which cannot be opened"
                                 " for appending; the log goes to the error stream\n", text);
        } else {
            g_stream = file;
            g_own_stream = true;
        }
    }
    g_on = true;
    bc250hsa_set_log(&sink, nullptr, g_level);
    std::FILE* out = g_stream != nullptr ? g_stream : stderr;
    std::fprintf(out, "amdhip64 [%lu:%lu] info log on, level %u (%s), interface %u.%u\n",
                 this_process(), this_thread(), g_level, level_name(g_level),
                 bc250hsa_abi_version_major(), bc250hsa_abi_version_minor());
    std::fflush(out);
}

}  // namespace

void log_start() {
    static const bool once = []() {
        start();
        return true;
    }();
    (void)once;
}

bool log_on() {
    log_start();
    std::lock_guard<std::mutex> held(g_log_lock);
    return g_on;
}

void log_line(uint32_t level, const char* format, ...) {
    log_start();
    {
        std::lock_guard<std::mutex> held(g_log_lock);
        if (!g_on || level > g_level) {
            return;
        }
    }
    char line[1024];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    std::lock_guard<std::mutex> held(g_log_lock);
    std::FILE* out = g_stream != nullptr ? g_stream : stderr;
    std::fprintf(out, "amdhip64 [%lu:%lu] %s %s\n", this_process(), this_thread(),
                 level_name(level), line);
    std::fflush(out);
}

hipError_t refuse(const char* call, const char* kernel, const char* why, hipError_t err) {
    log_line(BC250HSA_LOG_ERROR, "%s refuses%s%s: %s (%s)", call != nullptr ? call : "?",
             kernel != nullptr ? " kernel " : "", kernel != nullptr ? kernel : "",
             why != nullptr ? why : "no reason given", hipGetErrorName(err));
    return fail(err);
}

hipError_t refuse_status(const char* call, const char* kernel, bc250hsa_status status) {
    const hipError_t err = translate(status);
    log_line(BC250HSA_LOG_ERROR, "%s refuses%s%s: %s (%s)", call != nullptr ? call : "?",
             kernel != nullptr ? " kernel " : "", kernel != nullptr ? kernel : "",
             bc250hsa_status_string(status), hipGetErrorName(err));
    return fail(err);
}

}  // namespace bc250hip
