// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdio>
namespace bc250::umd {
inline void adapter_diagnostic(const char *stage,UINT interfaceVersion,UINT version,UINT flags) noexcept {
    char text[192];
    std::snprintf(text,sizeof(text),"M14: %s Interface=%08X Version=%08X Flags=%08X\n",
        stage,interfaceVersion,version,flags);
    OutputDebugStringA(text);
}
inline void failure_diagnostic(const char *stage,HRESULT status) noexcept {
    char text[160];
    std::snprintf(text,sizeof(text),"M14: %s failed HRESULT=%08X\n",stage,static_cast<unsigned>(status));
    OutputDebugStringA(text);
}
// A failed step of a runtime surface (swap-chain buffer, shared texture): which step, its status and the
// numbers it compared. The DDI error line names only the DDI; 342 could not tell allocation from import.
inline void surface_diagnostic(const char *stage,HRESULT status,unsigned format,unsigned width,unsigned height,
    unsigned long long a,unsigned long long b,unsigned long long c) noexcept {
    static std::atomic_uint remaining{32};
    unsigned count=remaining.load(std::memory_order_relaxed);
    while(count && !remaining.compare_exchange_weak(count,count-1,std::memory_order_relaxed)){}
    if(!count)return;
    char text[256];
    std::snprintf(text,sizeof(text),"M14 surface %s failed HRESULT=%08X format=%u %ux%u values=%llu/%llu/%llu\n",
        stage,static_cast<unsigned>(status),format,width,height,a,b,c);
    OutputDebugStringA(text);
}
inline void APIENTRY engine_diagnostic(void *,UINT32 level,const char *message) noexcept {
    if(!message || level<1 || level>2)return;
    // No adapter userdata: even a quarantined device can log after CloseAdapter.
    // A process-wide bound prevents a broken title flooding the debug channel.
    static std::atomic_uint remaining{64};
    unsigned count=remaining.load(std::memory_order_relaxed);
    while(count && !remaining.compare_exchange_weak(count,count-1,std::memory_order_relaxed)){}
    if(!count)return;
    char text[1152];
    std::snprintf(text,sizeof(text),"M14 engine %s: %.1024s%s\n",level==1?"error":"warning",message,
        count==1?" [diagnostic limit reached]":"");
    // Engine log lock is held. Never call engine/Vulkan or runtime callbacks here.
    OutputDebugStringA(text);
}
}
