// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
namespace bc250::umd {
// The final D3D11.1 table, and the WDDM 2.0 table where the adapter offers FL12.
// No older or intermediate table may be written through the runtime's union
// pointer merely because its prefix looks similar.
inline constexpr bool wddm2_0_ddi(UINT interfaceVersion,UINT version) {
    return interfaceVersion==D3DWDDM2_0_DDI_INTERFACE_VERSION && (version>>16)==D3DWDDM2_0_DDI_BUILD_VERSION;
}
inline HRESULT supported_ddi_versions(D3D_FEATURE_LEVEL maximum,UINT32 *entries,UINT64 *versions) {
    if (!entries) return E_INVALIDARG;
    // FL12_x exists only at the WDDM 2.0 interface; an FL11 adapter keeps
    // offering the D3D11.1 table alone, as before.
    const UINT32 count=maximum>=D3D_FEATURE_LEVEL_12_0 ? 2 : 1;
    // Follow the version-discovery protocol: a short array leaves both output
    // arguments untouched. A null array is the count-only query.
    if (versions && *entries<count) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *entries=count;
    if (!versions) return S_OK;
    versions[0]=D3D11_1_DDI_SUPPORTED;
    if (count>1) versions[1]=D3DWDDM2_0_DDI_SUPPORTED;
    return S_OK;
}
inline HRESULT requested_feature_level(UINT flags,D3D_FEATURE_LEVEL &out,
    UINT interfaceVersion=D3D11_1_DDI_INTERFACE_VERSION) {
    D3D_FEATURE_LEVEL result;
    const bool wddm2_0=interfaceVersion==D3DWDDM2_0_DDI_INTERFACE_VERSION;
    switch (D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(flags)) {
    case D3D11DDI_3DPIPELINELEVEL_10_0: result=D3D_FEATURE_LEVEL_10_0; break;
    case D3D11DDI_3DPIPELINELEVEL_10_1: result=D3D_FEATURE_LEVEL_10_1; break;
    case D3D11DDI_3DPIPELINELEVEL_11_0: result=D3D_FEATURE_LEVEL_11_0; break;
    case D3D11_1DDI_3DPIPELINELEVEL_11_1: result=D3D_FEATURE_LEVEL_11_1; break;
    case D3DWDDM2_0DDI_3DPIPELINELEVEL_12_0: if (!wddm2_0) return E_INVALIDARG; result=D3D_FEATURE_LEVEL_12_0; break;
    case D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1: if (!wddm2_0) return E_INVALIDARG; result=D3D_FEATURE_LEVEL_12_1; break;
    default: return E_INVALIDARG; // FL9 needs a different implemented contract.
    }
    out=result; return S_OK;
}
}
