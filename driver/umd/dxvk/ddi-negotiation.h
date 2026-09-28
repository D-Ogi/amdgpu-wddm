// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
namespace bc250::umd {
// Only the final D3D11.1 table is implemented. No older table may be written
// through the runtime's union pointer merely because its prefix looks similar.
inline HRESULT supported_ddi_versions(UINT32 *entries,UINT64 *versions) {
    if (!entries) return E_INVALIDARG;
    // Follow the version-discovery protocol: a short array leaves both output
    // arguments untouched. A null array is the count-only query.
    if (versions && *entries<1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *entries=1;
    if (!versions) return S_OK;
    versions[0]=D3D11_1_DDI_SUPPORTED;
    return S_OK;
}
inline HRESULT requested_feature_level(UINT flags,D3D_FEATURE_LEVEL &out) {
    D3D_FEATURE_LEVEL result;
    switch (D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(flags)) {
    case D3D11DDI_3DPIPELINELEVEL_10_0: result=D3D_FEATURE_LEVEL_10_0; break;
    case D3D11DDI_3DPIPELINELEVEL_10_1: result=D3D_FEATURE_LEVEL_10_1; break;
    case D3D11DDI_3DPIPELINELEVEL_11_0: result=D3D_FEATURE_LEVEL_11_0; break;
    case D3D11_1DDI_3DPIPELINELEVEL_11_1: result=D3D_FEATURE_LEVEL_11_1; break;
    default: return E_INVALIDARG; // FL9/12 need different implemented contracts.
    }
    out=result; return S_OK;
}
}
