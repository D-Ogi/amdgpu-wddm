// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include "../d3d12/stdio-log.h"
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
// BD-253: one line about the life of a device (a failed CreateDevice, a device that DestroyDevice could not clean
// up, a new device in the lab flavour), on the debugger channel and the AMDGPU_WDDM_LOG sink. Each caller passes its
// own budget, so that a process that repeats the failure cannot flood either channel.
inline void device_life_note(std::atomic_uint &budget,const char *format,...) noexcept {
    unsigned count=budget.load(std::memory_order_relaxed);
    while(count && !budget.compare_exchange_weak(count,count-1,std::memory_order_relaxed)){}
    if(!count)return;
    // 640: the longest line, the loss line with its fence records (BD-198), is about 420 characters.
    char text[640];
    va_list args;
    va_start(args,format);
    std::vsnprintf(text,sizeof(text),format,args);
    va_end(args);
    OutputDebugStringA(text);
    amdgpu_wddm_log::print("%s",text);
}
// A line budget for a process that lives for hours: `startup` lines, then `perMinute` lines in each minute. take()
// gets the minute from the caller (GetTickCount64()/60000), so a host test can move it. BD-283: a fixed budget of
// 256 engine info lines was spent at the start of a ShellExperienceHost, and its TrimMemory lines, which come at
// each hide, were lost. The count of lines that the minute limit skipped goes to the next line let through.
class LineAllowance {
public:
    constexpr LineAllowance(unsigned startup,unsigned perMinute) noexcept : startup_(startup),per_minute_(perMinute) {}
    // True when the line may go out. `skipped`: lines lost to the minute limit since the last line let through.
    // `lastStartup`: this line is the last of the start-up budget.
    bool take(ULONGLONG minute,unsigned &skipped,bool &lastStartup) noexcept {
        skipped=0; lastStartup=false;
        unsigned left=startup_.load(std::memory_order_relaxed);
        while(left && !startup_.compare_exchange_weak(left,left-1,std::memory_order_relaxed)){}
        if(left) { lastStartup=left==1; return true; }
        ULONGLONG seen=minute_.load(std::memory_order_relaxed);
        // Two threads at a minute change can each let one line more through: a bound, not an exact count.
        if(seen!=minute && minute_.compare_exchange_strong(seen,minute,std::memory_order_relaxed))
            used_.store(0,std::memory_order_relaxed);
        if(used_.fetch_add(1,std::memory_order_relaxed)>=per_minute_) {
            skipped_.fetch_add(1,std::memory_order_relaxed); return false;
        }
        skipped=skipped_.exchange(0,std::memory_order_relaxed);
        return true;
    }
private:
    std::atomic_uint startup_;
    const unsigned per_minute_;
    std::atomic<ULONGLONG> minute_{~0ull};
    std::atomic_uint used_{0},skipped_{0};
};
inline void APIENTRY engine_diagnostic(void *shell,UINT32 level,const char *message) noexcept {
    if(!message || level<1 || level>3)return;
    if(level==3){
        // Info lines only on the AMDGPU_WDDM_LOG sink, when the process asked for one (d3d12/stdio-log.h): DXVK's
        // "Effective configuration" there shows the options of the per-application settings. Bounded apart from
        // the errors and warnings below: 256 at the start, then 16 in each minute.
        if(!amdgpu_wddm_log::enabled())return;
        // BD-327: the engine's shader stats line of a device (one at each final Release) is never skipped by the
        // minute limit. shell is the Shell of the log sink that the engine used. An engine whose LogShaderStats sends
        // the line with Logger::logToSink uses the sink of the closing device: shell is then its RuntimeDevice
        // (DeviceOwner::initialize), the device= of its BD-327 line. An older engine sends the line through the
        // oldest live sink of the process (DXVK Logger, m_sinks.front()): with more than one device alive, shell is the
        // oldest live device, not the closing one. The translation counts in the line are the process's. A bound of
        // its own, 4096 lines in a process.
        static constexpr char kStats[]="amdgpu_wddm_dxvk: Shader stats:";
        if(!std::strncmp(message,kStats,sizeof(kStats)-1)){
            static std::atomic_uint stats{4096};
            unsigned left=stats.load(std::memory_order_relaxed);
            while(left && !stats.compare_exchange_weak(left,left-1,std::memory_order_relaxed)){}
            if(left)amdgpu_wddm_log::print("M14 engine stats: device=%p %.1024s\n",shell,message);
            return;
        }
        static LineAllowance info{256,16};
        unsigned skipped=0; bool last=false;
        if(!info.take(GetTickCount64()/60000,skipped,last))return;
        if(skipped)amdgpu_wddm_log::print("M14 engine info: %u lines skipped by the limit of 16 a minute\n",skipped);
        amdgpu_wddm_log::print("M14 engine info: %.1024s%s\n",message,last?" [start-up info budget spent, then 16 a minute]":"");
        return;
    }
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
    amdgpu_wddm_log::print("%s",text);
}
}
