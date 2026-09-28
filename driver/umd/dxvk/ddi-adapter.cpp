// SPDX-License-Identifier: MIT
#include "ddi-adapter.h"
#include "adapter-identity.h"
#include "ddi-negotiation.h"
#include <mutex>
#include <string>
#include <memory>
namespace bc250::umd {
namespace {
struct Adapter {
    UINT64 luid=0;
    AdapterCaps caps{};
    std::wstring engine_path,icd_path;
    EngineModules modules;
    std::mutex mutex;
    std::vector<DdiDeviceHandle> failed;
};
Adapter *adapter(D3D10DDI_HADAPTER handle) {return static_cast<Adapter *>(handle.pDrvPrivate);}
bool compatible_device(const Adapter &a,UINT interfaceVersion,UINT version,UINT flags,D3D_FEATURE_LEVEL &level) {
    return interfaceVersion==D3D11_1_DDI_INTERFACE_VERSION &&
        (version>>16)==D3D11_1_DDI_BUILD_VERSION &&
        SUCCEEDED(requested_feature_level(flags,level)) && level<=a.caps.maximum;
}
SIZE_T APIENTRY device_size(D3D10DDI_HADAPTER handle,const D3D10DDIARG_CALCPRIVATEDEVICESIZE *args) {
    auto *a=adapter(handle);D3D_FEATURE_LEVEL level{};
    if (!a || !args || !compatible_device(*a,args->Interface,args->Version,args->Flags,level)) return 0;
    return sizeof(DdiDeviceHandle);
}
HRESULT APIENTRY versions(D3D10DDI_HADAPTER handle,UINT32 *entries,UINT64 *values) {
    if (!adapter(handle)) return E_INVALIDARG;
    return supported_ddi_versions(entries,values);
}
HRESULT APIENTRY caps(D3D10DDI_HADAPTER handle,const D3D10_2DDIARG_GETCAPS *args) {
    auto *a=adapter(handle);
    return a && args ? get_adapter_caps(a->caps,*args) : E_INVALIDARG;
}
HRESULT APIENTRY create(D3D10DDI_HADAPTER handle,D3D10DDIARG_CREATEDEVICE *args) {
    auto *a=adapter(handle);D3D_FEATURE_LEVEL level{};
    if (!a || !args || !compatible_device(*a,args->Interface,args->Version,args->Flags,level) ||
        !args->hDrvDevice.pDrvPrivate || !args->p11_1DeviceFuncs || !args->DXGIBaseDDI.pDXGIDDIBaseFunctions3) return E_INVALIDARG;
    static_cast<DdiDeviceHandle *>(args->hDrvDevice.pDrvPrivate)->owner=nullptr;
    try {
        std::lock_guard<std::mutex> lock(a->mutex);
        if (!a->modules.loaded()) {
            HRESULT hr=a->modules.open(a->engine_path.c_str(),a->icd_path.c_str());
            if (FAILED(hr)) return hr;
        }
        // Allocate retention storage before any callback can create live state.
        a->failed.emplace_back();
        BC250_DXVK_SHELL_SERVICES services{};services.Size=sizeof(services);
        HRESULT hr=create_render_device(*args,a->luid,a->modules,level,services,a->failed.back(),a->caps);
        if (!a->failed.back().owner) a->failed.pop_back();
        return hr;
    } catch (const std::bad_alloc &) {return E_OUTOFMEMORY;}
      catch (...) {return E_FAIL;}
}
HRESULT APIENTRY close(D3D10DDI_HADAPTER handle) {
    auto *a=adapter(handle);if (!a) return E_INVALIDARG;
    // Runtime has destroyed every successful device before CloseAdapter.
    // Failed CreateDevice handles are already invalid: never retry runtime
    // cleanup through them. Retain the adapter and pinned owners until exit.
    if (!a->failed.empty()) {
        OutputDebugStringA("M14: retaining adapter with failed device cleanup\n");
        return S_OK;
    }
    delete a;return S_OK;
}
}
HRESULT open_render_adapter(D3D10DDIARG_OPENADAPTER &args,const AdapterConfiguration &config) noexcept {
    if (!args.pAdapterFuncs_2 || !args.pAdapterCallbacks || !valid_adapter_caps(config.caps) ||
        !EngineModules::absolute_path(config.engine_path) || !EngineModules::absolute_path(config.icd_path)) return E_INVALIDARG;
    try {
        auto a=std::make_unique<Adapter>();
        a->caps=config.caps;a->engine_path=config.engine_path;a->icd_path=config.icd_path;
        HRESULT hr=query_adapter_identity(args.hRTAdapter.handle,args.pAdapterCallbacks->pfnQueryAdapterInfoCb,a->luid);
        if (FAILED(hr)) return hr;
        D3D10_2DDI_ADAPTERFUNCS table{};
        table.pfnCalcPrivateDeviceSize=device_size;table.pfnCreateDevice=create;
        table.pfnCloseAdapter=close;table.pfnGetSupportedVersions=versions;table.pfnGetCaps=caps;
        *args.pAdapterFuncs_2=table;args.hAdapter.pDrvPrivate=a.release();
        return S_OK;
    } catch (const std::bad_alloc &) {return E_OUTOFMEMORY;}
      catch (...) {return E_FAIL;}
}
}
