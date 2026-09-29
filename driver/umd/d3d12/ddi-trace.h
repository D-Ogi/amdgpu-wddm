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
inline bool ddi_trace_enabled() noexcept {
    static const bool enabled=[]() noexcept {
        char value[2]{};
        return GetEnvironmentVariableA("AMDGPU_WDDM_DDI_TRACE",value,sizeof(value))==1 && value[0]=='1';
    }();
    return enabled;
}
inline std::atomic<uint64_t> ddi_trace_sequence{};
// Lab diagnostic switch: AMDGPU_WDDM_D3D12_EXPERIMENT names deviations from the driver's behaviour, for a
// measurement that needs them: one name or several separated by commas. Unset, empty or unknown means none.
// Read once per process.
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
inline uint64_t ddi_trace_begin(const char* name) noexcept {
    if(!ddi_trace_enabled())return 0;
    const auto id=ddi_trace_sequence.fetch_add(1,std::memory_order_relaxed)+1;
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    std::fprintf(stderr,"{\"event\":\"ddi\",\"edge\":\"begin\",\"sequence\":%llu,\"name\":\"%s\",\"qpc\":%lld,\"thread\":%lu}\n",
        static_cast<unsigned long long>(id),name,now.QuadPart,GetCurrentThreadId());
    std::fflush(stderr);return id;
}
inline void ddi_trace_end(const char* name,uint64_t id,HRESULT outcome) noexcept {
    if(!id)return;
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    std::fprintf(stderr,"{\"event\":\"ddi\",\"edge\":\"end\",\"sequence\":%llu,\"name\":\"%s\",\"qpc\":%lld,\"thread\":%lu,\"status\":\"%08lx\"}\n",
        static_cast<unsigned long long>(id),name,now.QuadPart,GetCurrentThreadId(),static_cast<unsigned long>(outcome));
    std::fflush(stderr);
}
}
