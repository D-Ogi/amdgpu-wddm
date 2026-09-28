// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <limits>
#include <span>

namespace native12 {
enum class ResidencyState { Failed, Ready, Pending, Unknown };
struct ResidencyResult {
    ResidencyState state{ResidencyState::Failed};
    HRESULT callback_status{E_UNEXPECTED};
    UINT64 paging_fence{};
    UINT64 bytes_to_trim{};
};

// Native D3D12 core-layer callbacks, not the D3DDDI_DEVICECALLBACKS variants.
// The caller translates runtime objects to allocation handles on this device.
// Submit the whole translated list in one call: splitting it would require
// rollback of earlier residency increments when a later batch fails.
// WDK 10.0.26100: PFND3D12DDI_MAKERESIDENT_CB / PFND3D12DDI_EVICT_CB.
class RuntimeResidency final {
    D3D12DDI_HRTDEVICE device_{};
    PFND3D12DDI_MAKERESIDENT_CB make_{};
    PFND3D12DDI_EVICT_CB evict_{};

    static bool valid(std::span<const D3DKMT_HANDLE> allocations) noexcept {
        if(allocations.empty() || allocations.size()>std::numeric_limits<UINT>::max()) return false;
        for(auto handle:allocations) if(!handle) return false;
        return true;
    }
public:
    RuntimeResidency(D3D12DDI_HRTDEVICE device,
                     const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb) noexcept
        : device_(device),make_(cb.pfnMakeResidentCb),evict_(cb.pfnEvictCb) {}
    RuntimeResidency(const RuntimeResidency&)=delete;
    RuntimeResidency& operator=(const RuntimeResidency&)=delete;

    // E_PENDING is preserved, not converted to success. The fence belongs to
    // the supplied runtime paging queue, never to PagingDomain's KT queue.
    // No polling, trimming, retries, or residency-reference deduplication here.
    HRESULT make_resident(D3D12DDI_HRTPAGINGQUEUE queue,
                          std::span<const D3DKMT_HANDLE> allocations,
                          D3DDDI_MAKERESIDENT_FLAGS flags,ResidencyResult& out) noexcept {
        out={};
        if(!make_ || !evict_) return E_UNEXPECTED;
        if(!queue.handle || !valid(allocations) || flags.Reserved) return E_INVALIDARG;
        D3DDDI_MAKERESIDENT args{};
        // The core-layer callback receives the opaque runtime queue separately.
        // It resolves the KMT queue; never cast this runtime handle to hPagingQueue.
        args.NumAllocations=static_cast<UINT>(allocations.size());
        args.AllocationList=allocations.data();args.Flags=flags;
        const HRESULT hr=make_(device_,queue,&args);
        out.callback_status=hr;
        if(hr==E_OUTOFMEMORY){
            // Atomic refusal: no increment to undo and no valid paging fence.
            out.bytes_to_trim=args.NumBytesToTrim;
            return hr;
        }
        if(FAILED(hr) && hr!=E_PENDING) return hr;
        if(hr!=S_OK && hr!=E_PENDING){out.state=ResidencyState::Unknown;return E_UNEXPECTED;}
        // Preserve evidence of a possibly accepted operation; never reissue it
        // automatically, because that could add a second residency reference.
        if(args.NumAllocations!=allocations.size() ||
           (hr==E_PENDING && (!args.PagingFenceValue || args.PagingFenceValue==UINT64_MAX))){
            out.state=ResidencyState::Unknown;return E_UNEXPECTED;
        }
        if(hr==E_PENDING){
            out.state=ResidencyState::Pending;out.paging_fence=args.PagingFenceValue;
        } else out.state=ResidencyState::Ready;
        return hr;
    }

    // The caller serializes eviction with all GPU uses. A successful callback
    // decrements one residency reference; it does not free an allocation.
    HRESULT evict_after_gpu_retirement(std::span<const D3DKMT_HANDLE> allocations,
                                       D3DDDI_EVICT_FLAGS flags) noexcept {
        if(!evict_) return E_UNEXPECTED;
        if(!valid(allocations) || flags.Reserved) return E_INVALIDARG;
        D3DDDICB_EVICT args{};args.NumAllocations=static_cast<UINT>(allocations.size());
        args.AllocationList=allocations.data();args.Flags=flags;
        return evict_(device_,&args);
    }
    void invalidate_runtime() noexcept {device_={};make_=nullptr;evict_=nullptr;}
};
}
