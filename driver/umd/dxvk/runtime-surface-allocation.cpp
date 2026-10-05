// SPDX-License-Identifier: MIT
#include "runtime-surface-allocation.h"
#include "runtime-surface-format.h"
namespace bc250::umd {
// E26R v2 resource-group ABI, consumed by KMD wddm.c resource-private parser.
struct ResourcePrivate { UINT magic,version,shared,access; };
static_assert(sizeof(ResourcePrivate)==16);
static_assert(sizeof(BC250_WDDM_ALLOCATION_PRIVATE)==32);
HRESULT allocate_runtime_surface(RuntimeDevice &device,const RuntimeSurfaceRequest &request,RuntimeSurfaceAllocation &out) {
    if (out.allocation || out.kernel_resource || out.runtime_resource) return E_UNEXPECTED;
    if (!device.domain.entered() || !device.hDevice || !device.KTCallbacks.pfnAllocateCb || !device.KTCallbacks.pfnDeallocate2Cb ||
        (request.primary && request.cpu_read)) return E_INVALIDARG;
    // The kernel driver's type-0 admission, by the same table: the composed rows and the scan-out
    // rows a primary may have (X8R8G8B8). The row sizes the pixel: 1 byte for A8, 8 for RGBA16F.
    const auto *row=amdgpu_wddm_surface_format_by_d3dddi(request.surface.Format);
    if (!amdgpu_wddm_surface_admit(row,AMDGPU_WDDM_SURFACE_COMPOSED) &&
        !amdgpu_wddm_surface_admit(row,AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY)) return E_NOTIMPL;
    if (!runtime_surface_geometry(request.surface,row->bytes_per_pixel)) return E_INVALIDARG;
    auto texture=request.texture;
    const bool extended=texture.Magic!=0;
    // The E26R v3 texture format is what an opener reads back; it must be the LB7A row's.
    if (extended && (texture.Magic!=BC250_SURFACE_RESOURCE_MAGIC ||
        texture.Version!=BC250_SURFACE_RESOURCE_TEXTURE_VERSION || texture.Shared!=UINT(request.shared) ||
        texture.Access!=((request.primary ? 1u : 0u)|(request.cpu_read ? 2u : 0u)) ||
        texture.Width!=request.surface.Width || texture.Height!=request.surface.Height ||
        !runtime_surface_format(request.surface.Format,DXGI_FORMAT(texture.Format)))) return E_INVALIDARG;
    static_assert(sizeof(texture)==64);
    auto surface=request.surface;
    ResourcePrivate group{0x52363245u,2,request.shared ? 1u : 0u,
        (request.primary ? 1u : 0u) | (request.cpu_read ? 2u : 0u)};
    D3DDDI_ALLOCATIONINFO2 info{};
    info.pPrivateDriverData=&surface; info.PrivateDriverDataSize=sizeof(surface);
    info.Flags.Primary=request.primary; info.VidPnSourceId=request.vidpn_source;
    D3DDDICB_ALLOCATE allocate{};
    allocate.hResource=request.runtime_resource;
    allocate.pPrivateDriverData=extended ? static_cast<void *>(&texture) : &group;
    allocate.PrivateDriverDataSize=extended ? sizeof(texture) : sizeof(group);
    allocate.NumAllocations=1; allocate.pAllocationInfo2=&info;
    HRESULT hr=device.KTCallbacks.pfnAllocateCb(device.hDevice,&allocate);
    if (FAILED(hr)) return hr;
    out={request.runtime_resource,info.hAllocation,allocate.hKMResource};
    // Preserve any successful callback output for explicit cleanup, including
    // malformed success. Never claim an allocation is ready without its handle.
    return out.allocation ? S_OK : E_FAIL;
}
HRESULT deallocate_runtime_surface(RuntimeDevice &device,RuntimeSurfaceAllocation &surface) {
    if (!device.domain.entered()) return E_INVALIDARG;
    if (!surface.allocation && !surface.kernel_resource && !surface.runtime_resource) return S_OK;
    if (!device.KTCallbacks.pfnDeallocate2Cb) return E_NOTIMPL;
    D3DDDICB_DEALLOCATE2 free{};
    if (surface.runtime_resource) free.hResource=surface.runtime_resource;
    else {
        if (!surface.allocation) return E_UNEXPECTED;
        free.NumAllocations=1; free.HandleList=&surface.allocation;
    }
    HRESULT hr=device.KTCallbacks.pfnDeallocate2Cb(device.hDevice,&free);
    if (SUCCEEDED(hr)) surface={};
    return hr;
}
HRESULT create_surface_paging_queue(RuntimeDevice &device,SurfacePagingQueue &out) {
    if (out.queue || out.sync || out.cpu) return E_UNEXPECTED;
    if (!device.domain.entered() || !device.hDevice || !device.KTCallbacks.pfnCreatePagingQueueCb ||
        !device.KTCallbacks.pfnDestroyPagingQueueCb) return E_INVALIDARG;
    D3DDDICB_CREATEPAGINGQUEUE request{}; request.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
    HRESULT hr=device.KTCallbacks.pfnCreatePagingQueueCb(device.hDevice,&request);
    if (FAILED(hr)) return hr;
    out={request.hPagingQueue,request.hSyncObject,static_cast<const volatile UINT64 *>(request.FenceValueCPUVirtualAddress)};
    return out.queue && out.sync && out.cpu ? S_OK : E_FAIL;
}
HRESULT destroy_surface_paging_queue(RuntimeDevice &device,SurfacePagingQueue &queue) {
    if (!device.domain.entered()) return E_INVALIDARG;
    if (!queue.queue) return (queue.sync || queue.cpu) ? E_UNEXPECTED : S_OK;
    if (!device.KTCallbacks.pfnDestroyPagingQueueCb) return E_NOTIMPL;
    D3DDDI_DESTROYPAGINGQUEUE request{}; request.hPagingQueue=queue.queue;
    HRESULT hr=device.KTCallbacks.pfnDestroyPagingQueueCb(device.hDevice,&request);
    if (SUCCEEDED(hr)) queue={}; return hr;
}
HRESULT surface_paging_status(const SurfacePagingQueue &queue,const SurfaceGpuMapping &mapping) {
    if (!queue.queue || !queue.cpu || !mapping.address) return E_INVALIDARG;
    const UINT64 completed=*queue.cpu;
    if (completed==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
    return completed>=mapping.fence ? S_OK : S_FALSE;
}
HRESULT map_runtime_surface(RuntimeDevice &device,const SurfacePagingQueue &queue,D3DKMT_HANDLE allocation,UINT64 bytes,SurfaceGpuMapping &out) {
    if (out.address || out.bytes || out.fence || out.resident) return E_UNEXPECTED;
    if (!device.domain.entered() || !queue.queue || !queue.sync || !queue.cpu || !allocation || !bytes || bytes>UINT64_MAX-4095 ||
        !device.KTCallbacks.pfnMapGpuVirtualAddressCb || !device.KTCallbacks.pfnMakeResidentCb) return E_INVALIDARG;
    if (*queue.cpu==UINT64_MAX) return D3DDDIERR_DEVICEREMOVED;
    const UINT64 rounded=(bytes+4095)&~UINT64(4095);
    D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hPagingQueue=queue.queue; map.hAllocation=allocation;
    map.SizeInPages=rounded/4096; map.Protection.Write=1;
    HRESULT hr=device.KTCallbacks.pfnMapGpuVirtualAddressCb(device.hDevice,&map);
    if (FAILED(hr) && hr!=E_PENDING) return hr;
    out.address=map.VirtualAddress; out.bytes=rounded; out.fence=map.PagingFenceValue;
    if (!out.address || out.address>UINT64_MAX-rounded || (hr==E_PENDING && !out.fence)) return E_FAIL;
    D3DDDI_MAKERESIDENT resident{}; resident.hPagingQueue=queue.queue;
    resident.NumAllocations=1; resident.AllocationList=&allocation;
    hr=device.KTCallbacks.pfnMakeResidentCb(device.hDevice,&resident);
    if (FAILED(hr) && hr!=E_PENDING) return hr; // Mapping retained for cleanup.
    // The output fence is defined only for E_PENDING. S_OK needs no new wait.
    if (hr==E_PENDING) {
        if (!resident.PagingFenceValue) return E_FAIL;
        if (resident.PagingFenceValue>out.fence) out.fence=resident.PagingFenceValue;
    }
    out.resident=true;
    return surface_paging_status(queue,out); // S_FALSE means accepted, pending.
}
HRESULT wait_surface_paging(RuntimeDevice &device,const SurfacePagingQueue &queue,const SurfaceGpuMapping &mapping,DWORD timeoutMs) {
    if (!device.domain.entered() || timeoutMs>10000) return E_INVALIDARG;
    HRESULT hr=surface_paging_status(queue,mapping);
    if (hr!=S_FALSE) return hr;
    if (!queue.sync || !device.KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb) return E_NOTIMPL;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!event) return HRESULT_FROM_WIN32(GetLastError());
    D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{};
    wait.ObjectCount=1; wait.ObjectHandleArray=&queue.sync; wait.FenceValueArray=&mapping.fence; wait.hAsyncEvent=event;
    hr=device.KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb(device.hDevice,&wait);
    if (SUCCEEDED(hr)) {
        DWORD result=WaitForSingleObject(event,timeoutMs);
        if (result==WAIT_OBJECT_0 || result==WAIT_TIMEOUT) {
            // Completion can race the timeout. Conversely an event alone does
            // not establish completion. Recheck the monitored fence itself.
            hr=surface_paging_status(queue,mapping);
            if (hr==S_FALSE) { MemoryBarrier(); hr=surface_paging_status(queue,mapping); }
            if (hr==S_FALSE) hr=D3DDDIERR_DEVICEREMOVED;
        } else hr=HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(event); return hr;
}
HRESULT unmap_runtime_surface(RuntimeDevice &device,const SurfacePagingQueue &queue,SurfaceGpuMapping &mapping) {
    if (!device.domain.entered()) return E_INVALIDARG;
    if (!mapping.address) return mapping.bytes ? E_UNEXPECTED : S_OK;
    HRESULT hr=surface_paging_status(queue,mapping);
    if (hr==S_FALSE) return E_PENDING; // Cleanup is not complete; retain ownership.
    if (FAILED(hr)) return hr;
    if (!device.KTCallbacks.pfnFreeGpuVirtualAddressCb) return E_NOTIMPL;
    D3DDDICB_FREEGPUVIRTUALADDRESS free{}; free.BaseAddress=mapping.address; free.Size=mapping.bytes;
    hr=device.KTCallbacks.pfnFreeGpuVirtualAddressCb(device.hDevice,&free);
    if (SUCCEEDED(hr)) mapping={}; return hr;
}}
