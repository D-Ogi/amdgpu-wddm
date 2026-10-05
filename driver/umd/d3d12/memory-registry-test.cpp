// SPDX-License-Identifier: MIT
#include "memory-registry.h"
#include <cassert>
#include <cstdio>
#include <vector>

static UINT64 completed;
static HRESULT allocate_result=S_OK,map_result=E_PENDING,free_result=S_OK,deallocate_result=S_OK;
static UINT64 mapped_address=0x200010000ull;
static unsigned next_allocation=100;
static std::vector<char> calls;
static native12::MemoryRegistry* registry;
static HANDLE device=reinterpret_cast<HANDLE>(UINT_PTR(27));
static HANDLE resource=reinterpret_cast<HANDLE>(UINT_PTR(99));
static void callback(char op){
    assert(registry && !registry->empty()); // Reentrant read proves callbacks are outside the registry lock.
    calls.push_back(op);
}
static HRESULT APIENTRY allocate(D3D12DDI_HRTDEVICE d,D3D12DDICB_ALLOCATE_0022* a){
    assert(d.handle==device && a->hResource==resource && a->NumAllocations==1);
    callback('A');if(FAILED(allocate_result)) return allocate_result;
    a->pAllocationInfo[0].hAllocation=++next_allocation;
    a->pAllocationInfo[0].GpuVirtualAddress=0;return S_OK;
}
static HRESULT APIENTRY deallocate(D3D12DDI_HRTDEVICE d,const D3D12DDICB_DEALLOCATE_0022* a){
    assert(d.handle==device && a->NumAllocations==1 && a->HandleList[0]>100 && !a->hResource);
    callback('D');return deallocate_result;
}
static HRESULT APIENTRY create_queue(HANDLE d,D3DDDICB_CREATEPAGINGQUEUE* a){
    assert(d==device);a->hPagingQueue=5;a->hSyncObject=6;a->FenceValueCPUVirtualAddress=&completed;return S_OK;
}
static HRESULT APIENTRY destroy_queue(HANDLE d,const D3DDDI_DESTROYPAGINGQUEUE* a){
    assert(d==device && a->hPagingQueue==5);return S_OK;
}
static HRESULT APIENTRY map(HANDLE d,D3DDDI_MAPGPUVIRTUALADDRESS* a){
    assert(d==device && a->hPagingQueue==5 && a->hAllocation>100 && a->SizeInPages==16);
    callback('M');a->VirtualAddress=mapped_address;a->PagingFenceValue=7;return map_result;
}
static HRESULT APIENTRY free_va(HANDLE d,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){
    assert(d==device && a->BaseAddress==mapped_address && a->Size==65536);
    callback('F');return free_result;
}
int main(){
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 cb{};cb.pfnAllocateCb=allocate;cb.pfnDeallocateCb=deallocate;
    D3DDDI_DEVICECALLBACKS kt{};kt.pfnCreatePagingQueueCb=create_queue;kt.pfnDestroyPagingQueueCb=destroy_queue;
    kt.pfnMapGpuVirtualAddressCb=map;kt.pfnFreeGpuVirtualAddressCb=free_va;
    native12::PagingDomain paging({device},kt);assert(paging.open()==S_OK);
    native12::MemoryRegistry a,b;registry=&a;
    auto create=[&](native12::MemoryKey& key){return a.create({device},cb,paging,1,65536,
        native12::AllocationAccess::GpuOnly,resource,key);};
    native12::MemoryKey key;native12::MemorySnapshot snapshot;
    assert(create(key)==S_OK && key.registry==&a && key.serial && !a.empty());
    assert(a.snapshot(key,snapshot)==E_PENDING && !snapshot.allocation && !snapshot.address);
    auto count=calls.size();assert(create(key)==E_UNEXPECTED && calls.size()==count);
    assert(b.snapshot(key,snapshot)==E_INVALIDARG && !snapshot.address);
    completed=7;assert(a.snapshot(key,snapshot)==S_OK && snapshot.allocation==101 && snapshot.bytes==65536);
    assert(snapshot.address==mapped_address);
    free_result=E_FAIL;count=calls.size();
    assert(a.release_after_gpu_retirement(key)==E_FAIL && calls.size()==count+1 && calls.back()=='F');
    assert(a.snapshot(key,snapshot)==E_UNEXPECTED && !snapshot.address);
    free_result=S_OK;deallocate_result=E_FAIL;count=calls.size();
    assert(a.release_after_gpu_retirement(key)==E_FAIL && calls.size()==count+2 && calls.back()=='D');
    deallocate_result=S_OK;auto stale=key;count=calls.size();
    assert(a.release_after_gpu_retirement(key)==S_OK && !key.registry && a.empty());
    assert(calls.size()==count+1 && calls.back()=='D'); // The successful unmap is not repeated.
    assert(a.snapshot(stale,snapshot)==E_INVALIDARG);
    assert(a.release_after_gpu_retirement(key)==S_OK && calls.size()==count+1);
    assert(create(key)==S_OK && key.serial!=stale.serial);
    assert(a.snapshot(stale,snapshot)==E_INVALIDARG); // No pointer-reuse/ABA lookup.
    assert(a.release_after_gpu_retirement(key)==S_OK);
    allocate_result=E_OUTOFMEMORY;count=calls.size();
    assert(create(key)==E_OUTOFMEMORY && a.empty() && !key.registry && calls.size()==count+1);
    allocate_result=S_OK;map_result=E_OUTOFMEMORY;count=calls.size();
    assert(create(key)==E_OUTOFMEMORY && a.empty() && calls.size()==count+3 && calls.back()=='D');
    deallocate_result=E_FAIL;
    assert(create(key)==E_OUTOFMEMORY && !key.registry && !a.empty());
    unsigned retired=0,active=0;count=calls.size();a.discard_device_metadata(retired,active);
    assert(retired==1 && active==0 && a.empty() && calls.size()==count);
    deallocate_result=S_OK;map_result=E_PENDING;completed=0;
    assert(create(key)==S_OK);count=calls.size();
    assert(a.release_after_gpu_retirement(key)==E_PENDING && calls.size()==count);
    completed=7;assert(a.release_after_gpu_retirement(key)==S_OK);
    assert(create(key)==S_OK);assert(a.invalidate_owner(key)==S_OK);count=calls.size();
    assert(a.release_after_gpu_retirement(key)==E_UNEXPECTED && calls.size()==count);
    a.discard_device_metadata(retired,active);assert(retired==1 && !active && calls.size()==count);
    // Discarding metadata is not successful GPU VA cleanup; pending mappings remain accounted for.
    assert(paging.close()==E_PENDING);
    puts("memory registry ownership, pending mapping, rollback, retry and expired-owner gates passed");
}
