// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "../../kmd/gdi_private.h"
#include "../../kmd/surface_resource_private.h"
namespace bc250::umd {
struct RuntimeSurfaceAllocation {
    HANDLE runtime_resource=nullptr;
    D3DKMT_HANDLE allocation=0,kernel_resource=0;
};
struct RuntimeSurfaceRequest {
    BC250_WDDM_ALLOCATION_PRIVATE surface{};
    BC250_SURFACE_RESOURCE_PRIVATE texture{}; // Zero means legacy E26R v2.
    HANDLE runtime_resource=nullptr;
    UINT vidpn_source=0;
    bool primary=false,shared=false,cpu_read=false;
};
HRESULT allocate_runtime_surface(RuntimeDevice &,const RuntimeSurfaceRequest &,RuntimeSurfaceAllocation &);
// Caller must retire engine/Present use and destroy image wrappers first.
// Deallocate2 flags zero retires all allocation VAs, including pending mappings.
HRESULT deallocate_runtime_surface(RuntimeDevice &,RuntimeSurfaceAllocation &);
struct SurfacePagingQueue {
    D3DKMT_HANDLE queue=0,sync=0;
    const volatile UINT64 *cpu=nullptr;
};
struct SurfaceGpuMapping {
    UINT64 address=0,bytes=0,fence=0;
    bool resident=false;
};
HRESULT create_surface_paging_queue(RuntimeDevice &,SurfacePagingQueue &);
// All mappings and pending paging work must be retired before queue destruction.
HRESULT destroy_surface_paging_queue(RuntimeDevice &,SurfacePagingQueue &);
HRESULT map_runtime_surface(RuntimeDevice &,const SurfacePagingQueue &,D3DKMT_HANDLE allocation,UINT64 bytes,SurfaceGpuMapping &);
// Paging completion only; import additionally requires mapping.resident=true.
HRESULT surface_paging_status(const SurfacePagingQueue &,const SurfaceGpuMapping &);
HRESULT wait_surface_paging(RuntimeDevice &,const SurfacePagingQueue &,const SurfaceGpuMapping &,DWORD timeoutMs=10000);
// Only after engine/Present use and paging have retired. E_PENDING preserves VA.
HRESULT unmap_runtime_surface(RuntimeDevice &,const SurfacePagingQueue &,SurfaceGpuMapping &);}
