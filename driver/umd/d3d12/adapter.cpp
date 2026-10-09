// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12umddi.h>
#include <cstdio>
#include <new>
#include <atomic>
#include <cstdint>
#include "device-state.h"
#include "device-engine.h"
#include "native-tables.h"
#include "adapter-caps.h"
#include "ddi-0092-layout.h"
#include "ddi-trace.h"
#include <cstring>
#include "stdio-log.h"
#include "../recent-launch/recent-launch.h"
namespace {
using native12::Adapter;
using native12::Device;
// The per-application settings this shell applies (docs/design/per-app-graphics-settings.md). PerformanceOverlay
// is D3D11 only, RenderOnCpu is the router's: a set value of those shows as not applied.
constexpr unsigned kAppliedSettings=amdgpu_wddm::app_settings::bit(amdgpu_wddm::app_settings::Setting::FrameRateLimit) |
    amdgpu_wddm::app_settings::bit(amdgpu_wddm::app_settings::Setting::VSync) |
    amdgpu_wddm::app_settings::bit(amdgpu_wddm::app_settings::Setting::Anisotropy) |
    amdgpu_wddm::app_settings::bit(amdgpu_wddm::app_settings::Setting::MaxFrameLatency);
bool supported(UINT interfaceVersion,UINT runtimeVersion) noexcept {
    return interfaceVersion==D3D12DDI_INTERFACE_VERSION_R8 &&
        (runtimeVersion>>16)==D3D12DDI_BUILD_VERSION_0092;
}
void trace(const char* operation,unsigned long long value=0) noexcept {
    amdgpu_wddm_log::print("d3d12-ddi %s %llu\n",operation,value);amdgpu_wddm_log::flush();
}
SIZE_T APIENTRY device_size(D3D12DDI_HADAPTER h,const D3D12DDIARG_CALCPRIVATEDEVICESIZE* a) {
    if(!h.pDrvPrivate || !a) return 0;
    trace("CalcPrivateDeviceSize-interface",a->Interface);
    trace("CalcPrivateDeviceSize-version",a->Version);trace("CalcPrivateDeviceSize-flags",a->Flags);
    return supported(a->Interface,a->Version)?sizeof(Device):0;
}
HRESULT APIENTRY create_device(D3D12DDI_HADAPTER h,const D3D12DDIARG_CREATEDEVICE_0003* a) {
    if(!h.pDrvPrivate || !a) return E_INVALIDARG;
    trace("CreateDevice-interface",a->Interface);trace("CreateDevice-version",a->Version);trace("CreateDevice-flags",a->Flags);
    if(!supported(a->Interface,a->Version)) return E_NOINTERFACE;
    if(!a->hDrvDevice.pDrvPrivate || reinterpret_cast<uintptr_t>(a->hDrvDevice.pDrvPrivate)%alignof(Device) ||
       !a->p12UMCallbacks_0062 || !a->pKTCallbacks) return E_INVALIDARG;
    const auto& cb=*a->p12UMCallbacks_0062;
    if(!a->hRTDevice.handle || !cb.pfnSetErrorCb || !cb.pfnCreateContextVirtualCb || !cb.pfnDestroyContextCb ||
       !cb.pfnAllocateCb || !cb.pfnDeallocateCb || !cb.pfnSetCommandListDDITableCb || !cb.pfnSetCommandListErrorCb) return E_INVALIDARG;
    auto adapter=static_cast<Adapter*>(h.pDrvPrivate);
    auto device=new(a->hDrvDevice.pDrvPrivate) Device{adapter,a->hRTDevice,cb,*a->pKTCallbacks};
    // The per-application settings: once per process, the effective values in the log; Anisotropy as the
    // vkd3d-proton fork's VKD3D_SAMPLER_ANISOTROPY, which the engine reads when it creates this device.
    namespace as=amdgpu_wddm::app_settings;
    const as::Settings& settings=as::process_settings();
    as::log_once(settings,"d3d12",kAppliedSettings,[](const char* line) {
        char text[1100];
        std::snprintf(text,sizeof(text),"%s\n",line);
        OutputDebugStringA(text);
        amdgpu_wddm_log::print("%s",text);
    });
    HRESULT hr=E_OUTOFMEMORY;
    try {
        const as::ScopedEnv anisotropy("VKD3D_SAMPLER_ANISOTROPY",as::vkd3d_anisotropy(settings),
            as::ScopedEnv::Mode::Replace);
        hr=native12::create_device_engine(*device);
    } catch(...) {
        hr=E_OUTOFMEMORY;
    }
    if(FAILED(hr)){device->~Device();trace("CreateDevice-engine-failed",static_cast<unsigned>(hr));return hr;}
    ++adapter->devices;
    // The outer device exists: note the launch once per process, off this thread (recent-launch.h).
    amdgpu_wddm::recent_launch::note_outer_device(amdgpu_wddm::recent_launch::ApiD3D12);
    return S_OK;
}
HRESULT APIENTRY close_adapter(D3D12DDI_HADAPTER h) {
    if(!h.pDrvPrivate) return E_INVALIDARG;
    auto adapter=static_cast<Adapter*>(h.pDrvPrivate);
    if(adapter->devices.load()!=0 || adapter->retained_engines.load()!=0) return E_UNEXPECTED;
    native12::close_adapter_caps(*adapter);delete adapter;trace("CloseAdapter");return S_OK;
}
HRESULT APIENTRY versions(D3D12DDI_HADAPTER h,UINT32* count,UINT64* values) {
    if(!h.pDrvPrivate || !count) return E_INVALIDARG;
    const UINT32 capacity=*count;*count=1;trace("GetSupportedVersions",capacity);
    if(!values) return S_OK;
    if(capacity<1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    // Diagnostic negotiation target only. Unimplemented capability/table queries remain fail-closed.
    values[0]=D3D12DDI_SUPPORTED_0092;return S_OK;
}
HRESULT APIENTRY caps(D3D12DDI_HADAPTER h,const D3D12DDIARG_GETCAPS* a) {
    const auto trace_id=native12::ddi_trace_begin("pfnGetCaps");
    if(!h.pDrvPrivate || !a || (!a->pData && a->DataSize)){
        native12::ddi_trace_end("pfnGetCaps",trace_id,E_INVALIDARG);return E_INVALIDARG;
    }
    HRESULT hr=native12::get_adapter_caps(*static_cast<Adapter*>(h.pDrvPrivate),a);
    if(hr==S_OK && a->pData && native12::ddi_trace_enabled()){
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        if(a->Type==D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE && a->DataSize==sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041)){
            const auto& value=*static_cast<const D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041*>(a->pData);
            UINT node=0;if(a->pInfo)std::memcpy(&node,a->pInfo,sizeof(node));
            amdgpu_wddm_log::print("{\"event\":\"ddi-caps-memory\",\"type\":1002,\"node\":%u,\"uma\":%u,\"io_coherent\":%u,\"cache_coherent\":%u,\"heap_serialization\":%u,\"resource_serialization\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
                node,unsigned(value.UMA),unsigned(value.IOCoherent),unsigned(value.CacheCoherent),
                unsigned(value.HeapSerializationTier),unsigned(value.ResourceSerializationTier),now.QuadPart,GetCurrentThreadId());
        }
        if(a->Type==D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT && !a->pInfo && a->DataSize==sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0026)){
            const auto& value=*static_cast<const D3D12DDI_TEXTURE_LAYOUT_CAPS_0026*>(a->pData);
            amdgpu_wddm_log::print("{\"event\":\"ddi-caps-layout\",\"type\":1060,\"layouts\":%u,\"swizzles\":%u,\"standard64k\":%u,\"row_major\":%u,\"indexable\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
                value.DeviceDependentLayoutCount,value.DeviceDependentSwizzleCount,unsigned(value.Supports64KStandardSwizzle),
                unsigned(value.SupportsRowMajorTexture),unsigned(value.IndexableSwizzlePatterns),now.QuadPart,GetCurrentThreadId());
        }
        if(a->Type==D3D12DDICAPS_TYPE_SHADER && a->DataSize==sizeof(D3D12DDI_SHADER_CAPS_0084)){
            UINT words[16]{};std::memcpy(words,a->pData,sizeof(words));
            amdgpu_wddm_log::print("d3d12-caps shader0084 words=");
            for(auto word:words)amdgpu_wddm_log::print(" %08x",word);
            amdgpu_wddm_log::print("\n");
        }
        if(a->Type==D3D12DDICAPS_TYPE_0011_SHADER_MODELS && a->DataSize==sizeof(D3D12DDI_D3D12_SHADER_MODELS_DATA_0011)){
            const auto* models=static_cast<const D3D12DDI_D3D12_SHADER_MODELS_DATA_0011*>(a->pData);
            if(models->pNumShaderModelsSupported){
                const UINT count=*models->pNumShaderModelsSupported;
                amdgpu_wddm_log::print("d3d12-caps shader-models count=%u array=%u values=",count,unsigned(models->pShaderModelsSupported!=nullptr));
                if(models->pShaderModelsSupported && count<=64)
                    for(UINT i=0;i<count;++i)amdgpu_wddm_log::print(" %08x",unsigned(models->pShaderModelsSupported[i]));
                amdgpu_wddm_log::print("\n");
            }
        }
    }
    amdgpu_wddm_log::print("d3d12-ddi GetCaps type=%u size=%u info_present=%u result=%08lx\n",
        unsigned(a->Type),a->DataSize,unsigned(a->pInfo!=nullptr),static_cast<unsigned long>(hr));amdgpu_wddm_log::flush();
    if(native12::ddi_trace_enabled()){
        amdgpu_wddm_log::print("{\"event\":\"ddi-caps\",\"type\":%u,\"data_size\":%u,\"info_present\":%u,\"status\":\"%08lx\"}\n",
            unsigned(a->Type),a->DataSize,unsigned(a->pInfo!=nullptr),static_cast<unsigned long>(hr));
        if(a->Type==D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS && a->pInfo && a->DataSize==sizeof(D3D12DDI_ROW_MAJOR_LAYOUT_CAPS)){
            UINT info[2]{};std::memcpy(info,a->pInfo,sizeof(info));
            amdgpu_wddm_log::print("{\"event\":\"ddi-layout-set\",\"layout\":%u,\"unit\":%u,\"status\":\"%08lx\"}\n",
                info[0],info[1],static_cast<unsigned long>(hr));
        }
        amdgpu_wddm_log::flush();
    }
    native12::ddi_trace_end("pfnGetCaps",trace_id,hr);return hr;
}
HRESULT APIENTRY optional_tables(D3D12DDI_HADAPTER h,UINT32* count,D3D12DDI_TABLE_REQUEST*) {
    if(!h.pDrvPrivate || !count) return E_INVALIDARG;
    *count=0;trace("GetOptionalDDITables");return S_OK;
}
HRESULT APIENTRY fill_table(D3D12DDI_HADAPTER h,D3D12DDI_TABLE_TYPE type,void* output,SIZE_T size,UINT number,D3D12DDI_HRTTABLE table) {
    HRESULT hr=h.pDrvPrivate?native12::fill_native_tables(*static_cast<Adapter*>(h.pDrvPrivate),type,output,size,number,table):E_INVALIDARG;
    amdgpu_wddm_log::print("d3d12-ddi FillDDITable type=%u size=%llu number=%u runtime_table_present=%u result=%08lx\n",
        unsigned(type),static_cast<unsigned long long>(size),number,unsigned(table.handle!=nullptr),static_cast<unsigned long>(hr));amdgpu_wddm_log::flush();
    return hr;
}
void APIENTRY destroy_device(D3D12DDI_HDEVICE h) {
    if(!h.pDrvPrivate) return;
    auto device=static_cast<Device*>(h.pDrvPrivate);auto adapter=device->adapter;
    native12::destroy_device_engine(*device);
    unsigned retired=0,active=0;
    device->queues.discard_device_metadata(retired,active);
    trace("DestroyDevice-unresolved-retired-contexts",retired);
    trace("DestroyDevice-unresolved-active-contexts",active);
    device->memory.discard_device_metadata(retired,active);
    trace("DestroyDevice-unresolved-retired-memory",retired);
    trace("DestroyDevice-unresolved-active-memory",active);
    // These are CPU records only. OS context reclamation remains a lab acceptance gate.
    device->~Device();--adapter->devices;trace("DestroyDevice");
}
}
extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER* a) {
    if(!a) return E_INVALIDARG;
    a->hAdapter.pDrvPrivate=nullptr;
    if(!a->pAdapterFuncs) return E_INVALIDARG;
    *a->pAdapterFuncs={};
    if(!a->pAdapterCallbacks || !a->pAdapterCallbacks->pfnQueryAdapterInfoCb) return E_INVALIDARG;
    native12::AdapterContract contract;
    HRESULT hr=native12::query_contract(a->hRTAdapter,a->pAdapterCallbacks->pfnQueryAdapterInfoCb,contract);
    if(FAILED(hr)){trace("OpenAdapter12-contract-failed",static_cast<unsigned>(hr));return hr;}
    auto adapter=new(std::nothrow) Adapter{a->hRTAdapter,*a->pAdapterCallbacks};
    if(!adapter) return E_OUTOFMEMORY;
    adapter->contract=contract;
    *a->pAdapterFuncs={device_size,create_device,close_adapter,versions,caps,optional_tables,fill_table,destroy_device};
    a->hAdapter.pDrvPrivate=adapter;trace("OpenAdapter12");return S_OK;
}
