// SPDX-License-Identifier: MIT
#pragma once
#include "allocation-request.h"
#include "paging.h"
#include <new>

namespace native12 {
class MemoryRegistry;
struct MemoryKey {
    const MemoryRegistry* registry{};
    UINT64 serial{};
};
struct MemorySnapshot {
    D3DKMT_HANDLE allocation{};
    UINT64 address{},bytes{};
};

// Device-owned metadata, independent of runtime heap/resource private storage.
// Runtime calls run outside lock_. Lifetime operations on a paging domain must
// still obey PagingDomain's serialization contract. This is not a Vulkan import
// or residency manager, and no address it returns proves residency.
class MemoryRegistry final {
    struct Record {
        RuntimeAllocation allocation;
        GpuMapping mapping;
        PagingDomain* paging;
        Record* next{};
        UINT64 serial{},bytes{};
        bool busy{true},retired{},releasing{};
        Record(D3D12DDI_HRTDEVICE d,const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb,
               PagingDomain& p,UINT64 size) noexcept:allocation(d,cb),paging(&p),bytes(size) {}
    };
    SRWLOCK lock_=SRWLOCK_INIT;
    Record* head_{};
    UINT64 serial_{};
    HRESULT pin(MemoryKey key,Record*& out) noexcept {
        out=nullptr;
        if(key.registry!=this || !key.serial) return E_INVALIDARG;
        AcquireSRWLockExclusive(&lock_);
        Record* r=head_;while(r && r->serial!=key.serial) r=r->next;
        HRESULT hr=!r?E_INVALIDARG:r->retired?E_UNEXPECTED:r->busy?E_PENDING:S_OK;
        if(hr==S_OK){r->busy=true;out=r;}
        ReleaseSRWLockExclusive(&lock_);
        return hr;
    }
    void unpin(Record* r) noexcept {
        AcquireSRWLockExclusive(&lock_);r->busy=false;ReleaseSRWLockExclusive(&lock_);
    }
    void erase(Record* r) noexcept {
        AcquireSRWLockExclusive(&lock_);
        auto link=&head_;while(*link && *link!=r) link=&(*link)->next;
        if(*link) *link=r->next;
        ReleaseSRWLockExclusive(&lock_);
        delete r;
    }
    void retire(Record* r) noexcept {
        // The runtime owner is about to expire. Do not retry its callbacks later.
        r->allocation.invalidate_runtime();
        AcquireSRWLockExclusive(&lock_);r->retired=true;r->busy=false;ReleaseSRWLockExclusive(&lock_);
    }
public:
    MemoryRegistry()=default;
    MemoryRegistry(const MemoryRegistry&)=delete;
    MemoryRegistry& operator=(const MemoryRegistry&)=delete;
    // All metadata is allocated and linked before the first callback. A failed
    // creation returns an empty key; failed cleanup remains tracked internally.
    // S_OK means allocation and mapping were accepted, not paging completion.
    HRESULT create(D3D12DDI_HRTDEVICE device,const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb,
                   PagingDomain& paging,UINT64 bytes,UINT64 alignment,AllocationAccess access,
                   HANDLE runtime_owner,MemoryKey& out) noexcept {
        if(out.registry || out.serial) return E_UNEXPECTED;
        AllocationRequest request;
        HRESULT hr=request.prepare(bytes,alignment,access,runtime_owner);if(FAILED(hr)) return hr;
        auto r=new(std::nothrow) Record(device,cb,paging,request.blob.alloc_size);
        if(!r) return E_OUTOFMEMORY;
        AcquireSRWLockExclusive(&lock_);
        if(serial_==UINT64_MAX){ReleaseSRWLockExclusive(&lock_);delete r;return E_OUTOFMEMORY;}
        r->serial=++serial_;r->next=head_;head_=r;
        ReleaseSRWLockExclusive(&lock_);
        hr=r->allocation.open(request.args);
        if(FAILED(hr)){erase(r);return hr;}
        hr=paging.map(r->allocation.handle(),r->bytes,r->mapping);
        if(FAILED(hr)){
            HRESULT cleanup=paging.unmap_after_gpu_retirement(r->mapping);
            if(SUCCEEDED(cleanup)) cleanup=r->allocation.close();
            if(FAILED(cleanup)) retire(r);else erase(r);
            return hr;
        }
        out={this,r->serial};unpin(r);return S_OK;
    }
    HRESULT snapshot(MemoryKey key,MemorySnapshot& out) noexcept {
        out={};Record* r=nullptr;HRESULT hr=pin(key,r);if(FAILED(hr)) return hr;
        if(r->releasing) hr=E_UNEXPECTED;
        else {
            UINT64 address=0;hr=r->paging->ready(r->mapping,&address);
            if(hr==S_OK) out={r->allocation.handle(),address,r->bytes};
        }
        unpin(r);return hr;
    }
    // Retire GPU uses and release any Vulkan import before entering this method.
    // A failed unmap/deallocation retains the record and key, allowing a retry
    // only while the owning runtime scope is still valid. It cannot be reused.
    HRESULT release_after_gpu_retirement(MemoryKey& key) noexcept {
        if(!key.registry && !key.serial) return S_OK;
        Record* r=nullptr;HRESULT hr=pin(key,r);if(FAILED(hr)) return hr;
        r->releasing=true;
        hr=r->paging->unmap_after_gpu_retirement(r->mapping);
        if(SUCCEEDED(hr)) hr=r->allocation.close();
        if(FAILED(hr)){unpin(r);return hr;}
        erase(r);key={};return S_OK;
    }
    HRESULT invalidate_owner(MemoryKey key) noexcept {
        Record* r=nullptr;HRESULT hr=pin(key,r);if(FAILED(hr)) return hr;
        retire(r);return S_OK;
    }
    bool empty() noexcept {
        AcquireSRWLockShared(&lock_);bool result=!head_;ReleaseSRWLockShared(&lock_);return result;
    }
    // Terminal teardown only, after callers stop and before PagingDomain dies.
    // This discards CPU metadata, never claims runtime allocation/VA reclamation.
    void discard_device_metadata(unsigned& retired,unsigned& active) noexcept {
        retired=active=0;
        AcquireSRWLockExclusive(&lock_);auto list=head_;head_=nullptr;ReleaseSRWLockExclusive(&lock_);
        while(list){auto r=list;list=r->next;if(r->retired) ++retired;else ++active;
            r->allocation.invalidate_runtime();delete r;}
    }
};
}
