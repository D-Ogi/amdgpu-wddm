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
// BD-312: OfferResources and ReclaimResources give the kernel allocations behind the objects to the runtime's
// pfnOfferAllocationsCb and pfnReclaimAllocations2Cb. The runtime treats any code it does not admit as a critical
// driver failure and removes the device (BD-075); E_NOTIMPL here cost OfferReclaim12 its device. Offer answers S_OK
// (an allocation the kernel refused to take is kept and reported as not discarded on reclaim) or
// D3DDDIERR_DEVICEREMOVED. Reclaim answers S_OK, E_PENDING with the paging fence of pRTPagingQueue[0],
// E_OUTOFMEMORY (nothing reclaimed, the allocations stay offered) or D3DDDIERR_DEVICEREMOVED.
HRESULT native_residency_offer(Device&,const D3D12DDIARG_OFFERRESOURCES*,
    ResidencyAllocationResolver,void*) noexcept;
HRESULT native_residency_reclaim(Device&,D3D12DDIARG_RECLAIMRESOURCES_0001*,
    ResidencyAllocationResolver,void*) noexcept;
HRESULT APIENTRY native_make_resident(D3D12DDI_HDEVICE,D3D12DDIARG_MAKERESIDENT_0001*);
HRESULT APIENTRY native_evict(D3D12DDI_HDEVICE,const D3D12DDIARG_EVICT*);
HRESULT APIENTRY native_offer_resources(D3D12DDI_HDEVICE,const D3D12DDIARG_OFFERRESOURCES*);
HRESULT APIENTRY native_reclaim_resources(D3D12DDI_HDEVICE,D3D12DDIARG_RECLAIMRESOURCES_0001*);
void install_native_residency_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088&) noexcept;
}
