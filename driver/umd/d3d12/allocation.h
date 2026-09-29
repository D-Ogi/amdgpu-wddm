// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include "ddi-trace.h"
namespace native12 {
// One allocation, invoked synchronously from its owning device's DDI thread.
// The owner must retain this record after failed release and invalidate callbacks
// before runtime lifetime ends. Destruction never retries a runtime callback.
// GPU retirement, residency and Vulkan import remain the caller's responsibility.
// Contract: WDK 10.0.26100 d3d12umddi.h ALLOCATE/DEALLOCATE_0022;
// hKMResource is reserved. This helper does not interpret it as a resource handle.
class RuntimeAllocation final {
    D3D12DDI_HRTDEVICE runtime_{};
    PFND3D12DDI_ALLOCATE_CB_0022 allocate_{};
    PFND3D12DDI_DEALLOCATE_CB_0022 deallocate_{};
    D3DKMT_HANDLE allocation_{};
    D3DGPU_VIRTUAL_ADDRESS address_{};
public:
    RuntimeAllocation(D3D12DDI_HRTDEVICE runtime,
                      const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& callbacks) noexcept
        : runtime_(runtime),allocate_(callbacks.pfnAllocateCb),deallocate_(callbacks.pfnDeallocateCb) {}
    RuntimeAllocation(const RuntimeAllocation&)=delete;
    RuntimeAllocation& operator=(const RuntimeAllocation&)=delete;
    HRESULT open(const D3D12DDICB_ALLOCATE_0022& request) noexcept {
        if(allocation_) return E_UNEXPECTED;
        if(!allocate_ || !deallocate_ || request.NumAllocations!=1 || !request.pAllocationInfo ||
           request.hKMResource || (request.PrivateDriverDataSize && !request.pPrivateDriverData)) return E_INVALIDARG;
        auto info=*request.pAllocationInfo;
        if(info.hAllocation || (info.PrivateDriverDataSize && !info.pPrivateDriverData)) return E_INVALIDARG;
        // Copy the in/out descriptor. Private-data buffers are borrowed only for
        // this synchronous call and are not retained in the owner record.
        auto args=request;args.pAllocationInfo=&info;
        // The callback's own edges, after the local checks: what it was given and what it returned.
        const bool traced=ddi_trace_enabled();
        if(traced){
            std::fprintf(stderr,"{\"event\":\"allocate-callback\",\"edge\":\"begin\",\"experiment\":\"%s\","
                "\"allocations\":%u,\"resource\":%u,\"private_size\":%u,\"info_flags\":%u,\"source\":%u,"
                "\"info_private_size\":%u,\"thread\":%lu}\n",ddi_experiment_name(),args.NumAllocations,
                args.hKMResource?1u:0u,args.PrivateDriverDataSize,static_cast<unsigned>(info.Flags),
                static_cast<unsigned>(info.VidPnSourceId),info.PrivateDriverDataSize,GetCurrentThreadId());
            std::fflush(stderr);
        }
        HRESULT hr=allocate_(runtime_,&args);
        if(traced){
            std::fprintf(stderr,"{\"event\":\"allocate-callback\",\"edge\":\"end\",\"status\":\"%08lx\","
                "\"allocation\":%u,\"thread\":%lu}\n",static_cast<unsigned long>(hr),info.hAllocation?1u:0u,
                GetCurrentThreadId());
            std::fflush(stderr);
        }
        if(FAILED(hr)) return hr;
        if(!info.hAllocation) return E_UNEXPECTED;
        allocation_=info.hAllocation;address_=info.GpuVirtualAddress;
        return S_OK;
    }
    HRESULT close() noexcept {
        if(!allocation_) return S_OK;
        if(!deallocate_) return E_UNEXPECTED;
        D3D12DDICB_DEALLOCATE_0022 args{};
        // Release this allocation only, not the runtime resource group.
        args.NumAllocations=1;args.HandleList=&allocation_;
        // No ASSUME_NOT_IN_USE: this wrapper does not prove GPU retirement.
        args.Flags=D3D12DDI_DEALLOCATE_FLAGS_0022_NONE;
        HRESULT hr=deallocate_(runtime_,&args);
        if(SUCCEEDED(hr)){allocation_=0;address_=0;}
        return hr;
    }
    void invalidate_runtime() noexcept {runtime_={};allocate_=nullptr;deallocate_=nullptr;}
    D3DKMT_HANDLE handle() const noexcept {return allocation_;}
    D3DGPU_VIRTUAL_ADDRESS address() const noexcept {return address_;}
};
}
