// SPDX-License-Identifier: MIT
#include "ddi-device-create.h"
#include "ddi-table.h"
#include "ddi-negotiation.h"
#include "ddi-dxgi-table.h"
#include "ddi-wddm2.h"
namespace bc250::umd {
HRESULT create_render_device(const D3D10DDIARG_CREATEDEVICE &args,UINT64 luid,
    PFN_vkGetInstanceProcAddr get,const BC250_DXVK_ENGINE_FUNCS &funcs,D3D_FEATURE_LEVEL level,
    const BC250_DXVK_SHELL_SERVICES &services,DdiDeviceHandle &failedCleanup,const AdapterCaps &advertised,
    UINT32 policy_flags,const ScanoutSource *scanout) noexcept {
    if (failedCleanup.owner) return E_UNEXPECTED;
    const bool wddm2_0=args.Interface==D3DWDDM2_0_DDI_INTERFACE_VERSION;
    // The two unions alias: p11_1DeviceFuncs/pWDDM2_0DeviceFuncs, pDXGIDDIBaseFunctions3/5.
    if ((args.Interface!=D3D11_1_DDI_INTERFACE_VERSION && !wddm2_0) || !args.hDrvDevice.pDrvPrivate ||
        !args.p11_1DeviceFuncs || !args.DXGIBaseDDI.pDXGIDDIBaseFunctions3)
        return E_INVALIDARG;
    if (wddm2_0 && advertised.maximum<D3D_FEATURE_LEVEL_12_0) return E_INVALIDARG;
    auto *storage=static_cast<DdiDeviceHandle *>(args.hDrvDevice.pDrvPrivate);
    // Runtime private memory need not be initialized. No owner is published
    // until initialization succeeds; failure must leave no dangling handle.
    storage->owner=nullptr;
    D3D_FEATURE_LEVEL requested{};
    HRESULT negotiated=requested_feature_level(args.Flags,requested,args.Interface);
    if (FAILED(negotiated) || requested!=level || !valid_adapter_caps(advertised) ||
        level>advertised.maximum) return E_INVALIDARG;
    DeviceOwner *owner=new(std::nothrow) DeviceOwner;
    if (!owner) return E_OUTOFMEMORY;
    if (scanout) owner->set_scanout(*scanout);
    HRESULT hr=E_FAIL,cleanup=S_OK;
    {
        RuntimeDomain::Scope scope(owner->runtime().domain);
        try {
            hr=owner->initialize(args,luid,get,funcs,level,services,policy_flags);
            if (SUCCEEDED(hr)) {
                IBc250DxvkDevice3 *capsEngine=nullptr;
                hr=owner->engine()->QueryInterface(__uuidof(IBc250DxvkDevice3),reinterpret_cast<void **>(&capsEngine));
                if (SUCCEEDED(hr)) {
                    if (!capsEngine) hr=E_NOINTERFACE;
                    else {
                        struct ReleaseCaps { IBc250DxvkDevice3 *p; ~ReleaseCaps(){p->Release();} } release{capsEngine};
                        hr=verify_adapter_caps(advertised,[&](D3D_FEATURE_LEVEL maximum,D3D11_FEATURE feature,void *data,UINT size) {
                            return capsEngine->CheckFeatureSupportAtLevel(maximum,feature,data,size);
                        });
                    }
                }
            }
        }
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
    // These tables are a matched pair for the negotiated interface, published only after initialization.
    static_assert(dxgi_table_layout_supported(D3D11_1_DDI_INTERFACE_VERSION,0));
    static_assert(dxgi1_4_table_layout_supported(D3DWDDM2_0_DDI_INTERFACE_VERSION,D3DWDDM2_0_DDI_BUILD_VERSION));
    if (wddm2_0) {
        *args.DXGIBaseDDI.pDXGIDDIBaseFunctions5=make_dxgi1_4_device_table();
        *args.pWDDM2_0DeviceFuncs=make_wddm2_0_device_table();
    } else {
        *args.DXGIBaseDDI.pDXGIDDIBaseFunctions3=make_dxgi_device_table();
        *args.p11_1DeviceFuncs=make_render_device_table();
    }
    storage->owner=owner;
    return S_OK;
}
}
