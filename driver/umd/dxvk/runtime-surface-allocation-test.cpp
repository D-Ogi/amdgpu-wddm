// SPDX-License-Identifier: MIT
#include "runtime-surface-allocation.h"
#include "../../kmd/surface_resource_private.h"
#include <cstdlib>
#include <iostream>
using namespace bc250::umd;
namespace {
int deviceIdentity,resourceIdentity; unsigned allocates=0,deallocates=0;
bool failAllocate=false,failFree=false,resourceClose=true,extendedPrivate=false;
void check(bool b) { if (!b) std::abort(); }
HRESULT APIENTRY allocate(HANDLE h,D3DDDICB_ALLOCATE *a) {
    ++allocates; check(h==&deviceIdentity && a->NumAllocations==1 && a->PrivateDriverDataSize==(extendedPrivate ? 64u : 16u));
    auto *words=static_cast<const UINT *>(a->pPrivateDriverData);
    check(words[0]==0x52363245u && words[1]==(extendedPrivate ? 3u : 2u) && words[2]==1 && words[3]==2);
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
namespace {
UINT64 pagingCpu=0; unsigned unmaps=0; bool rejectResident=false,rejectUnmap=false;
HRESULT APIENTRY createQueue(HANDLE,D3DDDICB_CREATEPAGINGQUEUE *q) { q->hPagingQueue=7; q->hSyncObject=8; q->FenceValueCPUVirtualAddress=&pagingCpu; return S_OK; }
HRESULT APIENTRY destroyQueue(HANDLE,const D3DDDI_DESTROYPAGINGQUEUE *q) { check(q->hPagingQueue==7); return S_OK; }
HRESULT APIENTRY mapVa(HANDLE,D3DDDI_MAPGPUVIRTUALADDRESS *m) {
    check(m->hPagingQueue==7 && m->hAllocation==41 && m->SizeInPages==2 && m->Protection.Write);
    m->VirtualAddress=65536; m->PagingFenceValue=5; return E_PENDING;
}
HRESULT APIENTRY resident(HANDLE,D3DDDI_MAKERESIDENT *r) {
    check(r->hPagingQueue==7 && r->NumAllocations==1 && *r->AllocationList==41);
    if (rejectResident) return E_OUTOFMEMORY;
    r->PagingFenceValue=9; return E_PENDING;
}
HRESULT APIENTRY freeVa(HANDLE,const D3DDDICB_FREEGPUVIRTUALADDRESS *f) {
    ++unmaps; check(f->BaseAddress==65536 && f->Size==8192); return rejectUnmap ? E_FAIL : S_OK;
}
}
namespace {
unsigned cpuWaits=0; int waitMode=0;
HRESULT APIENTRY waitPaging(HANDLE,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *w) {
    ++cpuWaits; check(w->ObjectCount==1 && *w->ObjectHandleArray==8 && *w->FenceValueArray==9 && w->hAsyncEvent);
    if (waitMode==3) return E_OUTOFMEMORY;
    if (waitMode==0) pagingCpu=9;
    if (waitMode!=2) SetEvent(w->hAsyncEvent);
    return S_OK;
}
}
int main() {
    static_assert(sizeof(BC250_SURFACE_RESOURCE_PRIVATE)==64);
    BC250_SURFACE_RESOURCE_PRIVATE privateData{};
    privateData.Magic=BC250_SURFACE_RESOURCE_MAGIC;
    int shared=-1,cached=-1;
    for (unsigned version=1;version<=3;++version) {
        privateData.Version=version;
        const unsigned size=version==1 ? 12 : version==2 ? 16 : 64;
        for (unsigned share=0;share<=1;++share) for (unsigned access=0;access<=3;++access) {
            privateData.Shared=share; privateData.Access=access;
            check(Bc250SurfaceResourcePolicy(&privateData,size,&shared,&cached));
            check(shared==int(share) && cached==int(version>1 && share && access==2));
        }
        for (unsigned bytes=4;bytes<=68;++bytes) if (bytes!=size)
            check(!Bc250SurfaceResourcePolicy(&privateData,bytes,&shared,&cached) && !shared && !cached);
    }
    privateData.Version=3; privateData.Shared=2;
    check(!Bc250SurfaceResourcePolicy(&privateData,64,&shared,&cached));
    privateData.Shared=1; privateData.Access=4;
    check(!Bc250SurfaceResourcePolicy(&privateData,64,&shared,&cached));
    privateData.Version=4; privateData.Access=0;
    check(!Bc250SurfaceResourcePolicy(&privateData,64,&shared,&cached));
    privateData.Magic=0;
    check(Bc250SurfaceResourcePolicy(&privateData,64,&shared,&cached) && !shared && !cached);
    check(Bc250SurfaceResourcePolicy(nullptr,0,&shared,&cached));

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
    device.KTCallbacks.pfnCreatePagingQueueCb=createQueue; device.KTCallbacks.pfnDestroyPagingQueueCb=destroyQueue;
    device.KTCallbacks.pfnMapGpuVirtualAddressCb=mapVa; device.KTCallbacks.pfnMakeResidentCb=resident;
    device.KTCallbacks.pfnFreeGpuVirtualAddressCb=freeVa;
    SurfacePagingQueue queue{}; SurfaceGpuMapping mapping{};
    check(create_surface_paging_queue(device,queue)==S_OK);
    check(map_runtime_surface(device,queue,41,4097,mapping)==S_FALSE && mapping.fence==9 && mapping.resident);
    pagingCpu=5; check(surface_paging_status(queue,mapping)==S_FALSE && unmap_runtime_surface(device,queue,mapping)==E_PENDING && !unmaps);
    device.KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb=waitPaging;
    waitMode=1; check(wait_surface_paging(device,queue,mapping,0)==E_FAIL && pagingCpu==5);
    waitMode=2; check(wait_surface_paging(device,queue,mapping,0)==DXGI_ERROR_DEVICE_HUNG);
    waitMode=3; check(wait_surface_paging(device,queue,mapping,0)==E_OUTOFMEMORY);
    waitMode=0; check(wait_surface_paging(device,queue,mapping,0)==S_OK && pagingCpu==9 && cpuWaits==4);
    check(wait_surface_paging(device,queue,mapping,0)==S_OK && cpuWaits==4);
    check(wait_surface_paging(device,queue,mapping,10001)==E_INVALIDARG && cpuWaits==4);
    pagingCpu=9; rejectUnmap=true;
    check(unmap_runtime_surface(device,queue,mapping)==E_FAIL && mapping.address==65536);
    rejectUnmap=false; check(unmap_runtime_surface(device,queue,mapping)==S_OK && !mapping.address);
    rejectResident=true; pagingCpu=0;
    check(map_runtime_surface(device,queue,41,4097,mapping)==E_OUTOFMEMORY && mapping.address==65536 && !mapping.resident && mapping.fence==5);
    pagingCpu=UINT64_MAX; check(surface_paging_status(queue,mapping)==D3DDDIERR_DEVICEREMOVED);
    pagingCpu=5; check(unmap_runtime_surface(device,queue,mapping)==S_OK);
    check(destroy_surface_paging_queue(device,queue)==S_OK && !queue.queue && !queue.cpu);
    extendedPrivate=true;
    request.texture={BC250_SURFACE_RESOURCE_MAGIC,3,1,2,64,16,1,1,87,1,0,0,40,0,0,0};
    check(allocate_runtime_surface(device,request,surface)==S_OK && deallocate_runtime_surface(device,surface)==S_OK);
    request.texture.Width=63;
    const unsigned beforeInvalidPrivate=allocates;
    check(allocate_runtime_surface(device,request,surface)==E_INVALIDARG && allocates==beforeInvalidPrivate);
    std::cout << "PASS runtime surface allocation ABI, domain, failure retention and resource-handle close\n";
}
