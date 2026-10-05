// SPDX-License-Identifier: MIT
#include "adapter-caps.h"
#include "device-state.h"
#include "adapter-query-scope.h"
#include "memory-policy.h"
#include "instance-policy.h"
#include "ddi-trace.h"
#include "engine-ddi/engine-ddi.h"
#include <memory>
#include <string>
#include <cstdio>
#include "stdio-log.h"
namespace native12 {
struct AdapterCapsOwner {
    HMODULE engine{},icd{};
    engine_ddi::AdapterCaps* caps{};
    D3DKMT_HANDLE unresolved_adapter{};
    AdapterEngineAccess access{};
    ~AdapterCapsOwner() {
        engine_ddi::free_adapter_caps(caps);
        if(icd)FreeLibrary(icd);
        if(engine)FreeLibrary(engine);
    }
};
namespace {
const int module_anchor=0;
HRESULT APIENTRY refuse_queue(void* count,void*,VkQueue){++*static_cast<unsigned*>(count);return E_NOTIMPL;}
void APIENTRY no_queue(void* count,void*,VkQueue){++*static_cast<unsigned*>(count);}
template<class Function> Function system_entry(HMODULE module,const char* name) noexcept {
    const auto address=GetProcAddress(module,name);Function entry{};
    static_assert(sizeof(entry)==sizeof(address));std::memcpy(&entry,&address,sizeof(entry));return entry;
}
const char* source_name(SparsePolicySource source) noexcept {
    switch(source){
    case SparsePolicySource::Default:return "default";
    case SparsePolicySource::RegistryOn:return "registry-on";
    case SparsePolicySource::RegistryOff:return "registry-off";
    case SparsePolicySource::Invalid:return "invalid";
    default:return "unreadable";
    }
}
HRESULT resolve_instance_policy(Adapter& adapter,AdapterCapsOwner& owner) noexcept {
    // The adapter's software key holds the off switch. It is read once, before the capability query, and
    // the answer stays with the adapter: a later edit of the key reaches neither its query nor its devices.
    adapter.instance_policy=0;
    HMODULE gdi=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!gdi)return HRESULT_FROM_WIN32(GetLastError());
    const MemoryPolicyKmt kmt{
        system_entry<PFND3DKMT_OPENADAPTERFROMLUID>(gdi,"D3DKMTOpenAdapterFromLuid"),
        system_entry<PFND3DKMT_QUERYADAPTERINFO>(gdi,"D3DKMTQueryAdapterInfo"),
        system_entry<PFND3DKMT_CLOSEADAPTER>(gdi,"D3DKMTCloseAdapter")};
    LUID luid{};std::memcpy(&luid,&adapter.contract.luid,sizeof(luid));
    SparsePolicy policy{};
    const HRESULT hr=query_sparse_policy(luid,kmt,&policy,&owner.unresolved_adapter);
    FreeLibrary(gdi);
    if(hr==S_OK && policy.sparse)adapter.instance_policy|=BC250_HOST_POLICY_SPARSE;
    const bool unexpected=hr!=S_OK || policy.source==SparsePolicySource::Invalid ||
                          policy.source==SparsePolicySource::Unreadable;
    if(unexpected || ddi_trace_enabled()){
        amdgpu_wddm_log::print("d3d12-caps instance-policy sparse=%u source=%s status=%08lx unresolved=%u result=%08lx\n",
            unsigned(policy.sparse),source_name(policy.source),static_cast<unsigned long>(policy.status),
            unsigned(owner.unresolved_adapter!=0),static_cast<unsigned long>(hr));amdgpu_wddm_log::flush();
    }
    return hr;
}
HRESULT apply_memory_policy(Adapter& adapter,AdapterCapsOwner& owner) noexcept {
    // KMT's OS-visible GPU-MMU capability is supplied by the admitted KMD. Read
    // it using this adapter's LUID; neither device type nor UMA implies I/O coherence.
    HMODULE gdi=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!gdi)return HRESULT_FROM_WIN32(GetLastError());
    const MemoryPolicyKmt kmt{
        system_entry<PFND3DKMT_OPENADAPTERFROMLUID>(gdi,"D3DKMTOpenAdapterFromLuid"),
        system_entry<PFND3DKMT_QUERYADAPTERINFO>(gdi,"D3DKMTQueryAdapterInfo"),
        system_entry<PFND3DKMT_CLOSEADAPTER>(gdi,"D3DKMTCloseAdapter")};
    LUID luid{};std::memcpy(&luid,&adapter.contract.luid,sizeof(luid));
    bool coherent=false;HRESULT hr=query_memory_policy(luid,kmt,&coherent,&owner.unresolved_adapter);
    FreeLibrary(gdi);
    if(hr==S_OK){
        engine_ddi::MemoryArchitecturePolicy policy{};policy.size=sizeof(policy);
        policy.io_coherent=coherent?engine_ddi::PolicyBool::True:engine_ddi::PolicyBool::False;
        // Resource tier 0 is reserved by the 0041 DDI. The engine's resource
        // barriers and copies provide tier 1 stateless-copy semantics; heap
        // serialization stays at the engine's answer, independently of this.
        policy.resource_serialization_tier={1,D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_1};
        hr=engine_ddi::set_memory_architecture_policy(owner.caps,&policy);
    }
    if(ddi_trace_enabled()){
        amdgpu_wddm_log::print("d3d12-caps host-memory-policy io_coherent=%u unresolved=%u result=%08lx\n",
            unsigned(coherent),unsigned(owner.unresolved_adapter!=0),static_cast<unsigned long>(hr));amdgpu_wddm_log::flush();
    }
    return hr;
}
HRESULT load_caps(Adapter& adapter,AdapterCapsOwner& owner) {
    HMODULE module{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&module_anchor),&module))return HRESULT_FROM_WIN32(GetLastError());
    std::wstring path(32768,L'\0');
    DWORD length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if(!length)return HRESULT_FROM_WIN32(GetLastError());
    if(length>=path.size())return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    path.resize(length);
    auto slash=path.find_last_of(L"\\/");if(slash==std::wstring::npos)return E_UNEXPECTED;
    path.resize(slash+1);
    // Only siblings of this UMD and system dependencies, never the game's directory or PATH.
    constexpr DWORD search=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32;
    owner.engine=LoadLibraryExW((path+L"amdgpu_wddm_vkd3d.dll").c_str(),nullptr,search);
    if(!owner.engine)return HRESULT_FROM_WIN32(GetLastError());
    owner.icd=LoadLibraryExW((path+L"amdgpu_wddm_radv.dll").c_str(),nullptr,search);
    if(!owner.icd)return HRESULT_FROM_WIN32(GetLastError());
    auto getter=reinterpret_cast<PFN_BC250_VKD3D_ENGINE_GET_FUNCS>(GetProcAddress(owner.engine,BC250_VKD3D_ENGINE_GET_FUNCS_NAME));
    auto get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(owner.icd,"vk_icdGetInstanceProcAddr"));
    if(!getter || !get)return E_NOINTERFACE;
    BC250_VKD3D_ENGINE_FUNCS funcs{};funcs.Size=sizeof(funcs);
    constexpr UINT32 abi=BC250_VKD3D_ENGINE_ABI_VERSION;
    HRESULT hr=getter(abi,&funcs);if(FAILED(hr))return hr;
    if(funcs.Size<sizeof(funcs) || (funcs.AbiVersion>>16)!=1 || funcs.AbiVersion<abi || !funcs.QueryAdapterCaps)return E_NOINTERFACE;
    hr=resolve_instance_policy(adapter,owner);if(FAILED(hr))return hr;
    AdapterQueryScope scope(get,adapter.contract.luid,adapter.instance_policy);if(!scope.entered())return E_UNEXPECTED;
    // These are admission-only placeholders. QueryAdapterCaps cannot call them;
    // the device path will use its own live runtime services, never these.
    unsigned queue_calls=0;
    BC250_VKD3D_SHELL_SERVICES services{sizeof(services),&queue_calls,refuse_queue,no_queue};
    LUID luid{};std::memcpy(&luid,&adapter.contract.luid,sizeof(luid));
    BC250_VKD3D_DEVICE_CREATE_INFO info{sizeof(info),abi,scope.entry(),luid,D3D_FEATURE_LEVEL_11_0,
        BC250_VKD3D_QUEUE_MODE_INLINE,&services,BC250_VKD3D_INSTANCE_MODE_PRIVATE};
    hr=engine_ddi::query_adapter_caps(&funcs,&info,&owner.caps);
    if(SUCCEEDED(hr) && (!scope.completed() || queue_calls))return E_UNEXPECTED;
    if(SUCCEEDED(hr))hr=apply_memory_policy(adapter,owner);
    // Lab diagnostic: the raytracing tier is reported for a measurement of the acceleration structure, state
    // object and DispatchRays slots, while AddToStateObject, existing collections and indirect dispatch refuse.
    if(SUCCEEDED(hr) && ddi_experiment("raytracing-tier")){
        hr=engine_ddi::set_diagnostic_raytracing_tier(owner.caps,true);
        amdgpu_wddm_log::print("d3d12-caps experiment raytracing-tier result=%08lx\n",static_cast<unsigned long>(hr));
        amdgpu_wddm_log::flush();
    }
    if(SUCCEEDED(hr))owner.access={funcs,get};
    return hr;
}
HRESULT ensure_caps(Adapter& adapter, AdapterCapsOwner** out) noexcept {
    *out=nullptr;
    AcquireSRWLockExclusive(&adapter.caps_lock);
    if(!adapter.caps_attempted) {
        adapter.caps_attempted=true;
        try {
            auto owner=std::make_unique<AdapterCapsOwner>();
            adapter.caps_status=load_caps(adapter,*owner);
            if(SUCCEEDED(adapter.caps_status))adapter.engine_caps=owner.release();
            else if(owner->unresolved_adapter){
                // A failed KMT close leaves ownership uncertain. Keep its record
                // and owners for process teardown; CloseAdapter must not claim
                // successful cleanup or retry a potentially consumed handle.
                ++adapter.retained_engines;adapter.engine_caps=owner.release();
                amdgpu_wddm_log::print("d3d12-caps unresolved query adapter retained\n");amdgpu_wddm_log::flush();
            }
        } catch(const std::bad_alloc&) {adapter.caps_status=E_OUTOFMEMORY;}
    }
    const HRESULT status=adapter.caps_status;
    auto owner=adapter.engine_caps;
    ReleaseSRWLockExclusive(&adapter.caps_lock);
    // Adapter lifetime is owned by the runtime; its close follows all GetCaps calls.
    if(FAILED(status))return status;
    if(!owner || !owner->caps)return E_UNEXPECTED;
    *out=owner;
    return S_OK;
}
}
HRESULT get_adapter_caps(Adapter& adapter,const D3D12DDIARG_GETCAPS* request) noexcept {
    if(!request || !request->pData)return E_INVALIDARG;
    AdapterCapsOwner* owner{};
    HRESULT hr=ensure_caps(adapter,&owner);
    if(FAILED(hr))return hr;
    return engine_ddi::build_caps(owner->caps,D3D12DDI_BUILD_VERSION_0092,request);
}
HRESULT get_adapter_engine(Adapter& adapter, AdapterEngineAccess* access) noexcept {
    if(!access)return E_INVALIDARG;
    *access={};
    AdapterCapsOwner* owner{};
    HRESULT hr=ensure_caps(adapter,&owner);
    if(FAILED(hr))return hr;
    if(!owner->access.driver_entry || !owner->access.functions.CreateDevice ||
       !owner->access.functions.GetVulkanHandles)return E_NOINTERFACE;
    *access=owner->access;
    return S_OK;
}
void close_adapter_caps(Adapter& adapter) noexcept {
    delete adapter.engine_caps;adapter.engine_caps=nullptr;
}
}
