// SPDX-License-Identifier: MIT
#include "runtime-surface-allocation.h"
#include <cstdlib>
#include <iostream>
using namespace bc250::umd;
namespace {
int deviceIdentity,resourceIdentity; unsigned allocates=0,deallocates=0;
bool failAllocate=false,failFree=false,resourceClose=true;
void check(bool b) { if (!b) std::abort(); }
HRESULT APIENTRY allocate(HANDLE h,D3DDDICB_ALLOCATE *a) {
    ++allocates; check(h==&deviceIdentity && a->NumAllocations==1 && a->PrivateDriverDataSize==16);
    auto *words=static_cast<const UINT *>(a->pPrivateDriverData);
    check(words[0]==0x52363245u && words[1]==2 && words[2]==1 && words[3]==2);
    auto *s=static_cast<const BC250_WDDM_ALLOCATION_PRIVATE *>(a->pAllocationInfo2[0].pPrivateDriverData);
    check(s->Pitch==256 && s->Size==4096 && !a->pAllocationInfo2[0].Flags.Primary);
    if (failAllocate) return E_OUTOFMEMORY;
    a->pAllocationInfo2[0].hAllocation=41; a->hKMResource=42; return S_OK;
}
HRESULT APIENTRY deallocate(HANDLE h,const D3DDDICB_DEALLOCATE2 *a) {
    ++deallocates; check(h==&deviceIdentity);
    if (resourceClose) check(a->hResource==&resourceIdentity && !a->NumAllocations && !a->HandleList);
    else check(!a->hResource && a->NumAllocations==1 && *a->HandleList==41);
    return failFree ? E_FAIL : S_OK;
}
}
int main() {
    RuntimeDevice device; device.hDevice=&deviceIdentity;
    device.KTCallbacks.pfnAllocateCb=allocate; device.KTCallbacks.pfnDeallocate2Cb=deallocate;
    RuntimeSurfaceRequest request{}; request.runtime_resource=&resourceIdentity; request.shared=true; request.cpu_read=true;
    request.surface={BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,1,64,16,256,D3DDDIFMT_A8R8G8B8,4096};
    RuntimeSurfaceAllocation surface{};
    check(allocate_runtime_surface(device,request,surface)==E_INVALIDARG && !allocates);
    RuntimeDomain::Scope scope(device.domain);
    request.primary=true; check(allocate_runtime_surface(device,request,surface)==E_INVALIDARG && !allocates); request.primary=false;
    failAllocate=true; check(allocate_runtime_surface(device,request,surface)==E_OUTOFMEMORY && !surface.allocation);
    failAllocate=false; check(allocate_runtime_surface(device,request,surface)==S_OK && surface.allocation==41 && surface.kernel_resource==42);
    check(allocate_runtime_surface(device,request,surface)==E_UNEXPECTED && allocates==2);
    failFree=true; check(deallocate_runtime_surface(device,surface)==E_FAIL && surface.allocation==41);
    failFree=false; check(deallocate_runtime_surface(device,surface)==S_OK && !surface.runtime_resource && !surface.allocation);
    check(deallocate_runtime_surface(device,surface)==S_OK && deallocates==2);
    request.runtime_resource=nullptr; resourceClose=false;
    check(allocate_runtime_surface(device,request,surface)==S_OK && deallocate_runtime_surface(device,surface)==S_OK);
    std::cout << "PASS runtime surface allocation ABI, domain, failure retention and resource-handle close\n";
}
