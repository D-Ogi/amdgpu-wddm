// SPDX-License-Identifier: MIT
#pragma once
#include "device-owner.h"
#include <new>
#include <source_location>
#include "diagnostics.h"
namespace bc250::umd {
struct DdiDeviceHandle { DeviceOwner *owner=nullptr; };
// API HRESULT values are not the DDI values, even for the same condition.
inline HRESULT ddi_device_status(HRESULT hr) {
    switch (hr) {
    case DXGI_ERROR_DEVICE_REMOVED:
    case DXGI_ERROR_DEVICE_RESET:
    case DXGI_ERROR_DEVICE_HUNG:
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: return D3DDDIERR_DEVICEREMOVED;
    default: return hr;
    }
}
inline HRESULT ddi_map_status(HRESULT hr,bool doNotWait) {
    if (doNotWait && hr==DXGI_ERROR_WAS_STILL_DRAWING) return DXGI_DDI_ERR_WASSTILLDRAWING;
    // ResourceMap permits only DEVICEREMOVED, or WASSTILLDRAWING with DONOTWAIT.
    // Even allocation failure from WRITE_DISCARD must use the terminal DDI path.
    return FAILED(hr) ? D3DDDIERR_DEVICEREMOVED : hr;
}
inline void report_ddi_error(DeviceOwner &owner,HRESULT hr,
    const std::source_location where=std::source_location::current()) {
    static std::atomic_uint remaining{32};
    unsigned count=remaining.load(std::memory_order_relaxed);
    while(count && !remaining.compare_exchange_weak(count,count-1,std::memory_order_relaxed)){}
    if(count) {
        char message[768];
        std::snprintf(message,sizeof(message),"M14 DDI error HRESULT=%08X mapped=%08X line=%u function=%.600s\n",
            static_cast<unsigned>(hr),static_cast<unsigned>(ddi_device_status(hr)),where.line(),where.function_name());
        OutputDebugStringA(message);
    }
    auto &r=owner.runtime();
    if (r.UMCallbacks.pfnSetErrorCb) r.UMCallbacks.pfnSetErrorCb(r.hRTCoreLayer,ddi_device_status(hr));
}
template<typename F> void enter_context(D3D10DDI_HDEVICE handle,F &&call) noexcept {
    auto *storage=static_cast<DdiDeviceHandle *>(handle.pDrvPrivate);
    if (!storage || !storage->owner) return;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    if (!owner.context()) { report_ddi_error(owner,E_FAIL); return; }
    try { call(*owner.context()); }
    catch (const std::bad_alloc &) { report_ddi_error(owner,E_OUTOFMEMORY); }
    catch (...) { report_ddi_error(owner,E_FAIL); }
}

}
