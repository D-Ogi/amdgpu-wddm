// SPDX-License-Identifier: MIT
#include "device-owner.h"
namespace bc250::umd {
HRESULT DeviceOwner::initialize(const D3D10DDIARG_CREATEDEVICE &args, UINT64 luid,
    PFN_vkGetInstanceProcAddr get, const BC250_DXVK_ENGINE_FUNCS &funcs,
    D3D_FEATURE_LEVEL level, const BC250_DXVK_SHELL_SERVICES &services) {
    if (!runtime_.domain.entered() || initialized_ || runtime_.present_context || instance_.info().Instance)
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
    runtime_.DXGICallbacks=*args.DXGIBaseDDI.pDXGIBaseCallbacks;
    D3DDDICB_CREATECONTEXTVIRTUAL create{}; create.EngineAffinity=1;
    HRESULT hr=runtime_.KTCallbacks.pfnCreateContextVirtualCb(runtime_.hDevice,&create);
    if (FAILED(hr)) return hr;
    runtime_.present_context=create.hContext;
    if (!runtime_.present_context) return E_FAIL;
    auto host=host_descriptor(bridge_,luid);
    hr=instance_.open(get,host);
    if (SUCCEEDED(hr)) hr=session_.open(funcs,instance_.info(),level,services);
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
HRESULT DeviceOwner::close() {
    if (!runtime_.domain.entered()) return E_UNEXPECTED;
    if (session_.must_retain_owner()) return E_UNEXPECTED;
    HRESULT hr=S_OK;
    if (session_.device()) {
        hr=wait_present_idle(bridge_);
        if (FAILED(hr) && hr!=D3DDDIERR_DEVICEREMOVED) return hr;
    }
    if (context_) { context_->Release(); context_=nullptr; }
    if (device_) { device_->Release(); device_=nullptr; }
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
    bridge_={}; bridge_.device=&runtime_; initialized_=false;
    return S_OK;
}
}
