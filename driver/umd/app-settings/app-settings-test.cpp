// SPDX-License-Identifier: MIT
// Host tests of app-settings.h: number parsing, the accepted range of every setting, the precedence environment >
// application key > global key, ignored values (out of range, wrong type, not a number) with their log lines, the
// effective-settings line, the engine options the shells derive, the frame rate limit clock and its wait, the
// scoped environment variable, and the system registry reader on a private hive.
//
// The router's half (app-settings-core.h) is also compiled as the router DLL is built, C++14 without exceptions:
// test-umd-app-settings.ps1 does that with core-c++14-check.cpp.
//
// No machine state is touched: the registry part loads a private application hive below the working directory
// (RegLoadAppKey) and overrides HKEY_LOCAL_MACHINE with it for this process (RegOverridePredefKey). Environment
// variables are set only in this process.
#include "app-settings.h"
#include <map>
#include <string>
#include <vector>

namespace as=amdgpu_wddm::app_settings;
using as::Setting;
using as::Source;

namespace {
int failures=0,checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);++failures;}}while(0)

// A registry and an environment double. Keys: L"" for the global key, else the application name in lower case.
struct Fake {
    struct Entry {as::Read read; uint32_t value;};
    std::map<std::wstring,std::map<std::wstring,Entry>> keys;
    std::map<std::string,std::string> env;
    std::vector<std::wstring> asked;   // "<key>|<value>" of every registry read
    void dword(const std::wstring& key,const wchar_t* name,uint32_t v) {keys[key][name]={as::Read::Value,v};}
    void wrong(const std::wstring& key,const wchar_t* name) {keys[key][name]={as::Read::WrongType,0};}
};
std::wstring lower(std::wstring s) {for(auto& c:s)c=static_cast<wchar_t>(towlower(c));return s;}
as::Read fake_registry(void* context,const wchar_t* application,const wchar_t* value,uint32_t* out) noexcept {
    auto& f=*static_cast<Fake*>(context);
    const std::wstring key=application ? lower(application) : L"";
    f.asked.push_back(key+L"|"+value);
    const auto k=f.keys.find(key);
    if(k==f.keys.end())return as::Read::Absent;
    const auto v=k->second.find(value);
    if(v==k->second.end())return as::Read::Absent;
    if(v->second.read==as::Read::Value)*out=v->second.value;
    return v->second.read;
}
bool fake_environment(void* context,const char* name,char* text,size_t size) noexcept {
    auto& f=*static_cast<Fake*>(context);
    const auto e=f.env.find(name);
    if(e==f.env.end())return false;
    as::detail::copy(text,size,e->second.c_str());
    return true;
}
as::Settings run(Fake& f,const wchar_t* app=L"Game.EXE") {
    as::Settings s;as::resolve(app,{&f,fake_registry,fake_environment},s);return s;
}
bool has(const std::string& text,const char* part) {return text.find(part)!=std::string::npos;}
std::string effective(const as::Settings& s,unsigned applied=as::kAll) {
    char line[1024];as::format_effective(s,"d3d11",applied,line,sizeof(line));return line;
}
std::string rejection(const as::Settings& s,unsigned i) {
    char line[512];as::format_rejection(s.rejections[i],line,sizeof(line));return line;
}

void parse_tests() {
    uint32_t v=0;
    CHECK(as::parse_decimal("0",&v) && v==0);
    CHECK(as::parse_decimal("60",&v) && v==60);
    CHECK(as::parse_decimal("4294967295",&v) && v==4294967295u);
    CHECK(!as::parse_decimal("4294967296",&v));
    CHECK(!as::parse_decimal("12345678901",&v));
    CHECK(!as::parse_decimal("",&v));
    CHECK(!as::parse_decimal(nullptr,&v));
    CHECK(!as::parse_decimal(" 60",&v));
    CHECK(!as::parse_decimal("60 ",&v));
    CHECK(!as::parse_decimal("-1",&v));
    CHECK(!as::parse_decimal("+1",&v));
    CHECK(!as::parse_decimal("0x10",&v));
    CHECK(!as::parse_decimal("6O",&v));
}

void range_tests() {
    struct Case {Setting s; uint32_t v; bool ok;};
    const Case cases[]={
        {Setting::FrameRateLimit,0,true},{Setting::FrameRateLimit,1,false},{Setting::FrameRateLimit,19,false},
        {Setting::FrameRateLimit,20,true},{Setting::FrameRateLimit,144,true},{Setting::FrameRateLimit,300,true},
        {Setting::FrameRateLimit,301,false},{Setting::FrameRateLimit,0xFFFFFFFFu,false},
        {Setting::VSync,0,true},{Setting::VSync,1,true},{Setting::VSync,2,false},
        {Setting::Anisotropy,0,false},{Setting::Anisotropy,1,true},{Setting::Anisotropy,2,true},
        {Setting::Anisotropy,3,false},{Setting::Anisotropy,4,true},{Setting::Anisotropy,6,false},
        {Setting::Anisotropy,8,true},{Setting::Anisotropy,12,false},{Setting::Anisotropy,16,true},
        {Setting::Anisotropy,32,false},
        {Setting::MaxFrameLatency,0,false},{Setting::MaxFrameLatency,1,true},{Setting::MaxFrameLatency,3,true},
        {Setting::MaxFrameLatency,4,false},
        {Setting::PerformanceOverlay,0,true},{Setting::PerformanceOverlay,1,true},{Setting::PerformanceOverlay,2,false},
        {Setting::RenderOnCpu,0,true},{Setting::RenderOnCpu,1,true},{Setting::RenderOnCpu,2,false},
    };
    for(const auto& c:cases)CHECK(as::valid(c.s,c.v)==c.ok);
    CHECK(!as::valid(Setting::Count,0));
    // The environment names: AMDGPU_WDDM_ and the value name in upper snake case (VSync as one word).
    CHECK(!std::strcmp(as::descriptor(Setting::FrameRateLimit).env,"AMDGPU_WDDM_FRAME_RATE_LIMIT"));
    CHECK(!std::strcmp(as::descriptor(Setting::VSync).env,"AMDGPU_WDDM_VSYNC"));
    CHECK(!std::strcmp(as::descriptor(Setting::Anisotropy).env,"AMDGPU_WDDM_ANISOTROPY"));
    CHECK(!std::strcmp(as::descriptor(Setting::MaxFrameLatency).env,"AMDGPU_WDDM_MAX_FRAME_LATENCY"));
    CHECK(!std::strcmp(as::descriptor(Setting::PerformanceOverlay).env,"AMDGPU_WDDM_PERFORMANCE_OVERLAY"));
    CHECK(!std::strcmp(as::descriptor(Setting::RenderOnCpu).env,"AMDGPU_WDDM_RENDER_ON_CPU"));
    CHECK(!std::wcscmp(as::descriptor(Setting::FrameRateLimit).value,L"FrameRateLimit"));
    CHECK(!std::wcscmp(as::descriptor(Setting::RenderOnCpu).value,L"RenderOnCpu"));
}

void precedence_tests() {
    {   // Nothing anywhere: every setting unset, the application decides.
        Fake f;const auto s=run(f);
        for(unsigned i=0;i<as::kCount;++i)CHECK(!s.values[i].set());
        CHECK(s.rejection_count==0);
        CHECK(!std::wcscmp(s.application,L"Game.EXE"));
        CHECK(!as::sync_override(s).valid && as::frame_rate_limit(s)==0 && as::max_frame_latency(s)==0);
        CHECK(!as::render_on_cpu(s) && as::dxvk_config(s).empty() && as::vkd3d_anisotropy(s).empty());
    }
    {   // Global only.
        Fake f;f.dword(L"",L"FrameRateLimit",60);f.dword(L"",L"VSync",0);
        const auto s=run(f);
        CHECK(s[Setting::FrameRateLimit].value==60 && s[Setting::FrameRateLimit].source==Source::Global);
        CHECK(s[Setting::VSync].value==0 && s[Setting::VSync].source==Source::Global);
        CHECK(as::sync_override(s).valid && as::sync_override(s).interval==0);
    }
    {   // The application key wins over the global key; the key name is matched without case.
        Fake f;f.dword(L"",L"FrameRateLimit",60);f.dword(L"game.exe",L"FrameRateLimit",144);
        f.dword(L"",L"Anisotropy",4);
        const auto s=run(f);
        CHECK(s[Setting::FrameRateLimit].value==144 && s[Setting::FrameRateLimit].source==Source::Application);
        CHECK(s[Setting::Anisotropy].value==4 && s[Setting::Anisotropy].source==Source::Global);
    }
    {   // The environment wins over both keys.
        Fake f;f.dword(L"",L"FrameRateLimit",60);f.dword(L"game.exe",L"FrameRateLimit",144);
        f.env["AMDGPU_WDDM_FRAME_RATE_LIMIT"]="30";
        const auto s=run(f);
        CHECK(s[Setting::FrameRateLimit].value==30 && s[Setting::FrameRateLimit].source==Source::Environment);
        CHECK(s.rejection_count==0);
    }
    {   // 0 is a value: an application 0 turns a global limit and a global CPU route off for that application.
        Fake f;f.dword(L"",L"FrameRateLimit",60);f.dword(L"game.exe",L"FrameRateLimit",0);
        f.dword(L"",L"RenderOnCpu",1);f.dword(L"game.exe",L"RenderOnCpu",0);
        const auto s=run(f);
        CHECK(s[Setting::FrameRateLimit].set() && as::frame_rate_limit(s)==0);
        CHECK(s[Setting::RenderOnCpu].source==Source::Application && !as::render_on_cpu(s));
    }
    {   // An ignored value is as if absent: the next source is read, and one rejection is recorded per ignored value.
        Fake f;f.env["AMDGPU_WDDM_FRAME_RATE_LIMIT"]="5";f.dword(L"game.exe",L"FrameRateLimit",400);
        f.dword(L"",L"FrameRateLimit",75);
        f.env["AMDGPU_WDDM_ANISOTROPY"]="sixteen";f.wrong(L"game.exe",L"Anisotropy");f.dword(L"",L"Anisotropy",3);
        f.dword(L"game.exe",L"MaxFrameLatency",0);
        const auto s=run(f);
        CHECK(s[Setting::FrameRateLimit].value==75 && s[Setting::FrameRateLimit].source==Source::Global);
        CHECK(!s[Setting::Anisotropy].set());
        CHECK(!s[Setting::MaxFrameLatency].set());
        CHECK(s.rejection_count==6);
        if(s.rejection_count==6){
            CHECK(s.rejections[0].setting==Setting::FrameRateLimit && s.rejections[0].source==Source::Environment &&
                  s.rejections[0].problem==as::Problem::OutOfRange && s.rejections[0].value==5);
            CHECK(s.rejections[1].source==Source::Application && s.rejections[1].value==400);
            CHECK(s.rejections[2].setting==Setting::Anisotropy && s.rejections[2].problem==as::Problem::NotNumber &&
                  !std::strcmp(s.rejections[2].text,"sixteen"));
            CHECK(s.rejections[3].problem==as::Problem::WrongType && s.rejections[3].source==Source::Application);
            CHECK(s.rejections[4].problem==as::Problem::OutOfRange && s.rejections[4].source==Source::Global &&
                  s.rejections[4].value==3);
            CHECK(s.rejections[5].setting==Setting::MaxFrameLatency && s.rejections[5].value==0);
            const std::string r0=rejection(s,0),r2=rejection(s,2),r3=rejection(s,3),r4=rejection(s,4);
            CHECK(has(r0,"ignored FrameRateLimit=5 from the environment (accepted: 0, 20-300)"));
            CHECK(has(r2,"ignored AMDGPU_WDDM_ANISOTROPY=\"sixteen\" from the environment: not a decimal number"));
            CHECK(has(r3,"ignored Anisotropy from the application key: not a REG_DWORD"));
            CHECK(has(r4,"ignored Anisotropy=3 from the global key (accepted: 1, 2, 4, 8, 16)"));
        }
    }
    {   // No image name: the application key is never read.
        Fake f;f.dword(L"game.exe",L"VSync",1);f.dword(L"",L"VSync",0);
        const auto s=run(f,L"");
        CHECK(s[Setting::VSync].source==Source::Global && s[Setting::VSync].value==0);
        bool app=false;for(auto& a:f.asked)if(a.rfind(L"|",0)!=0)app=true;
        CHECK(!app);
        CHECK(!s.application[0]);
        const auto t=run(f,nullptr);
        CHECK(t[Setting::VSync].source==Source::Global);
    }
    {   // Every setting reads exactly its own value name, the application key before the global one.
        Fake f;run(f);
        CHECK(f.asked.size()==as::kCount*2);
        if(f.asked.size()==as::kCount*2){
            CHECK(f.asked[0]==L"game.exe|FrameRateLimit" && f.asked[1]==L"|FrameRateLimit");
            CHECK(f.asked[10]==L"game.exe|RenderOnCpu" && f.asked[11]==L"|RenderOnCpu");
        }
    }
}

void output_tests() {
    Fake f;f.dword(L"",L"FrameRateLimit",60);f.dword(L"game.exe",L"Anisotropy",16);
    f.env["AMDGPU_WDDM_PERFORMANCE_OVERLAY"]="1";f.dword(L"",L"MaxFrameLatency",2);f.dword(L"",L"VSync",1);
    const auto s=run(f);
    const std::string all=effective(s);
    CHECK(has(all,"amdgpu-wddm settings: api=d3d11 app=Game.EXE FrameRateLimit=60/global VSync=1/global "
                  "Anisotropy=16/application MaxFrameLatency=2/global PerformanceOverlay=1/environment RenderOnCpu=unset"));
    // A shell marks what it does not apply (the D3D12 shell: no overlay, and RenderOnCpu is the router's).
    const unsigned d3d12=as::kAll & ~as::bit(Setting::PerformanceOverlay) & ~as::bit(Setting::RenderOnCpu);
    const std::string part=effective(s,d3d12);
    CHECK(has(part,"MaxFrameLatency=2/global ") && has(part,"PerformanceOverlay=1/environment/not-applied"));
    CHECK(has(part,"Anisotropy=16/application ") && has(part,"RenderOnCpu=unset"));
    // A shell that applies neither still marks both, so the line never claims a setting took effect.
    const std::string none=effective(s,d3d12 & ~as::bit(Setting::MaxFrameLatency));
    CHECK(has(none,"MaxFrameLatency=2/global/not-applied"));
    CHECK(as::sync_override(s).valid && as::sync_override(s).interval==1);
    // BD-099: on the old present callback the D3D11 shell waits only for the part of VSync that the
    // application's interval does not give.
    CHECK(as::extra_vblanks(as::sync_override(s),0,false)==1 && as::extra_vblanks(as::sync_override(s),1,false)==0);
    CHECK(as::extra_vblanks(as::sync_override(s),2,false)==0 && as::extra_vblanks(as::SyncOverride{false,0u},0,false)==0);
    CHECK(as::extra_vblanks(as::SyncOverride{true,0u},0,false)==0 && as::extra_vblanks(as::SyncOverride{true,0u},1,false)==0);
    // BD-099: on the old present callback the shell passes VSync 0 to the runtime, and VSync 1 or no VSync not
    // at all.
    CHECK(!as::d3d11_runtime_override(as::sync_override(s),false).valid);
    CHECK(!as::d3d11_runtime_override(as::SyncOverride{false,0u},false).valid);
    CHECK(as::d3d11_runtime_override(as::SyncOverride{true,0u},false).valid &&
          as::d3d11_runtime_override(as::SyncOverride{true,0u},false).interval==0);
    // BD-099: on the whole present callback (the WDDM 2.2 interface) every override goes to the runtime, and the
    // shell waits for no vertical blank of its own.
    CHECK(as::d3d11_runtime_override(as::sync_override(s),true).valid &&
          as::d3d11_runtime_override(as::sync_override(s),true).interval==1);
    CHECK(as::d3d11_runtime_override(as::SyncOverride{true,0u},true).valid &&
          !as::d3d11_runtime_override(as::SyncOverride{true,0u},true).interval);
    CHECK(!as::d3d11_runtime_override(as::SyncOverride{false,0u},true).valid);
    CHECK(!as::extra_vblanks(as::sync_override(s),0,true) && !as::extra_vblanks(as::sync_override(s),1,true));
    CHECK(!as::extra_vblanks(as::SyncOverride{true,2u},0,true));
    CHECK(as::frame_rate_limit(s)==60 && as::max_frame_latency(s)==2);
    CHECK(as::dxvk_config(s)=="d3d11.samplerAnisotropy = 16;dxvk.hud = fps,frametimes,gpuload,api");
    CHECK(as::vkd3d_anisotropy(s)=="16");
    {   // Overlay 0 adds no HUD option; anisotropy alone has no separator.
        Fake g;g.dword(L"",L"PerformanceOverlay",0);g.dword(L"",L"Anisotropy",1);
        const auto t=run(g);
        CHECK(as::dxvk_config(t)=="d3d11.samplerAnisotropy = 1");
        Fake h;h.dword(L"",L"PerformanceOverlay",1);
        CHECK(as::dxvk_config(run(h))=="dxvk.hud = fps,frametimes,gpuload,api");
    }
    {   // The line of a process without an image name, and of a non-ASCII image name (UTF-8).
        Fake g;const auto t=run(g,L"");
        CHECK(has(effective(t),"app=- FrameRateLimit=unset"));
        const wchar_t polish[]={L'W',L'i',L'e',L'd',wchar_t(0x017A),L'm',L'i',L'n',L'.',L'e',L'x',L'e',0};
        const auto u=run(g,polish);   // a Polish letter: the line is UTF-8
        const unsigned char bytes[]={'a','p','p','=','W','i','e','d',0xC5,0xBA,'m','i','n','.','e','x','e',' ',0};
        const char* utf8=reinterpret_cast<const char*>(bytes);
        CHECK(has(effective(u),utf8));
    }
    {   // log_once: the rejections first, then the effective line, and only on the first call. No profile: the lab
        // flavour's default, this process's profile, adds the line of the engine that is not there.
        Fake g;g.dword(L"",L"VSync",7);
        const auto t=run(g);
        std::vector<std::string> lines;
        as::log_once(t,"d3d12",as::kAll,[&](const char* l){lines.emplace_back(l);},nullptr);
        as::log_once(t,"d3d12",as::kAll,[&](const char* l){lines.emplace_back(l);},nullptr);
        CHECK(lines.size()==2);
        if(lines.size()==2)CHECK(has(lines[0],"ignored VSync=7 from the global key") && has(lines[1],"api=d3d12"));
    }
}

void limiter_tests() {
    uint64_t last=0;
    CHECK(as::limiter_target(1000,0,last)==0 && last==0);           // no limit: nothing kept
    CHECK(as::limiter_target(1000,100,last)==0 && last==1000);      // the first frame starts the clock
    CHECK(as::limiter_target(1030,100,last)==1100 && last==1100);   // early: wait to one interval after the last
    CHECK(as::limiter_target(1100,100,last)==1200 && last==1200);   // still early after that wait
    CHECK(as::limiter_target(1350,100,last)==0 && last==1300);      // late by less than an interval: keep the beat
    CHECK(as::limiter_target(1800,100,last)==0 && last==1800);      // later than an interval: restart at now
    CHECK(as::limiter_target(1850,100,last)==1900 && last==1900);
    CHECK(as::limiter_target(10,100,last)==0 && last==10);          // a clock that went back restarts
    // The real wait: 30 frames at 100 per second take at least 0.28 s (29 intervals after the first frame). The number
    // of waits depends on the scheduler: a thread that wakes more than one interval late restarts the clock
    // (limiter_target), and on a loaded host every second frame does that (15 waits in each of 40 runs with 28 busy
    // threads on 24 logical processors, 2026-10-11; that failed a release rebuild). So the count only shows that the
    // timer path ran, and the upper bound only catches a wait that does not end.
    as::FrameLimiter limiter;
    LARGE_INTEGER f{},a{},b{};QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
    for(int i=0;i<30;++i)limiter.frame(100);
    QueryPerformanceCounter(&b);
    const double seconds=double(b.QuadPart-a.QuadPart)/double(f.QuadPart);
    std::printf("limiter: 30 frames at 100/s in %.3f s, %llu waits\n",seconds,
        static_cast<unsigned long long>(limiter.waits()));
    CHECK(seconds>=0.28 && seconds<5.0);
    CHECK(limiter.frames()==30 && limiter.waits()>=1);
    as::FrameLimiter off;off.frame(0);CHECK(off.frames()==0);
}

void env_tests() {
    const char* name="AMDGPU_WDDM_APP_SETTINGS_TEST";
    char text[2048];
    SetEnvironmentVariableA(name,nullptr);
    {
        as::ScopedEnv e(name,"a = 1",as::ScopedEnv::Mode::Append);
        CHECK(e.changed() && GetEnvironmentVariableA(name,text,sizeof(text)) && !std::strcmp(text,"a = 1"));
    }
    CHECK(!GetEnvironmentVariableA(name,text,sizeof(text)) && GetLastError()==ERROR_ENVVAR_NOT_FOUND);
    SetEnvironmentVariableA(name,"user = 2");
    {
        as::ScopedEnv e(name,"a = 1",as::ScopedEnv::Mode::Append);
        CHECK(GetEnvironmentVariableA(name,text,sizeof(text)) && !std::strcmp(text,"user = 2;a = 1"));
    }
    CHECK(GetEnvironmentVariableA(name,text,sizeof(text)) && !std::strcmp(text,"user = 2"));
    {
        as::ScopedEnv e(name,"16",as::ScopedEnv::Mode::Replace);
        CHECK(GetEnvironmentVariableA(name,text,sizeof(text)) && !std::strcmp(text,"16"));
    }
    CHECK(GetEnvironmentVariableA(name,text,sizeof(text)) && !std::strcmp(text,"user = 2"));
    {   // An empty value changes nothing.
        as::ScopedEnv e(name,"",as::ScopedEnv::Mode::Replace);
        CHECK(!e.changed());
        CHECK(GetEnvironmentVariableA(name,text,sizeof(text)) && !std::strcmp(text,"user = 2"));
    }
    {   // A previous value longer than MAX_PATH comes back unchanged; the combination would be too long for DXVK,
        // so only the settings are passed while the object lives.
        const std::string longer(1500,'x');
        SetEnvironmentVariableA(name,longer.c_str());
        {
            as::ScopedEnv e(name,"a = 1",as::ScopedEnv::Mode::Append);
            CHECK(GetEnvironmentVariableA(name,text,sizeof(text)) && !std::strcmp(text,"a = 1"));
        }
        CHECK(GetEnvironmentVariableA(name,text,sizeof(text))==1500 && longer==text);
    }
    SetEnvironmentVariableA(name,nullptr);
    {   // The system environment reader: unset, set, too long.
        char value[64];
        CHECK(!as::system_environment(nullptr,name,value,sizeof(value)));
        SetEnvironmentVariableA(name,"60");
        CHECK(as::system_environment(nullptr,name,value,sizeof(value)) && !std::strcmp(value,"60"));
        SetEnvironmentVariableA(name,std::string(100,'7').c_str());
        CHECK(as::system_environment(nullptr,name,value,sizeof(value)) && !std::strcmp(value,"(too long)"));
        uint32_t v=0;CHECK(!as::parse_decimal(value,&v));
        SetEnvironmentVariableA(name,nullptr);
    }
}

// The system reader on a private hive in place of HKEY_LOCAL_MACHINE: REG_DWORD, other types, the application key
// and its case, the 64-bit view flag, and the whole resolve with system_sources().
void registry_tests() {
    wchar_t cwd[MAX_PATH];GetCurrentDirectoryW(MAX_PATH,cwd);
    const std::wstring hive=std::wstring(cwd)+L"\\app-settings-test.hive";
    DeleteFileW(hive.c_str());
    HKEY root=nullptr;
    LSTATUS st=RegLoadAppKeyW(hive.c_str(),&root,KEY_ALL_ACCESS,0,0);
    CHECK(st==ERROR_SUCCESS);
    if(st!=ERROR_SUCCESS)return;
    auto key=[&](const wchar_t* path){HKEY k=nullptr;RegCreateKeyExW(root,path,0,nullptr,0,KEY_ALL_ACCESS,nullptr,&k,nullptr);return k;};
    HKEY global=key(L"SOFTWARE\\amdgpu-wddm\\Graphics");
    HKEY app=key(L"SOFTWARE\\amdgpu-wddm\\Graphics\\Applications\\settings-probe.exe");
    const DWORD sixty=60,one=1,big=999;
    RegSetValueExW(global,L"FrameRateLimit",0,REG_DWORD,reinterpret_cast<const BYTE*>(&sixty),sizeof(sixty));
    RegSetValueExW(global,L"MaxFrameLatency",0,REG_DWORD,reinterpret_cast<const BYTE*>(&big),sizeof(big));
    RegSetValueExW(global,L"VSync",0,REG_SZ,reinterpret_cast<const BYTE*>(L"1"),4);
    const unsigned long long qword=1;
    RegSetValueExW(global,L"Anisotropy",0,REG_QWORD,reinterpret_cast<const BYTE*>(&qword),sizeof(qword));
    RegSetValueExW(app,L"PerformanceOverlay",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one));
    RegSetValueExW(app,L"RenderOnCpu",0,REG_DWORD,reinterpret_cast<const BYTE*>(&one),sizeof(one));
    RegCloseKey(global);RegCloseKey(app);
    st=RegOverridePredefKey(HKEY_LOCAL_MACHINE,root);
    CHECK(st==ERROR_SUCCESS);
    uint32_t v=0;
    CHECK(as::system_registry(nullptr,nullptr,L"FrameRateLimit",&v)==as::Read::Value && v==60);
    CHECK(as::system_registry(nullptr,nullptr,L"VSync",&v)==as::Read::WrongType);
    CHECK(as::system_registry(nullptr,nullptr,L"Anisotropy",&v)==as::Read::WrongType);
    CHECK(as::system_registry(nullptr,nullptr,L"Absent",&v)==as::Read::Absent);
    CHECK(as::system_registry(nullptr,L"SETTINGS-PROBE.EXE",L"RenderOnCpu",&v)==as::Read::Value && v==1);
    CHECK(as::system_registry(nullptr,L"other.exe",L"RenderOnCpu",&v)==as::Read::Absent);
    CHECK(as::system_registry(nullptr,L"..\\settings-probe.exe",L"RenderOnCpu",&v)==as::Read::Absent);
    SetEnvironmentVariableA("AMDGPU_WDDM_VSYNC","0");
    as::Settings s;as::resolve(L"Settings-Probe.exe",as::system_sources(),s);
    SetEnvironmentVariableA("AMDGPU_WDDM_VSYNC",nullptr);
    CHECK(s[Setting::FrameRateLimit].value==60 && s[Setting::FrameRateLimit].source==Source::Global);
    CHECK(s[Setting::VSync].value==0 && s[Setting::VSync].source==Source::Environment);
    CHECK(!s[Setting::Anisotropy].set() && !s[Setting::MaxFrameLatency].set());
    CHECK(s[Setting::PerformanceOverlay].source==Source::Application && as::render_on_cpu(s));
    CHECK(s.rejection_count==2);   // MaxFrameLatency 999 and the QWORD Anisotropy; VSync came from the environment
    RegOverridePredefKey(HKEY_LOCAL_MACHINE,nullptr);
    RegCloseKey(root);
    DeleteFileW(hive.c_str());
}

void image_name_test() {
    wchar_t name[MAX_PATH];as::image_name(name,MAX_PATH);
    CHECK(!_wcsicmp(name,L"app-settings-test.exe"));
    wchar_t tiny[4];as::image_name(tiny,4);
    CHECK(std::wcslen(tiny)==3);
}
// A stand-in for the engine's Bc250DxvkEngineGetAppOptions: the lines of g_engine_text, with the engine's buffer
// rules (bc250_dxvk_engine.h r10).
std::string g_engine_text;
HRESULT APIENTRY fake_get_app_options(UINT32 abi,char* text,UINT32 size,UINT32* length) {
    if(!length || (!text && size))return E_INVALIDARG;
    if((abi>>16)!=1)return E_NOINTERFACE;
    *length=static_cast<UINT32>(g_engine_text.size());
    if(g_engine_text.size()>=size){
        if(size)text[0]=0;   // whole lines only; the reader asks again with the length
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    std::memcpy(text,g_engine_text.c_str(),g_engine_text.size()+1);
    return S_OK;
}
namespace dp=as::dxvk_profile;
as::Settings run_profile(Fake& f,const dp::Profile& p,const wchar_t* app=L"Game.EXE") {
    as::Settings s;
    as::Sources from{&f,fake_registry,fake_environment,nullptr};
    // The registry double and the profile share one context in resolve: wrap both.
    struct Both {Fake* fake;const dp::Profile* profile;};
    static Both both;both={&f,&p};
    from.context=&both;
    from.registry=[](void* c,const wchar_t* a,const wchar_t* v,uint32_t* o) noexcept {
        return fake_registry(static_cast<Both*>(c)->fake,a,v,o);};
    from.environment=[](void* c,const char* n,char* t,size_t z) noexcept {
        return fake_environment(static_cast<Both*>(c)->fake,n,t,z);};
    from.profile=[](void* c,Setting s,uint32_t* o) noexcept {
        return dp::source(const_cast<dp::Profile*>(static_cast<Both*>(c)->profile),s,o);};
    as::resolve(app,from,s);
    return s;
}
dp::Profile profile_of(const char* text) {g_engine_text=text;return dp::from_engine(fake_get_app_options);}

void profile_tests() {
    {   // The lines of the engine, in its order; a line without a name is skipped.
        const auto o=dp::parse("dxgi.a=1\n=x\ndxgi.b=two words\nno-equals\ndxgi.c=");
        CHECK(o.size()==3 && o[0].name=="dxgi.a" && o[0].value=="1" && o[1].value=="two words" && o[2].value.empty());
        CHECK(dp::parse("").empty() && dp::parse(nullptr).empty());
    }
    {   // DXVK's int32 rule.
        int64_t v=0;
        CHECK(dp::parse_int("-60",&v) && v==-60);
        CHECK(dp::parse_int("1",&v) && v==1);
        CHECK(!dp::parse_int("",&v) && !dp::parse_int("-",&v) && !dp::parse_int("1.5",&v) && !dp::parse_int("0x10",&v));
        CHECK(!dp::parse_int("99999999999",&v));
    }
    {   // The DXVK profiles of tester.33's DXVK: P3R (syncInterval 1), ShantaeCurse (maxFrameRate -60), A Way Out
        // (maxFrameLatency 1). A soft limit -n is a cap of n, -1 is no limiter.
        auto p=profile_of("dxgi.syncInterval=1\n");
        CHECK(p.read && p.has[unsigned(Setting::VSync)] && p.value[unsigned(Setting::VSync)]==1);
        p=profile_of("dxgi.maxFrameRate=-60\n");
        CHECK(p.has[unsigned(Setting::FrameRateLimit)] && p.value[unsigned(Setting::FrameRateLimit)]==60);
        p=profile_of("dxgi.maxFrameRate=-1\n");
        CHECK(p.has[unsigned(Setting::FrameRateLimit)] && p.value[unsigned(Setting::FrameRateLimit)]==0);
        p=profile_of("dxgi.maxFrameRate=0\ndxgi.maxFrameLatency=0\ndxgi.syncInterval=-1\n");
        for(unsigned i=0;i<as::kCount;++i)CHECK(!p.has[i]);
        p=profile_of("dxgi.maxFrameLatency=1\n");
        CHECK(p.has[unsigned(Setting::MaxFrameLatency)] && p.value[unsigned(Setting::MaxFrameLatency)]==1);
        // dxvk.maxFrameRate wins over dxgi.maxFrameRate, as in DxgiOptions.
        p=profile_of("dxgi.maxFrameRate=30\ndxvk.maxFrameRate=90\n");
        CHECK(p.value[unsigned(Setting::FrameRateLimit)]==90 && !std::strcmp(p.option[unsigned(Setting::FrameRateLimit)],"dxvk.maxFrameRate"));
        CHECK(dp::not_applied_reason(p,"dxgi.maxFrameRate")!=nullptr && !dp::not_applied_reason(p,"dxvk.maxFrameRate"));
        // A long answer: the reader asks again with the length the engine gives.
        std::string many;
        for(int i=0;i<400;++i)many+="dxgi.unknownOption"+std::to_string(i)+"=1\n";
        many+="dxgi.syncInterval=0\n";
        p=profile_of(many.c_str());
        CHECK(p.read && p.options.size()==401 && p.has[unsigned(Setting::VSync)] && p.value[unsigned(Setting::VSync)]==0);
        // No export (an engine before ABI 1.5): no profile.
        p=dp::from_engine(nullptr);
        CHECK(!p.read && p.result==E_NOINTERFACE);
    }
    {   // The profile is the lowest source: the global key and the application key win over it.
        Fake f;
        const auto p=profile_of("dxgi.syncInterval=1\ndxgi.maxFrameRate=-60\ndxgi.maxFrameLatency=1\n");
        auto s=run_profile(f,p);
        CHECK(s[Setting::VSync].value==1 && s[Setting::VSync].source==Source::Profile);
        CHECK(s[Setting::FrameRateLimit].value==60 && s[Setting::FrameRateLimit].source==Source::Profile);
        CHECK(s[Setting::MaxFrameLatency].value==1 && s[Setting::MaxFrameLatency].source==Source::Profile);
        CHECK(!s[Setting::Anisotropy].set());
        CHECK(has(effective(s),"VSync=1/dxvk-profile") && has(effective(s),"FrameRateLimit=60/dxvk-profile"));
        f.dword(L"",L"VSync",0);f.dword(L"game.exe",L"FrameRateLimit",144);
        s=run_profile(f,p);
        CHECK(s[Setting::VSync].value==0 && s[Setting::VSync].source==Source::Global);
        CHECK(s[Setting::FrameRateLimit].value==144 && s[Setting::FrameRateLimit].source==Source::Application);
        CHECK(s[Setting::MaxFrameLatency].source==Source::Profile);
    }
    {   // A value out of the setting's range is ignored with a log line that names the DXVK profile.
        Fake f;
        const auto p=profile_of("dxgi.syncInterval=2\ndxgi.maxFrameRate=-500\ndxgi.maxFrameLatency=8\n");
        const auto s=run_profile(f,p);
        CHECK(!s[Setting::VSync].set() && !s[Setting::FrameRateLimit].set() && !s[Setting::MaxFrameLatency].set());
        CHECK(s.rejection_count==3);
        CHECK(has(rejection(s,0),"ignored FrameRateLimit=500 from the DXVK profile (accepted: 0, 20-300)"));
        CHECK(has(rejection(s,1),"ignored VSync=2 from the DXVK profile"));
        CHECK(has(rejection(s,2),"ignored MaxFrameLatency=8 from the DXVK profile"));
    }
    {   // The options that do not apply on this route, each with its reason, in the lab log.
        const auto p=profile_of("dxgi.customVendorId=10de\ndxgi.enableDummyCompositionSwapchain=True\n"
            "dxgi.maxDeviceMemory=4095\ndxgi.someFutureOption=1\ndxgi.syncInterval=1\n");
        std::vector<std::string> lines;
        dp::log_not_applied(p,[&](const char* l){lines.emplace_back(l);});
        CHECK(lines.size()==4);
        CHECK(lines.size()==4 && has(lines[0],"dxgi.customVendorId=10de not applied: the system DXGI reports the adapter identity"));
        CHECK(lines.size()==4 && has(lines[1],"dxgi.enableDummyCompositionSwapchain=True not applied: composition swap chains"));
        CHECK(lines.size()==4 && has(lines[2],"dxgi.maxDeviceMemory=4095 not applied: the system DXGI reports the memory sizes"));
        CHECK(lines.size()==4 && has(lines[3],"dxgi.someFutureOption=1 not applied: no mechanism on this route"));
        lines.clear();
        dp::log_not_applied(dp::from_engine(nullptr),[&](const char* l){lines.emplace_back(l);});
        CHECK(lines.size()==1 && has(lines[0],"no DXVK profile options"));
    }
    {   // log_once adds the not-applied lines after the effective line, for the profile it is given, in the lab
        // flavour only. test-umd-app-settings.ps1 builds and runs this test in both flavours.
        as::logged().store(false);
        Fake f;const auto p=profile_of("dxgi.hideNvidiaGpu=False\n");
        const auto s=run_profile(f,p);
        std::vector<std::string> lines;
        as::log_once(s,"d3d12",as::kAll,[&](const char* l){lines.emplace_back(l);},&p);
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        CHECK(lines.size()==2 && has(lines[0],"api=d3d12") && has(lines[1],"dxgi.hideNvidiaGpu=False not applied"));
        CHECK(as::lab_log_profile()!=nullptr);
#else
        CHECK(lines.size()==1 && has(lines[0],"api=d3d12"));
        CHECK(as::lab_log_profile()==nullptr);
#endif
        as::logged().store(false);
    }
    {   // The engine path: the D3D11 directory next to this module's directory.
        const std::wstring path=dp::engine_path();
        CHECK(path.size()>std::wcslen(AMDGPU_DXVK_FILE) && path.find(L"\\d3d11\\")!=std::wstring::npos &&
              path.find(L"..")==std::wstring::npos && path.compare(path.size()-std::wcslen(AMDGPU_DXVK_FILE),
              std::wstring::npos,AMDGPU_DXVK_FILE)==0);
        // No engine at that path: no profile, and the reason is the load error.
        const auto p=dp::from_path(L"P:\\no-such-directory-for-the-dxvk-profile-test\\amdgpu_wddm_dxvk.dll");
        CHECK(!p.read && FAILED(p.result));
    }
}
} // namespace

int main() {
    // Hermetic: no setting of the person running the gate leaks in.
    for(unsigned i=0;i<as::kCount;++i)SetEnvironmentVariableA(as::descriptor(static_cast<Setting>(i)).env,nullptr);
    parse_tests();
    range_tests();
    precedence_tests();
    output_tests();
    limiter_tests();
    env_tests();
    registry_tests();
    image_name_test();
    profile_tests();
    std::printf("app-settings-test: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
