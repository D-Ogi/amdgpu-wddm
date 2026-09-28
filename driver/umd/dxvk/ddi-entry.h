// SPDX-License-Identifier: MIT
#pragma once
#include "device-owner.h"
#include <new>
namespace bc250::umd {
struct DdiDeviceHandle { DeviceOwner *owner=nullptr; };
inline void report_ddi_error(DeviceOwner &owner,HRESULT hr) {
    auto &r=owner.runtime();
    if (r.UMCallbacks.pfnSetErrorCb) r.UMCallbacks.pfnSetErrorCb(r.hRTCoreLayer,hr);
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
