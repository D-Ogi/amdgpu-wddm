// SPDX-License-Identifier: MIT
#pragma once
// M15.14 increment 3: whether the D3D11 shell asks the kernel driver for a scan-out primary.
//
// Until this increment the shell wrote the E26R v3 record of a primary with PRIMARY and never with
// SCANOUT. The kernel driver then places the buffer in the shared aperture, the display core cannot read
// it there, and the router's front answers CheckDirectFlipSupport FALSE under client-scannable: a D3D11
// game stays composed whatever the rest of the stack admits. The D3D12 shell sets the bit since increment
// 2, and The Witcher 3 flips with it (trial 478).
//
// The decision is the D3D12 shell's, through the rule both shells share
// (driver/contract/bc250_scanout_primary.h), plus the clauses that only this shell has:
//   Off          the operator's switch is off. This shell's default is OFF until a D3D11 game is measured
//                with the request (the train rule: a feature is on by default only after its lab
//                measurement). AMDGPU_WDDM_D3D11_SCANOUT=1 in the environment, or the REG_DWORD
//                ScanoutPrimary=1 under HKLM\SOFTWARE\amdgpu-wddm\D3D11 (64-bit view), asks for it;
//                the environment wins over the machine value, and any value that is not "1" is off.
//   NotPrimary   the swap-chain buffer came without a primary descriptor (a DISPLAYABLE surface of a
//                windowed flip-model chain, M746). The record's PRIMARY bit and the video present source
//                come from that descriptor, and both the kernel driver's SCANOUT policy and the front's
//                rule demand PRIMARY, so such a buffer cannot ask for scan-out. The line names the case so
//                that a trial can say which shape a game uses.
//   VidPnSource  the descriptor names a video present source other than the one this adapter has
//                (BC250_SCANOUT_VIDPN_SOURCE).
//   then the shared rule: ForceCpu, DesktopRoute (the compositor's record, driver/contract/bc250_desktop_route.h,
//   read for every primary), CapsClosed, SourceGeometry, Format, Pitch.
//
// Every answer except Admitted keeps the composed primary this shell has always made: same record without
// the SCANOUT bit, same aperture placement. A stand-down is never a failure of CreateResource.
#include "adapter-identity.h"
#include "../../contract/bc250_scanout_primary.h"
#include "../../contract/bc250_desktop_route.h"
#include "../../kmd/surface_resource_private.h"
#include <cstring>
namespace bc250::umd {
inline constexpr bool kScanoutPrimaryDefaultOn=false;
enum class ScanoutPrimaryReason : unsigned {
    Admitted,Off,NotPrimary,VidPnSource,ForceCpu,DesktopRoute,CapsClosed,SourceGeometry,Format,Pitch
};
inline const char *scanout_primary_reason_text(ScanoutPrimaryReason reason) noexcept {
    switch (reason) {
    case ScanoutPrimaryReason::Admitted: return "admitted";
    case ScanoutPrimaryReason::Off: return "mode-off";
    case ScanoutPrimaryReason::NotPrimary: return "not-primary";
    case ScanoutPrimaryReason::VidPnSource: return "vidpn-source";
    case ScanoutPrimaryReason::ForceCpu: return "force-cpu";
    case ScanoutPrimaryReason::DesktopRoute: return "desktop-route";
    case ScanoutPrimaryReason::CapsClosed: return "caps-closed";
    case ScanoutPrimaryReason::SourceGeometry: return "source-geometry";
    case ScanoutPrimaryReason::Format: return "format";
    case ScanoutPrimaryReason::Pitch: return "pitch";
    }
    return "unknown";
}
// One switch setting as read: absent, asks, refuses.
enum class ScanoutPrimarySetting : unsigned { Absent,On,Off };
// Where the answer came from, for the line: the default, an explicit on, an explicit off.
enum class ScanoutPrimarySwitch : unsigned { Default,On,Off };
inline const char *scanout_primary_switch_text(ScanoutPrimarySwitch value) noexcept {
    switch (value) {
    case ScanoutPrimarySwitch::Default: return "default";
    case ScanoutPrimarySwitch::On: return "on";
    case ScanoutPrimarySwitch::Off: return "off";
    }
    return "unknown";
}
inline ScanoutPrimarySwitch scanout_primary_switch(ScanoutPrimarySetting environment,
    ScanoutPrimarySetting machine) noexcept {
    const ScanoutPrimarySetting setting=environment!=ScanoutPrimarySetting::Absent ? environment : machine;
    if (setting==ScanoutPrimarySetting::On) return ScanoutPrimarySwitch::On;
    if (setting==ScanoutPrimarySetting::Off) return ScanoutPrimarySwitch::Off;
    return ScanoutPrimarySwitch::Default;
}
inline bool scanout_primary_requested(ScanoutPrimarySwitch value) noexcept {
    return value==ScanoutPrimarySwitch::On || (value==ScanoutPrimarySwitch::Default && kScanoutPrimaryDefaultOn);
}
// The environment value: absent (nullptr), exactly "1" asks, anything else refuses.
inline ScanoutPrimarySetting scanout_primary_environment_setting(const char *value) noexcept {
    if (!value) return ScanoutPrimarySetting::Absent;
    return std::strcmp(value,"1")==0 ? ScanoutPrimarySetting::On : ScanoutPrimarySetting::Off;
}
// A REG_DWORD as RegGetValueW returned it: not found is absent, a DWORD of 1 asks, anything else refuses.
inline ScanoutPrimarySetting scanout_primary_machine_setting(LSTATUS status,DWORD type,DWORD bytes,
    DWORD value) noexcept {
    if (status==ERROR_FILE_NOT_FOUND) return ScanoutPrimarySetting::Absent;
    return status==ERROR_SUCCESS && type==REG_DWORD && bytes==sizeof(DWORD) && value==1 ?
        ScanoutPrimarySetting::On : ScanoutPrimarySetting::Off;
}
// The desktop router's kill switch DwmForceCpu, with the router's own reading (driver/umd/router/router.cpp,
// ReadDword): absent is 0, a DWORD is its value, any other type or size is on.
inline unsigned long scanout_primary_force_cpu_setting(LSTATUS status,DWORD type,DWORD bytes,
    DWORD value) noexcept {
    if (status==ERROR_FILE_NOT_FOUND) return 0;
    if (status==ERROR_SUCCESS && type==REG_DWORD && bytes==sizeof(DWORD)) return value;
    return 1;
}
// The 64-bit view on both images: the router lives in the 64-bit compositor, and one machine value steers
// the x64 and the x86 shell alike.
inline constexpr DWORD kScanoutPrimaryRegistryView=sizeof(void *)==4 ? RRF_SUBKEY_WOW6464KEY : 0;
inline DWORD scanout_primary_registry_dword(const wchar_t *key,const wchar_t *name,LSTATUS &status,
    DWORD &type,DWORD &bytes) noexcept {
    DWORD value=0; type=0; bytes=sizeof(value);
    status=RegGetValueW(HKEY_LOCAL_MACHINE,key,name,RRF_RT_ANY|kScanoutPrimaryRegistryView,&type,&value,&bytes);
    return value;
}
// The compositor's desktop-route record of this session, from the compositor's account only.
inline unsigned scanout_primary_desktop_route_read(bc250_desktop_route *record) noexcept {
    return bc250_desktop_route_read_session(record);
}
// What the device needs to decide: the adapter query of the runtime's adapter (valid until CloseAdapter,
// after every device is gone), the two switches, read once at the adapter's open, and the reader of the
// compositor's record, called for every primary (a host test puts a double there).
struct ScanoutSource {
    HANDLE adapter=nullptr;
    PFND3DDDI_QUERYADAPTERINFOCB query=nullptr;
    ScanoutPrimarySwitch switch_state=kScanoutPrimaryDefaultOn ? ScanoutPrimarySwitch::Default :
                                                                 ScanoutPrimarySwitch::Off;
    unsigned long force_cpu=0;
    unsigned (*desktop_route)(bc250_desktop_route *) noexcept=&scanout_primary_desktop_route_read;
};
inline ScanoutSource read_scanout_source(HANDLE adapter,PFND3DDDI_QUERYADAPTERINFOCB query) noexcept {
    ScanoutSource source{};
    source.adapter=adapter; source.query=query;
    char text[8]{};
    SetLastError(ERROR_SUCCESS);   // an empty value also returns 0, and only "not found" is absent
    const DWORD length=GetEnvironmentVariableA("AMDGPU_WDDM_D3D11_SCANOUT",text,sizeof(text));
    ScanoutPrimarySetting environment=ScanoutPrimarySetting::Absent;
    if (length>=sizeof(text)) environment=ScanoutPrimarySetting::Off;
    else if (length || GetLastError()!=ERROR_ENVVAR_NOT_FOUND) environment=scanout_primary_environment_setting(text);
    LSTATUS status=ERROR_SUCCESS; DWORD type=0,bytes=0;
    DWORD value=scanout_primary_registry_dword(L"SOFTWARE\\amdgpu-wddm\\D3D11",L"ScanoutPrimary",status,type,bytes);
    source.switch_state=scanout_primary_switch(environment,scanout_primary_machine_setting(status,type,bytes,value));
    value=scanout_primary_registry_dword(L"SOFTWARE\\amdgpu-wddm\\DesktopRouter",L"DwmForceCpu",status,type,bytes);
    source.force_cpu=scanout_primary_force_cpu_setting(status,type,bytes,value);
    return source;
}
struct ScanoutPrimaryDecision {
    ScanoutPrimaryReason reason=ScanoutPrimaryReason::Off;
    bool admitted=false;
    ScanoutPrimarySwitch switch_state=ScanoutPrimarySwitch::Off;
    bc250_scanout_caps caps{};    // the trailer this decision read; zero when it read none
    bool desktop_read=false;      // whether the decision read the compositor's record
    unsigned desktop_status=BC250_DESKTOP_ROUTE_READ_ABSENT;
    bc250_desktop_route desktop{};
};
// primary is whether the runtime gave a primary descriptor, vidpn_source the descriptor's source, dxgi the
// chain's DXGI format, width, height and pitch the LB7A description this shell writes, and mode_width and
// mode_height the descriptor's ModeDesc (0 without a descriptor). The switch is asked before the adapter
// query, so a start with the request off makes no query at all.
inline ScanoutPrimaryDecision scanout_primary_decide(const ScanoutSource *source,bool primary,UINT vidpn_source,
    unsigned dxgi,unsigned width,unsigned height,unsigned pitch,unsigned mode_width=0,
    unsigned mode_height=0) noexcept {
    ScanoutPrimaryDecision out{};
    if (!source) return out;
    out.switch_state=source->switch_state;
    if (!scanout_primary_requested(source->switch_state)) return out;
    if (!primary) { out.reason=ScanoutPrimaryReason::NotPrimary; return out; }
    if (vidpn_source!=BC250_SCANOUT_VIDPN_SOURCE) { out.reason=ScanoutPrimaryReason::VidPnSource; return out; }
    out.caps=query_scanout_caps(source->adapter,source->query);
    out.desktop_read=source->desktop_route!=nullptr;
    if (out.desktop_read) out.desktop_status=source->desktop_route(&out.desktop);
    // C71: D3DKMT_SETDISPLAYMODE takes the new mode's primary as input, so the runtime creates that primary
    // before the mode commit and the trailer still names the previous mode. A D3D11 primary names its own
    // mode in the descriptor's ModeDesc, and the runtime sets only a mode that the kernel driver offers: that
    // mode at the chain's geometry is the offered mode of the shared rule. The flips still wait for the commit.
    const int offered_mode=width && mode_width==width && mode_height==height;
    switch (bc250_scanout_primary_rule(&out.caps,source->force_cpu,bc250_desktop_route_gpu(out.desktop_status,&out.desktop),
                                       dxgi,width,height,pitch,offered_mode)) {
    case BC250_SCANOUT_PRIMARY_ADMITTED: out.reason=ScanoutPrimaryReason::Admitted; out.admitted=true; break;
    case BC250_SCANOUT_PRIMARY_FORCE_CPU: out.reason=ScanoutPrimaryReason::ForceCpu; break;
    case BC250_SCANOUT_PRIMARY_DESKTOP_ROUTE: out.reason=ScanoutPrimaryReason::DesktopRoute; break;
    case BC250_SCANOUT_PRIMARY_CAPS_CLOSED: out.reason=ScanoutPrimaryReason::CapsClosed; break;
    case BC250_SCANOUT_PRIMARY_SOURCE_GEOMETRY: out.reason=ScanoutPrimaryReason::SourceGeometry; break;
    case BC250_SCANOUT_PRIMARY_FORMAT: out.reason=ScanoutPrimaryReason::Format; break;
    default: out.reason=ScanoutPrimaryReason::Pitch; break;
    }
    return out;
}
}
