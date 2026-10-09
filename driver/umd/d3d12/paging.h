// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <atomic>
#include <cstdint>
namespace native12 {
class PagingDomain;
class GpuMapping final {
    friend class PagingDomain;
    const PagingDomain* owner_{};
    UINT64 address_{},bytes_{},fence_{};       // fence_: the largest pending value of map and residency
    // BD-101: the GPU virtual address reservation this mapping lives inside, and its whole span. Zero for a
    // mapping whose address the runtime picked by itself, which is every mapping of an alignment the runtime
    // satisfies on its own. The release frees this range, not the mapped sub-range (unmap_after_gpu_retirement).
    UINT64 reservation_{},reservation_bytes_{};
    D3DKMT_HANDLE allocation_{};
    bool valid_{};
    bool resident_{};                           // this domain holds one residency reference
public:
    GpuMapping()=default;
    GpuMapping(const GpuMapping&)=delete;
    GpuMapping& operator=(const GpuMapping&)=delete;
};
// Calls run inside a live device DDI scope, on any number of threads at once for different
// mappings: map, make_resident, ready, wait_ready, address_for_context and unmap touch the given
// mapping and only the domain's two counters, which are atomic. One mapping belongs to one caller at a
// time. open, close and invalidate_runtime are the owner's to serialize against everything else (the
// heap imports open the queue once under their own lock and close it at device teardown). No
// worker/destructor callbacks. Mapping alone does not establish residency: a mapping of an allocation
// that is not resident yet has no valid page table entries until VidMm commits it
// (trial 153: a GPU job wrote such a range 2 ms after its map fence, bugcheck 0x116).
// make_resident() takes this domain's own residency reference on the same paging
// queue, so the mapping's one fence covers the commit, the fill and the entries.
// A mapping whose request names an alignment the runtime does not satisfy by itself lives inside a GPU virtual
// address reservation this domain takes and frees with it (BD-101, map and unmap_after_gpu_retirement).
class PagingDomain final {
    HANDLE device_{};
    PFND3DDDI_CREATEPAGINGQUEUECB create_{};
    PFND3DDDI_DESTROYPAGINGQUEUECB destroy_{};
    PFND3DDDI_MAPGPUVIRTUALADDRESSCB map_{};
    PFND3DDDI_FREEGPUVIRTUALADDRESSCB free_{};
    PFND3DDDI_RESERVEGPUVIRTUALADDRESSCB reserve_{};
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMGPUCB wait_gpu_{};
    PFND3DDDI_MAKERESIDENTCB make_resident_{};
    PFND3DDDI_EVICTCB evict_{};
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMCPUCB wait_cpu_{};
    D3DKMT_HANDLE queue_{},sync_{};
    const volatile UINT64* completed_{};
    std::atomic<unsigned> mappings_{};
    std::atomic<unsigned> evict_failures_{},evicts_skipped_{};
    std::atomic<unsigned> reservations_{},reservation_failures_{};
    // Clears a mapping this domain no longer holds anything for. Only the two paths that gave its address
    // back call it, so a mapping is either complete or empty and never half of either.
    static void forget(GpuMapping& mapping) noexcept {
        mapping.owner_=nullptr;mapping.address_=0;mapping.bytes_=0;mapping.fence_=0;mapping.valid_=false;
        mapping.resident_=false;mapping.allocation_=0;mapping.reservation_=0;mapping.reservation_bytes_=0;
    }
    // Gives a reservation back, on a path that has nothing else left to free. A failure here is counted and
    // not reported: the caller is already returning a failure of its own, and a retry of a free that did not
    // clear could never succeed.
    void release_reservation(UINT64 base,UINT64 bytes) noexcept {
        if(!free_ || !base || !bytes)return;
        D3DDDICB_FREEGPUVIRTUALADDRESS args{};args.BaseAddress=base;args.Size=bytes;
        if(FAILED(free_(device_,&args)))++reservation_failures_;
    }
public:
    // The GPU virtual address granularity the runtime places a mapping on when it picks the address itself.
    // D3DDDI_MAPGPUVIRTUALADDRESS has no alignment field at all (d3dukmdt.h line 1595, WDK 10.0.26100), so up
    // to this value the address VidMm picks answers the request - every heap of this driver before BD-101 asked
    // for 64 KiB and got a 64 KiB address - and above it the only documented way to ask is a reservation. The
    // same 64 KiB is what the reservation structure itself is specified in: BaseAddress, MinimumAddress and
    // MaximumAddress must be 64 KiB aligned and Size a multiple of 64 KiB (D3DDDI_RESERVEGPUVIRTUALADDRESS).
    static constexpr UINT64 kMapGranularity=65536;
    unsigned evict_failures() const noexcept {return evict_failures_.load();}
    // Borrowed allocations whose residency reference was dropped without an Evict callback: see
    // unmap_after_gpu_retirement.
    unsigned evicts_skipped() const noexcept {return evicts_skipped_.load();}
    // BD-101: reservations taken for an alignment above kMapGranularity, and the frees of such a range that
    // the runtime refused (a leak of GPU virtual address space, nothing else).
    unsigned reservations() const noexcept {return reservations_.load();}
    unsigned reservation_failures() const noexcept {return reservation_failures_.load();}
    PagingDomain(D3D12DDI_HRTDEVICE device,const D3DDDI_DEVICECALLBACKS& cb) noexcept
        : device_(device.handle),create_(cb.pfnCreatePagingQueueCb),destroy_(cb.pfnDestroyPagingQueueCb),
          map_(cb.pfnMapGpuVirtualAddressCb),free_(cb.pfnFreeGpuVirtualAddressCb),
          reserve_(cb.pfnReserveGpuVirtualAddressCb),
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
    // alignment (BD-101): the GPU virtual address this mapping must have, a power of two; 0 asks for nothing.
    // A D3D12 heap that holds a 4x MSAA render target arrives with
    // D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT (4 MiB), and the address the runtime picks by itself is
    // 64 KiB aligned, so that heap was refused and the application read "out of memory" with 8 GB free.
    //   Above kMapGranularity the address comes from the documented route, in two callbacks:
    // pfnReserveGpuVirtualAddressCb takes a range of bytes + alignment with no memory behind it, and
    // pfnMapGpuVirtualAddressCb maps the allocation at the aligned offset inside that range. The DDI admits
    // exactly this: with a non-NULL BaseAddress "the entire range from BaseAddress to BaseAddress+Size must be
    // in a freed state or belong to a VA range that was obtained by calling pfnMapGpuVirtualAddressCb or
    // pfnReserveGpuVirtualAddressCb" (ref/ddi-display/d3dumddi.md, PFND3DDDI_MAPGPUVIRTUALADDRESSCB, Remarks;
    // WDK 10.0.26100). bytes + alignment holds an aligned window of bytes wherever the reservation lands, so
    // nothing here depends on an undocumented property of the base the runtime chose.
    //   The reservation is what the release frees, as one range: pfnFreeGpuVirtualAddressCb "releases a range
    // of GPU virtual addresses, which was previously reserved or mapped", and after the free "if there are
    // outstanding MapGpuVirtualAddress ... operations, which reference the virtual address, they will be
    // ignored" (ref/ddi-display/d3dkmthk.md, D3DKMTFreeGpuVirtualAddress, Remarks). Freeing the mapped
    // sub-range alone would leave the two outer pieces of the reservation behind for the device's life.
    //   A failure of this function releases the reservation it took: on every path that returns a failure, and
    // on an acceptance the checks below do not admit, the mapping is left empty, so the caller's release path
    // has nothing to do. An address the runtime picked is not freed here, because a mapping the checks refuse
    // has no address this driver may name.
    HRESULT map(D3DKMT_HANDLE allocation,UINT64 bytes,GpuMapping& out,UINT64 alignment=0) noexcept {
        if(out.owner_) return E_UNEXPECTED;
        if(!queue_ || !sync_ || !completed_ || !map_ || !allocation || !bytes || (bytes&4095)) return E_INVALIDARG;
        if(alignment&(alignment-1)) return E_INVALIDARG;            // a power of two, or nothing asked
        if(*completed_==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
        UINT64 base=0,reservation=0,reserved_bytes=0;
        if(alignment>kMapGranularity){
            if(!reserve_ || !free_) return E_UNEXPECTED;
            if(bytes>UINT64_MAX-alignment-kMapGranularity) return E_INVALIDARG;
            const UINT64 span=(bytes+alignment+kMapGranularity-1)&~(kMapGranularity-1);
            D3DDDI_RESERVEGPUVIRTUALADDRESS args{};args.Size=span;
            HRESULT hr=reserve_(device_,&args);
            if(FAILED(hr)) return hr;
            ++reservations_;
            reservation=args.VirtualAddress;reserved_bytes=span;
            base=reservation?(reservation+alignment-1)&~(alignment-1):0;
            // An acceptance that is not S_OK, an address this driver may not name, or a range that does not
            // hold the aligned window: whatever the call did take goes back here. The window itself always
            // fits, because the offset to the next aligned address is below the alignment and the span is
            // bytes plus the alignment; the check is the guard of that arithmetic, not a condition on the
            // base the runtime chose. Only the 4 KiB alignment that the free requires is asked of that base.
            if(hr!=S_OK || !reservation || (reservation&4095) || reservation>UINT64_MAX-span ||
               base-reservation>span-bytes){
                release_reservation(reservation,span);
                return E_UNEXPECTED;
            }
        }
        D3DDDI_MAPGPUVIRTUALADDRESS args{};args.hPagingQueue=queue_;args.hAllocation=allocation;
        args.BaseAddress=base;args.SizeInPages=bytes/4096;args.Protection.Write=1;
        HRESULT hr=map_(device_,&args);
        if(FAILED(hr) && hr!=E_PENDING){release_reservation(reservation,reserved_bytes);return hr;}
        out.owner_=this;out.address_=args.VirtualAddress;out.bytes_=bytes;out.allocation_=allocation;
        out.reservation_=reservation;out.reservation_bytes_=reserved_bytes;
        // The output fence is only meaningful for the asynchronous result.
        out.fence_=hr==E_PENDING?args.PagingFenceValue:0;
        // A base this domain asked for is a base the mapping must have: the reservation was taken for that one
        // address, and an answer anywhere else is not the mapping that was asked for. Whether an address
        // satisfies the caller's alignment is the caller's judgement and stays there (heap-import.cpp,
        // ImportStage::AddressAlignment), so that one stage names every such refusal, reserved or not.
        out.valid_=out.address_ && !(out.address_&4095) && out.address_<=UINT64_MAX-bytes &&
            (!base || out.address_==base) &&
            (hr!=E_PENDING || (out.fence_ && out.fence_!=UINT64_MAX));
        if(!out.valid_ && reservation){
            release_reservation(reservation,reserved_bytes);
            forget(out);
            return E_UNEXPECTED;
        }
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
    // borrowed (BD-075): the allocation is not this driver's. The runtime destroys it as soon as
    // pfnDestroyHeapAndResource returns, and that destroy can outrun this release - the in-DDI release of a
    // borrowed backing waits only in_ddi_bound_ms and then queues the node anyway (engine-ddi context.cpp). An
    // Evict callback naming a handle the runtime has already destroyed, and dxgkrnl may have recycled, could evict
    // somebody else's allocation, so it is not made at all: destroying an allocation drops its residency, and
    // nothing of ours needs the eviction (BD-075 review, 2026-10-06). The VA is still freed; that names an address,
    // not a handle.
    HRESULT unmap_after_gpu_retirement(GpuMapping& mapping,bool borrowed=false) noexcept {
        if(!mapping.owner_) return S_OK;
        UINT64 address=0;HRESULT hr=ready(mapping,&address);if(FAILED(hr)) return hr;
        if(!free_) return E_UNEXPECTED;
        if(mapping.resident_){
            if(borrowed) ++evicts_skipped_;
            else {
                HRESULT evicted=E_UNEXPECTED;
                if(evict_){
                    D3DDDICB_EVICT evict{};evict.NumAllocations=1;evict.AllocationList=&mapping.allocation_;
                    evicted=evict_(device_,&evict);
                }
                if(FAILED(evicted)) ++evict_failures_;
            }
            mapping.resident_=false;
        }
        D3DDDICB_FREEGPUVIRTUALADDRESS args{};
        if(mapping.reservation_){
            // BD-101: one free of the reservation releases the mapping inside it as well (d3dkmthk.md,
            // D3DKMTFreeGpuVirtualAddress). Freeing the mapped sub-range alone would leak the two outer pieces.
            args.BaseAddress=mapping.reservation_;args.Size=mapping.reservation_bytes_;
        } else {args.BaseAddress=address;args.Size=mapping.bytes_;}
        hr=free_(device_,&args);
        if(SUCCEEDED(hr)){
            forget(mapping);
            --mappings_;
        }
        return hr;
    }
    HRESULT close() noexcept {
        if(mappings_.load()) return E_PENDING;
        if(!queue_) return S_OK;
        if(!destroy_) return E_UNEXPECTED;
        D3DDDI_DESTROYPAGINGQUEUE args{};args.hPagingQueue=queue_;
        HRESULT hr=destroy_(device_,&args);
        if(SUCCEEDED(hr)){queue_=0;sync_=0;completed_=nullptr;}
        return hr;
    }
    void invalidate_runtime() noexcept {device_=nullptr;create_=nullptr;destroy_=nullptr;map_=nullptr;free_=nullptr;reserve_=nullptr;wait_gpu_=nullptr;
        make_resident_=nullptr;evict_=nullptr;wait_cpu_=nullptr;completed_=nullptr;}
};
}
