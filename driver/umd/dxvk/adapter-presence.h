// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <cstring>
namespace bc250::umd {
// BD-179: whether the adapter of a device is still in the system. A PnP restart of the display device
// (pnputil /restart-device, the enable-driver action of the control app, a driver install) stops the adapter
// and starts a new one with a new LUID. The kernel does not end the waits on the fences of the old adapter's
// devices: their fences keep their last value, no fence reads the lost-device value and no wait event is set.
// In the dump of the hung explorer.exe (2026-11-02, tester.35) the devices of the old adapter had LUID 0x8582
// and the devices made after the restart had LUID 0x3a8fa1, and the wallpaper's device release waited with
// no bound for a fence that read 1. So the bridge asks the kernel's adapter list: an adapter that left the
// list was stopped, and DxgkDdiStopDevice resets the adapter (nc-dispmprt-dxgkddi_stop_device.md,
// windows-driver-docs-ddi 7515063), so no work of its devices runs any more.
enum class AdapterPresence : unsigned { present, gone, unknown };
// The kernel entries the probe uses. The tests give their own.
struct AdapterPresenceKmt {
    PFND3DKMT_ENUMADAPTERS2 enum_adapters2=nullptr;
    PFND3DKMT_CLOSEADAPTER close_adapter=nullptr;
};
inline constexpr ULONG kAdapterPresenceListSize=32;
// present when the adapter list has luid, gone when a complete list does not have it, unknown when no complete
// list was read. Every handle that the enumeration opened is closed again.
inline AdapterPresence probe_adapter_presence(UINT64 luid,const AdapterPresenceKmt &kmt) noexcept {
    if (!luid || !kmt.enum_adapters2 || !kmt.close_adapter) return AdapterPresence::unknown;
    D3DKMT_ADAPTERINFO list[kAdapterPresenceListSize]{};
    D3DKMT_ENUMADAPTERS2 enumeration{};
    enumeration.NumAdapters=kAdapterPresenceListSize; enumeration.pAdapters=list;
    if (!NT_SUCCESS(kmt.enum_adapters2(&enumeration))) return AdapterPresence::unknown;
    if (enumeration.NumAdapters>kAdapterPresenceListSize) return AdapterPresence::unknown;
    bool found=false;
    for (ULONG i=0;i<enumeration.NumAdapters;++i) {
        UINT64 id=0; std::memcpy(&id,&list[i].AdapterLuid,sizeof(id));
        if (id==luid) found=true;
        if (list[i].hAdapter) { D3DKMT_CLOSEADAPTER close{}; close.hAdapter=list[i].hAdapter; (void)kmt.close_adapter(&close); }
    }
    return found ? AdapterPresence::present : AdapterPresence::gone;
}
// The entries of gdi32.dll, read once. gdi32.dll is loaded in every process that has a D3D11 device.
inline const AdapterPresenceKmt &system_adapter_presence_kmt() noexcept {
    static const AdapterPresenceKmt kmt=[] {
        AdapterPresenceKmt entries{};
        HMODULE gdi=GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (gdi) {
            entries.enum_adapters2=reinterpret_cast<PFND3DKMT_ENUMADAPTERS2>(
                reinterpret_cast<void *>(GetProcAddress(gdi,"D3DKMTEnumAdapters2")));
            entries.close_adapter=reinterpret_cast<PFND3DKMT_CLOSEADAPTER>(
                reinterpret_cast<void *>(GetProcAddress(gdi,"D3DKMTCloseAdapter")));
        }
        return entries;
    }();
    return kmt;
}
}
