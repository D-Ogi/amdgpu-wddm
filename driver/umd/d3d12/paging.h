// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <cstdint>
namespace native12 {
class PagingDomain;
class GpuMapping final {
    friend class PagingDomain;
    const PagingDomain* owner_{};
    UINT64 address_{},bytes_{},fence_{};
    bool valid_{};
public:
    GpuMapping()=default;
    GpuMapping(const GpuMapping&)=delete;
    GpuMapping& operator=(const GpuMapping&)=delete;
};
// Calls must be serialized inside live device DDI scope. No worker/destructor
// callbacks, and no assumption that mapping also establishes residency.
class PagingDomain final {
    HANDLE device_{};
    PFND3DDDI_CREATEPAGINGQUEUECB create_{};
    PFND3DDDI_DESTROYPAGINGQUEUECB destroy_{};
    PFND3DDDI_MAPGPUVIRTUALADDRESSCB map_{};
    PFND3DDDI_FREEGPUVIRTUALADDRESSCB free_{};
    D3DKMT_HANDLE queue_{};
    const volatile UINT64* completed_{};
    unsigned mappings_{};
public:
    PagingDomain(D3D12DDI_HRTDEVICE device,const D3DDDI_DEVICECALLBACKS& cb) noexcept
        : device_(device.handle),create_(cb.pfnCreatePagingQueueCb),destroy_(cb.pfnDestroyPagingQueueCb),
          map_(cb.pfnMapGpuVirtualAddressCb),free_(cb.pfnFreeGpuVirtualAddressCb) {}
    PagingDomain(const PagingDomain&)=delete;
    PagingDomain& operator=(const PagingDomain&)=delete;
    HRESULT open() noexcept {
        if(queue_) return E_UNEXPECTED;
        if(!create_ || !destroy_ || !map_ || !free_) return E_INVALIDARG;
        D3DDDICB_CREATEPAGINGQUEUE args{};args.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        HRESULT hr=create_(device_,&args);if(FAILED(hr)) return hr;
        queue_=args.hPagingQueue;
        completed_=static_cast<const volatile UINT64*>(args.FenceValueCPUVirtualAddress);
        // Retain a malformed successful queue handle for explicit cleanup.
        return queue_ && args.hSyncObject && completed_ ? S_OK:E_UNEXPECTED;
    }
    HRESULT map(D3DKMT_HANDLE allocation,UINT64 bytes,GpuMapping& out) noexcept {
        if(out.owner_) return E_UNEXPECTED;
        if(!queue_ || !completed_ || !map_ || !allocation || !bytes || (bytes&4095)) return E_INVALIDARG;
        if(*completed_==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
        D3DDDI_MAPGPUVIRTUALADDRESS args{};args.hPagingQueue=queue_;args.hAllocation=allocation;
        args.SizeInPages=bytes/4096;args.Protection.Write=1;
        HRESULT hr=map_(device_,&args);
        if(FAILED(hr) && hr!=E_PENDING) return hr;
        out.owner_=this;out.address_=args.VirtualAddress;out.bytes_=bytes;
        // The output fence is only meaningful for the asynchronous result.
        out.fence_=hr==E_PENDING?args.PagingFenceValue:0;
        out.valid_=out.address_ && !(out.address_&4095) && out.address_<=UINT64_MAX-bytes &&
            (hr!=E_PENDING || out.fence_);
        ++mappings_;
        return out.valid_?S_OK:E_UNEXPECTED;
    }
    HRESULT ready(const GpuMapping& mapping,UINT64* address) const noexcept {
        if(!address) return E_INVALIDARG;*address=0;
        if(mapping.owner_!=this || !mapping.valid_) return E_INVALIDARG;
        if(!completed_) return E_UNEXPECTED;
        UINT64 value=*completed_;MemoryBarrier();
        if(value==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
        if(value<mapping.fence_) return E_PENDING;
        *address=mapping.address_;return S_OK;
    }
    // Caller proves all GPU uses have retired before invoking this method.
    // The paging fence checked here proves mapping completion only.
    HRESULT unmap_after_gpu_retirement(GpuMapping& mapping) noexcept {
        if(!mapping.owner_) return S_OK;
        UINT64 address=0;HRESULT hr=ready(mapping,&address);if(FAILED(hr)) return hr;
        if(!free_) return E_UNEXPECTED;
        D3DDDICB_FREEGPUVIRTUALADDRESS args{};args.BaseAddress=address;args.Size=mapping.bytes_;
        hr=free_(device_,&args);
        if(SUCCEEDED(hr)){
            mapping.owner_=nullptr;mapping.address_=0;mapping.bytes_=0;mapping.fence_=0;mapping.valid_=false;
            --mappings_;
        }
        return hr;
    }
    HRESULT close() noexcept {
        if(mappings_) return E_PENDING;
        if(!queue_) return S_OK;
        if(!destroy_) return E_UNEXPECTED;
        D3DDDI_DESTROYPAGINGQUEUE args{};args.hPagingQueue=queue_;
        HRESULT hr=destroy_(device_,&args);
        if(SUCCEEDED(hr)){queue_=0;completed_=nullptr;}
        return hr;
    }
    void invalidate_runtime() noexcept {device_=nullptr;create_=nullptr;destroy_=nullptr;map_=nullptr;free_=nullptr;completed_=nullptr;}
};
}
