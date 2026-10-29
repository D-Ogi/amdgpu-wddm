// SPDX-License-Identifier: MIT
// The D3D12 shell's half of the FSR 4 identity answer (docs/design/fsr4-identity.md): once per process, after the
// first device of the process exists, the shell sends the resolved FsrUpgrade setting (app-settings-core.h) and the
// image's file name to the KMD in one escape (BC250_ESCAPE_SET_PROCESS_POLICY). The KMD keeps the value in its
// process object and answers AMD's 608-byte identity query with it (driver/kmd/amd_identity.c).
//
// The KMD reads no registry value for this: the setting is the control application's, and the shell resolves it like
// every other per-game setting. A setting that is not set sends nothing, so the KMD's default applies (ON, except for
// the games in its anti-cheat list, which the shell's value cannot change). The escape needs a dxgkrnl process
// object, which exists from the process's first device on (DXGKARG_ESCAPE.hKmdProcessHandle), so the shell sends it
// after the device engine was created. A failure is logged and is never an error of CreateDevice.
#pragma once
#include <windows.h>
#include <d3dkmthk.h>
#include <cstring>
#include "../../kmd/bc250kmd_escape.h"
#include "../../kmd/amd_identity_policy.h"

namespace native12 {
struct FsrIdentityKmt {
    PFND3DKMT_OPENADAPTERFROMLUID open{};
    PFND3DKMT_ESCAPE escape{};
    PFND3DKMT_CLOSEADAPTER close{};
};

// What the send did, for the log line.
struct FsrIdentityResult {
    HRESULT hr=S_OK;                // S_FALSE: nothing to send (the setting is not set)
    NTSTATUS open=0,escape=0,close=0;
    unsigned long status=0xFFFFFFFFul;  // the escape's Status (BC250_ESCAPE_STATUS_*), all ones when not sent
    unsigned long answer=0,source=0,name_flags=0;
};

// The escape for value (0 or 1) and the image file name (truncated to fit, always zero-terminated).
inline BC250_ESCAPE_PROCESS_POLICY fsr_identity_request(unsigned value,const wchar_t* image) noexcept {
    BC250_ESCAPE_PROCESS_POLICY p{};
    p.Magic=BC250_ESCAPE_MAGIC;p.Command=BC250_ESCAPE_SET_PROCESS_POLICY;
    p.AbiVersion=BC250_PROCESS_POLICY_ABI;p.FsrIdentity=value;
    static_assert(sizeof(wchar_t)==sizeof(p.Image[0]),"UTF-16 image name");
    for(unsigned i=0;image && image[i] && i+1<BC250_PROCESS_POLICY_IMAGE_CHARS;++i)
        p.Image[i]=static_cast<unsigned short>(image[i]);
    return p;
}

// set: whether the setting has a value; value: 0 or 1. Opens the adapter by LUID, sends the escape, closes it.
// No device, context or allocation is created here.
inline FsrIdentityResult send_fsr_identity(const LUID& luid,const FsrIdentityKmt& kmt,bool set,unsigned value,
                                           const wchar_t* image) noexcept {
    FsrIdentityResult r{};
    if(!set){r.hr=S_FALSE;return r;}
    if(value>1 || !kmt.open || !kmt.escape || !kmt.close){r.hr=E_INVALIDARG;return r;}
    D3DKMT_OPENADAPTERFROMLUID opened{};opened.AdapterLuid=luid;
    r.open=kmt.open(&opened);
    if(r.open!=0 || !opened.hAdapter){r.hr=r.open<0?HRESULT_FROM_NT(r.open):E_UNEXPECTED;return r;}
    BC250_ESCAPE_PROCESS_POLICY data=fsr_identity_request(value,image);
    D3DKMT_ESCAPE e{};
    e.hAdapter=opened.hAdapter;e.Type=D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData=&data;e.PrivateDriverDataSize=sizeof(data);
    r.escape=kmt.escape(&e);
    D3DKMT_CLOSEADAPTER closing{};closing.hAdapter=opened.hAdapter;
    r.close=kmt.close(&closing);
    if(r.escape!=0){r.hr=r.escape<0?HRESULT_FROM_NT(r.escape):E_UNEXPECTED;return r;}
    r.status=data.Status;r.answer=data.Answer;r.source=data.Source;r.name_flags=data.NameFlags;
    r.hr=data.Status==BC250_ESCAPE_STATUS_DONE?S_OK:E_FAIL;
    if(r.hr==S_OK && r.close!=0)r.hr=r.close<0?HRESULT_FROM_NT(r.close):E_UNEXPECTED;
    return r;
}

inline const char* fsr_identity_source_name(unsigned long source) noexcept {
    switch(source){
    case BC250_AMD_IDENTITY_SOURCE_DEFAULT:return "default";
    case BC250_AMD_IDENTITY_SOURCE_LIST:return "list";
    case BC250_AMD_IDENTITY_SOURCE_SHELL:return "shell";
    case BC250_AMD_IDENTITY_SOURCE_NO_NAME:return "no-name";
    case BC250_AMD_IDENTITY_SOURCE_NO_PROCESS:return "no-process";
    default:return "?";
    }
}
} // namespace native12
