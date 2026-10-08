// SPDX-License-Identifier: MIT
// Per-application graphics settings: the reader that the D3D11 shell, the D3D12 shell and the router compile in.
// Contract and per-setting status: docs/design/per-app-graphics-settings.md. The control application writes the
// values; the driver only reads them.
//
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics                          the global values
//   HKLM\SOFTWARE\amdgpu-wddm\Graphics\Applications\<image>     the values of one application: <image> is the file
//                                                               name of the process image (game.exe); key names are
//                                                               not case-sensitive
//
// Precedence, highest first: the environment variable AMDGPU_WDDM_<NAME> (descriptor()), the application key, the
// global key. No value from any source = the application decides. Every value is a REG_DWORD. A value out of its
// range, a value of another type, or an environment string that is not a decimal number is ignored with one log
// line, and the next source is read. A 32-bit process reads the 64-bit registry view, as the router does (BD-064).
//
// ReportAmdDriverVersion (global only) is not read here: the kernel-mode driver reads it at adapter start
// (driver/kmd/driver_version.c).
//
// This part is C++14 without exceptions and without the standard library containers, because the router DLL is
// built that way (tools/build/build-umd-router.ps1); test-umd-app-settings.ps1 compiles it so. The shells include
// app-settings.h, which adds what they do with the values.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#pragma comment(lib,"advapi32.lib") // RegGetValueW

namespace amdgpu_wddm {
namespace app_settings {

enum class Setting : unsigned {FrameRateLimit,VSync,Anisotropy,MaxFrameLatency,PerformanceOverlay,RenderOnCpu,Count};
enum : unsigned {kCount=static_cast<unsigned>(Setting::Count),kAll=(1u<<kCount)-1u};
inline constexpr unsigned bit(Setting s) noexcept {return 1u<<static_cast<unsigned>(s);}

// Where a value came from. None: no source has a valid value, the application decides.
enum class Source : unsigned {None,Global,Application,Environment};

struct Descriptor {
    const wchar_t* value;   // REG_DWORD value name in both keys
    const char* name;       // the same name, for log lines
    const char* env;        // the environment variable that wins over both keys
    const char* range;      // the accepted values, for the log line of an ignored value
};
inline const Descriptor& descriptor(Setting s) noexcept {
    static const Descriptor table[kCount]={
        {L"FrameRateLimit","FrameRateLimit","AMDGPU_WDDM_FRAME_RATE_LIMIT","0, 20-300"},
        {L"VSync","VSync","AMDGPU_WDDM_VSYNC","0, 1"},
        {L"Anisotropy","Anisotropy","AMDGPU_WDDM_ANISOTROPY","1, 2, 4, 8, 16"},
        {L"MaxFrameLatency","MaxFrameLatency","AMDGPU_WDDM_MAX_FRAME_LATENCY","1-3"},
        {L"PerformanceOverlay","PerformanceOverlay","AMDGPU_WDDM_PERFORMANCE_OVERLAY","0, 1"},
        {L"RenderOnCpu","RenderOnCpu","AMDGPU_WDDM_RENDER_ON_CPU","0, 1"},
    };
    return table[static_cast<unsigned>(s)<kCount ? static_cast<unsigned>(s) : 0u];
}
inline const wchar_t* global_key() noexcept {return L"SOFTWARE\\amdgpu-wddm\\Graphics";}
inline const wchar_t* applications_key() noexcept {return L"SOFTWARE\\amdgpu-wddm\\Graphics\\Applications";}

inline bool valid(Setting s,uint32_t v) noexcept {
    switch(s){
    case Setting::FrameRateLimit:return v==0 || (v>=20 && v<=300);
    case Setting::VSync:return v<=1;
    case Setting::Anisotropy:return v==1 || v==2 || v==4 || v==8 || v==16;
    case Setting::MaxFrameLatency:return v>=1 && v<=3;
    case Setting::PerformanceOverlay:return v<=1;
    case Setting::RenderOnCpu:return v<=1;
    default:return false;
    }
}
inline const char* source_name(Source s) noexcept {
    switch(s){
    case Source::Global:return "global";
    case Source::Application:return "application";
    case Source::Environment:return "environment";
    default:return "unset";
    }
}

// What a registry read found. WrongType: the value exists but is not a 4-byte REG_DWORD.
enum class Read : unsigned {Absent,Value,WrongType};
// The two inputs of resolve(); the host tests give doubles, system_sources() the system. Neither may throw.
struct Sources {
    void* context;
    // application: null for the global key, else the image file name.
    Read (*registry)(void* context,const wchar_t* application,const wchar_t* value,uint32_t* out);
    // false: the variable is not set. text gets at most size-1 characters and a terminating zero.
    bool (*environment)(void* context,const char* name,char* text,size_t size);
};

struct Value {
    uint32_t value=0;
    Source source=Source::None;
    bool set() const noexcept {return source!=Source::None;}
};
enum class Problem : unsigned {OutOfRange,WrongType,NotNumber};
struct Rejection {
    Setting setting=Setting::FrameRateLimit;
    Source source=Source::None;
    Problem problem=Problem::OutOfRange;
    uint32_t value=0;       // OutOfRange: the value
    char text[24]={};       // NotNumber: the start of the environment string
};
struct Settings {
    Value values[kCount];
    Rejection rejections[kCount*3];
    unsigned rejection_count=0;
    wchar_t application[MAX_PATH]={};   // the image file name the application key was read for ("" = none)
    const Value& operator[](Setting s) const noexcept {return values[static_cast<unsigned>(s)];}
};

namespace detail {
// Bounded copies that always end with a zero (the CRT's _s forms stop the process on truncation).
template<class C> void copy(C* out,size_t size,const C* in) noexcept {
    if(!size)return;
    size_t i=0;
    for(;in && in[i] && i+1<size;++i)out[i]=in[i];
    out[i]=0;
}
inline void reject(Settings& s,Setting setting,Source source,Problem problem,uint32_t value,const char* text) noexcept {
    if(s.rejection_count>=sizeof(s.rejections)/sizeof(s.rejections[0]))return;
    Rejection& r=s.rejections[s.rejection_count++];
    r.setting=setting;r.source=source;r.problem=problem;r.value=value;
    if(text)copy(r.text,sizeof(r.text),text);
}
// One registry source of one setting: true when it gave the effective value.
inline bool take_registry(Settings& s,Setting setting,Source source,const Sources& from,const wchar_t* application) noexcept {
    if(!from.registry)return false;
    uint32_t v=0;
    switch(from.registry(from.context,application,descriptor(setting).value,&v)){
    case Read::Value:
        if(valid(setting,v)){s.values[static_cast<unsigned>(setting)].value=v;
            s.values[static_cast<unsigned>(setting)].source=source;return true;}
        reject(s,setting,source,Problem::OutOfRange,v,nullptr);return false;
    case Read::WrongType:reject(s,setting,source,Problem::WrongType,0,nullptr);return false;
    default:return false;
    }
}
} // namespace detail

// A decimal number of 1 to 10 digits that fits 32 bits, nothing else (no sign, no space, no "0x").
inline bool parse_decimal(const char* text,uint32_t* out) noexcept {
    if(!text || !*text)return false;
    uint64_t v=0;unsigned digits=0;
    for(const char* p=text;*p;++p){
        if(*p<'0' || *p>'9' || ++digits>10)return false;
        v=v*10u+static_cast<unsigned>(*p-'0');
    }
    if(v>0xFFFFFFFFull)return false;
    *out=static_cast<uint32_t>(v);return true;
}

// The effective value of every setting into s. application: the image file name, or null or "" for none (then only
// the environment and the global key count).
inline void resolve(const wchar_t* application,const Sources& from,Settings& s) noexcept {
    s=Settings{};
    if(application && *application)detail::copy(s.application,MAX_PATH,application);
    for(unsigned i=0;i<kCount;++i){
        const Setting setting=static_cast<Setting>(i);
        char text[64]={};
        if(from.environment && from.environment(from.context,descriptor(setting).env,text,sizeof(text))){
            uint32_t v=0;
            if(!parse_decimal(text,&v))detail::reject(s,setting,Source::Environment,Problem::NotNumber,0,text);
            else if(!valid(setting,v))detail::reject(s,setting,Source::Environment,Problem::OutOfRange,v,nullptr);
            else {s.values[i].value=v;s.values[i].source=Source::Environment;continue;}
        }
        if(s.application[0] && detail::take_registry(s,setting,Source::Application,from,s.application))continue;
        detail::take_registry(s,setting,Source::Global,from,nullptr);
    }
}

// ---- the system sources --------------------------------------------------------------------------------------

inline Read system_registry(void*,const wchar_t* application,const wchar_t* value,uint32_t* out) noexcept {
    wchar_t key[MAX_PATH+64];
    if(application){
        if(std::wcschr(application,L'\\') || std::wcschr(application,L'/'))return Read::Absent;
        if(std::swprintf(key,sizeof(key)/sizeof(key[0]),L"%ls\\%ls",applications_key(),application)<0)return Read::Absent;
    } else detail::copy(key,sizeof(key)/sizeof(key[0]),global_key());
    DWORD type=0,size=sizeof(uint32_t);uint32_t data=0;
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,key,value,RRF_RT_ANY|RRF_NOEXPAND|RRF_SUBKEY_WOW6464KEY,
        &type,&data,&size);
    if(status==ERROR_MORE_DATA)return Read::WrongType;
    // Absent, or a key this process may not read (an AppContainer): as if no value were there.
    if(status!=ERROR_SUCCESS)return Read::Absent;
    if(type!=REG_DWORD || size!=sizeof(uint32_t))return Read::WrongType;
    *out=data;return Read::Value;
}
inline bool system_environment(void*,const char* name,char* text,size_t size) noexcept {
    if(!size)return false;
    text[0]=0;
    const DWORD n=GetEnvironmentVariableA(name,text,static_cast<DWORD>(size));
    if(!n)return false;
    // Too long for any accepted value: the log line says that, not the value.
    if(n>=size)detail::copy(text,size,"(too long)");
    return true;
}
inline Sources system_sources() noexcept {Sources s={nullptr,system_registry,system_environment};return s;}

// The file name of the process image (game.exe), or "" when it cannot be read.
inline void image_name(wchar_t* out,size_t size) noexcept {
    if(!size)return;
    out[0]=0;
    wchar_t path[2048];
    const DWORD n=GetModuleFileNameW(nullptr,path,static_cast<DWORD>(sizeof(path)/sizeof(path[0])));
    if(!n || n>=sizeof(path)/sizeof(path[0]))return;
    const wchar_t* base=path;
    for(const wchar_t* p=path;*p;++p)if(*p==L'\\' || *p==L'/')base=p+1;
    detail::copy(out,size,base);
}

inline bool render_on_cpu(const Settings& s) noexcept {
    const Value& v=s[Setting::RenderOnCpu];
    return v.set() && v.value==1;
}

} // namespace app_settings
} // namespace amdgpu_wddm
