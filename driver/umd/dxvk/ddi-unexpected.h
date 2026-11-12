// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include "ddi-status-note.h"
#include <atomic>
#include <type_traits>
namespace bc250::umd {
// A DDI entry that the runtime must never call, because a capability or a flag of this shell already tells the
// runtime that the feature is absent. A call is therefore a contract break. The entry writes one diagnostic line
// for each process and then gives the narrowest answer its signature allows: a void entry loses the device (the
// baseline of handling-errors.md), an HRESULT entry returns E_NOTIMPL through ddi_status_note, and an entry with
// another return type answers 0. Name is the entry name for the diagnostic line.
template<typename Callback,const char *Name> struct Unexpected;
template<typename R,typename... Args,const char *Name>
struct Unexpected<R (APIENTRY *)(D3D10DDI_HDEVICE,Args...),Name> {
    static R APIENTRY call(D3D10DDI_HDEVICE h,Args...) {
        static std::atomic_bool logged{false};
        if(!logged.exchange(true))failure_diagnostic(Name,E_NOTIMPL);
        if constexpr(std::is_void_v<R>) {
            auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
            if(storage && storage->owner) {
                RuntimeDomain::Scope scope(storage->owner->runtime().domain);
                report_ddi_error(*storage->owner,D3DDDIERR_DEVICEREMOVED);
            }
        }else if constexpr(std::is_same_v<R,HRESULT>)return ddi_status_note(Name,E_NOTIMPL,DdiInputs("an entry this shell does not implement"));
        else { ddi_status_note(Name,E_NOTIMPL,DdiInputs("an entry this shell does not implement answers 0")); return 0; }
    }
};
}
