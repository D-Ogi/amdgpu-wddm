// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace native12 {
// Opt-in for one diagnostic process, sampled once. Names and scalar outcomes
// and selected public scalar request/output fields only: no resource contents,
// handles or private pointers.
// AMDGPU_WDDM_DDI_TRACE: 1 is the full trace on stderr, two lines per call. 2 is failures only, for a
// process that makes millions of calls: nothing on success, one debugger-output line per failed call
// or reported device error, and silence after the budget is spent.
inline int ddi_trace_mode() noexcept {
    static const int mode=[]() noexcept {
        char value[2]{};
        if(GetEnvironmentVariableA("AMDGPU_WDDM_DDI_TRACE",value,sizeof(value))!=1)return 0;
        return value[0]=='1'?1:value[0]=='2'?2:0;
    }();
    return mode;
}
inline bool ddi_trace_enabled() noexcept {return ddi_trace_mode()==1;}
inline std::atomic<uint64_t> ddi_trace_sequence{};
inline std::atomic<int32_t> ddi_failure_budget{4096};
inline void ddi_failure_note(const char* name,HRESULT outcome) noexcept {
    if(ddi_trace_mode()!=2 || ddi_failure_budget.fetch_sub(1,std::memory_order_relaxed)<=0)return;
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    char line[192];
    std::snprintf(line,sizeof(line),"amdgpu_wddm_d3d12 failure name=%s status=%08lx qpc=%lld thread=%lu\n",
        name,static_cast<unsigned long>(outcome),now.QuadPart,GetCurrentThreadId());
    OutputDebugStringA(line);
}
// Lab diagnostic switch: AMDGPU_WDDM_D3D12_EXPERIMENT names deviations from the driver's behaviour, for a
// measurement that needs them: one name or several separated by commas. Unset, empty or unknown means none.
// Read once per process. Names: raytracing-tier (adapter-caps.cpp), present-cached and present-noprimary
// (heap-import.cpp), recording-bind and retire-handoff (device-engine.cpp: the recording entry binding and the
// retire hand-off, both read once per device).
// The three switches of the release gate (M15.8, the fixes of the trial 245 report) are the other way round:
// each names the fix to turn OFF, because all three are the driver's behaviour.
//   release-two-phase-off: engine-ddi waits for one snapshot per release, not two (device-engine.cpp, F1).
//   import-progress-gate-off: a released import is not held for the device-wide progress of its release
//     (heap-import.cpp, ImportReleasePolicy, F2).
//   import-quarantine-off: no release delay, no caps (heap-import.cpp, F3).
// All three off is adapter106's release behaviour.
// The value as given, for traces: lower-case letters, digits, hyphens and commas only, else empty.
inline const char* ddi_experiment_name() noexcept {
    static const struct Value {
        char text[96]{};
        Value() noexcept {
            const DWORD n=GetEnvironmentVariableA("AMDGPU_WDDM_D3D12_EXPERIMENT",text,sizeof(text));
            if(!n || n>=sizeof(text))text[0]=0;
            for(const char* p=text;*p;++p)
                if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-' || *p==',')){text[0]=0;break;}
        }
    } value;
    return value.text;
}
inline bool ddi_experiment(const char* name) noexcept {
    if(!name || !name[0])return false;
    const size_t length=std::strlen(name);
    for(const char* given=ddi_experiment_name();*given;){
        const char* end=std::strchr(given,',');
        const size_t size=end?static_cast<size_t>(end-given):std::strlen(given);
        if(size==length && !std::memcmp(given,name,length))return true;
        given+=size+(end?1:0);
    }
    return false;
}
// A formatted line of the failures-only mode, under the same budget.
inline void ddi_mode2_note(const char* text) noexcept {
    if(ddi_trace_mode()!=2 || ddi_failure_budget.fetch_sub(1,std::memory_order_relaxed)<=0)return;
    OutputDebugStringA(text);
}
inline uint64_t ddi_trace_begin(const char* name) noexcept {
    if(ddi_trace_mode()==2)return 1;
    if(!ddi_trace_enabled())return 0;
    const auto id=ddi_trace_sequence.fetch_add(1,std::memory_order_relaxed)+1;
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    std::fprintf(stderr,"{\"event\":\"ddi\",\"edge\":\"begin\",\"sequence\":%llu,\"name\":\"%s\",\"qpc\":%lld,\"thread\":%lu}\n",
        static_cast<unsigned long long>(id),name,now.QuadPart,GetCurrentThreadId());
    std::fflush(stderr);return id;
}
inline void ddi_trace_end(const char* name,uint64_t id,HRESULT outcome) noexcept {
    if(!id)return;
    if(ddi_trace_mode()==2){if(FAILED(outcome) && outcome!=E_PENDING)ddi_failure_note(name,outcome);return;}
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    std::fprintf(stderr,"{\"event\":\"ddi\",\"edge\":\"end\",\"sequence\":%llu,\"name\":\"%s\",\"qpc\":%lld,\"thread\":%lu,\"status\":\"%08lx\"}\n",
        static_cast<unsigned long long>(id),name,now.QuadPart,GetCurrentThreadId(),static_cast<unsigned long>(outcome));
    std::fflush(stderr);
}
}
