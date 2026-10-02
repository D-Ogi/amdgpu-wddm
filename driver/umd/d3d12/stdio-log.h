// SPDX-License-Identifier: MIT
// Where the D3D12 UMD's diagnostic lines go: the shell, engine-ddi and the trace in ddi-trace.h all print here.
// A UMD runs inside the application, whose stdio is the application's. 3DMark's adapter_info.exe pipes stderr,
// never reads it, and blocked in WriteFile once ~12 KB of these lines had filled the pipe; its parent gave up
// after 15 s and Time Spy found no rendering device (session 283). So nothing is printed unless the process asks
// through AMDGPU_WDDM_LOG, read once per process (the vkd3d-proton engine and the RADV ICD read the same switch):
//   unset, empty, "0" or anything else: no output.
//   "stderr": the process's stderr, the behaviour before this switch.
//   "file:<path>": appended to <path>, opened for append only and shared, so several modules and processes may
//     write to one file; a path that cannot be opened means no output.
// AMDGPU_WDDM_DDI_TRACE and the experiment switches choose what is logged; this switch only chooses where.
// Debugger output (OutputDebugString) is not stdio and stays as it is.
#pragma once
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace amdgpu_wddm_log {
namespace detail {
inline FILE* open_append(const char* path) noexcept {
    const HANDLE file=CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,
        OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return nullptr;
    const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(file),_O_APPEND|_O_WRONLY);
    if(fd<0){CloseHandle(file);return nullptr;}
    FILE* const out=_fdopen(fd,"a");
    if(!out){_close(fd);return nullptr;}
    std::setvbuf(out,nullptr,_IONBF,0); // one write per printf: lines of concurrent writers do not tear
    return out;
}
} // namespace detail

// The sink, or null when the switch is off.
inline FILE* stream() noexcept {
    static FILE* const out=[]() noexcept -> FILE* {
        char value[MAX_PATH+8]{};
        const DWORD n=GetEnvironmentVariableA("AMDGPU_WDDM_LOG",value,sizeof(value));
        if(!n || n>=sizeof(value))return nullptr;
        if(!std::strcmp(value,"stderr"))return stderr;
        if(std::strncmp(value,"file:",5) || !value[5])return nullptr;
        return detail::open_append(value+5);
    }();
    return out;
}
inline bool enabled() noexcept {return stream()!=nullptr;}
inline void print(_Printf_format_string_ const char* format,...) noexcept {
    FILE* const out=stream();
    if(!out)return;
    va_list args;
    va_start(args,format);
    std::vfprintf(out,format,args);
    va_end(args);
}
inline void flush() noexcept {
    if(FILE* const out=stream())std::fflush(out);
}
} // namespace amdgpu_wddm_log
