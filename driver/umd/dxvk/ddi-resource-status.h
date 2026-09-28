// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
// S_OK only if every subresource is idle. Stop on busy or failure, never wait.
template<typename Query> HRESULT query_subresources_idle(UINT count,Query &&query) {
    if (!count) return E_INVALIDARG;
    for (UINT i=0;i<count;++i) {
        const HRESULT hr=query(i);
        if (hr!=S_OK) return (hr==S_FALSE || FAILED(hr)) ? hr : E_FAIL;
    }
    return S_OK;
}
void install_resource_status_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
