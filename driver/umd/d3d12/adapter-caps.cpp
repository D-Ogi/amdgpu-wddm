// SPDX-License-Identifier: MIT
#include "adapter-caps.h"
#include "device-state.h"
#include "adapter-query-scope.h"
#include "engine-ddi/engine-ddi.h"
#include <memory>
#include <string>
#include <cstdio>
namespace native12 {
struct AdapterCapsOwner {
    HMODULE engine{},icd{};
    engine_ddi::AdapterCaps* caps{};
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
    constexpr UINT32 abi=0x10002;
    HRESULT hr=getter(abi,&funcs);if(FAILED(hr))return hr;
    if(funcs.Size<sizeof(funcs) || (funcs.AbiVersion>>16)!=1 || (funcs.AbiVersion&0xffff)<2 || !funcs.QueryAdapterCaps)return E_NOINTERFACE;
    AdapterQueryScope scope(get,adapter.contract.luid);if(!scope.entered())return E_UNEXPECTED;
    // These are admission-only placeholders. QueryAdapterCaps cannot call them;
    // the device path will use its own live runtime services, never these.
    unsigned queue_calls=0;
    BC250_VKD3D_SHELL_SERVICES services{sizeof(services),&queue_calls,refuse_queue,no_queue};
    LUID luid{};std::memcpy(&luid,&adapter.contract.luid,sizeof(luid));
    BC250_VKD3D_DEVICE_CREATE_INFO info{sizeof(info),abi,scope.entry(),luid,D3D_FEATURE_LEVEL_11_0,
        BC250_VKD3D_QUEUE_MODE_INLINE,&services};
    hr=engine_ddi::query_adapter_caps(&funcs,&info,&owner.caps);
    if(SUCCEEDED(hr) && (!scope.completed() || queue_calls))return E_UNEXPECTED;
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
