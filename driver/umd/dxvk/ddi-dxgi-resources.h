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
}
