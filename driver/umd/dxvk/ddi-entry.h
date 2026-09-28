// SPDX-License-Identifier: MIT
#pragma once
#include "device-owner.h"
#include <new>
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
    // Busy without DONOTWAIT violates Map's contract. Keep the unexpected
    // error critical instead of silently treating it as a legal polling result.
    return ddi_device_status(hr);
}
inline void report_ddi_error(DeviceOwner &owner,HRESULT hr) {
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
