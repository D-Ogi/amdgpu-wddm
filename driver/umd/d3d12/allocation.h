// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include "ddi-trace.h"
#include "stdio-log.h"
namespace native12 {
// One allocation, invoked synchronously from its owning device's DDI thread.
// The owner must retain this record after failed release and invalidate callbacks
// before runtime lifetime ends. Destruction never retries a runtime callback.
// GPU retirement, residency and Vulkan import remain the caller's responsibility.
// Contract: WDK 10.0.26100 d3d12umddi.h ALLOCATE/DEALLOCATE_0022;
// hKMResource is reserved. This helper does not interpret it as a resource handle.
// Handle names this allocation alone. Owner names the runtime resource the allocation was created
// with and no handle: the caller has proven that the resource's uses have retired and that the
// resource's own DDI is running, which this wrapper cannot know.
enum class ReleaseForm { Handle, Owner };
// Retired: the caller has proven that no GPU work uses the allocation. Only then does the release
// carry ASSUME_NOT_IN_USE and SYNCHRONOUS_DESTROY. Owner is never released otherwise.
enum class Retirement { Unknown, Retired };
class RuntimeAllocation final {
    D3D12DDI_HRTDEVICE runtime_{};
    PFND3D12DDI_ALLOCATE_CB_0022 allocate_{};
    PFND3D12DDI_DEALLOCATE_CB_0022 deallocate_{};
    D3DKMT_HANDLE allocation_{};
    HANDLE resource_{};                         // the runtime owner the allocation was created with
    D3DGPU_VIRTUAL_ADDRESS address_{};
    // BD-075: the allocation came from the runtime already made (pfnOpenHeapAndResource), so no allocate
    // callback created it and no deallocate callback may destroy it. The runtime destroys it itself when the
    // open's resource goes.
    bool borrowed_{};
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
            amdgpu_wddm_log::print("{\"event\":\"allocate-callback\",\"edge\":\"begin\",\"experiment\":\"%s\","
                "\"allocations\":%u,\"runtime_resource\":%u,\"kernel_resource\":%u,\"private_size\":%u,"
                "\"info_flags\":%u,\"source\":%u,"
                "\"info_private_size\":%u,\"thread\":%lu}\n",ddi_experiment_name(),args.NumAllocations,
                args.hResource?1u:0u,args.hKMResource?1u:0u,args.PrivateDriverDataSize,static_cast<unsigned>(info.Flags),
                static_cast<unsigned>(info.VidPnSourceId),info.PrivateDriverDataSize,GetCurrentThreadId());
            amdgpu_wddm_log::flush();
        }
        HRESULT hr=allocate_(runtime_,&args);
        if(traced){
            amdgpu_wddm_log::print("{\"event\":\"allocate-callback\",\"edge\":\"end\",\"status\":\"%08lx\","
                "\"allocation\":%u,\"thread\":%lu}\n",static_cast<unsigned long>(hr),info.hAllocation?1u:0u,
                GetCurrentThreadId());
            amdgpu_wddm_log::flush();
        }
        if(FAILED(hr)) return hr;
        if(!info.hAllocation) return E_UNEXPECTED;
        allocation_=info.hAllocation;address_=info.GpuVirtualAddress;resource_=request.hResource;
        return S_OK;
    }
    // BD-075: adopts an allocation the runtime opened for this device. There is no callback and nothing to
    // release later; the record exists so that the mapping and the Vulkan import have the same owner as a
    // created allocation's. A borrowed allocation never names a runtime resource as its release owner.
    HRESULT adopt(D3DKMT_HANDLE allocation) noexcept {
        if(allocation_) return E_UNEXPECTED;
        if(!allocation) return E_INVALIDARG;
        allocation_=allocation;address_=0;resource_=nullptr;borrowed_=true;
        return S_OK;
    }
    HRESULT close(ReleaseForm form=ReleaseForm::Handle,Retirement retirement=Retirement::Unknown) noexcept {
        if(!allocation_) return S_OK;
        // A borrowed allocation is forgotten, not deallocated: this driver never created it. The owner form
        // would name a runtime resource for a destruction that is not ours to ask for, so it is refused
        // rather than silently turned into the handle form.
        if(borrowed_){
            if(form==ReleaseForm::Owner) return E_UNEXPECTED;
            allocation_=0;address_=0;resource_=nullptr;
            return S_OK;
        }
        if(!deallocate_) return E_UNEXPECTED;
        const bool by_resource=form==ReleaseForm::Owner;
        if(by_resource && (!resource_ || retirement!=Retirement::Retired)) return E_UNEXPECTED;
        D3D12DDICB_DEALLOCATE_0022 args{};
        if(by_resource)args.hResource=resource_;
        else {args.NumAllocations=1;args.HandleList=&allocation_;}  // this allocation only
        // A resident object's allocation (DirectX-Specs, ResourceHeaps.md, "Resident Object
        // Destruction Requirements"): not in use, destroyed before the call returns. Without the
        // caller's proof of retirement the flags stay NONE.
        args.Flags=retirement==Retirement::Retired?static_cast<D3D12DDI_DEALLOCATE_FLAGS_0022>(
            D3D12DDI_DEALLOCATE_FLAGS_0022_ASSUME_NOT_IN_USE|D3D12DDI_DEALLOCATE_FLAGS_0022_SYNCHRONOUS_DESTROY):
            D3D12DDI_DEALLOCATE_FLAGS_0022_NONE;
        const bool traced=ddi_trace_enabled();
        if(traced){
            amdgpu_wddm_log::print("{\"event\":\"deallocate-callback\",\"edge\":\"begin\",\"experiment\":\"%s\","
                "\"resource\":%u,\"owner_known\":%u,\"allocations\":%u,\"flags\":%u,\"thread\":%lu}\n",
                ddi_experiment_name(),args.hResource?1u:0u,resource_?1u:0u,args.NumAllocations,
                static_cast<unsigned>(args.Flags),GetCurrentThreadId());
            amdgpu_wddm_log::flush();
        }
        HRESULT hr=deallocate_(runtime_,&args);
        if(traced){
            amdgpu_wddm_log::print("{\"event\":\"deallocate-callback\",\"edge\":\"end\",\"status\":\"%08lx\","
                "\"thread\":%lu}\n",static_cast<unsigned long>(hr),GetCurrentThreadId());
            amdgpu_wddm_log::flush();
        }
        if(SUCCEEDED(hr)){allocation_=0;address_=0;resource_=nullptr;}
        return hr;
    }
    void invalidate_runtime() noexcept {runtime_={};allocate_=nullptr;deallocate_=nullptr;resource_=nullptr;}
    // The runtime resource is gone or going: the allocation stays owned, the owner is never named again.
    void revoke_owner() noexcept {resource_=nullptr;}
    bool owner_known() const noexcept {return resource_!=nullptr;}
    D3DKMT_HANDLE handle() const noexcept {return allocation_;}
    D3DGPU_VIRTUAL_ADDRESS address() const noexcept {return address_;}
};
}
