// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>

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
