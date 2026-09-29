// SPDX-License-Identifier: MIT
#include "allocation.h"
#include <cassert>
#include <cstdio>
#include <cstring>
static unsigned allocations,frees;
static bool failAllocation,failFree,byOwner;
static D3D12DDI_HRTDEVICE seen{};
static HANDLE resource=reinterpret_cast<HANDLE>(UINT_PTR(77));
static HRESULT APIENTRY allocate(D3D12DDI_HRTDEVICE device,D3D12DDICB_ALLOCATE_0022* a) {
    ++allocations;seen=device;assert(a->NumAllocations==1 && !a->hKMResource);
    assert(a->hResource==resource && a->pAllocationInfo->PrivateDriverDataSize==4);
    if(failAllocation) return E_OUTOFMEMORY;
    a->pAllocationInfo->hAllocation=100+allocations;
    a->pAllocationInfo->GpuVirtualAddress=0x123456780000ull+allocations*0x10000ull;
    return S_OK;
}
static HRESULT APIENTRY deallocate(D3D12DDI_HRTDEVICE device,const D3D12DDICB_DEALLOCATE_0022* a) {
    ++frees;seen=device;
    if(byOwner){
        // The owner form: the runtime resource, no handle, the resident object's two flags.
        assert(a->hResource==resource && !a->NumAllocations && !a->HandleList);
        assert(unsigned(a->Flags)==(unsigned(D3D12DDI_DEALLOCATE_FLAGS_0022_ASSUME_NOT_IN_USE)|
            unsigned(D3D12DDI_DEALLOCATE_FLAGS_0022_SYNCHRONOUS_DESTROY)));
        return failFree?E_FAIL:S_OK;
    }
    assert(a->NumAllocations==1 && *a->HandleList>=100);
    assert(!a->hResource && a->Flags==D3D12DDI_DEALLOCATE_FLAGS_0022_NONE);
    return failFree?E_FAIL:S_OK;
}
int main() {
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 cb{};cb.pfnAllocateCb=allocate;cb.pfnDeallocateCb=deallocate;
    D3D12DDI_HRTDEVICE device{};UINT_PTR token=42;memcpy(&device,&token,sizeof(device));
    UINT privateData=123;
    D3D12DDI_ALLOCATION_INFO_0022 info{};info.pPrivateDriverData=&privateData;info.PrivateDriverDataSize=4;
    D3D12DDICB_ALLOCATE_0022 args{};args.hResource=resource;args.NumAllocations=1;args.pAllocationInfo=&info;
    native12::RuntimeAllocation a(device,cb);
    failAllocation=true;assert(a.open(args)==E_OUTOFMEMORY && !a.handle());
    failAllocation=false;assert(a.open(args)==S_OK && !memcmp(&seen,&device,sizeof(device)));
    assert(!info.hAllocation && !info.GpuVirtualAddress); // caller descriptor not retained or modified
    auto handle=a.handle();auto address=a.address();assert(handle && address>0xffffffffull);
    auto count=allocations;assert(a.open(args)==E_UNEXPECTED && allocations==count);
    failFree=true;assert(a.close()==E_FAIL && a.handle()==handle && a.address()==address);
    failFree=false;assert(a.close()==S_OK && !a.handle() && !a.address());
    count=frees;assert(a.close()==S_OK && frees==count);
    assert(a.open(args)==S_OK);handle=a.handle();a.invalidate_runtime();count=frees;
    assert(a.close()==E_UNEXPECTED && a.handle()==handle && frees==count);
    {
        using native12::ReleaseForm;
        native12::RuntimeAllocation owned(device,cb);assert(owned.open(args)==S_OK && owned.owner_known());
        byOwner=true;failFree=true;count=frees;
        assert(owned.close(ReleaseForm::Owner)==E_FAIL && owned.handle() && owned.owner_known() && frees==count+1);
        failFree=false;assert(owned.close(ReleaseForm::Owner)==S_OK && !owned.handle() && !owned.owner_known());
        // Without its owner the allocation stays, and the owner form makes no callback.
        native12::RuntimeAllocation expired(device,cb);assert(expired.open(args)==S_OK);
        const auto kept=expired.handle();expired.revoke_owner();count=frees;
        assert(!expired.owner_known() && expired.close(ReleaseForm::Owner)==E_UNEXPECTED);
        assert(expired.handle()==kept && frees==count);
        byOwner=false;
    }
    cb.pfnDeallocateCb=nullptr;native12::RuntimeAllocation bad(device,cb);count=allocations;
    assert(bad.open(args)==E_INVALIDARG && allocations==count);
    cb.pfnDeallocateCb=deallocate;
    count=frees;
    {native12::RuntimeAllocation transient(device,cb);assert(transient.open(args)==S_OK);}
    assert(frees==count); // destruction never invokes a runtime callback
    native12::RuntimeAllocation invalid(device,cb);count=allocations;
    args.NumAllocations=2;assert(invalid.open(args)==E_INVALIDARG && allocations==count);
    args.NumAllocations=1;args.hKMResource=1;assert(invalid.open(args)==E_INVALIDARG && allocations==count);
    args.hKMResource=0;info.pPrivateDriverData=nullptr;assert(invalid.open(args)==E_INVALIDARG && allocations==count);
    puts("runtime allocation ownership/failure tests passed, handle form and owner form");
}
