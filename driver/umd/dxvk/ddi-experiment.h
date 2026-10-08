// SPDX-License-Identifier: MIT
// The experiment switches of the D3D11 shell.
//
// Shape and parser are the D3D12 shell's (driver/umd/d3d12/ddi-trace.h): a comma-separated list of lower-case
// names, read once per process. The first source that exists wins:
//   1. AMDGPU_WDDM_D3D11_EXPERIMENT in the process environment, "none" and empty included,
//   2. the REG_SZ value "Experiment" of HKLM\SOFTWARE\amdgpu-wddm\D3D11\Applications\<image file name>,
//   3. the REG_SZ value "Experiment" of HKLM\SOFTWARE\amdgpu-wddm\D3D11.
// A value outside the syntax, or one too long, names no switch. Only the variable name and the registry path
// differ from the D3D12 shell, so the syntax check and the registry read come from there.
//
// The names:
//   d3d11-wddm20-ddi  the shell offers the WDDM 2.0 interface as its newest one and does not offer WDDM 2.2
//                     (BD-099). Then the runtime gives the short Present callback again, and VSync 1 is paced
//                     by vertical-blank waits (vblank-pacer.h).
#pragma once
#include "../d3d12/ddi-trace.h"
namespace bc250::umd {
namespace experiment_detail {
// The profile key of an image path: the file name after the last separator, under the D3D11 Applications key.
inline bool profile_key(const wchar_t *image,wchar_t *key,size_t capacity) noexcept {
    static constexpr wchar_t prefix[]=L"SOFTWARE\\amdgpu-wddm\\D3D11\\Applications\\";
    const wchar_t *name=image;
    for (const wchar_t *p=image;*p;++p) if (*p==L'\\' || *p==L'/') name=p+1;
    const size_t prefixLength=sizeof(prefix)/sizeof(wchar_t)-1,nameLength=std::wcslen(name);
    if (!nameLength || prefixLength+nameLength+1>capacity) return false;
    std::wmemcpy(key,prefix,prefixLength);
    std::wmemcpy(key+prefixLength,name,nameLength+1);
    return true;
}
inline int image_profile(char *text,size_t capacity) noexcept {
    wchar_t image[MAX_PATH]{};
    const DWORD n=GetModuleFileNameW(nullptr,image,MAX_PATH);
    wchar_t key[96+MAX_PATH]{};
    if (!n || n>=MAX_PATH || !profile_key(image,key,sizeof(key)/sizeof(wchar_t))) return 0;
    return native12::ddi_detail::registry_experiment(key,text,capacity);
}
struct Value { char text[256]{}; };
inline Value resolve() noexcept {
    Value v{};
    SetLastError(ERROR_SUCCESS);
    const DWORD n=GetEnvironmentVariableA("AMDGPU_WDDM_D3D11_EXPERIMENT",v.text,sizeof(v.text));
    bool valid=true;
    if (n || GetLastError()!=ERROR_ENVVAR_NOT_FOUND) {
        valid=n<sizeof(v.text);
    } else {
        int found=image_profile(v.text,sizeof(v.text));
        if (!found) found=native12::ddi_detail::registry_experiment(L"SOFTWARE\\amdgpu-wddm\\D3D11",v.text,sizeof(v.text));
        valid=found>=0;
    }
    if (!valid || !native12::ddi_detail::experiment_syntax(v.text)) v.text[0]=0;
    if (!std::strcmp(v.text,"none")) v.text[0]=0;
    return v;
}
}
// The list as given, for the log: lower-case letters, digits, hyphens and commas only, else empty.
inline const char *d3d11_experiment_name() noexcept {
    static const experiment_detail::Value value=experiment_detail::resolve();
    return value.text;
}
// Whether the list names the switch exactly.
inline bool d3d11_experiment(const char *name) noexcept {
    return native12::ddi_experiment_listed(d3d11_experiment_name(),name);
}
// BD-099: the newest D3D11 DDI interface this process offers. The WDDM 2.2 interface is the one whose Present
// callback carries the whole DXGIDDICB_PRESENT, so it is the default. The switch takes the shell back to the
// WDDM 2.0 interface for a comparison or for a trial that must exclude the newer table.
inline bool wddm2_2_offered() noexcept { return !d3d11_experiment("d3d11-wddm20-ddi"); }
}
