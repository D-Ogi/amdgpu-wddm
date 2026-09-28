// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "../../kmd/gdi_private.h"
namespace bc250::umd {
struct RuntimeSurfaceAllocation {
    HANDLE runtime_resource=nullptr;
    D3DKMT_HANDLE allocation=0,kernel_resource=0;
};
struct RuntimeSurfaceRequest {
    BC250_WDDM_ALLOCATION_PRIVATE surface{};
    HANDLE runtime_resource=nullptr;
    UINT vidpn_source=0;
    bool primary=false,shared=false,cpu_read=false;
};
HRESULT allocate_runtime_surface(RuntimeDevice &,const RuntimeSurfaceRequest &,RuntimeSurfaceAllocation &);
// Caller must retire GPU/Present, destroy image wrappers and free its GPU VA first.
HRESULT deallocate_runtime_surface(RuntimeDevice &,RuntimeSurfaceAllocation &);
}
