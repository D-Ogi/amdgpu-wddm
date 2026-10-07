// SPDX-License-Identifier: MIT
#pragma once
// M15.14 increment 2: whether this shell asks for a scan-out primary, in one function the host test can
// drive through every answer.
//
// The decision is a stand-down, never a failure. Everything it can refuse is a property of the start, of
// the operator's switches or of the chain's geometry, and in every one of those cases the right outcome is
// the composed primary this shell has always made: the same buffer, in the shared aperture, with the CPU
// mapping the compositor's readers need. Failing the allocation instead would take a game down over a
// display capability it never asked for, and asking for scan-out anyway would move the buffer into the
// local segment with no CPU access for a flip the kernel driver would refuse (and refuse after the OS has
// taken SharedPrimaryTransition, which does not fall back to composition seamlessly - a black output).
//
// The clauses, in the order they are asked:
//   ModeOff        AMDGPU_WDDM_D3D12_EXPERIMENT does not name the scan-out mode. The driver's default.
//   OtherIntent    the same list also names present-cached or present-noprimary, which describe the
//                  opposite intent for the same buffer (a cached CPU reader, or no primary at all).
//   ModeGeometry   the chain is not the geometry the mode names ("scanout-flip-1920x1200").
//   ForceCpu       the desktop route's kill switch DwmForceCpu is on, so the compositor is the CPU UMD,
//                  which reads every primary on the CPU to compose it. There is no CheckDirectFlipSupport
//                  on that route and no flip to be had, while the request alone would cost that reader its
//                  write-combined aperture mapping (experiments 104 and 107).
//   CapsClosed     the kernel driver published no scan-out trailer, or published one without
//                  BC250_SCANOUT_CAPS_DIRECT_FLIP: an older driver, or an operator switch that is off.
//   SourceGeometry the chain is not the geometry of the source mode the trailer carries, which is the one
//                  geometry Bc250ScanoutAdmit admits a flip at. The clause compares against the trailer
//                  and never against a size of its own, so it follows whatever source mode the kernel
//                  driver offers (the same rule the router's front applies on the compositor's side).
//   Format         the chain's format is not a SCANOUT_PRIMARY row of the shared table, or is a row with
//                  no DXGI name (so the compositor's opener could not take its record), or is a row other
//                  than the firmware's own format while the trailer lacks BC250_SCANOUT_CAPS_PLANE_FORMATS
//                  (bc250_scanout_format_admitted: an older kernel driver does not program the plane's
//                  pixel format, and its refusal would come after SharedPrimaryTransition).
//   Pitch          the engine's row pitch is not the one pitch every component derives
//                  (scanout_row_pitch, with the row's bytes_per_pixel). Nothing downstream could check a
//                  pitch only this shell knows.
//
// ModeGeometry sits before the switches on purpose: a start with the mode named for another monitor must
// read as "this chain is not the one" and not as "the kernel driver said no", or a trial would chase a
// switch that was never the reason.
#include "allocation-request.h"
#include "ddi-trace.h"
#include "../../contract/bc250_scanout_caps.h"
namespace native12 {
enum class ScanoutStandDown : unsigned {
    Admitted,ModeOff,OtherIntent,ModeGeometry,ForceCpu,CapsClosed,SourceGeometry,Format,Pitch,Count
};
inline const char* scanout_stand_down_text(ScanoutStandDown reason) noexcept {
    switch(reason){
    case ScanoutStandDown::Admitted:return "admitted";
    case ScanoutStandDown::ModeOff:return "mode-off";
    case ScanoutStandDown::OtherIntent:return "other-intent";
    case ScanoutStandDown::ModeGeometry:return "mode-geometry";
    case ScanoutStandDown::ForceCpu:return "force-cpu";
    case ScanoutStandDown::CapsClosed:return "caps-closed";
    case ScanoutStandDown::SourceGeometry:return "source-geometry";
    case ScanoutStandDown::Format:return "format";
    case ScanoutStandDown::Pitch:return "pitch";
    default:return "unknown";
    }
}
struct ScanoutDecision {
    ScanoutStandDown reason{ScanoutStandDown::ModeOff};
    bool admitted{};                            // reason==Admitted
    // What the mode named, for the trace: 0 when the list named no mode.
    unsigned mode_width{},mode_height{};
};
// width, height and pitch are the chain's as engine-ddi described it; dxgi is D3D12DDIARG_CREATERESOURCE's
// Format. caps is the adapter's published trailer (all zero when there is none) and force_cpu the desktop
// router's kill switch as that router reads it (any non-zero value, and any value of the wrong type, is on).
inline ScanoutDecision scanout_decide(const char* experiments,const bc250_scanout_caps& caps,
                                      unsigned long force_cpu,unsigned dxgi,
                                      unsigned width,unsigned height,unsigned pitch) noexcept {
    ScanoutDecision out{};
    if(!ddi_experiment_scanout(experiments,&out.mode_width,&out.mode_height)){
        out.reason=ScanoutStandDown::ModeOff;return out;
    }
    if(ddi_experiment_listed(experiments,"present-cached") ||
       ddi_experiment_listed(experiments,"present-noprimary")){
        out.reason=ScanoutStandDown::OtherIntent;return out;
    }
    if(width!=out.mode_width || height!=out.mode_height){
        out.reason=ScanoutStandDown::ModeGeometry;return out;
    }
    if(force_cpu){out.reason=ScanoutStandDown::ForceCpu;return out;}
    if(!(caps.flags&BC250_SCANOUT_CAPS_DIRECT_FLIP)){out.reason=ScanoutStandDown::CapsClosed;return out;}
    if(width!=caps.post_width || height!=caps.post_height){
        out.reason=ScanoutStandDown::SourceGeometry;return out;
    }
    const auto* row=amdgpu_wddm_surface_format_by_dxgi(dxgi);
    if(!row || !row->dxgi || !bc250_scanout_format_admitted(row,caps.flags)){
        out.reason=ScanoutStandDown::Format;return out;
    }
    if(!pitch || pitch!=scanout_row_pitch(width,row->bytes_per_pixel)){
        out.reason=ScanoutStandDown::Pitch;return out;
    }
    out.reason=ScanoutStandDown::Admitted;out.admitted=true;return out;
}
// The desktop router's kill switch, read the way the router itself reads it (driver/umd/router/router.cpp
// ReadConfig): absent is 0, a DWORD is its value, any other type counts as set. Read once per process,
// like every other switch of this shell: the router reads it at the compositor's adapter open and a change
// takes a new dwm.exe anyway, so a later value could not describe the compositor this process is talking to.
inline unsigned long scanout_force_cpu_read() noexcept {
    DWORD value=0,bytes=sizeof(value),type=0;
    const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\amdgpu-wddm\\DesktopRouter",
                                      L"DwmForceCpu",RRF_RT_ANY,&type,&value,&bytes);
    if(status!=ERROR_SUCCESS)return 0;                       // absent, or unreadable: the router's 0
    if(type!=REG_DWORD || bytes!=sizeof(value))return 1;     // fail safe, exactly as the router does
    return value;
}
inline unsigned long scanout_force_cpu() noexcept {
    static const unsigned long value=scanout_force_cpu_read();
    return value;
}
}
