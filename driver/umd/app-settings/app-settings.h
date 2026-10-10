// SPDX-License-Identifier: MIT
// Per-application graphics settings in the D3D11 and D3D12 shells: the values of app-settings-core.h once per
// process, their log lines, and what the shells do with them (present sync interval, frame rate limit, engine
// options). Contract and per-setting status: docs/design/per-app-graphics-settings.md.
//
// The values are read once per process and module (process_settings), at the first adapter open or device
// creation. A change takes effect at the next start of the application.
#pragma once
#include "app-settings-core.h"
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace amdgpu_wddm::app_settings {

// This module's reading of the settings: once per process, at the first call.
inline const Settings& process_settings() noexcept {
    static const Settings settings=[]() noexcept {
        Settings s;
        wchar_t name[MAX_PATH];image_name(name,MAX_PATH);
        resolve(name,system_sources(),s);
        return s;
    }();
    return settings;
}

// ---- log lines -----------------------------------------------------------------------------------------------

// "amdgpu-wddm settings: ignored FrameRateLimit=5 from the application key (accepted: 0, 20-300)"
inline int format_rejection(const Rejection& r,char* out,size_t size) noexcept {
    const Descriptor& d=descriptor(r.setting);
    const char* where=r.source==Source::Environment ? "the environment" :
        r.source==Source::Application ? "the application key" : "the global key";
    switch(r.problem){
    case Problem::OutOfRange:
        return std::snprintf(out,size,"amdgpu-wddm settings: ignored %s=%u from %s (accepted: %s)",
            d.name,r.value,where,d.range);
    case Problem::WrongType:
        return std::snprintf(out,size,"amdgpu-wddm settings: ignored %s from %s: not a REG_DWORD",d.name,where);
    default:
        return std::snprintf(out,size,"amdgpu-wddm settings: ignored %s=\"%s\" from %s: not a decimal number",
            d.env,r.text,where);
    }
}
// "amdgpu-wddm settings: api=d3d11 app=game.exe FrameRateLimit=60/application VSync=unset Anisotropy=16/global
//  MaxFrameLatency=unset PerformanceOverlay=1/environment RenderOnCpu=unset"
// applied: bits of the settings this module applies; a set value of any other setting is marked "/not-applied".
inline int format_effective(const Settings& s,const char* api,unsigned applied,char* out,size_t size) noexcept {
    char app[MAX_PATH*3]{};
    if(s.application[0] &&
       !WideCharToMultiByte(CP_UTF8,0,s.application,-1,app,static_cast<int>(sizeof(app)),nullptr,nullptr))
        detail::copy(app,sizeof(app),"?");
    int n=std::snprintf(out,size,"amdgpu-wddm settings: api=%s app=%s",api,app[0] ? app : "-");
    for(unsigned i=0;i<kCount && n>=0 && static_cast<size_t>(n)<size;++i){
        const Value& v=s.values[i];
        const char* name=descriptor(static_cast<Setting>(i)).name;
        if(!v.set())n+=std::snprintf(out+n,size-n," %s=unset",name);
        else n+=std::snprintf(out+n,size-n," %s=%u/%s%s",name,v.value,source_name(v.source),
            applied&(1u<<i) ? "" : "/not-applied");
    }
    return n;
}
inline std::atomic<bool>& logged() noexcept {static std::atomic<bool> done{false};return done;}
// The rejection lines and the effective line, each through emit (one line, no newline), once per module.
template<class Emit> void log_once(const Settings& s,const char* api,unsigned applied,Emit&& emit) noexcept {
    if(logged().exchange(true))return;
    char line[1024];
    for(unsigned i=0;i<s.rejection_count;++i)
        if(format_rejection(s.rejections[i],line,sizeof(line))>0)emit(line);
    if(format_effective(s,api,applied,line,sizeof(line))>0)emit(line);
}

// ---- what the shells do with the values ----------------------------------------------------------------------

// VSync as the sync interval override of the present callbacks (DXGIDDICB_PRESENT and D3D12DDI_PRESENT_0051):
// 0 = DXGI_DDI_FLIP_INTERVAL_IMMEDIATE (the API's sync interval 0), 1 = DXGI_DDI_FLIP_INTERVAL_ONE.
struct SyncOverride {bool valid; unsigned interval;};
inline SyncOverride sync_override(const Settings& s) noexcept {
    const Value& v=s[Setting::VSync];
    return v.set() ? SyncOverride{true,v.value ? 1u : 0u} : SyncOverride{false,0u};
}
// BD-099: the override that the D3D11 shell passes in DXGIDDICB_PRESENT. `full` is the shell's answer to "does the
// runtime read the whole structure": true at the WDDM 2.2 interface, build 5 and later, where the override field
// exists on both architectures. The whole override goes through there. Below that interface the runtime copies a
// shorter structure and the override reaches it as interval 0 (x64) or not at all (x86) (vblank-pacer.h), so the
// shell passes VSync 0 only and gives VSync 1 with its own vertical-blank waits (extra_vblanks).
inline SyncOverride d3d11_runtime_override(SyncOverride o,bool full) noexcept {
    if(full)return o;
    return o.valid && o.interval==0u ? o : SyncOverride{false,0u};
}
// BD-099: the vertical blanks a shell waits for itself after a Present, where the runtime does not apply the override
// (the D3D11 shell on the old present callback, vblank-pacer.h): the part of the VSync interval that the
// application's own interval does not give. Never negative: an override below the application's interval cannot be
// done by waiting. `full` is the flag of d3d11_runtime_override: the runtime then paces the frame itself, and a wait
// of the shell on top of that would halve the rate.
inline unsigned extra_vblanks(SyncOverride o,unsigned application_interval,bool full) noexcept {
    if(full)return 0u;
    return o.valid && o.interval>application_interval ? o.interval-application_interval : 0u;
}
// The frame rate cap in frames per second, 0 for none.
inline uint32_t frame_rate_limit(const Settings& s) noexcept {
    const Value& v=s[Setting::FrameRateLimit];
    return v.set() ? v.value : 0u;
}
// MaxFrameLatency, 0 for none (the runtime's own limit stays).
inline uint32_t max_frame_latency(const Settings& s) noexcept {
    const Value& v=s[Setting::MaxFrameLatency];
    return v.set() ? v.value : 0u;
}

// The DXVK user options of the settings, ';'-separated as DXVK_CONFIG takes them, or "" for none. Option names as
// in DXVK 5611118e: d3d11.samplerAnisotropy (src/d3d11/d3d11_options.cpp), dxvk.hud (src/dxvk/dxvk_options.cpp;
// the items of src/dxvk/hud/dxvk_hud.cpp, "api" added by the engine's DDI present path).
inline constexpr char kOverlayItems[]="fps,frametimes,gpuload,api";
inline std::string dxvk_config(const Settings& s) {
    std::string out;
    if(s[Setting::Anisotropy].set())out+="d3d11.samplerAnisotropy = "+std::to_string(s[Setting::Anisotropy].value);
    if(s[Setting::PerformanceOverlay].set() && s[Setting::PerformanceOverlay].value==1){
        if(!out.empty())out+=';';
        out+="dxvk.hud = ";out+=kOverlayItems;
    }
    return out;
}
// The value of the vkd3d-proton fork's VKD3D_SAMPLER_ANISOTROPY, or "" for none.
inline std::string vkd3d_anisotropy(const Settings& s) {
    return s[Setting::Anisotropy].set() ? std::to_string(s[Setting::Anisotropy].value) : std::string();
}

// Sets an environment variable of the process for the life of the object, then puts the previous state back, so
// that a child process never inherits it. The engines read it while the object lives (DXVK in its instance
// constructor, vkd3d-proton at device creation). One object at a time in the process: env_lock().
inline std::mutex& env_lock() noexcept {static std::mutex lock;return lock;}
class ScopedEnv {
public:
    enum class Mode {Replace,Append};   // Append: after a value already set, with ';' between (DXVK_CONFIG)
    // value "": nothing is changed.
    ScopedEnv(const char* name,const std::string& value,Mode mode) : name_(name) {
        if(value.empty())return;
        lock_=std::unique_lock<std::mutex>(env_lock());
        // The previous value whatever its length: it must come back unchanged.
        for(DWORD need=GetEnvironmentVariableA(name,nullptr,0);need;){
            std::string old(need,char{});
            const DWORD n=GetEnvironmentVariableA(name,old.data(),need);
            if(!n)break;                         // removed between the calls (or empty): absent
            if(n<need){old.resize(n);old_=old;had_=true;break;}
            need=n;                              // grew between the calls
        }
        std::string next=value;
        if(mode==Mode::Append && had_ && !old_.empty())next=old_+";"+value;
        // DXVK reads at most MAX_PATH-1 characters of a variable (src/util/util_env.cpp); beyond that it would read
        // nothing, so the settings alone are passed then.
        if(next.size()>=MAX_PATH)next=value;
        changed_=SetEnvironmentVariableA(name,next.c_str())!=FALSE;
        value_=next;
    }
    ~ScopedEnv() {
        if(!changed_)return;
        SetEnvironmentVariableA(name_,had_ ? old_.c_str() : nullptr);
    }
    ScopedEnv(const ScopedEnv&)=delete;
    ScopedEnv& operator=(const ScopedEnv&)=delete;
    bool changed() const noexcept {return changed_;}
    const std::string& value() const noexcept {return value_;}
private:
    const char* name_;
    std::unique_lock<std::mutex> lock_;
    std::string old_,value_;
    bool had_=false,changed_=false;
};

// ---- the frame rate limit ------------------------------------------------------------------------------------

// The clock of a frame rate limit. last: the release time of the previous frame (0 = none yet). Returns the time to
// wait for, or 0 to go on at once. A frame more than one interval late restarts the clock, so that a stall is not
// followed by a burst of frames above the cap.
inline uint64_t limiter_target(uint64_t now,uint64_t interval,uint64_t& last) noexcept {
    if(!interval)return 0;
    if(!last || now<last){last=now;return 0;}
    const uint64_t target=last+interval;
    if(now>=target){last=now-target>interval ? now : target;return 0;}
    last=target;return target;
}

// Per device. The D3D11 shell calls frame() once per Present after the Present callback, the D3D12 shell before it
// enters the device's queue domain, so that no other operation of the device waits with it.
class FrameLimiter {
public:
    FrameLimiter() noexcept=default;
    ~FrameLimiter() {if(timer_)CloseHandle(timer_);}
    FrameLimiter(const FrameLimiter&)=delete;
    FrameLimiter& operator=(const FrameLimiter&)=delete;
    // rate: frames per second, 0 = no limit.
    void frame(uint32_t rate) noexcept {
        if(!rate)return;
        LARGE_INTEGER frequency{},now{};
        QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&now);
        if(frequency.QuadPart<=0)return;
        AcquireSRWLockExclusive(&lock_);
        if(!timer_tried_){
            timer_tried_=true;
            timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
            if(!timer_)timer_=CreateWaitableTimerExW(nullptr,nullptr,0,TIMER_ALL_ACCESS);
        }
        const uint64_t target=limiter_target(static_cast<uint64_t>(now.QuadPart),
            static_cast<uint64_t>(frequency.QuadPart)/rate,last_);
        const HANDLE timer=timer_;
        ReleaseSRWLockExclusive(&lock_);
        if(target)wait_until(target,static_cast<uint64_t>(frequency.QuadPart),timer);
        frames_.fetch_add(1,std::memory_order_relaxed);
        if(target)waits_.fetch_add(1,std::memory_order_relaxed);
    }
    uint64_t frames() const noexcept {return frames_.load(std::memory_order_relaxed);}
    uint64_t waits() const noexcept {return waits_.load(std::memory_order_relaxed);}
private:
    // A timer sleep to about 1 ms before the target, then short yields.
    static void wait_until(uint64_t target,uint64_t frequency,HANDLE timer) noexcept {
        for(;;){
            LARGE_INTEGER now{};QueryPerformanceCounter(&now);
            const uint64_t at=static_cast<uint64_t>(now.QuadPart);
            if(at>=target)return;
            const uint64_t left=(target-at)*10000000ull/frequency;   // 100 ns units
            if(left>20000){
                LARGE_INTEGER due{};due.QuadPart=-static_cast<LONGLONG>(left-10000);
                // Concurrent Presents share this synchronization timer. A rearm or another waiter can
                // consume its only signal, so never wait indefinitely: the target clock below is authoritative.
                if(timer && SetWaitableTimerEx(timer,&due,0,nullptr,nullptr,nullptr,0))WaitForSingleObject(timer,1);
                else Sleep(1);
            } else if(left>2000)Sleep(0);
            else YieldProcessor();
        }
    }
    SRWLOCK lock_=SRWLOCK_INIT;
    uint64_t last_=0;
    HANDLE timer_=nullptr;
    bool timer_tried_=false;
    std::atomic<uint64_t> frames_{0},waits_{0};
};

} // namespace amdgpu_wddm::app_settings
