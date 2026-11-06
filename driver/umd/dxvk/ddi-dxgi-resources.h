// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
// Submission must precede OfferCb, which handles retirement of submitted work.
// The caller validates the resource batch before invoking this helper.
template<typename Submit> HRESULT offer_after_submit(RuntimeDevice &runtime,
    const D3DKMT_HANDLE *handles,UINT count,D3DDDI_OFFER_PRIORITY priority,Submit &&submit) {
    if (!runtime.KTCallbacks.pfnOfferAllocationsCb) return E_NOTIMPL;
    HRESULT hr=submit();
    if (FAILED(hr)) return hr;
    D3DDDICB_OFFERALLOCATIONS request{};
    request.HandleList=handles; request.NumAllocations=count; request.Priority=priority;
    return runtime.KTCallbacks.pfnOfferAllocationsCb(runtime.hDevice,&request);
}
void install_dxgi_resource_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &);
class DeviceOwner;
// BD-309: the first step of an offer of an engine-private resource (OfferResources calls it for each one). With the
// engine and the ICD of owner.offer_path() it puts the resource on memory of its own (PrepareOffer) and appends the
// kernel allocations of that memory to `handles`: S_OK with `taken` set, the allocations kept in the resource until
// its reclaim. S_OK with `taken` clear: the resource is an offer hint, and nothing stays prepared. A failure is the
// engine's device-loss status. The DXGI resource tests call it to set up a reclaim.
HRESULT engine_prepare_offer(DeviceOwner &owner,DdiResource &resource,std::vector<D3DKMT_HANDLE> &handles,bool &taken);
// BD-283: one line for each Offer, Reclaim and Trim of the device (entry, counts, status), so that the order of a
// hide, its Trim and Offer, the Reclaim at the next show and a device mark can be read from the log of a host that
// runs for hours. 64 lines at the start, then 8 in each minute (LineAllowance, diagnostics.h); a printf format.
void residency_note(const char *format,...) noexcept;
// The two resource entries that the DXGI 1.6.1 table replaces: OfferResources1, which adds the offer
// flags, and ReclaimResources1, which reports a result per resource in place of a discarded flag. Every
// other resource entry of that table is the one above, at the same offset (ddi-wddm22.cpp).
void install_dxgi1_6_1_resource_ddi(DXGI1_6_1_DDI_BASE_FUNCTIONS &);
}
