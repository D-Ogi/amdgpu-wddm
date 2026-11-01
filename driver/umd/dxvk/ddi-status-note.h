// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <source_location>
#include "../d3d12/stdio-log.h"
namespace bc250::umd {
// BD-283: one line for each status other than S_OK that an HRESULT entry of the shell returns to the runtime. d3d11.dll
// removes the device for such a status of a DXGI entry (D3DDDIERR_DEVICEREMOVED through NDXGI::CDevice::RemoveDevice,
// every other nonzero value through DriverInternalError), and the kernel then records VidSchMarkDeviceAsError with
// FromUserMode=True and Reason 6. A void entry reports through report_ddi_error (ddi-entry.h), which writes its own
// line; an HRESULT entry has no such door, so a removal from one of them left no record in the game log (ShellExperienceHost
// at 20:30:58Z on 2026-11-01). The line names the entry, the status, the source line of the return and the inputs that
// decided it. Both flavours, at most 32 lines for each process, on the debugger channel and the game log.
struct DdiInputs {
    char text[224]{};
    DdiInputs() noexcept=default;
    explicit DdiInputs(_Printf_format_string_ const char *format,...) noexcept {
        va_list args;
        va_start(args,format);
        std::vsnprintf(text,sizeof(text),format,args);
        va_end(args);
    }
};
inline const char *ddi_source_name(const char *path) noexcept {
    const char *name=path ? path : "";
    for (const char *p=name;*p;++p) if (*p=='\\' || *p=='/') name=p+1;
    return name;
}
// Returns `status` unchanged, so a return site reads `return ddi_status_note("Present1",status,DdiInputs(...));`.
inline HRESULT ddi_status_note(const char *entry,HRESULT status,const DdiInputs &inputs=DdiInputs(),
    const std::source_location where=std::source_location::current()) noexcept {
    if (status==S_OK) return status;
    static std::atomic_uint left{32};
    unsigned count=left.load(std::memory_order_relaxed);
    while (count && !left.compare_exchange_weak(count,count-1,std::memory_order_relaxed)) {}
    if (!count) return status;
    char line[448];
    std::snprintf(line,sizeof(line),"BC250 DDI status: %s returns %08X at %s:%u; inputs: %s%s\n",entry ? entry : "?",
        unsigned(status),ddi_source_name(where.file_name()),unsigned(where.line()),inputs.text[0] ? inputs.text : "-",
        count==1 ? " [limit of these lines reached]" : "");
    OutputDebugStringA(line);
    amdgpu_wddm_log::print("%s",line);
    return status;
}
}
