// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include "stdio-log.h"
#pragma comment(lib,"advapi32.lib") // RegGetValueW: the application profile below

namespace native12 {
// Opt-in for one diagnostic process, sampled once. Names and scalar outcomes
// and selected public scalar request/output fields only: no resource contents,
// handles or private pointers.
// AMDGPU_WDDM_DDI_TRACE: 1 is the full trace, two lines per call, on the AMDGPU_WDDM_LOG sink (stdio-log.h:
// nothing unless that switch names one). 2 is failures only, for a process that makes millions of calls:
// nothing on success, one debugger-output line per failed call or reported device error, and silence after the
// budget is spent.
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
// measurement that needs them: one name or several separated by commas. Empty, "none" or unknown names mean
// none; unset means the application profile below, if any. Read once per process. Names: raytracing-tier
// (adapter-caps.cpp), present-cached and present-noprimary (heap-import.cpp), recording-bind, retire-handoff and
// deferred-replay (device-engine.cpp: the recording entry binding, the retire hand-off and the deferred
// command-list replay, all read once per device).
// The three switches of the release gate (M15.8, the fixes of the trial 245 report) are the other way round:
// each names the fix to turn OFF, because all three are the driver's behaviour.
//   release-two-phase-off: engine-ddi waits for one snapshot per release, not two (device-engine.cpp, F1).
//   import-progress-gate-off: a released import is not held for the device-wide progress of its release
//     (heap-import.cpp, ImportReleasePolicy, F2).
//   import-quarantine-off: no release delay, no caps (heap-import.cpp, F3).
// All three off is adapter106's release behaviour.
// Application profile (M15.7): a game started by its own launcher (Steam) inherits nothing from a trial, so it
// would run without the switches its trials measured. When the process has no AMDGPU_WDDM_D3D12_EXPERIMENT,
// the REG_SZ value "Experiment" of HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<image file name> gives the
// list in the same syntax (registry key names compare without case). The variable wins whenever it is present;
// its value "none" names no switch and keeps a trial independent of any profile.
enum class DdiExperimentSource : unsigned {
    None,         // neither the variable nor a profile for this image
    Environment,  // AMDGPU_WDDM_D3D12_EXPERIMENT, "none" and empty included
    Profile,      // the image's application profile
    Invalid,      // a value outside the syntax or too long: no switch
};
struct DdiExperimentValue {
    char text[256]{};
    DdiExperimentSource source{DdiExperimentSource::None};
};
namespace ddi_detail {
inline bool experiment_syntax(const char* text) noexcept {
    for(const char* p=text;*p;++p)
        if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='-' || *p==','))return false;
    return true;
}
// The profile key of an image path: the file name after the last separator, under the Applications key.
inline bool application_profile_key(const wchar_t* image,wchar_t* key,size_t capacity) noexcept {
    static constexpr wchar_t prefix[]=L"SOFTWARE\\amdgpu-wddm\\D3D12\\Applications\\";
    const wchar_t* name=image;
    for(const wchar_t* p=image;*p;++p)if(*p==L'\\' || *p==L'/')name=p+1;
    const size_t prefix_length=sizeof(prefix)/sizeof(wchar_t)-1,name_length=std::wcslen(name);
    if(!name_length || prefix_length+name_length+1>capacity)return false;
    std::wmemcpy(key,prefix,prefix_length);
    std::wmemcpy(key+prefix_length,name,name_length+1);
    return true;
}
// The profile's list for this process into text, 1 when found, 0 when there is none, -1 when the value is not a
// string of the syntax that fits.
inline int application_profile(char* text,size_t capacity) noexcept {
    wchar_t image[MAX_PATH]{};
    const DWORD n=GetModuleFileNameW(nullptr,image,MAX_PATH);
    wchar_t key[96+MAX_PATH]{};
    if(!n || n>=MAX_PATH || !application_profile_key(image,key,sizeof(key)/sizeof(wchar_t)))return 0;
    wchar_t value[256]{};DWORD bytes=sizeof(value);
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,key,L"Experiment",RRF_RT_REG_SZ,nullptr,value,&bytes);
    if(status==ERROR_FILE_NOT_FOUND)return 0;
    if(status!=ERROR_SUCCESS)return -1;
    size_t i=0;
    for(;value[i];++i){
        if(i+1>=capacity || value[i]>0x7F)return -1;
        text[i]=static_cast<char>(value[i]);
    }
    text[i]=0;
    return 1;
}
inline DdiExperimentValue resolve_experiment() noexcept {
    DdiExperimentValue v{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n=GetEnvironmentVariableA("AMDGPU_WDDM_D3D12_EXPERIMENT",v.text,sizeof(v.text));
    if(n || GetLastError()!=ERROR_ENVVAR_NOT_FOUND){
        v.source=n<sizeof(v.text)?DdiExperimentSource::Environment:DdiExperimentSource::Invalid;
    } else {
        const int found=application_profile(v.text,sizeof(v.text));
        v.source=found>0?DdiExperimentSource::Profile:found<0?DdiExperimentSource::Invalid:DdiExperimentSource::None;
    }
    if(v.source==DdiExperimentSource::Invalid || !experiment_syntax(v.text)){
        v.text[0]=0;v.source=DdiExperimentSource::Invalid;
    }
    if(!std::strcmp(v.text,"none"))v.text[0]=0;
    return v;
}
}
// Read once per process.
inline const DdiExperimentValue& ddi_experiment_value() noexcept {
    static const DdiExperimentValue value=ddi_detail::resolve_experiment();
    return value;
}
// The list as given, for traces: lower-case letters, digits, hyphens and commas only, else empty.
inline const char* ddi_experiment_name() noexcept {return ddi_experiment_value().text;}
// Whether the comma-separated list names the switch exactly.
inline bool ddi_experiment_listed(const char* list,const char* name) noexcept {
    if(!list || !name || !name[0])return false;
    const size_t length=std::strlen(name);
    for(const char* given=list;*given;){
        const char* end=std::strchr(given,',');
        const size_t size=end?static_cast<size_t>(end-given):std::strlen(given);
        if(size==length && !std::memcmp(given,name,length))return true;
        given+=size+(end?1:0);
    }
    return false;
}
inline bool ddi_experiment(const char* name) noexcept {return ddi_experiment_listed(ddi_experiment_name(),name);}
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
    amdgpu_wddm_log::print("{\"event\":\"ddi\",\"edge\":\"begin\",\"sequence\":%llu,\"name\":\"%s\",\"qpc\":%lld,\"thread\":%lu}\n",
        static_cast<unsigned long long>(id),name,now.QuadPart,GetCurrentThreadId());
    amdgpu_wddm_log::flush();return id;
}
inline void ddi_trace_end(const char* name,uint64_t id,HRESULT outcome) noexcept {
    if(!id)return;
    if(ddi_trace_mode()==2){if(FAILED(outcome) && outcome!=E_PENDING)ddi_failure_note(name,outcome);return;}
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    amdgpu_wddm_log::print("{\"event\":\"ddi\",\"edge\":\"end\",\"sequence\":%llu,\"name\":\"%s\",\"qpc\":%lld,\"thread\":%lu,\"status\":\"%08lx\"}\n",
        static_cast<unsigned long long>(id),name,now.QuadPart,GetCurrentThreadId(),static_cast<unsigned long>(outcome));
    amdgpu_wddm_log::flush();
}
}
