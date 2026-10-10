// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
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
// A refusal the shell decided, written whether or not any switch is on: to the AMDGPU_WDDM_LOG sink when the
// process named one, and always one line to the debugger channel, as engine-ddi's log_refusal does. The channel
// costs a raised-and-swallowed exception when nothing listens, and a refusal happens once per declined object,
// never per frame; capture-share and any attached debugger record the line. One budget bounds both channels, so
// a process that declines in a loop - which a survivable refusal invites (BD-075) - cannot be slowed by this
// diagnostic without bound. No handles, no pointers and no resource contents: names and scalars only.
inline std::atomic<int32_t> ddi_refusal_budget{4096};
inline void ddi_refusal(_Printf_format_string_ const char* format,...) noexcept {
    if(ddi_refusal_budget.fetch_sub(1,std::memory_order_relaxed)<=0)return;
    char text[600];
    va_list args;
    va_start(args,format);
    std::vsnprintf(text,sizeof(text),format,args);
    va_end(args);
    amdgpu_wddm_log::print("amdgpu_wddm_d3d12: %s\n",text);
    char line[640];
    std::snprintf(line,sizeof(line),"amdgpu_wddm_d3d12 %s\n",text);
    OutputDebugStringA(line);
}
// The file name of a __builtin_FILE() path, without its directories.
inline const char* ddi_source_name(const char* path) noexcept {
    if(!path)return "?";
    const char* base=path;
    for(const char* c=path;*c;c++){if(*c=='\\' || *c=='/')base=c+1;}
    return base;
}
// The FIRST device removal of this process, named where it was decided, on the always-on channel.
//
// Why this is not behind a switch. Every other record of a removal is: ddi_failure_note needs
// AMDGPU_WDDM_DDI_TRACE=2 and writes to the debugger channel only, so a removal left no trace in the
// process log that lab evidence collects. On 2026-10-09 the Vulkan WSI DXGI route read
// ID3D12Fence::GetCompletedValue() == UINT64_MAX on a shared blit fence of this device, which is the
// documented answer for a REMOVED device, and no line of any log said that a device had been removed,
// let alone which decision removed it (BD-105). A removal happens at most once per device and ends
// every later call anyway, so one line costs nothing measurable and answers the question that an
// argument from absence cannot.
//
// One line per process: the first removal is the one that matters, and a cascade of dependent
// removals after it would bury it.
inline void ddi_first_removal(const char* what,const char* file,int line) noexcept {
    static std::atomic<bool> seen{};
    if(seen.exchange(true,std::memory_order_relaxed))return;
    ddi_refusal("device removed: %s at %s:%d, the first removal in this process",
        what?what:"?",ddi_source_name(file),line);
}
// The FIRST failing DDI of each group of the presenter's path, named where it failed, on the
// always-on channel.
//
// Why a group and not one line per process. ddi_first_removal above answers "which decision removed
// the device". It does not answer the question BD-105 round 3 actually left open, which is which CALL
// of the Vulkan WSI's DXGI present route first refused: the command list's Close, the queue's Wait,
// Signal or ExecuteCommandLists, the fence, the shared-resource open, or the Present. Each of those
// reaches this driver as a DDI of its own, and the removal - if there is one - is a consequence of
// the first refusal, not the refusal itself. One line per process would be spent by whichever
// unrelated slot refused first in a game's start-up; one line per GROUP keeps the presenter's path
// readable while still bounding the cost to a handful of lines per process, inside the same refusal
// budget every refusal shares.
//
// The groups are the boundaries the audit of 2026-10-10 named (the "Vulkan/DXGI call-by-call
// conclusion" table): the recording close, the queue's synchronisation and submission, the fence
// objects, the shared-resource open, and the present. Any is the process's very first failing DDI
// whatever it was, so a refusal on a path nobody listed is still named once.
//
// What this does NOT do: it does not say that the named DDI is the cause. It says which DDI of that
// group failed first, with its HRESULT and the site of the binding that reported it, which is the
// reading a timeout two seconds later cannot produce.
enum class DdiFailureGroup:unsigned {Any=0,List,Queue,Fence,Shared,Present,Count};
inline const char* ddi_failure_group_name(DdiFailureGroup group) noexcept {
    switch(group){
    case DdiFailureGroup::Any:return "any";
    case DdiFailureGroup::List:return "list-close";
    case DdiFailureGroup::Queue:return "queue-sync";
    case DdiFailureGroup::Fence:return "fence";
    case DdiFailureGroup::Shared:return "shared-open";
    case DdiFailureGroup::Present:return "present";
    default:return "?";
    }
}
// Which group a DDI slot name belongs to. The names are the WDK's own pfn* spellings
// (toolchain/nuget microsoft.windows.sdk.cpp Include/10.0.26100.0/um/d3d12umddi.h), matched by
// substring so that a versioned slot (pfnCreateFence_0003 and the like) lands in its own group.
// Pure, and the experiment test drives it against every name of the route.
inline DdiFailureGroup ddi_failure_group(const char* name) noexcept {
    if(!name)return DdiFailureGroup::Any;
    const auto has=[name](const char* part) noexcept {return std::strstr(name,part)!=nullptr;};
    if(has("CloseCommandList"))return DdiFailureGroup::List;
    if(has("Present"))return DdiFailureGroup::Present;
    if(has("Fence"))return DdiFailureGroup::Fence;   // CreateFence, SetFenceEventOnCompletion, ...
    if(has("Shared") || has("OpenHeap") || has("OpenResource"))return DdiFailureGroup::Shared;
    if(has("ExecuteCommandLists") || has("SignalSynchronizationObject") ||
       has("WaitForSynchronizationObject") || has("CommandQueue"))return DdiFailureGroup::Queue;
    return DdiFailureGroup::Any;
}
// E_PENDING is a slot's ordinary "not yet", not a refusal: ddi_failure_note already excludes it and
// so does this ledger.
inline bool ddi_failure_is_refusal(HRESULT outcome) noexcept {
    return FAILED(outcome) && outcome!=E_PENDING;
}
inline std::atomic<bool> ddi_first_failure_seen[unsigned(DdiFailureGroup::Count)]{};
inline void ddi_first_failure(const char* name,HRESULT outcome,bool device_lost,
    const char* file=__builtin_FILE(),int line=__builtin_LINE()) noexcept {
    if(!ddi_failure_is_refusal(outcome))return;
    const DdiFailureGroup group=ddi_failure_group(name);
    bool wrote=false;
    // The process's very first failing DDI, and then the first of this slot's own group. A refusal
    // that is both writes one line, because Any is claimed first and the group is claimed after it.
    for(const DdiFailureGroup g:{DdiFailureGroup::Any,group}){
        if(ddi_first_failure_seen[unsigned(g)].exchange(true,std::memory_order_relaxed))continue;
        if(wrote)break;   // the same refusal: one line names both
        ddi_refusal("first failing DDI of group %s: %s hr=%08lx at %s:%d, device %s",
            ddi_failure_group_name(g),name?name:"?",static_cast<unsigned long>(outcome),
            ddi_source_name(file),line,device_lost?"lost":"live");
        wrote=true;
    }
}
inline void ddi_failure_note(const char* name,HRESULT outcome) noexcept {
    if(ddi_trace_mode()!=2 || ddi_failure_budget.fetch_sub(1,std::memory_order_relaxed)<=0)return;
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);
    char line[192];
    std::snprintf(line,sizeof(line),"amdgpu_wddm_d3d12 failure name=%s status=%08lx qpc=%lld thread=%lu\n",
        name,static_cast<unsigned long>(outcome),now.QuadPart,GetCurrentThreadId());
    OutputDebugStringA(line);
}
// AMDGPU_WDDM_D3D12_EXPERIMENT names deviations from the driver's behaviour, for a measurement that needs
// them: one name or several separated by commas. Empty, "none" or unknown names mean none. Read once per
// process.
//
// **Every validated behaviour is the driver's default and needs no switch, no profile and no variable.** A
// switch is only ever how an operator turns one OFF again, for a bisect or a rollback without a new binary,
// and every such name ends in "-off". The defaults and their off switches:
//   raytracing-tier-off: the adapter reports D3D12 RaytracingTier NOT_SUPPORTED instead of the tier the
//     engine computed (adapter-caps.cpp, engine-ddi set_raytracing_tier_reporting).
//   recording-bind-off: the command-list recording entry is not published (device-engine.cpp, lever L2).
//   retire-handoff-off: submissions keep the release sequence instead of leaving it to the resource DDIs
//     (device-engine.cpp, lever L3).
//   deferred-replay-off: recording calls make their engine calls in line, with no ring and no worker thread
//     (device-engine.cpp).
//   direct-entry-off: the graphics list table keeps the shell's entry for the value-only recording calls
//     instead of engine-ddi's direct entry, which writes their ring entries itself (native-tables.cpp; trial 327:
//     +3.8 % at Witcher 3 LOW). recording-bind-off or deferred-replay-off implies it.
//   release-two-phase-off: engine-ddi waits for one snapshot per release, not two (device-engine.cpp, F1 of
//     the trial 245 report, M15.8).
//   import-progress-gate-off: a released import is not held for the device-wide progress of its release
//     (heap-import.cpp, ImportReleasePolicy, F2).
//   import-quarantine-off: no release delay, no caps (heap-import.cpp, F3).
//   shader-model-68-off: the adapter reports shader model 6.7 at most, and no 1091 answer (adapter-caps.cpp,
//     engine-ddi set_shader_model_ceiling, M840).
//   shader-model-67-off: the adapter reports shader model 6.6 at most, the answer of earlier builds; it wins over
//     shader-model-68-off.
//   scanout-flip-off: a swap-chain primary is always the composed one, never a scan-out primary
//     (scanout-mode.h, M15.14 increment 3; trial 478).
// All eleven off is the release and reporting behaviour of adapter106 with no ray tracing.
//
// Two names are still opt-in, because no measurement admits them as defaults under the GPU compositor:
//   present-cached: the swap-chain surface is placed in the Cached aperture (heap-import.cpp). dxgkrnl
//     refuses Cached on a PRIMARY allocation (trial 105), so it needs present-noprimary with it.
//   present-noprimary: the swap-chain buffers are not PRIMARY (heap-import.cpp), which also forfeits the
//     scanout route. Measured gain of the pair under the CPU compositor, none under the GPU one (K115).
// One more name asks for a diagnostic rather than a deviation:
//   replay-log: with deferred replay on, the replay lines also go to a file of their own (replay-log.h), for a
//     game whose stderr nobody reads. It changes no behaviour; it costs the writes.
// The positive names of the defaults (raytracing-tier, recording-bind, retire-handoff, deferred-replay) are
// still accepted and do nothing, so that an older trial string or profile naming them still resolves.
//
// Where the list comes from, first source that exists wins:
//   1. AMDGPU_WDDM_D3D12_EXPERIMENT in the process environment, "none" and empty included. A trial sets it
//      so that it never inherits a profile.
//   2. the REG_SZ value "Experiment" of HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<image file name>, the
//      application profile (M15.7), for an application-specific workaround shipped by the package.
//   3. the REG_SZ value "Experiment" of HKLM\SOFTWARE\amdgpu-wddm\D3D12, which turns a default off for the
//      whole machine without replacing the binary.
// Registry key names compare without case; both values take the same syntax.
enum class DdiExperimentSource : unsigned {
    None,         // no variable, no profile for this image and no machine-wide value
    Environment,  // AMDGPU_WDDM_D3D12_EXPERIMENT, "none" and empty included
    Profile,      // the image's application profile
    Machine,      // the machine-wide value under HKLM\SOFTWARE\amdgpu-wddm\D3D12
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
// The registry view of every HKLM read of the shell. The installer and the control application write
// HKLM\SOFTWARE\amdgpu-wddm in the 64-bit view; the x86 shell of a 32-bit process would otherwise read the empty
// WOW6432Node copy (the x86 router reads the 64-bit view as well). 0 on x64 keeps that image as it was.
inline constexpr DWORD registry_view=sizeof(void*)==4?RRF_SUBKEY_WOW6464KEY:0;
// The "Experiment" value of one key into text, 1 when found, 0 when the key or the value does not exist, -1
// when the value is not a string of the syntax that fits.
inline int registry_experiment(const wchar_t* key,char* text,size_t capacity) noexcept {
    wchar_t value[256]{};DWORD bytes=sizeof(value);
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,key,L"Experiment",RRF_RT_REG_SZ|registry_view,nullptr,value,&bytes);
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
// The profile's list for this process into text, with the same three outcomes. An image whose name cannot be
// read has no profile, which leaves the machine-wide value to answer.
inline int application_profile(char* text,size_t capacity) noexcept {
    wchar_t image[MAX_PATH]{};
    const DWORD n=GetModuleFileNameW(nullptr,image,MAX_PATH);
    wchar_t key[96+MAX_PATH]{};
    if(!n || n>=MAX_PATH || !application_profile_key(image,key,sizeof(key)/sizeof(wchar_t)))return 0;
    return registry_experiment(key,text,capacity);
}
inline int machine_experiment(char* text,size_t capacity) noexcept {
    return registry_experiment(L"SOFTWARE\\amdgpu-wddm\\D3D12",text,capacity);
}
inline DdiExperimentValue resolve_experiment() noexcept {
    DdiExperimentValue v{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n=GetEnvironmentVariableA("AMDGPU_WDDM_D3D12_EXPERIMENT",v.text,sizeof(v.text));
    if(n || GetLastError()!=ERROR_ENVVAR_NOT_FOUND){
        v.source=n<sizeof(v.text)?DdiExperimentSource::Environment:DdiExperimentSource::Invalid;
    } else {
        int found=application_profile(v.text,sizeof(v.text));
        DdiExperimentSource source=DdiExperimentSource::Profile;
        if(found==0){found=machine_experiment(v.text,sizeof(v.text));source=DdiExperimentSource::Machine;}
        v.source=found>0?source:found<0?DdiExperimentSource::Invalid:DdiExperimentSource::None;
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
// M15.14 increment 2 spelled the scan-out mode with the geometry it was for, as one token -
// "scanout-flip-1920x1200" - because the kernel driver then admitted a flip at the POST geometry only.
// Increment 3 compares the chain with the source mode the kernel driver publishes (scanout-mode.h), so the
// geometry in the token is no longer compared: the token still parses, and reads as an explicit on, so that a
// lab script written for increment 2 keeps working. Width and height are written on true only.
inline bool ddi_experiment_scanout(const char* list,unsigned* width,unsigned* height) noexcept {
    static const char prefix[]="scanout-flip-";
    const size_t length=sizeof(prefix)-1;
    if(!list || !width || !height)return false;
    for(const char* given=list;*given;){
        const char* end=std::strchr(given,',');
        const size_t size=end?static_cast<size_t>(end-given):std::strlen(given);
        if(size>length && !std::memcmp(given,prefix,length)){
            unsigned value[2]{0,0};unsigned digits[2]{0,0};unsigned part=0;
            size_t i=length;
            for(;i<size;++i){
                const char c=given[i];
                if(c=='x'){if(part)break;++part;continue;}
                if(c<'0' || c>'9')break;
                if(digits[part]>=5)break;                 // 16384 is the widest surface either shell makes
                value[part]=value[part]*10u+unsigned(c-'0');++digits[part];
            }
            if(i==size && part==1 && digits[0] && digits[1] && value[0] && value[1]){
                *width=value[0];*height=value[1];
                return true;
            }
        }
        given+=size+(end?1:0);
    }
    return false;
}
inline bool ddi_experiment_scanout(unsigned* width,unsigned* height) noexcept {
    return ddi_experiment_scanout(ddi_experiment_name(),width,height);
}
// Whether the list turns the named default off, i.e. whether it names "<name>-off". Every default of this
// driver is read this way, so that the behaviour an application gets is the one the lab validated and a
// switch can only ever subtract from it. A name too long for the buffer is no switch, never an accidental off.
inline bool ddi_experiment_off(const char* name) noexcept {
    char text[64];
    const size_t length=name?std::strlen(name):0;
    if(!length || length+5>sizeof(text))return false;
    std::memcpy(text,name,length);
    std::memcpy(text+length,"-off",5);
    return ddi_experiment(text);
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
