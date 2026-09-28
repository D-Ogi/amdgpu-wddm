// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
// Prepare all storage before engine mutation. On failure, shell identity stays
// unchanged; after success the commit consists only of non-throwing scalar moves.
template<typename Rotate> HRESULT rotate_present_resources(const DXGI_DDI_HRESOURCE *handles,UINT count,Rotate &&rotate) {
    if (!count || !handles) return E_INVALIDARG;
    std::vector<DdiResource *> resources(count);
    std::vector<ID3D11Resource *> objects(count);
    for (UINT i=0;i<count;++i) {
        auto *r=reinterpret_cast<DdiResource *>(handles[i]);
        if (!r || !r->object) return E_INVALIDARG;
        for (UINT j=0;j<i;++j) if (objects[j]==r->object) return E_INVALIDARG;
        resources[i]=r; objects[i]=r->object;
    }
    HRESULT hr=rotate(objects.data(),count);
    if (hr!=S_OK) return hr;
    const D3DKMT_HANDLE firstAllocation=resources[0]->present_allocation;
    const UINT firstSubresource=resources[0]->present_subresource;
    for (UINT i=0;i+1<count;++i) {
        resources[i]->present_allocation=resources[i+1]->present_allocation;
        resources[i]->present_subresource=resources[i+1]->present_subresource;
    }
    resources[count-1]->present_allocation=firstAllocation;
    resources[count-1]->present_subresource=firstSubresource;
    return S_OK;
}
void install_present_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &);
}
