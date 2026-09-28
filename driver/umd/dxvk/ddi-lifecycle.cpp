// SPDX-License-Identifier: MIT
#include "ddi-lifecycle.h"
namespace bc250::umd {
HRESULT retire_device_handle(DdiDeviceHandle &handle) noexcept {
    auto *owner=handle.owner;
    if (!owner) return S_OK;
    HRESULT hr=E_FAIL;
    {
        RuntimeDomain::Scope scope(owner->runtime().domain);
        try { hr=owner->close(); }
        catch (const std::bad_alloc &) { hr=E_OUTOFMEMORY; }
        catch (...) { hr=E_FAIL; }
        if (SUCCEEDED(hr) && owner->has_live_objects()) hr=E_UNEXPECTED;
        if (FAILED(hr)) report_ddi_error(*owner,hr);
    }
    // Scope destructor refers to the owner's domain. Never delete it while
    // that scope is active, even after the last Vulkan/runtime object is gone.
    if (SUCCEEDED(hr)) { handle.owner=nullptr; delete owner; }
    return hr;
}
namespace {
void APIENTRY destroy(D3D10DDI_HDEVICE h) {
    auto *handle=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if (handle) (void)retire_device_handle(*handle);
    // Runtime may free handle storage on return. Failed retirement deliberately
    // retains the heap owner rather than invalidating live callback userdata.
    // DLL lifetime retention belongs to the adapter/module owner integration.
}
}
void install_lifecycle_ddi(D3D11_1DDI_DEVICEFUNCS &t) { t.pfnDestroyDevice=destroy; }
}
