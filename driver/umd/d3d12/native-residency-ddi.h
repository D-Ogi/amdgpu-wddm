// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
namespace native12 {
struct Device;
using ResidencyAllocationResolver=HRESULT (*)(void*,D3D12DDI_HANDLE_AND_TYPE,D3DKMT_HANDLE*) noexcept;
// Runtime keeps input objects alive and serializes residency against their GPU
// use. Root wrappers provide the native DDI scope. Resolution/deduplication is
// complete before the single callback; no partial batches or implicit retries.
HRESULT native_residency_make(Device&,D3D12DDIARG_MAKERESIDENT_0001*,
    ResidencyAllocationResolver,void*) noexcept;
HRESULT native_residency_evict(Device&,const D3D12DDIARG_EVICT*,
    ResidencyAllocationResolver,void*) noexcept;
HRESULT APIENTRY native_make_resident(D3D12DDI_HDEVICE,D3D12DDIARG_MAKERESIDENT_0001*);
HRESULT APIENTRY native_evict(D3D12DDI_HDEVICE,const D3D12DDIARG_EVICT*);
void install_native_residency_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088&) noexcept;
}
