// SPDX-License-Identifier: MIT
#include "ddi-adapter.h"
#include "adapter-identity.h"
#include "ddi-negotiation.h"
#include "ddi-experiment.h"
#include "diagnostics.h"
#include "../d3d12/instance-policy.h"
#include "../recent-launch/recent-launch.h"
#include "../app-settings/app-settings.h"
#include "../d3d12/stdio-log.h"
#include <mutex>
#include <string>
#include <memory>
namespace bc250::umd {
namespace {
struct Adapter {
    UINT64 luid=0;
    std::atomic_uint observations{0};
    AdapterCaps caps{};
    UINT32 policy_flags=0;
    D3DKMT_HANDLE unresolved_policy_adapter=0; // a KMT adapter handle the policy query could not close
    unsigned char engine_sha256[32]{},icd_sha256[32]{};
    std::wstring engine_path,icd_path;
    EngineModules modules;
    std::mutex mutex;
    std::vector<DdiDeviceHandle> failed;
    // M15.14: the runtime's adapter query and the scan-out switches, read once at the open. The runtime's
    // adapter handle stays valid until CloseAdapter, and every device is destroyed before that.
    ScanoutSource scanout;
};
Adapter *adapter(D3D10DDI_HADAPTER handle) {return static_cast<Adapter *>(handle.pDrvPrivate);}
// The settings this shell applies; RenderOnCpu is the router's (driver/umd/router), so a set value shows here as
// not applied: the router kept the application on the GPU (a protected application, for example).
constexpr unsigned kAppliedSettings=amdgpu_wddm::app_settings::kAll &
    ~amdgpu_wddm::app_settings::bit(amdgpu_wddm::app_settings::Setting::RenderOnCpu);
void log_app_settings(const amdgpu_wddm::app_settings::Settings &settings) noexcept {
    amdgpu_wddm::app_settings::log_once(settings,"d3d11",kAppliedSettings,[](const char *line) {
        char text[1100];
        std::snprintf(text,sizeof(text),"%s\n",line);
        OutputDebugStringA(text);
        amdgpu_wddm_log::print("%s",text);
    });
}
bool compatible_device(const Adapter &a,UINT interfaceVersion,UINT version,UINT flags,D3D_FEATURE_LEVEL &level) {
    const bool d3d11_1=interfaceVersion==D3D11_1_DDI_INTERFACE_VERSION && (version>>16)==D3D11_1_DDI_BUILD_VERSION;
    const bool fl12=a.caps.maximum>=D3D_FEATURE_LEVEL_12_0;
    // The WDDM 2.0 and WDDM 2.2 tables are offered only by an FL12 adapter (supported_ddi_versions). A
    // runtime that asks for the WDDM 2.2 interface which the process switch withholds gets no device from
    // this table: it asks again for the newest interface the adapter did offer.
    const bool wddm2_0=wddm2_0_ddi(interfaceVersion,version) && fl12;
    const bool wddm2_2=wddm2_2_ddi(interfaceVersion,version) && fl12 && wddm2_2_offered();
    return (d3d11_1 || wddm2_0 || wddm2_2) &&
        SUCCEEDED(requested_feature_level(flags,level,interfaceVersion)) && level<=a.caps.maximum;
}
template<typename T> T system_entry(HMODULE module,const char *name) noexcept {
    return reinterpret_cast<T>(reinterpret_cast<void *>(GetProcAddress(module,name)));
}
// The D3D12 shell's reading of the adapter's software key, on the admitted LUID.
HRESULT system_sparse_policy(UINT64 luid,bool &sparse,D3DKMT_HANDLE &unresolved) noexcept {
    sparse=false;unresolved=0;
    HMODULE gdi=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!gdi)return HRESULT_FROM_WIN32(GetLastError());
    const native12::MemoryPolicyKmt kmt{
        system_entry<PFND3DKMT_OPENADAPTERFROMLUID>(gdi,"D3DKMTOpenAdapterFromLuid"),
        system_entry<PFND3DKMT_QUERYADAPTERINFO>(gdi,"D3DKMTQueryAdapterInfo"),
        system_entry<PFND3DKMT_CLOSEADAPTER>(gdi,"D3DKMTCloseAdapter")};
    LUID id{};std::memcpy(&id,&luid,sizeof(id));
    native12::SparsePolicy policy{};
    const HRESULT hr=native12::query_sparse_policy(id,kmt,&policy,&unresolved);
    FreeLibrary(gdi);
    sparse=hr==S_OK && policy.sparse;
    char text[128];
    std::snprintf(text,sizeof(text),"M14: instance policy sparse=%u source=%u status=%08lX result=%08lX\n",
        unsigned(sparse),unsigned(policy.source),static_cast<unsigned long>(policy.status),static_cast<unsigned long>(hr));
    OutputDebugStringA(text);
    return hr;
}
SIZE_T APIENTRY device_size(D3D10DDI_HADAPTER handle,const D3D10DDIARG_CALCPRIVATEDEVICESIZE *args) {
    auto *a=adapter(handle);D3D_FEATURE_LEVEL level{};
    if (!a || !args) return 0;
    if(!(a->observations.fetch_or(1,std::memory_order_relaxed)&1))
        adapter_diagnostic("CalcPrivateDeviceSize",args->Interface,args->Version,args->Flags);
    if (!compatible_device(*a,args->Interface,args->Version,args->Flags,level)) return 0;
    return sizeof(DdiDeviceHandle);
}
HRESULT APIENTRY versions(D3D10DDI_HADAPTER handle,UINT32 *entries,UINT64 *values) {
    auto *a=adapter(handle);
    if (!a) return E_INVALIDARG;
    const bool wddm2_2=wddm2_2_offered();
    const HRESULT hr=supported_ddi_versions(a->caps.maximum,wddm2_2,entries,values);
    // The offered set, once per adapter, read out of the array the shell just filled: the runtime asks twice
    // (count, then array), and an FL11 adapter offers the D3D11.1 table alone.
    if (SUCCEEDED(hr) && values && !(a->observations.fetch_or(4,std::memory_order_relaxed)&4)) {
        char list[64]{}; size_t used=0;
        for (UINT32 i=0;i<*entries;++i) {
            const char *parts[2]={i ? " + " : "",ddi_version_name(values[i])};
            for (const char *part:parts)
                for (const char *p=part;*p && used+1<sizeof(list);++p) list[used++]=*p;
        }
        list[used]=0;
        char text[192];
        std::snprintf(text,sizeof(text),"M14 DDI versions: %s (BD-099 experiment wddm22-ddi-off=%u)\n",
            list,unsigned(!wddm2_2));
        OutputDebugStringA(text);
        amdgpu_wddm_log::print("%s",text);
    }
    return hr;
}
HRESULT APIENTRY caps(D3D10DDI_HADAPTER handle,const D3D10_2DDIARG_GETCAPS *args) {
    auto *a=adapter(handle);
    return a && args ? get_adapter_caps(a->caps,*args) : E_INVALIDARG;
}
HRESULT APIENTRY create(D3D10DDI_HADAPTER handle,D3D10DDIARG_CREATEDEVICE *args) {
    auto *a=adapter(handle);D3D_FEATURE_LEVEL level{};
    if (!a || !args) return E_INVALIDARG;
    if(!(a->observations.fetch_or(2,std::memory_order_relaxed)&2))
        adapter_diagnostic("CreateDevice",args->Interface,args->Version,args->Flags);
    // p11_1DeviceFuncs, pWDDM2_0DeviceFuncs, pWDDM2_2DeviceFuncs and pDXGIDDIBaseFunctions3/5/6_1 share
    // their unions, so one null check covers whichever member the negotiated interface names.
    if (!compatible_device(*a,args->Interface,args->Version,args->Flags,level) ||
        !args->hDrvDevice.pDrvPrivate || !args->p11_1DeviceFuncs || !args->DXGIBaseDDI.pDXGIDDIBaseFunctions3) return E_INVALIDARG;
    static_cast<DdiDeviceHandle *>(args->hDrvDevice.pDrvPrivate)->owner=nullptr;
    try {
        std::lock_guard<std::mutex> lock(a->mutex);
        if (!a->modules.loaded()) {
            HRESULT hr=a->modules.open(a->engine_path.c_str(),a->icd_path.c_str(),a->engine_sha256,a->icd_sha256);
            if (FAILED(hr)) {failure_diagnostic("module load",hr);return hr;}
        }
        // Allocate retention storage before any callback can create live state.
        a->failed.emplace_back();
        BC250_DXVK_SHELL_SERVICES services{};services.Size=sizeof(services);services.Log=engine_diagnostic;
        // The per-application settings: once per process, the effective values in the log; Anisotropy and
        // PerformanceOverlay as DXVK user options, which the engine's DXVK instance reads in this call only.
        namespace as=amdgpu_wddm::app_settings;
        const as::Settings &settings=as::process_settings();
        log_app_settings(settings);
        HRESULT hr=S_OK;
        {
            const as::ScopedEnv options("DXVK_CONFIG",as::dxvk_config(settings),as::ScopedEnv::Mode::Append);
            hr=create_render_device(*args,a->luid,a->modules,level,services,a->failed.back(),a->caps,a->policy_flags,
                &a->scanout);
        }
        if (!a->failed.back().owner) a->failed.pop_back();
        if(FAILED(hr))failure_diagnostic("CreateDevice",hr);
        // The outer device exists: note the launch once per process, off this thread (recent-launch.h).
        else amdgpu_wddm::recent_launch::note_outer_device(amdgpu_wddm::recent_launch::ApiD3D11);
        return hr;
    } catch (const std::bad_alloc &) {return E_OUTOFMEMORY;}
      catch (...) {return E_FAIL;}
}
HRESULT APIENTRY close(D3D10DDI_HADAPTER handle) {
    auto *a=adapter(handle);if (!a) return E_INVALIDARG;
    // Runtime has destroyed every successful device before CloseAdapter.
    // Failed CreateDevice handles are already invalid: never retry runtime
    // cleanup through them. Retain the adapter and pinned owners until exit,
    // and likewise an adapter whose policy query left a KMT handle open.
    if (!a->failed.empty() || a->unresolved_policy_adapter) {
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
        a->engine_path=config.engine_path;a->icd_path=config.icd_path;
        std::memcpy(a->engine_sha256,config.engine_sha256,sizeof(a->engine_sha256));
        std::memcpy(a->icd_sha256,config.icd_sha256,sizeof(a->icd_sha256));
        HRESULT hr=query_adapter_identity(args.hRTAdapter.handle,args.pAdapterCallbacks->pfnQueryAdapterInfoCb,a->luid);
        if (FAILED(hr)) return hr;
        // Read once per adapter, before any device: every device and the advertised caps get the same answer.
        bool sparse=false;
        const HRESULT policy=(config.sparse_policy ? config.sparse_policy : system_sparse_policy)(
            a->luid,sparse,a->unresolved_policy_adapter);
        a->policy_flags=policy==S_OK && sparse ? BC250_HOST_POLICY_SPARSE : 0;
        a->caps=adapter_caps_for_policy(config.caps,a->policy_flags);
        a->scanout=read_scanout_source(args.hRTAdapter.handle,args.pAdapterCallbacks->pfnQueryAdapterInfoCb);
        D3D10_2DDI_ADAPTERFUNCS table{};
        table.pfnCalcPrivateDeviceSize=device_size;table.pfnCreateDevice=create;
        table.pfnCloseAdapter=close;table.pfnGetSupportedVersions=versions;table.pfnGetCaps=caps;
        *args.pAdapterFuncs_2=table;args.hAdapter.pDrvPrivate=a.release();
        return S_OK;
    } catch (const std::bad_alloc &) {return E_OUTOFMEMORY;}
      catch (...) {return E_FAIL;}
}
}
