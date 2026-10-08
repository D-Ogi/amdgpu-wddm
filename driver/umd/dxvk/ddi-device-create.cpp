// SPDX-License-Identifier: MIT
#include "ddi-device-create.h"
#include "ddi-table.h"
#include "ddi-negotiation.h"
#include "ddi-dxgi-table.h"
#include "ddi-wddm2.h"
#include "ddi-wddm22.h"
namespace bc250::umd {
HRESULT create_render_device(const D3D10DDIARG_CREATEDEVICE &args,UINT64 luid,
    PFN_vkGetInstanceProcAddr get,const BC250_DXVK_ENGINE_FUNCS &funcs,D3D_FEATURE_LEVEL level,
    const BC250_DXVK_SHELL_SERVICES &services,DdiDeviceHandle &failedCleanup,const AdapterCaps &advertised,
    UINT32 policy_flags) noexcept {
    if (failedCleanup.owner) return E_UNEXPECTED;
    const bool wddm2_0=args.Interface==D3DWDDM2_0_DDI_INTERFACE_VERSION;
    // BD-099: the WDDM 2.2 interface, which gives this shell the whole DXGIDDICB_PRESENT. Only at the build
    // the adapter advertised: an older build of the same interface has a shorter present callback and no
    // DXGI 1.6.1 table, so it is not this table's contract.
    const bool wddm2_2=wddm2_2_ddi(args.Interface,args.Version);
    // The unions alias: p11_1DeviceFuncs/pWDDM2_0DeviceFuncs/pWDDM2_2DeviceFuncs, and
    // pDXGIDDIBaseFunctions3/5/6_1.
    if ((args.Interface!=D3D11_1_DDI_INTERFACE_VERSION && !wddm2_0 && !wddm2_2) || !args.hDrvDevice.pDrvPrivate ||
        !args.p11_1DeviceFuncs || !args.DXGIBaseDDI.pDXGIDDIBaseFunctions3)
        return E_INVALIDARG;
    if ((wddm2_0 || wddm2_2) && advertised.maximum<D3D_FEATURE_LEVEL_12_0) return E_INVALIDARG;
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
    // The build the adapter advertises is the first that asks for the DXGI 1.6.1 table and gives the whole
    // present callback. The two go together in the runtime, so one check covers both.
    static_assert(dxgi1_6_1_table_layout_supported(D3DWDDM2_2_DDI_INTERFACE_VERSION,D3DWDDM2_2_DDI_BUILD_VERSION));
    static_assert(!dxgi1_6_1_table_layout_supported(D3DWDDM2_2_DDI_INTERFACE_VERSION,D3DWDDM2_2_DDI_BUILD_VERSION-1));
    if (wddm2_2) {
        *args.DXGIBaseDDI.pDXGIDDIBaseFunctions6_1=make_dxgi1_6_1_device_table();
        *args.pWDDM2_2DeviceFuncs=make_wddm2_2_device_table();
    } else if (wddm2_0) {
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
