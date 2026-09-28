// SPDX-License-Identifier: MIT
#include "paging.h"
#include <cassert>
#include <cstdio>
static UINT64 completed;
static bool asynchronous=true,failFree,failDestroy;
static unsigned maps,frees,destroys,waits;
static HRESULT wait_result=S_OK;
static bool lose_during_wait;
static HANDLE expected_context=reinterpret_cast<HANDLE>(UINT_PTR(0x100000031ull));
static HANDLE owner=reinterpret_cast<HANDLE>(UINT_PTR(27));
static HRESULT APIENTRY create(HANDLE d,D3DDDICB_CREATEPAGINGQUEUE* a){
    assert(d==owner);a->hPagingQueue=5;a->hSyncObject=6;a->FenceValueCPUVirtualAddress=&completed;return S_OK;
}
static HRESULT APIENTRY destroy(HANDLE d,const D3DDDI_DESTROYPAGINGQUEUE* a){
    assert(d==owner && a->hPagingQueue==5);++destroys;return failDestroy?E_FAIL:S_OK;
}
static HRESULT APIENTRY map(HANDLE d,D3DDDI_MAPGPUVIRTUALADDRESS* a){
    assert(d==owner && a->hPagingQueue==5 && a->hAllocation==9 && a->SizeInPages==16 && a->Protection.Write);
    ++maps;a->VirtualAddress=0x200010000ull;a->PagingFenceValue=7;
    return asynchronous?E_PENDING:S_OK;
}
static HRESULT APIENTRY free_va(HANDLE d,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){
    assert(d==owner && a->BaseAddress==0x200010000ull && a->Size==65536);++frees;return failFree?E_FAIL:S_OK;
}
static HRESULT APIENTRY wait_gpu(HANDLE d,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a){
    assert(d==owner && a->hContext==expected_context && a->ObjectCount==1);
    assert(a->ObjectHandleArray[0]==6 && a->MonitoredFenceValueArray[0]==7);
    ++waits;if(lose_during_wait) completed=UINT64_MAX;
    // The mock only queues the wait. It deliberately does not advance the fence.
    return wait_result;
}
int main(){
    D3DDDI_DEVICECALLBACKS cb{};cb.pfnCreatePagingQueueCb=create;cb.pfnDestroyPagingQueueCb=destroy;
    cb.pfnMapGpuVirtualAddressCb=map;cb.pfnFreeGpuVirtualAddressCb=free_va;
    cb.pfnWaitForSynchronizationObjectFromGpuCb=wait_gpu;
    native12::PagingDomain a({owner},cb),b({owner},cb);assert(a.open()==S_OK && b.open()==S_OK);
    native12::GpuMapping m;UINT64 address=99;
    assert(a.map(9,65536,m)==S_OK);assert(a.ready(m,&address)==E_PENDING && !address);
    assert(a.address_for_context(m,expected_context,&address)==S_OK && address==0x200010000ull && waits==1);
    assert(completed==0 && a.ready(m,&address)==E_PENDING && !address);
    // Each independent context gets its own ordering; no global ready-state cache.
    expected_context=reinterpret_cast<HANDLE>(UINT_PTR(0x200000031ull));
    assert(a.address_for_context(m,expected_context,&address)==S_OK && address && waits==2);
    wait_result=E_FAIL;
    assert(a.address_for_context(m,expected_context,&address)==E_FAIL && !address && waits==3);
    wait_result=S_FALSE;
    assert(a.address_for_context(m,expected_context,&address)==E_UNEXPECTED && !address && waits==4);
    wait_result=S_OK;lose_during_wait=true;
    assert(a.address_for_context(m,expected_context,&address)==D3DDDIERR_DEVICEREMOVED && !address && waits==5);
    lose_during_wait=false;completed=0;
    assert(a.address_for_context(m,nullptr,&address)==E_INVALIDARG && !address && waits==5);
    assert(b.address_for_context(m,expected_context,&address)==E_INVALIDARG && !address && waits==5);
    assert(a.close()==E_PENDING && !destroys);
    assert(a.unmap_after_gpu_retirement(m)==E_PENDING && !frees);
    assert(b.ready(m,&address)==E_INVALIDARG && !address);
    auto count=maps;assert(a.map(9,65536,m)==E_UNEXPECTED && maps==count);
    completed=7;assert(a.ready(m,&address)==S_OK && address==0x200010000ull);
    auto wait_count=waits;
    assert(a.address_for_context(m,expected_context,&address)==S_OK && address && waits==wait_count);
    failFree=true;assert(a.unmap_after_gpu_retirement(m)==E_FAIL);
    assert(a.ready(m,&address)==S_OK && address);failFree=false;
    assert(a.unmap_after_gpu_retirement(m)==S_OK);count=frees;
    assert(a.unmap_after_gpu_retirement(m)==S_OK && frees==count);
    // S_OK must ignore the callback's unrelated fence output.
    asynchronous=false;completed=0;assert(a.map(9,65536,m)==S_OK);
    assert(a.ready(m,&address)==S_OK && address);
    completed=UINT64_MAX;assert(a.ready(m,&address)==D3DDDIERR_DEVICEREMOVED && !address);
    completed=0;assert(a.unmap_after_gpu_retirement(m)==S_OK);
    failDestroy=true;assert(a.close()==E_FAIL);failDestroy=false;assert(a.close()==S_OK);
    assert(b.close()==S_OK);
    native12::PagingDomain expired({owner},cb);assert(expired.open()==S_OK);
    assert(expired.map(9,65536,m)==S_OK);expired.invalidate_runtime();count=frees;
    assert(expired.ready(m,&address)==E_UNEXPECTED && !address);
    assert(expired.unmap_after_gpu_retirement(m)==E_UNEXPECTED && frees==count);
    wait_count=waits;
    assert(expired.address_for_context(m,expected_context,&address)==E_UNEXPECTED && !address && waits==wait_count);
    cb.pfnWaitForSynchronizationObjectFromGpuCb=nullptr;
    native12::PagingDomain no_wait({owner},cb);native12::GpuMapping pending;
    asynchronous=true;completed=0;assert(no_wait.open()==S_OK && no_wait.map(9,65536,pending)==S_OK);
    assert(no_wait.address_for_context(pending,expected_context,&address)==E_UNEXPECTED && !address && waits==wait_count);
    completed=7;assert(no_wait.unmap_after_gpu_retirement(pending)==S_OK && no_wait.close()==S_OK);
    puts("paging ownership, context GPU waits, pending VA, loss and callback lifetime gates passed");
}
