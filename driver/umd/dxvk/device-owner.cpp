// SPDX-License-Identifier: MIT
#include "device-owner.h"
#include <algorithm>
namespace bc250::umd {
namespace { void module_anchor() {} }
HRESULT DeviceOwner::retain_code_modules(const void *engineEntry,const void *icdEntry) {
    if (!runtime_.domain.entered() || retained_module_count()) return E_UNEXPECTED;
    if (!engineEntry || !icdEntry) return E_INVALIDARG;
    const void *entries[]={reinterpret_cast<const void *>(&module_anchor),engineEntry,icdEntry};
    for (unsigned i=0;i<3;++i) {
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(entries[i]),&modules_[i])) {
            // Keep any acquired references until close, including on failure.
            return HRESULT_FROM_WIN32(GetLastError());
        }
    }
    return S_OK;
}
HRESULT DeviceOwner::initialize(const D3D10DDIARG_CREATEDEVICE &args, UINT64 luid,
    PFN_vkGetInstanceProcAddr get, const BC250_DXVK_ENGINE_FUNCS &funcs,
    D3D_FEATURE_LEVEL level, const BC250_DXVK_SHELL_SERVICES &services) {
    if (!runtime_.domain.entered() || initialized_ || closing_ || has_live_objects())
        return E_UNEXPECTED;
    if (!args.pKTCallbacks || !args.pUMCallbacks || !args.DXGIBaseDDI.pDXGIBaseCallbacks || !args.hRTDevice.handle)
        return E_INVALIDARG;
    if (!args.pKTCallbacks->pfnCreateContextVirtualCb || !args.pKTCallbacks->pfnDestroyContextCb ||
        !args.pKTCallbacks->pfnDestroySynchronizationObjectCb || !args.pUMCallbacks->pfnSetErrorCb)
        return E_NOTIMPL;
    runtime_.hDevice=reinterpret_cast<HANDLE>(args.hRTDevice.handle);
    runtime_.hRTCoreLayer=args.hRTCoreLayer;
    runtime_.KTCallbacks=*args.pKTCallbacks;
    runtime_.UMCallbacks=*args.pUMCallbacks;
    runtime_.DXGICallbacks=args.DXGIBaseDDI.pDXGIBaseCallbacks;
    D3DDDICB_CREATECONTEXTVIRTUAL create{}; create.EngineAffinity=1;
    HRESULT hr=runtime_.KTCallbacks.pfnCreateContextVirtualCb(runtime_.hDevice,&create);
    if (FAILED(hr)) return hr;
    runtime_.present_context=create.hContext;
    if (!runtime_.present_context) return E_FAIL;
    auto host=host_descriptor(bridge_,luid);
    hr=retain_code_modules(reinterpret_cast<const void *>(funcs.CreateDevice),reinterpret_cast<const void *>(get));
    if (SUCCEEDED(hr)) hr=instance_.open(get,host);
    if (SUCCEEDED(hr)) hr=session_.open(funcs,instance_.info(),level,services);
    if (SUCCEEDED(hr)) {
        hr=session_.engine()->QueryInterface(__uuidof(IBc250DxvkDevice4),reinterpret_cast<void **>(&engine4_));
        if (SUCCEEDED(hr) && !engine4_) hr=E_NOINTERFACE;
    }
    if (SUCCEEDED(hr)) {
        hr=session_.engine()->GetD3D11Device(__uuidof(ID3D11Device5),reinterpret_cast<void **>(&device_));
        if (SUCCEEDED(hr) && !device_) hr=E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        hr=session_.engine()->GetImmediateContext(__uuidof(ID3D11DeviceContext4),reinterpret_cast<void **>(&context_));
        if (SUCCEEDED(hr) && !context_) hr=E_FAIL;
    }
    if (FAILED(hr)) { close(); return hr; }
    initialized_=true;
    return S_OK;
}
HRESULT DeviceOwner::take_deferred_error(EngineErrorPolicy policy) {
    if (!runtime_.domain.entered() || !engine4_) return E_UNEXPECTED;
    const HRESULT hr=errors_.poll([&]() { return engine4_->TakeDeferredError(); },policy);
    if (hr==DXGI_ERROR_DEVICE_REMOVED) bridge_.device_lost=true;
    return hr;
}
HRESULT DeviceOwner::prepare_surface_import() {
    if (!runtime_.domain.entered() || closing_ || !initialized_ || !session_.device() || !engine()) return E_UNEXPECTED;
    if (!surface_vk_.create) {
        const auto &instance=instance_.info();
        auto gdpa=reinterpret_cast<PFN_vkGetDeviceProcAddr>(instance.GetInstanceProcAddr(instance.Instance,"vkGetDeviceProcAddr"));
        auto memory=reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(instance.GetInstanceProcAddr(instance.Instance,"vkGetPhysicalDeviceMemoryProperties"));
        if (!gdpa || !memory) return E_NOTIMPL;
        RuntimeImageDispatch vk{};
#define LOAD(field,type,name) vk.field=reinterpret_cast<type>(gdpa(session_.device(),name)); if (!vk.field) return E_NOTIMPL
        LOAD(create,PFN_vkCreateImage,"vkCreateImage");
        LOAD(destroy,PFN_vkDestroyImage,"vkDestroyImage");
        LOAD(layout,PFN_vkGetImageSubresourceLayout,"vkGetImageSubresourceLayout");
        LOAD(memory.requirements,PFN_vkGetImageMemoryRequirements,"vkGetImageMemoryRequirements");
        LOAD(memory.allocate,PFN_vkAllocateMemory,"vkAllocateMemory");
        LOAD(memory.bind,PFN_vkBindImageMemory,"vkBindImageMemory");
        LOAD(memory.free,PFN_vkFreeMemory,"vkFreeMemory");
#undef LOAD
        memory(instance.PhysicalDevice,&surface_memory_);
        surface_vk_=vk;
    }
    if (!surface_queue_.queue && !surface_queue_.sync && !surface_queue_.cpu)
        return create_surface_paging_queue(runtime_,surface_queue_);
    return surface_queue_.queue && surface_queue_.sync && surface_queue_.cpu ? S_OK : E_UNEXPECTED;
}
bool DeviceOwner::owns_surface(const RuntimeSurface &surface) const {
    for (const auto &p:surfaces_) if (p.get()==&surface) return true;
    return false;
}
HRESULT DeviceOwner::begin_surface(const RuntimeSurfaceRequest &request,const D3D11_TEXTURE2D_DESC1 &desc,RuntimeSurface *&out) {
    if (out) return E_UNEXPECTED;
    HRESULT hr=prepare_surface_import();
    if (FAILED(hr)) return hr;
    auto pending=std::make_unique<RuntimeSurface>();
    // Register before callbacks: allocation failure cannot orphan ownership.
    surfaces_.push_back(std::move(pending));
    out=surfaces_.back().get();
    return begin_runtime_surface(runtime_,surface_queue_,request,desc,*out);
}
HRESULT DeviceOwner::adopt_surface(RuntimeSurfaceAllocation &allocation,const BC250_WDDM_ALLOCATION_PRIVATE &metadata,
    const D3D11_TEXTURE2D_DESC1 &desc,RuntimeSurface *&out) {
    if (out) return E_UNEXPECTED;
    HRESULT hr=prepare_surface_import();
    if (FAILED(hr)) return hr;
    auto pending=std::make_unique<RuntimeSurface>();
    surfaces_.push_back(std::move(pending)); out=surfaces_.back().get();
    return adopt_runtime_surface(runtime_,surface_queue_,allocation,metadata,desc,*out);
}
HRESULT DeviceOwner::finish_surface(RuntimeSurface &surface) {
    if (!runtime_.domain.entered() || closing_ || !owns_surface(surface) || !engine()) return E_INVALIDARG;
    return finish_runtime_surface(session_.device(),surface_vk_,texture_import_dispatch(*engine()),surface_memory_,surface);
}
HRESULT DeviceOwner::wait_surface(RuntimeSurface &surface) {
    if (!runtime_.domain.entered() || closing_ || !owns_surface(surface)) return E_INVALIDARG;
    HRESULT hr=wait_surface_paging(runtime_,surface.queue,surface.mapping);
    if (hr==S_OK) hr=finish_surface(surface);
    if (hr==D3DDDIERR_DEVICEREMOVED) bridge_.device_lost=true;
    return hr;
}
HRESULT DeviceOwner::close_surface(RuntimeSurface &surface) {
    if (!runtime_.domain.entered() || !engine()) return E_INVALIDARG;
    auto position=std::find_if(surfaces_.begin(),surfaces_.end(),[&](const auto &p) { return p.get()==&surface; });
    if (position==surfaces_.end()) return E_INVALIDARG;
    HRESULT hr=close_runtime_surface(bridge_,session_.device(),surface_vk_,texture_import_dispatch(*engine()),surface);
    if (hr==S_OK) surfaces_.erase(position);
    return hr;
}
HRESULT DeviceOwner::release_surface_handle(RuntimeSurface &surface) {
    if (!runtime_.domain.entered() || !owns_surface(surface)) return E_INVALIDARG;
    HRESULT hr=close_surface(surface);
    if (hr!=S_OK) {
        // The runtime will invalidate hRTResource after this DDI returns. A
        // failure must not turn into a callback on that handle during teardown.
        // Keep backing/device/modules pinned if COM/GPU cleanup is unproven.
        surface.allocation.runtime_resource=nullptr;
        surface.phase=SurfacePhase::quarantined;
        OutputDebugStringA("BC250 M14: resource cleanup failed; owner quarantined before runtime handle invalidation\n");
    }
    return hr;
}
HRESULT DeviceOwner::close() {
    if (!runtime_.domain.entered()) return E_UNEXPECTED;
    closing_=true;
    if (session_.must_retain_owner()) return E_UNEXPECTED;
    HRESULT hr=S_OK;
    if (session_.device()) {
        hr=wait_present_idle(bridge_);
        if (FAILED(hr) && hr!=D3DDDIERR_DEVICEREMOVED) return hr;
    }
    // Drop bound views before releasing imported textures, while the engine and
    // hosted Vulkan device still exist. A pending/failed close retains everything.
    if (!surfaces_.empty() && context_) context_->ClearState();
    while (!surfaces_.empty()) {
        hr=close_surface(*surfaces_.back());
        if (hr!=S_OK) return hr;
    }
    hr=destroy_surface_paging_queue(runtime_,surface_queue_);
    if (hr!=S_OK) return hr;
    surface_vk_={}; surface_memory_={};
    if (context_) { context_->Release(); context_=nullptr; }
    if (device_) { device_->Release(); device_=nullptr; }
    if (engine4_) { engine4_->Release(); engine4_=nullptr; }
    hr=session_.close();
    if (FAILED(hr)) return hr;
    for (auto &progress:bridge_.progress) progress={};
    hr=instance_.close();
    if (FAILED(hr)) return hr;
    if (bridge_.present_sync) {
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT destroy{}; destroy.hSyncObject=bridge_.present_sync;
        hr=runtime_.KTCallbacks.pfnDestroySynchronizationObjectCb(runtime_.hDevice,&destroy);
        if (FAILED(hr)) return hr;
        bridge_.present_sync=0; bridge_.present_cpu=nullptr; bridge_.present_value=0;
    }
    if (runtime_.present_context) {
        D3DDDICB_DESTROYCONTEXT destroy{}; destroy.hContext=runtime_.present_context;
        hr=runtime_.KTCallbacks.pfnDestroyContextCb(runtime_.hDevice,&destroy);
        if (FAILED(hr)) return hr;
        runtime_.present_context=nullptr;
    }
    // Both Vulkan and all runtime handles are gone. Release engine and ICD
    // first, our extra UMD reference last. The caller owns its normal loader
    // reference while executing this API. Failed close never reaches this.
    for (int i=2;i>=0;--i) {
        if (modules_[i]) {
            if (!FreeLibrary(modules_[i])) return HRESULT_FROM_WIN32(GetLastError());
            modules_[i]=nullptr;
        }
    }
    runtime_.DXGICallbacks=nullptr;
    bridge_={}; bridge_.device=&runtime_; errors_={}; initialized_=false; closing_=false;
    return S_OK;
}
}
