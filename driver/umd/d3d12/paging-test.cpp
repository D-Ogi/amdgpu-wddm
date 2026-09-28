// SPDX-License-Identifier: MIT
#include "paging.h"
#include <cassert>
#include <cstdio>
static UINT64 completed;
static bool asynchronous=true,failFree,failDestroy;
static unsigned maps,frees,destroys;
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
int main(){
    D3DDDI_DEVICECALLBACKS cb{};cb.pfnCreatePagingQueueCb=create;cb.pfnDestroyPagingQueueCb=destroy;
    cb.pfnMapGpuVirtualAddressCb=map;cb.pfnFreeGpuVirtualAddressCb=free_va;
    native12::PagingDomain a({owner},cb),b({owner},cb);assert(a.open()==S_OK && b.open()==S_OK);
    native12::GpuMapping m;UINT64 address=99;
    assert(a.map(9,65536,m)==S_OK);assert(a.ready(m,&address)==E_PENDING && !address);
    assert(a.close()==E_PENDING && !destroys);
    assert(a.unmap_after_gpu_retirement(m)==E_PENDING && !frees);
    assert(b.ready(m,&address)==E_INVALIDARG && !address);
    auto count=maps;assert(a.map(9,65536,m)==E_UNEXPECTED && maps==count);
    completed=7;assert(a.ready(m,&address)==S_OK && address==0x200010000ull);
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
    puts("paging ownership, pending VA, loss and callback lifetime gates passed");
}
