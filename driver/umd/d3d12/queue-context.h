// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
namespace native12 {
// Called only within a runtime DDI entry. No worker thread or direct KMT entry point.
// The queue owns the context until close succeeds; a failed close retains ownership.
class QueueContext final {
    D3D12DDI_HRTCOMMANDQUEUE runtime_{};
    PFND3D12DDI_CREATECONTEXTVIRTUAL_CB create_{};
    PFND3D12DDI_DESTROYCONTEXT_CB destroy_{};
    HANDLE context_{};
public:
    QueueContext(D3D12DDI_HRTCOMMANDQUEUE runtime,
                 const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& callbacks) noexcept
        : runtime_(runtime),create_(callbacks.pfnCreateContextVirtualCb),destroy_(callbacks.pfnDestroyContextCb) {}
    QueueContext(const QueueContext&)=delete;
    QueueContext& operator=(const QueueContext&)=delete;
    HRESULT open(const D3DDDICB_CREATECONTEXTVIRTUAL& request) noexcept {
        if(context_) return E_UNEXPECTED;
        if(!create_ || !destroy_) return E_INVALIDARG;
        auto args=request;args.hContext=nullptr;
        HRESULT hr=create_(runtime_,&args);
        if(FAILED(hr)) return hr;
        if(!args.hContext) return E_UNEXPECTED;
        context_=args.hContext;return S_OK;
    }
    HRESULT close() noexcept {
        if(!context_) return S_OK;
        if(!destroy_) return E_UNEXPECTED;
        D3DDDICB_DESTROYCONTEXT args{};args.hContext=context_;
        HRESULT hr=destroy_(runtime_,&args);
        if(SUCCEEDED(hr)) context_=nullptr;
        return hr;
    }
    void invalidate_runtime() noexcept {runtime_={};create_=nullptr;destroy_=nullptr;}
    HANDLE handle() const noexcept {return context_;}
};
}
