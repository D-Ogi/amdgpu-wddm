// SPDX-License-Identifier: MIT
#pragma once
// M15.14: whether this shell asks for a scan-out primary, in one function the host test can drive through
// every answer.
//
// The decision is a stand-down, never a failure. Everything it can refuse is a property of the start, of
// the operator's switches or of the chain's geometry, and in every one of those cases the right outcome is
// the composed primary this shell has always made: the same buffer, in the shared aperture, with the CPU
// mapping the compositor's readers need. Failing the allocation instead would take a game down over a
// display capability it never asked for, and asking for scan-out anyway would move the buffer into the
// local segment with no CPU access for a flip the kernel driver would refuse (and refuse after the OS has
// taken SharedPrimaryTransition, which does not fall back to composition seamlessly - a black output).
//
// Increment 3 (2026-10-08). The scan-out primary is the driver's default, and the geometry it is asked for
// is the source mode the kernel driver has committed, not a size the operator names. Trial 478 is why:
// The Witcher 3 scanned out its own buffers at the native mode in exclusive fullscreen and in borderless,
// and stayed composed in exclusive fullscreen at 1920x1080 only because increment 2's experiment named
// 1920x1200 while the kernel driver had committed a real 1080 mode. The caller reads the trailer again for
// every primary it creates (RuntimeHeapImports::scanout_caps_now), so the geometry clause follows a mode
// change of a running game.
//
// The clauses, in the order they are asked:
//   ModeOff        the operator's off switch: the experiment list (AMDGPU_WDDM_D3D12_EXPERIMENT, the
//                  application's profile or the machine value under HKLM\SOFTWARE\amdgpu-wddm\D3D12) names
//                  "scanout-flip-off", as every default of this shell is turned off (ddi_experiment_off).
//   OtherIntent    the same list names present-cached or present-noprimary, which describe the opposite
//                  intent for the same buffer (a cached CPU reader, or no primary at all).
//   then the rule both application shells share (driver/contract/bc250_scanout_primary.h):
//   ForceCpu       the desktop route's kill switch DwmForceCpu is on, so the compositor is the CPU UMD.
//   DesktopRoute   the compositor's record (driver/contract/bc250_desktop_route.h, read for every primary)
//                  does not say GPU: the router in dwm.exe took the CPU UMD, including the fallback after a
//                  failed hosted open, or there is no record from the compositor's account.
//   CapsClosed     the kernel driver published no scan-out trailer, or one without DIRECT_FLIP.
//   SourceGeometry the chain is not the geometry of the source mode the trailer carries now.
//   Format         the chain's format is not a SCANOUT_PRIMARY row with a DXGI name.
//   Pitch          the engine's row pitch is not the one pitch every component derives (scanout_row_pitch).
//
// The increment-2 spelling "scanout-flip-1920x1200" still reads as an explicit on, and so does a bare
// "scanout-flip": a lab script written for increment 2 keeps working. The named geometry is not compared
// any more. A geometry named by the operator is a guess about the mode, and the kernel driver's trailer is
// the mode itself; the guess is what kept 478's 1080 chain composed.
#include "allocation-request.h"
#include "ddi-trace.h"
#include "../../contract/bc250_scanout_caps.h"
#include "../../contract/bc250_scanout_primary.h"
#include "../../contract/bc250_desktop_route.h"
namespace native12 {
// The train rule (owner, 2026-10-05): a finished, measured feature is on by default, with a switch to turn
// it off. Measured: the plan A client 600 of 600 frames at FlipOnNextVSync (K227), The Witcher 3 2028 of
// 2028 flips in exclusive fullscreen and 1696 of 1696 in borderless at the native mode with FlipImmediate,
// 0 refusals, and a mode change away from a flipping chain without a black output (trial 478).
inline constexpr bool kScanoutDefaultOn=true;
enum class ScanoutStandDown : unsigned {
    Admitted,ModeOff,OtherIntent,ForceCpu,DesktopRoute,CapsClosed,SourceGeometry,Format,Pitch,Count
};
inline const char* scanout_stand_down_text(ScanoutStandDown reason) noexcept {
    switch(reason){
    case ScanoutStandDown::Admitted:return "admitted";
    case ScanoutStandDown::ModeOff:return "mode-off";
    case ScanoutStandDown::OtherIntent:return "other-intent";
    case ScanoutStandDown::ForceCpu:return "force-cpu";
    case ScanoutStandDown::DesktopRoute:return "desktop-route";
    case ScanoutStandDown::CapsClosed:return "caps-closed";
    case ScanoutStandDown::SourceGeometry:return "source-geometry";
    case ScanoutStandDown::Format:return "format";
    case ScanoutStandDown::Pitch:return "pitch";
    default:return "unknown";
    }
}
// Where the on or off came from, for the trace: the default, a list that names the mode, or the off switch.
enum class ScanoutSwitch : unsigned {Default,Named,Off};
inline const char* scanout_switch_text(ScanoutSwitch value) noexcept {
    switch(value){
    case ScanoutSwitch::Default:return "default";
    case ScanoutSwitch::Named:return "named";
    case ScanoutSwitch::Off:return "off";
    default:return "unknown";
    }
}
// The off switch wins over everything, because a switch may only subtract from the validated default.
// With kScanoutDefaultOn false, Default reads as Off and only a named mode turns the request on.
inline ScanoutSwitch scanout_switch(const char* experiments) noexcept {
    if(ddi_experiment_listed(experiments,"scanout-flip-off"))return ScanoutSwitch::Off;
    unsigned width=0,height=0;
    if(ddi_experiment_listed(experiments,"scanout-flip") || ddi_experiment_scanout(experiments,&width,&height))
        return ScanoutSwitch::Named;
    return kScanoutDefaultOn?ScanoutSwitch::Default:ScanoutSwitch::Off;
}
struct ScanoutDecision {
    ScanoutStandDown reason{ScanoutStandDown::ModeOff};
    bool admitted{};                            // reason==Admitted
    ScanoutSwitch switch_state{ScanoutSwitch::Off};
};
// width, height and pitch are the chain's as engine-ddi described it; dxgi is D3D12DDIARG_CREATERESOURCE's
// Format. caps is the trailer as read for this primary (all zero when there is none), force_cpu the
// desktop router's kill switch as that router reads it (any non-zero value, and any value of the wrong
// type, is on) and desktop_gpu whether the compositor's record, read for this primary, says GPU.
inline ScanoutDecision scanout_decide(const char* experiments,const bc250_scanout_caps& caps,
                                      unsigned long force_cpu,bool desktop_gpu,unsigned dxgi,
                                      unsigned width,unsigned height,unsigned pitch) noexcept {
    ScanoutDecision out{};
    out.switch_state=scanout_switch(experiments);
    if(out.switch_state==ScanoutSwitch::Off){out.reason=ScanoutStandDown::ModeOff;return out;}
    if(ddi_experiment_listed(experiments,"present-cached") ||
       ddi_experiment_listed(experiments,"present-noprimary")){
        out.reason=ScanoutStandDown::OtherIntent;return out;
    }
    switch(bc250_scanout_primary_rule(&caps,force_cpu,desktop_gpu?1:0,dxgi,width,height,pitch)){
    case BC250_SCANOUT_PRIMARY_ADMITTED:out.reason=ScanoutStandDown::Admitted;out.admitted=true;break;
    case BC250_SCANOUT_PRIMARY_FORCE_CPU:out.reason=ScanoutStandDown::ForceCpu;break;
    case BC250_SCANOUT_PRIMARY_DESKTOP_ROUTE:out.reason=ScanoutStandDown::DesktopRoute;break;
    case BC250_SCANOUT_PRIMARY_CAPS_CLOSED:out.reason=ScanoutStandDown::CapsClosed;break;
    case BC250_SCANOUT_PRIMARY_SOURCE_GEOMETRY:out.reason=ScanoutStandDown::SourceGeometry;break;
    case BC250_SCANOUT_PRIMARY_FORMAT:out.reason=ScanoutStandDown::Format;break;
    default:out.reason=ScanoutStandDown::Pitch;break;
    }
    return out;
}
// The desktop router's kill switch, read the way the router itself reads it (driver/umd/router/router.cpp
// ReadDword): absent is 0, a DWORD is its value, anything else - another type, a longer value
// (ERROR_MORE_DATA), a value the process may not read - counts as set. Read once per process,
// like every other switch of this shell: the router reads it at the compositor's adapter open and a change
// takes a new dwm.exe anyway, so a later value could not describe the compositor this process is talking to.
// The 64-bit view on both images (ddi_detail::registry_view): the router lives in the 64-bit compositor, so
// the value that steers it is the 64-bit one, and the x86 shell of a 32-bit game must read that one too.
inline unsigned long scanout_force_cpu_read() noexcept {
    DWORD value=0,bytes=sizeof(value),type=0;
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\amdgpu-wddm\\DesktopRouter",
                                      L"DwmForceCpu",RRF_RT_ANY|ddi_detail::registry_view,&type,&value,&bytes);
    if(status==ERROR_FILE_NOT_FOUND)return 0;                // absent: the router's 0
    if(status!=ERROR_SUCCESS || type!=REG_DWORD || bytes!=sizeof(value))return 1;  // fail safe, as the router
    return value;
}
inline unsigned long scanout_force_cpu() noexcept {
    static const unsigned long value=scanout_force_cpu_read();
    return value;
}
// The compositor's desktop-route record, read for every primary like the trailer and never cached: dwm.exe
// can restart while a game runs, and its route with it (the kernel driver swap does exactly that). The
// caller holds the reader as a function pointer so that the host tests can put a double in place of the
// compositor; the shell itself only ever reads the session's record from the compositor's account.
inline unsigned scanout_desktop_route_read(bc250_desktop_route* record) noexcept {
    return bc250_desktop_route_read_session(record);
}
}
