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
// BD-052: that reference is an ordering device, not a lifetime hold. release_residency() gives it back
// without unmapping, and the owner gives it back inside the creation DDI that took it, because the runtime
// owns the residency of an application's heap and the driver "must no longer create allocations ... as
// resident during creation" (DirectX-Specs ResourceHeaps.md @ 5a4139b, Residency DDIs, line 1011).
// order_context() orders what an internal context submits behind the paging work this driver has queued on
// its own paging queue; it is an ordering of this driver's paging operations and not a residency guarantee.
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
    // BD-052. queued_: the largest value this domain has ever asked of its own monitored fence, over every
    // mapping (the map and the residency operations). It only grows, so a context that waits for it waits
    // for every paging operation this driver has queued, whichever mapping queued it. residency_releases_:
    // Evict callbacks made by release_residency alone (not by the unmap). ordered_waits_: GPU waits
    // order_context inserted, which is the cost of the guarantee a lab run can read.
    std::atomic<UINT64> queued_{};
    std::atomic<unsigned> residency_releases_{},ordered_waits_{};
    void note_queued(UINT64 value) noexcept {
        UINT64 seen=queued_.load(std::memory_order_relaxed);
        while(value>seen && !queued_.compare_exchange_weak(seen,value,std::memory_order_relaxed)){}
    }
    // A creation hand-back retains the increment on failure and reports it, so
    // that the caller fails creation and cleans up. Ordinary teardown is still
    // best effort: allocation destruction clears an increment a refused Evict
    // leaves in VidMm. borrowed: an allocation the runtime may already have
    // destroyed gets no callback on its potentially recycled handle (BD-075).
    HRESULT evict_residency(GpuMapping& mapping,bool borrowed,bool retain_on_failure=false) noexcept {
        if(borrowed) ++evicts_skipped_;
        else {
            HRESULT evicted=E_UNEXPECTED;
            if(evict_){
                D3DDDICB_EVICT evict{};evict.NumAllocations=1;evict.AllocationList=&mapping.allocation_;
                evicted=evict_(device_,&evict);
            }
            if(evicted!=S_OK){
                ++evict_failures_;
                // PFND3DDDI_EVICTCB returns S_OK on success, an HRESULT error
                // otherwise. A creation cannot publish the heap while its own
                // residency increment remains. Keep it owned for failed-create
                // cleanup; the ordinary teardown below remains best effort.
                if(retain_on_failure)return FAILED(evicted)?evicted:E_UNEXPECTED;
            }
        }
        mapping.resident_=false;
        return S_OK;
    }
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
    // BD-052: the counters of the residency hand-back and of the ordering waits (see queued_).
    unsigned residency_releases() const noexcept {return residency_releases_.load();}
    unsigned ordered_waits() const noexcept {return ordered_waits_.load();}
    UINT64 queued_fence() const noexcept {return queued_.load();}
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
        if(out.valid_)note_queued(out.fence_);
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
    // queue; released by release_residency, or by unmap_after_gpu_retirement when a failed
    // import never reached that call. Its pending value joins the mapping's
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
            note_queued(mapping.fence_);
        }
        return S_OK;
    }
    // Whether this domain holds the residency reference of that mapping now (BD-052, the heap imports read
    // it before they take one: make_resident refuses a second reference on one mapping).
    bool holds_residency(const GpuMapping& mapping) const noexcept {
        return mapping.owner_==this && mapping.resident_;
    }
    // BD-052: gives this domain's residency reference back on its own, without unmapping. The caller proves
    // that every GPU use which the reference was taken for has retired, exactly as the unmap's caller does;
    // the mapping, its address and its fence are untouched, so the address stays usable and the record stays
    // releasable afterwards. A refused callback leaves the reference owned and
    // returns the error; the caller must not publish a successful import.
    //   The import takes this reference only for the window inside its creation DDI where the mapping is not
    // committed yet, and gives it back before that DDI returns (heap-import.cpp,
    // release_import_residency). Holding it for the life of the heap made the application's Evict a no-op
    // and kept a heap the application asked to be created non-resident resident anyway (BD-052;
    // DirectX-Specs ResourceHeaps.md @ 5a4139b, "the driver must no longer create allocations ... as
    // resident during creation", line 1011).
    //   borrowed: as in unmap_after_gpu_retirement, an allocation the runtime owns and may already have
    // destroyed gets no Evict callback at all; destroying an allocation drops its residency (BD-075).
    HRESULT release_residency(GpuMapping& mapping,bool borrowed=false) noexcept {
        if(mapping.owner_!=this) return E_INVALIDARG;
        if(!mapping.resident_) return S_FALSE;
        const HRESULT hr=evict_residency(mapping,borrowed,true);
        if(hr!=S_OK)return hr;
        ++residency_releases_;
        return S_OK;
    }
    // BD-052, for the engine's and the ICD's internal contexts, which the runtime never sees and therefore
    // never makes wait for a paging operation of ours (trial 153: a GPU job wrote a mapped range 2 ms after
    // its map fence and bugchecked 0x116).
    //   Orders everything the given context submits after this call behind every operation this domain has
    // queued on its own paging queue: one GPU wait on the domain's monitored fence for queued_.
    //   What it is not: a statement that anything is resident. It covers the paging operations of this
    // driver, which is all the driver has an object for - D3D12DDIARG_MAKERESIDENT_0001 gives a value and an
    // opaque D3D12DDI_HRTPAGINGQUEUE, and "the driver must not attempt to use these values"
    // (ResourceHeaps.md @ 5a4139b, line 1017), so the runtime's residency fence cannot be named here at all.
    // The residency of an application's heap after its creation DDI is the runtime's (heap-import.h).
    //   S_FALSE: nothing to wait for, which is the steady state (the paging queue has caught up, and an
    // import waits for its own map on the CPU before its VA leaves the module, so it normally is). No wait is
    // inserted then, so the cost on a submission path is one atomic read and one comparison.
    HRESULT order_context(HANDLE context) noexcept {
        if(!context) return E_INVALIDARG;
        if(!queue_ || !sync_ || !completed_) return E_UNEXPECTED;
        UINT64 wanted=queued_.load();
        if(!wanted) return S_FALSE;
        UINT64 value=*completed_;MemoryBarrier();
        if(value==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
        if(value>=wanted) return S_FALSE;
        if(!wait_gpu_) return E_UNEXPECTED;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU args{};
        args.hContext=context;args.ObjectCount=1;args.ObjectHandleArray=&sync_;
        args.MonitoredFenceValueArray=&wanted;
        HRESULT hr=wait_gpu_(device_,&args);
        if(hr!=S_OK) return FAILED(hr)?hr:E_UNEXPECTED;
        ++ordered_waits_;
        // Loss may become visible while the runtime processes the wait request.
        value=*completed_;MemoryBarrier();
        return value==UINT64_MAX?D3DDDIERR_DEVICEREMOVED:S_OK;
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
        // BD-052: the same Evict this path always made, now in the one place the heap imports also use
        // (release_residency). residency_releases_ counts the hand-backs alone, so it is not raised here.
        if(mapping.resident_) evict_residency(mapping,borrowed);
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
    // BD-201 R v2: the release of a mapping whose GPU retirement is NOT proved, which asks the runtime for
    // nothing. No Evict: destroying the allocation drops its residency anyway (unmap_after_gpu_retirement).
    // No FreeGpuVirtualAddress: a free takes effect at once and would make the address reusable while work
    // may still read it, while the range the map call allocated needs no free at all - "The GPU VA range,
    // which is allocated by the API, is freed when the allocation (the GPU VA is mapped to) is destroyed"
    // (d3dkmthk.h, D3DKMTMapGpuVirtualAddress). The caller then destroys the allocation with flags NONE,
    // which VidMm defers behind the commands queued before the request (d3dukmdt.h,
    // D3DDDICB_DESTROYALLOCATION2FLAGS, AssumeNotInUse FALSE).
    // A reservation is this driver's own range and no allocation destroy frees it: its span is handed to the
    // caller, which counts it as leaked, and it is never freed without the proof. The mapping is forgotten
    // either way, so the domain's count falls and the paging queue can still close.
    void release_mapping_to_os(GpuMapping& mapping,UINT64* reservation,UINT64* reservation_bytes) noexcept {
        if(!mapping.owner_)return;
        if(mapping.reservation_){
            if(reservation)*reservation=mapping.reservation_;
            if(reservation_bytes)*reservation_bytes=mapping.reservation_bytes_;
        }
        forget(mapping);
        --mappings_;
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
