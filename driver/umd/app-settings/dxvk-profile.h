// SPDX-License-Identifier: MIT
// The DXVK profile source of the per-application settings (app-settings-core.h, Source::Profile): the dxgi.* options
// of DXVK's own configuration for this process (its app profiles, dxvk.conf, DXVK_CONFIG), as the DXVK engine reads
// them with DXVK's own config code (Bc250DxvkEngineGetAppOptions, engine ABI 1.5, bc250_dxvk_engine.h). A newer DXVK
// with new or changed profiles therefore needs no change here. Contract: docs/design/per-app-graphics-settings.md.
//
// On this route the system DXGI owns the swap chain and the adapter identity, so DXVK's DXGI front never runs and
// its dxgi.* options would apply nowhere. The shells give the options that they have a mechanism for as the lowest
// source, under the registry keys and the environment:
//   dxvk.maxFrameRate, else dxgi.maxFrameRate   FrameRateLimit. n > 0: a cap of n. -1 (no limiter): 0. -n: DXVK's
//                                               limiter that engages only when the game runs faster than n, given
//                                               as a cap of n. 0: no value.
//   dxgi.syncInterval                           VSync. A negative value: no value.
//   dxgi.maxFrameLatency                        MaxFrameLatency. 0: no value.
// A value out of the setting's range is ignored with the usual log line (Source::Profile). Every other option is
// listed once in the lab flavour with the reason it does not apply (not_applied_reason).
//
// Both shells load the engine from the D3D11 directory of the package: <this module's directory>\..\d3d11\ (the D3D11
// shell lives there itself, the D3D12 shell next to it in d3d12\). The D3D12 shell loads it for this one call and
// unloads it; an engine before ABI 1.5 has no such export, and then there is no profile source.
#pragma once
#include "app-settings-core.h"
#include "../../contract/amdgpu_driver_package.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace amdgpu_wddm::app_settings::dxvk_profile {

// As bc250_dxvk_engine.h r10 declares it; the D3D11 shell, which includes that header, checks that the two agree.
using GetAppOptions=HRESULT (APIENTRY*)(UINT32 abiVersion,char* text,UINT32 size,UINT32* length);
inline constexpr char kGetAppOptionsName[]="Bc250DxvkEngineGetAppOptions";
inline constexpr UINT32 kEngineAbi=0x00010005u;

struct Option {std::string name,value;};

struct Profile {
    bool read=false;                    // the engine answered
    HRESULT result=S_OK;                // the engine's answer, or the load error
    std::vector<Option> options;        // the engine's lines, in its order (sorted by name)
    bool has[kCount]={};                // the mapped values
    uint32_t value[kCount]={};
    const char* option[kCount]={};      // the option name that gave the value
};

// "name=value\n" lines; a line without '=' is skipped.
inline std::vector<Option> parse(const char* text) {
    std::vector<Option> out;
    if(!text)return out;
    for(const char* p=text;*p;){
        const char* end=std::strchr(p,'\n');
        const std::string line=end ? std::string(p,end) : std::string(p);
        const size_t eq=line.find('=');
        if(eq!=std::string::npos && eq)out.push_back({line.substr(0,eq),line.substr(eq+1)});
        if(!end)break;
        p=end+1;
    }
    return out;
}

// DXVK's int32 parse (Config::parseOptionValue): an optional '-' and decimal digits, nothing else.
inline bool parse_int(const std::string& text,int64_t* out) {
    size_t i=text.size() && text[0]=='-' ? 1u : 0u;
    if(i>=text.size() || text.size()-i>10)return false;
    int64_t v=0;
    for(;i<text.size();++i){
        if(text[i]<'0' || text[i]>'9')return false;
        v=v*10+(text[i]-'0');
    }
    if(text[0]=='-')v=-v;
    if(v<INT32_MIN || v>INT32_MAX)return false;
    *out=v;return true;
}

inline const Option* find(const std::vector<Option>& options,const char* name) {
    for(const auto& o:options)if(o.name==name)return &o;
    return nullptr;
}

// The settings that the options give (the rules in the header comment).
inline void map(Profile& p) {
    auto put=[&](Setting s,int64_t v,const char* name){
        const unsigned i=static_cast<unsigned>(s);
        p.has[i]=true;p.value[i]=v<0 ? 0xFFFFFFFFu : static_cast<uint32_t>(v);p.option[i]=name;
    };
    int64_t v=0;
    const Option* rate=find(p.options,"dxvk.maxFrameRate");
    const char* rate_name="dxvk.maxFrameRate";
    if(!rate){rate=find(p.options,"dxgi.maxFrameRate");rate_name="dxgi.maxFrameRate";}
    if(rate && parse_int(rate->value,&v) && v!=0)put(Setting::FrameRateLimit,v==-1 ? 0 : (v<0 ? -v : v),rate_name);
    if(const Option* o=find(p.options,"dxgi.syncInterval"); o && parse_int(o->value,&v) && v>=0)
        put(Setting::VSync,v,"dxgi.syncInterval");
    if(const Option* o=find(p.options,"dxgi.maxFrameLatency"); o && parse_int(o->value,&v) && v!=0)
        put(Setting::MaxFrameLatency,v<0 ? -1 : v,"dxgi.maxFrameLatency");
}

// Why an option that no setting takes does not apply here. nullptr: a setting takes it (map).
inline const char* not_applied_reason(const Profile& p,const std::string& name) {
    for(unsigned i=0;i<kCount;++i)if(p.has[i] && p.option[i] && name==p.option[i])return nullptr;
    if(name=="dxgi.maxFrameRate" || name=="dxvk.maxFrameRate"){
        // Both names set: dxvk.maxFrameRate wins, as in DXVK; or 0, which is DXVK's default.
        return "no limit asked (0), or dxvk.maxFrameRate wins over dxgi.maxFrameRate";
    }
    if(name=="dxgi.syncInterval")return "a negative value has no effect in DXVK either";
    if(name=="dxgi.maxFrameLatency")return "0 has no effect in DXVK either";
    if(name=="dxgi.customVendorId" || name=="dxgi.customDeviceId" || name=="dxgi.customDeviceDesc" ||
       name=="dxgi.hideNvidiaGpu" || name=="dxgi.hideNvkGpu" || name=="dxgi.hideAmdGpu" || name=="dxgi.hideIntelGpu")
        return "the system DXGI reports the adapter identity from the kernel-mode driver, the same for every process";
    if(name=="dxgi.maxDeviceMemory" || name=="dxgi.maxSharedMemory")
        return "the system DXGI reports the memory sizes from the kernel-mode driver, the same for every process";
    if(name=="dxgi.enableDummyCompositionSwapchain")return "composition swap chains are the system DXGI's";
    if(name=="dxgi.deferSurfaceCreation" || name=="dxgi.tearFree" || name=="dxgi.forceRefreshRate" ||
       name=="dxgi.enableHDR" || name=="dxgi.enableUe4Workarounds")
        return "an option of DXVK's DXGI and swap chain, which the system DXGI replaces on this route";
    return "no mechanism on this route";
}

// The profile source of Sources (app-settings-core.h); context is a const Profile*.
inline Read source(void* context,Setting setting,uint32_t* out) noexcept {
    const Profile* p=static_cast<const Profile*>(context);
    const unsigned i=static_cast<unsigned>(setting);
    if(!p || i>=kCount || !p->has[i])return Read::Absent;
    *out=p->value[i];return Read::Value;
}

// Asks a loaded engine. get: its export, or null.
inline Profile from_engine(GetAppOptions get) {
    Profile p;
    if(!get){p.result=E_NOINTERFACE;return p;}
    std::vector<char> text(4096);
    for(int attempt=0;attempt<4;++attempt){
        UINT32 length=0;
        p.result=get(kEngineAbi,text.data(),static_cast<UINT32>(text.size()),&length);
        if(p.result!=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) || length>(1u<<20))break;
        text.assign(length+1u,'\0');
    }
    if(FAILED(p.result))return p;
    p.read=true;
    p.options=parse(text.data());
    map(p);
    return p;
}

// The engine of the package, from the directory of the module that holds this code.
inline std::wstring engine_path() {
    HMODULE self=nullptr;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&engine_path),&self))return std::wstring();
    std::vector<wchar_t> path(32768);
    const DWORD n=GetModuleFileNameW(self,path.data(),static_cast<DWORD>(path.size()));
    if(!n || n>=path.size())return std::wstring();
    std::wstring dir(path.data(),n);
    const size_t slash=dir.find_last_of(L"\\/");
    if(slash==std::wstring::npos)return std::wstring();
    dir.resize(slash+1);
    const std::wstring relative=dir+L"..\\d3d11\\"+AMDGPU_DXVK_FILE;
    std::vector<wchar_t> full(32768);
    const DWORD m=GetFullPathNameW(relative.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
    return m && m<full.size() ? std::wstring(full.data(),m) : std::wstring();
}

// Loads the engine at path for this one query (a module already loaded keeps its reference count) and asks it.
inline Profile from_path(const std::wstring& path) {
    Profile p;
    if(path.empty()){p.result=E_FAIL;return p;}
    // Only the engine's own directory and System32, never the game's directory or PATH.
    HMODULE engine=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!engine){p.result=HRESULT_FROM_WIN32(GetLastError());return p;}
    p=from_engine(reinterpret_cast<GetAppOptions>(GetProcAddress(engine,kGetAppOptionsName)));
    FreeLibrary(engine);
    return p;
}

// This process's profile: once per module, at the first call. Never throws.
inline const Profile& process_profile() noexcept {
    static const Profile profile=[]() noexcept {
        try {return from_path(engine_path());}
        catch(...) {Profile p;p.result=E_OUTOFMEMORY;return p;}
    }();
    return profile;
}

// The lab log lines: what the engine answered, and every option that does not apply with its reason.
template<class Emit> void log_not_applied(const Profile& p,Emit&& emit) {
    char line[512];
    if(!p.read){
        std::snprintf(line,sizeof(line),"amdgpu-wddm settings: no DXVK profile options (engine result %08lx)",
            static_cast<unsigned long>(p.result));
        emit(line);return;
    }
    for(const auto& o:p.options){
        const char* reason=not_applied_reason(p,o.name);
        if(!reason)continue;
        std::snprintf(line,sizeof(line),"amdgpu-wddm settings: DXVK profile option %s=%s not applied: %s",
            o.name.c_str(),o.value.c_str(),reason);
        emit(line);
    }
}

} // namespace amdgpu_wddm::app_settings::dxvk_profile
