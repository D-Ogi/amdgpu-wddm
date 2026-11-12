// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
// BD-372: the offer flags that go to the kernel. AllowDecommit is the one flag (d3dukmdt.h, D3DDDI_OFFER_FLAGS); the
// reserved bits never go. The kernel gets AllowDecommit only when the shell can hear a decommit at the reclaim: the
// runtime gives pfnOfferAllocations2Cb, the offer with flags, and pfnReclaimAllocations3Cb, the reclaim that reports
// D3DDDI_RECLAIM_RESULT_NOT_COMMITTED (d3dumddi.h, WDDM 2.1.1 and 2.1.2), and the device has the paging queue that
// reclaim runs on (`reclaimQueue`, DeviceOwner::surface_queue). Otherwise the flag is dropped and the offer is the
// plain offer: a decommitted allocation that came back as discarded only would be used without backing.
inline D3DDDI_OFFER_FLAGS kernel_offer_flags(const RuntimeDevice &runtime,D3DKMT_HANDLE reclaimQueue,
                                             D3DDDI_OFFER_FLAGS asked) {
    D3DDDI_OFFER_FLAGS flags{};
    flags.AllowDecommit=asked.AllowDecommit && reclaimQueue && runtime.KTCallbacks.pfnOfferAllocations2Cb &&
        runtime.KTCallbacks.pfnReclaimAllocations3Cb ? 1u : 0u;
    return flags;
}
// Submission must precede OfferCb, which handles retirement of submitted work.
// The caller validates the resource batch before invoking this helper. `flags` comes from kernel_offer_flags: an offer
// with a flag goes through pfnOfferAllocations2Cb, an offer without one through pfnOfferAllocationsCb as before.
template<typename Submit> HRESULT offer_after_submit(RuntimeDevice &runtime,
    const D3DKMT_HANDLE *handles,UINT count,D3DDDI_OFFER_PRIORITY priority,D3DDDI_OFFER_FLAGS flags,Submit &&submit) {
    const auto &callbacks=runtime.KTCallbacks;
    if (flags.Value ? !callbacks.pfnOfferAllocations2Cb : !callbacks.pfnOfferAllocationsCb) return E_NOTIMPL;
    HRESULT hr=submit();
    if (FAILED(hr)) return hr;
    if (flags.Value) {
        D3DDDICB_OFFERALLOCATIONS2 request{};
        request.HandleList=handles; request.NumAllocations=count; request.Priority=priority; request.Flags=flags;
        return callbacks.pfnOfferAllocations2Cb(runtime.hDevice,&request);
    }
    D3DDDICB_OFFERALLOCATIONS request{};
    request.HandleList=handles; request.NumAllocations=count; request.Priority=priority;
    return callbacks.pfnOfferAllocationsCb(runtime.hDevice,&request);
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
// flags, and ReclaimResources1, which reports a result per resource in place of a discarded flag (OK, DISCARDED or,
// since BD-372, NOT_COMMITTED). Every other resource entry of that table is the one above, at the same offset
// (ddi-wddm22.cpp).
void install_dxgi1_6_1_resource_ddi(DXGI1_6_1_DDI_BASE_FUNCTIONS &);
}
