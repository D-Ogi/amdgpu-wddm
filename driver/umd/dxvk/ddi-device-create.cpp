// SPDX-License-Identifier: MIT
#include "ddi-device-create.h"
#include "ddi-table.h"
namespace bc250::umd {
HRESULT create_render_device(const D3D10DDIARG_CREATEDEVICE &args,UINT64 luid,
    PFN_vkGetInstanceProcAddr get,const BC250_DXVK_ENGINE_FUNCS &funcs,D3D_FEATURE_LEVEL level,
    const BC250_DXVK_SHELL_SERVICES &services,DdiDeviceHandle &failedCleanup) noexcept {
    if (failedCleanup.owner) return E_UNEXPECTED;
    if (args.Interface!=D3D11_1_DDI_INTERFACE_VERSION || !args.hDrvDevice.pDrvPrivate || !args.p11_1DeviceFuncs)
        return E_INVALIDARG;
    auto *storage=static_cast<DdiDeviceHandle *>(args.hDrvDevice.pDrvPrivate);
    // Runtime private memory need not be initialized. No owner is published
    // until initialization succeeds; failure must leave no dangling handle.
    storage->owner=nullptr;
    DeviceOwner *owner=new(std::nothrow) DeviceOwner;
    if (!owner) return E_OUTOFMEMORY;
    HRESULT hr=E_FAIL,cleanup=S_OK;
    {
        RuntimeDomain::Scope scope(owner->runtime().domain);
        try { hr=owner->initialize(args,luid,get,funcs,level,services); }
        catch (const std::bad_alloc &) { hr=E_OUTOFMEMORY; }
        catch (...) { hr=E_FAIL; }
        if (FAILED(hr)) {
            try { cleanup=owner->close(); }
            catch (...) { cleanup=E_FAIL; }
        }
    }
    if (FAILED(hr)) {
        if (FAILED(cleanup) || owner->has_live_objects()) failedCleanup.owner=owner;
        else delete owner;
        // HRESULT return is the CreateDevice error channel, not SetErrorCb.
        return ddi_device_status(hr);
    }
    *args.p11_1DeviceFuncs=make_render_device_table();
    storage->owner=owner;
    return S_OK;
}
}
