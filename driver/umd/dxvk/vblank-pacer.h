// SPDX-License-Identifier: MIT
// BD-099: the fallback for the per-application VSync of the D3D11 shell, as vertical-blank waits.
//
// The shell now offers the WDDM 2.2 interface as well (ddi-negotiation.h, ddi-wddm22.h). There the runtime copies the
// whole DXGIDDICB_PRESENT, the override reaches it, and the runtime paces the frame: this pacer then waits for
// nothing (extra_vblanks gives 0 while DeviceOwner::full_present_callback is true). The pacer stays for the two cases
// that keep the old callback: a process that asks for the WDDM 2.0 interface through the experiment switch
// (ddi-experiment.h), and an FL11 adapter, which offers the D3D11.1 interface alone.
//
// Why the older interfaces need it. The WDK declares DXGIDDICB_PRESENT.SyncIntervalOverride only for
// D3D_UMD_INTERFACE_VERSION_WDDM2_2_2 and later. The D3D11 runtime gives the whole Present callback only from
// interface 0xB0023 (WDDM 2.2) build 5 (IS_DXGI1_6_1_BASE_FUNCTIONS in d3d10umddi.h). Below that, d3d11.dll
// 10.0.22621 gives PresentCB_PreWDDM2_2, which copies only the older, shorter structure into its own
// DXGIDDICB_PRESENT (BD-099). On x64 the copy holds SyncIntervalOverrideValid, which is in the tail padding of the older
// structure, but not SyncIntervalOverride: each override arrives as interval 0. On unit A (59 Hz, d3d11bench in a
// window, a flip-discard swap chain) an override for VSync 1 gave 873-928 frames/s for the intervals 1 and 2. On x86
// the copy holds neither field. So on the old callback the shell passes VSync 0 only (d3d11_runtime_override in
// app-settings.h). The DDI arguments do not show the interval of the application either: FlipInterval is 0 and Flags
// is Blt for the intervals 0, 1 and 2. For VSync 1 the Present path therefore waits for the missing vertical blanks
// after the Present callback (extra_vblanks in app-settings.h), and the runtime keeps the interval of the application
// (docs/design/per-app-graphics-settings.md).
//
// The output is the desktop output of this adapter that EnumDisplayDevices lists first, the primary one when it is on
// this adapter. The DDI does not name the window, so on a desktop with several outputs of different refresh rates the
// pacing follows that one output.
#pragma once
#include "runtime-bridge.h"
#include "../d3d12/stdio-log.h"
#include <d3dkmthk.h>
#include <cstdio>
#include <cstring>

namespace bc250::umd {

class VBlankPacer {
public:
    VBlankPacer() noexcept=default;
    ~VBlankPacer() {
        if (adapter_ && close_) {
            D3DKMT_CLOSEADAPTER close{}; close.hAdapter=adapter_;
            close_(&close);
        }
        if (gdi_) FreeLibrary(gdi_);
        if (user_) FreeLibrary(user_);
    }
    VBlankPacer(const VBlankPacer &)=delete;
    VBlankPacer &operator=(const VBlankPacer &)=delete;
    // The LUID of the adapter that the device runs on, before the first wait.
    void set_luid(UINT64 luid) noexcept { luid_=luid; }
    // Waits for count vertical blanks of the output. False when no output of this adapter can be opened or a wait
    // fails; the Present goes on unpaced then, as before this pacer.
    bool wait(unsigned count) noexcept {
        if (!count) return true;
        AcquireSRWLockExclusive(&lock_);
        if (!tried_) { tried_=true; open(); }
        const D3DKMT_HANDLE adapter=adapter_;
        const D3DDDI_VIDEO_PRESENT_SOURCE_ID source=source_;
        ReleaseSRWLockExclusive(&lock_);
        if (!adapter || !wait_) return false;
        D3DKMT_WAITFORVERTICALBLANKEVENT event{};
        event.hAdapter=adapter; event.hDevice=0; event.VidPnSourceId=source;
        for (unsigned i=0;i<count;++i) {
            const NTSTATUS status=wait_(&event);
            if (status<0) { report("wait failed",status); return false; }
        }
        return true;
    }

private:
    template<typename T> static T entry(HMODULE module,const char *name) noexcept {
        return reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(module,name)));
    }
    void open() noexcept {
        gdi_=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!gdi_) { report("gdi32.dll not loaded",0); return; }
        const auto from_name=entry<PFND3DKMT_OPENADAPTERFROMGDIDISPLAYNAME>(gdi_,"D3DKMTOpenAdapterFromGdiDisplayName");
        close_=entry<PFND3DKMT_CLOSEADAPTER>(gdi_,"D3DKMTCloseAdapter");
        wait_=entry<PFND3DKMT_WAITFORVERTICALBLANKEVENT>(gdi_,"D3DKMTWaitForVerticalBlankEvent");
        if (!from_name || !close_ || !wait_) { report("D3DKMT entries missing",0); return; }
        // user32 by address, as gdi32: the shell links neither, and a D3D11 application has both loaded.
        user_=LoadLibraryExW(L"user32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        using EnumDisplays=BOOL (WINAPI *)(LPCWSTR,DWORD,PDISPLAY_DEVICEW,DWORD);
        const auto enum_displays=user_ ? entry<EnumDisplays>(user_,"EnumDisplayDevicesW") : nullptr;
        if (!enum_displays) { report("EnumDisplayDevicesW missing",0); return; }
        for (DWORD i=0;;++i) {
            DISPLAY_DEVICEW display{}; display.cb=sizeof(display);
            if (!enum_displays(nullptr,i,&display,0)) break;
            if (!(display.StateFlags&DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME name{};
            wcsncpy_s(name.DeviceName,display.DeviceName,_TRUNCATE);
            if (from_name(&name)<0) continue;
            UINT64 luid=0; std::memcpy(&luid,&name.AdapterLuid,sizeof(luid));
            const bool ours=!luid_ || luid==luid_;
            const bool better=ours && (!adapter_ || (display.StateFlags&DISPLAY_DEVICE_PRIMARY_DEVICE));
            if (better) {
                if (adapter_) { D3DKMT_CLOSEADAPTER close{}; close.hAdapter=adapter_; close_(&close); }
                adapter_=name.hAdapter; source_=name.VidPnSourceId;
                if (display.StateFlags&DISPLAY_DEVICE_PRIMARY_DEVICE) break;
            } else {
                D3DKMT_CLOSEADAPTER close{}; close.hAdapter=name.hAdapter; close_(&close);
            }
        }
        char text[160];
        if (adapter_) std::snprintf(text,sizeof(text),"BC250 BD-099: VSync paced by vertical-blank waits on source %u\n",
            unsigned(source_));
        else std::snprintf(text,sizeof(text),"BC250 BD-099: no desktop output of this adapter, VSync not paced\n");
        OutputDebugStringA(text);
        amdgpu_wddm_log::print("%s",text);
    }
    static void report(const char *what,NTSTATUS status) noexcept {
        static LONG reports=0;
        if (InterlockedIncrement(&reports)>4) return;
        char text[160];
        std::snprintf(text,sizeof(text),"BC250 BD-099: %s (0x%08lX), VSync not paced\n",what,
            static_cast<unsigned long>(status));
        OutputDebugStringA(text);
        amdgpu_wddm_log::print("%s",text);
    }
    SRWLOCK lock_=SRWLOCK_INIT;
    bool tried_=false;
    UINT64 luid_=0;
    HMODULE gdi_=nullptr,user_=nullptr;
    PFND3DKMT_CLOSEADAPTER close_=nullptr;
    PFND3DKMT_WAITFORVERTICALBLANKEVENT wait_=nullptr;
    D3DKMT_HANDLE adapter_=0;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID source_=0;
};

}
