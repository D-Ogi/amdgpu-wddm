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
    UINT64 address_{},bytes_{},fence_{};       // fence_: the largest pending value of map and residency
    D3DKMT_HANDLE allocation_{};
    bool valid_{};
    bool resident_{};                           // this domain holds one residency reference
public:
    GpuMapping()=default;
    GpuMapping(const GpuMapping&)=delete;
    GpuMapping& operator=(const GpuMapping&)=delete;
};
// Calls must be serialized inside live device DDI scope. No worker/destructor
// callbacks. Mapping alone does not establish residency: a mapping of an allocation
// that is not resident yet has no valid page table entries until VidMm commits it
// (trial 153: a GPU job wrote such a range 2 ms after its map fence, bugcheck 0x116).
// make_resident() takes this domain's own residency reference on the same paging
// queue, so the mapping's one fence covers the commit, the fill and the entries.
class PagingDomain final {
    HANDLE device_{};
    PFND3DDDI_CREATEPAGINGQUEUECB create_{};
    PFND3DDDI_DESTROYPAGINGQUEUECB destroy_{};
    PFND3DDDI_MAPGPUVIRTUALADDRESSCB map_{};
    PFND3DDDI_FREEGPUVIRTUALADDRESSCB free_{};
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMGPUCB wait_gpu_{};
    PFND3DDDI_MAKERESIDENTCB make_resident_{};
    PFND3DDDI_EVICTCB evict_{};
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPUCB wait_cpu_{};
    D3DKMT_HANDLE queue_{},sync_{};
    const volatile UINT64* completed_{};
    unsigned mappings_{};
    unsigned evict_failures_{};
public:
    unsigned evict_failures() const noexcept {return evict_failures_;}
    PagingDomain(D3D12DDI_HRTDEVICE device,const D3DDDI_DEVICECALLBACKS& cb) noexcept
        : device_(device.handle),create_(cb.pfnCreatePagingQueueCb),destroy_(cb.pfnDestroyPagingQueueCb),
          map_(cb.pfnMapGpuVirtualAddressCb),free_(cb.pfnFreeGpuVirtualAddressCb),
          wait_gpu_(cb.pfnWaitForSynchronizationObjectFromGpuCb),make_resident_(cb.pfnMakeResidentCb),
          evict_(cb.pfnEvictCb),wait_cpu_(cb.pfnWaitForSynchronizationObjectFromCpuCb) {}
    PagingDomain(const PagingDomain&)=delete;
    PagingDomain& operator=(const PagingDomain&)=delete;
    HRESULT open() noexcept {
        if(queue_) return E_UNEXPECTED;
        if(!create_ || !destroy_ || !map_ || !free_) return E_INVALIDARG;
        D3DDDICB_CREATEPAGINGQUEUE args{};args.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        HRESULT hr=create_(device_,&args);if(FAILED(hr)) return hr;
        queue_=args.hPagingQueue;sync_=args.hSyncObject;
        completed_=static_cast<const volatile UINT64*>(args.FenceValueCPUVirtualAddress);
        // Retain a malformed successful queue handle for explicit cleanup.
        return queue_ && sync_ && completed_ ? S_OK:E_UNEXPECTED;
    }
    HRESULT map(D3DKMT_HANDLE allocation,UINT64 bytes,GpuMapping& out) noexcept {
        if(out.owner_) return E_UNEXPECTED;
        if(!queue_ || !sync_ || !completed_ || !map_ || !allocation || !bytes || (bytes&4095)) return E_INVALIDARG;
        if(*completed_==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
        D3DDDI_MAPGPUVIRTUALADDRESS args{};args.hPagingQueue=queue_;args.hAllocation=allocation;
        args.SizeInPages=bytes/4096;args.Protection.Write=1;
        HRESULT hr=map_(device_,&args);
        if(FAILED(hr) && hr!=E_PENDING) return hr;
        out.owner_=this;out.address_=args.VirtualAddress;out.bytes_=bytes;out.allocation_=allocation;
        // The output fence is only meaningful for the asynchronous result.
        out.fence_=hr==E_PENDING?args.PagingFenceValue:0;
        out.valid_=out.address_ && !(out.address_&4095) && out.address_<=UINT64_MAX-bytes &&
            (hr!=E_PENDING || (out.fence_ && out.fence_!=UINT64_MAX));
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
    // One residency reference of this domain, queued behind the mapping on the same paging
    // queue; released by unmap_after_gpu_retirement. Its pending value joins the mapping's
    // fence (a monitored fence value only grows, so the largest covers both operations).
    // CantTrimFurther: D3D12's default residency policy succeeds regardless of the current budget
    // (D3D12_RESIDENCY_FLAG_NONE), and this flag is the WDDM form of that: over the current budget
    // the call still succeeds, over the maximum budget it refuses (E_OUTOFMEMORY) and takes nothing.
    // Without it a heap the runtime would create could fail here under memory pressure.
    HRESULT make_resident(GpuMapping& mapping) noexcept {
        if(mapping.owner_!=this || !mapping.valid_ || !mapping.allocation_) return E_INVALIDARG;
        if(mapping.resident_) return E_UNEXPECTED;
        if(!queue_ || !completed_ || !make_resident_ || !evict_) return E_UNEXPECTED;
        if(*completed_==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
        D3DDDI_MAKERESIDENT args{};args.hPagingQueue=queue_;args.NumAllocations=1;
        args.AllocationList=&mapping.allocation_;args.Flags.CantTrimFurther=1;
        HRESULT hr=make_resident_(device_,&args);
        if(hr!=S_OK && hr!=E_PENDING) return FAILED(hr)?hr:E_UNEXPECTED;
        // Accepted: the reference exists whatever the rest of the answer says.
        mapping.resident_=true;
        if(args.NumAllocations!=1) return E_UNEXPECTED;
        if(hr==E_PENDING){
            if(!args.PagingFenceValue || args.PagingFenceValue==UINT64_MAX) return E_UNEXPECTED;
            if(args.PagingFenceValue>mapping.fence_) mapping.fence_=args.PagingFenceValue;
        }
        return S_OK;
    }
    // Blocks this thread, with no time limit, until the mapping and any residency it took have
    // completed, then answers like ready() (UINT64_MAX there is the removed-device value D3D12
    // fences report). A paging engine that never completes stalls the caller; on this GPU such
    // a stall ends in a TDR anyway, and the old 2 s poll only turned it into a refused heap.
    HRESULT wait_ready(const GpuMapping& mapping,UINT64* address) const noexcept {
        HRESULT hr=ready(mapping,address);
        if(hr!=E_PENDING) return hr;
        if(!wait_cpu_ || !sync_) return E_UNEXPECTED;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU args{};
        args.ObjectCount=1;args.ObjectHandleArray=&sync_;args.FenceValueArray=&mapping.fence_;
        hr=wait_cpu_(device_,&args);
        if(hr!=S_OK) return FAILED(hr)?hr:E_UNEXPECTED;
        hr=ready(mapping,address);
        return hr==E_PENDING?E_UNEXPECTED:hr;
    }
    // Returns an address usable by subsequent submissions on this context only.
    // A pending mapping inserts a GPU wait; it does not become CPU-ready, resident,
    // or safe to free. The caller owns/serializes the context on this same device
    // and must not submit a dependent command after this method fails.
    HRESULT address_for_context(const GpuMapping& mapping,HANDLE context,UINT64* address) const noexcept {
        if(!address) return E_INVALIDARG;
        *address=0;
        if(!context) return E_INVALIDARG;
        HRESULT hr=ready(mapping,address);
        if(hr!=E_PENDING) return hr;
        if(!wait_gpu_ || !sync_) return E_UNEXPECTED;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU args{};
        args.hContext=context;args.ObjectCount=1;args.ObjectHandleArray=&sync_;
        args.MonitoredFenceValueArray=&mapping.fence_;
        hr=wait_gpu_(device_,&args);
        if(hr!=S_OK) return FAILED(hr)?hr:E_UNEXPECTED;
        // Loss may become visible while the runtime processes the wait request.
        UINT64 value=*completed_;MemoryBarrier();
        if(value==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
        *address=mapping.address_;
        return S_OK;
    }
    // Caller proves all GPU uses have retired before invoking this method.
    // The paging fence checked here proves completion of the mapping and of any residency
    // this domain took; that reference is released first. A failed eviction is counted and
    // does not hold the VA: residency and mapping are independent, and deallocating the
    // allocation drops its residency anyway, while a retry could never succeed on an error
    // that does not clear (the record, its VA and the paging queue would leak).
    HRESULT unmap_after_gpu_retirement(GpuMapping& mapping) noexcept {
        if(!mapping.owner_) return S_OK;
        UINT64 address=0;HRESULT hr=ready(mapping,&address);if(FAILED(hr)) return hr;
        if(!free_) return E_UNEXPECTED;
        if(mapping.resident_){
            HRESULT evicted=E_UNEXPECTED;
            if(evict_){
                D3DDDICB_EVICT evict{};evict.NumAllocations=1;evict.AllocationList=&mapping.allocation_;
                evicted=evict_(device_,&evict);
            }
            if(FAILED(evicted)) ++evict_failures_;
            mapping.resident_=false;
        }
        D3DDDICB_FREEGPUVIRTUALADDRESS args{};args.BaseAddress=address;args.Size=mapping.bytes_;
        hr=free_(device_,&args);
        if(SUCCEEDED(hr)){
            mapping.owner_=nullptr;mapping.address_=0;mapping.bytes_=0;mapping.fence_=0;mapping.valid_=false;
            mapping.allocation_=0;
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
        if(SUCCEEDED(hr)){queue_=0;sync_=0;completed_=nullptr;}
        return hr;
    }
    void invalidate_runtime() noexcept {device_=nullptr;create_=nullptr;destroy_=nullptr;map_=nullptr;free_=nullptr;wait_gpu_=nullptr;
        make_resident_=nullptr;evict_=nullptr;wait_cpu_=nullptr;completed_=nullptr;}
};
}
