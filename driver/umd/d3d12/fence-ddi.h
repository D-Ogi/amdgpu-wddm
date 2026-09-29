// SPDX-License-Identifier: MIT
#pragma once
#include "device-state.h"
#include <cstdint>
#include <new>
namespace native12 {
// Single physical adapter. Placements are GPU addresses, not CPU pointers or KMT handles.
struct FenceState { Device* device; D3D12DDI_FENCE placement; };
inline HRESULT validate_fence(const D3D12DDIARG_CREATE_FENCE* args) noexcept {
    if(!args || !args->FenceCount || !args->Fences)return E_INVALIDARG;
    if(args->FenceCount!=1)return E_NOTIMPL; // Linked adapters are not advertised.
    if(static_cast<unsigned>(args->Fences[0].Flags)&
       ~static_cast<unsigned>(D3D12DDI_FENCE_FLAG_BOTTOM_OF_PIPE))return E_INVALIDARG;
    return S_OK;
}
// The size never depends on the request: the runtime reserves what this entry
// returns and calls pfnCreateFence regardless, so a zero here would make the
// later placement write land outside the reservation. Refusals belong to create.
inline SIZE_T APIENTRY fence_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATE_FENCE*) {
    return sizeof(FenceState);
}
inline HRESULT APIENTRY fence_create(D3D12DDI_HDEVICE h,D3D12DDI_HFENCE fence,
                                    const D3D12DDIARG_CREATE_FENCE* args) {
    if(!h.pDrvPrivate || !fence.pDrvPrivate ||
       reinterpret_cast<uintptr_t>(fence.pDrvPrivate)%alignof(FenceState))return E_INVALIDARG;
    HRESULT hr=validate_fence(args);if(FAILED(hr))return hr;
    auto device=static_cast<Device*>(h.pDrvPrivate);
    if(device->lost.load())return D3DDDIERR_DEVICEREMOVED;
    // The runtime's argument array need not outlive this call.
    new(fence.pDrvPrivate) FenceState{device,args->Fences[0]};
    return S_OK;
}
inline void APIENTRY fence_destroy(D3D12DDI_HDEVICE h,D3D12DDI_HFENCE fence) {
    if(!h.pDrvPrivate || !fence.pDrvPrivate)return;
    auto state=static_cast<FenceState*>(fence.pDrvPrivate);
    if(state->device!=h.pDrvPrivate){static_cast<Device*>(h.pDrvPrivate)->remove();return;}
    state->~FenceState(); // Runtime owns this storage and all underlying fence objects.
}
inline void install_fence_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088& table) noexcept {
    table.pfnCalcPrivateFenceSize=fence_size;
    table.pfnCreateFence=fence_create;
    table.pfnDestroyFence=fence_destroy;
}
}
