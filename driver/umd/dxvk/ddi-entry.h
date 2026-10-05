// SPDX-License-Identifier: MIT
#pragma once
#include "device-owner.h"
#include "ddi-error-policy.h"
#include <new>
#include <source_location>
#include "diagnostics.h"
namespace bc250::umd {
struct DdiDeviceHandle { DeviceOwner *owner=nullptr; };
inline HRESULT ddi_map_status(HRESULT hr,bool doNotWait) {
    if (doNotWait && hr==DXGI_ERROR_WAS_STILL_DRAWING) return DXGI_DDI_ERR_WASSTILLDRAWING;
    // ResourceMap permits only DEVICEREMOVED, or WASSTILLDRAWING with DONOTWAIT.
    // Even allocation failure from WRITE_DISCARD must use the terminal DDI path.
    return FAILED(hr) ? D3DDDIERR_DEVICEREMOVED : hr;
}
// `status` is the status this entry reports, not a private reason code: it must be one the entry's
// reference page allows for `policy`. The log keeps the source line, so the condition stays
// identifiable without smuggling an illegal status past the runtime. ddi_class_status is the last
// guard: a status the page does not allow becomes the page's own general failure, and an entry whose
// page allows no status at all reports nothing.
inline void report_ddi_error(DeviceOwner &owner,HRESULT status,
    DdiErrorClass policy=DdiErrorClass::removed_only,
    const std::source_location where=std::source_location::current()) {
    const HRESULT reported=ddi_class_status(policy,status);
    static std::atomic_uint remaining{32};
    unsigned count=remaining.load(std::memory_order_relaxed);
    while(count && !remaining.compare_exchange_weak(count,count-1,std::memory_order_relaxed)){}
    if(count) {
        char message[768];
        std::snprintf(message,sizeof(message),
            "M14 DDI error HRESULT=%08X class=%u reported=%08X line=%u function=%.600s\n",
            static_cast<unsigned>(status),static_cast<unsigned>(policy),
            static_cast<unsigned>(reported),where.line(),where.function_name());
        OutputDebugStringA(message);
    }
    if (reported==S_OK) return; // This entry may report no status at all.
    auto &r=owner.runtime();
    if (r.UMCallbacks.pfnSetErrorCb) r.UMCallbacks.pfnSetErrorCb(r.hRTCoreLayer,reported);
}
// A lost context and an exception are terminal for the device, so they report device removal where
// the entry allows it. A check-type or tile entry does not allow it, and then the entry reports the
// strongest status its own page allows, through ddi_class_internal_failure.
template<typename F> void enter_context(D3D10DDI_HDEVICE handle,F &&call,
    DdiErrorClass policy=DdiErrorClass::removed_only) noexcept {
    auto *storage=static_cast<DdiDeviceHandle *>(handle.pDrvPrivate);
    if (!storage || !storage->owner) return;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    if (!owner.context()) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,policy); return; }
    try { call(*owner.context()); }
    catch (const std::bad_alloc &) {
        report_ddi_error(owner,ddi_status_allowed(policy,E_OUTOFMEMORY) ? E_OUTOFMEMORY : D3DDDIERR_DEVICEREMOVED,policy);
    }
    catch (...) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,policy); }
}

}
