// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
namespace bc250::umd {
// The final D3D11.1 table, and the WDDM 2.0 and WDDM 2.2 tables where the adapter offers FL12.
// No older or intermediate table may be written through the runtime's union
// pointer merely because its prefix looks similar.
inline constexpr bool wddm2_0_ddi(UINT interfaceVersion,UINT version) {
    return interfaceVersion==D3DWDDM2_0_DDI_INTERFACE_VERSION && (version>>16)==D3DWDDM2_0_DDI_BUILD_VERSION;
}
// BD-099: the WDDM 2.2 interface at the build this shell advertises. The build matters: the runtime gives the
// whole DXGIDDICB_PRESENT, with SyncIntervalOverride in it, only from interface 0xB0023 build 5
// (IS_DXGI1_6_1_BASE_FUNCTIONS in d3d10umddi.h, and the version byte of d3d11.dll in
// docs/design/per-app-graphics-settings.md). A build this shell did not advertise is declined, and the runtime
// then takes the WDDM 2.0 table, which the adapter advertises as well.
inline constexpr bool wddm2_2_ddi(UINT interfaceVersion,UINT version) {
    return interfaceVersion==D3DWDDM2_2_DDI_INTERFACE_VERSION && (version>>16)==D3DWDDM2_2_DDI_BUILD_VERSION;
}
// Every table whose entries this shell fills, newest last. wddm2_2 is the process switch of
// ddi-experiment.h: false keeps the WDDM 2.0 table as the newest offer.
inline HRESULT supported_ddi_versions(D3D_FEATURE_LEVEL maximum,bool wddm2_2,UINT32 *entries,UINT64 *versions) {
    if (!entries) return E_INVALIDARG;
    // FL12_x exists only at the WDDM 2.0 interface and later; an FL11 adapter keeps
    // offering the D3D11.1 table alone, as before.
    const bool fl12=maximum>=D3D_FEATURE_LEVEL_12_0;
    const UINT32 count=fl12 ? (wddm2_2 ? 3 : 2) : 1;
    // Follow the version-discovery protocol: a short array leaves both output
    // arguments untouched. A null array is the count-only query.
    if (versions && *entries<count) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *entries=count;
    if (!versions) return S_OK;
    versions[0]=D3D11_1_DDI_SUPPORTED;
    if (count>1) versions[1]=D3DWDDM2_0_DDI_SUPPORTED;
    if (count>2) versions[2]=D3DWDDM2_2_DDI_SUPPORTED;
    return S_OK;
}
inline HRESULT requested_feature_level(UINT flags,D3D_FEATURE_LEVEL &out,
    UINT interfaceVersion=D3D11_1_DDI_INTERFACE_VERSION) {
    D3D_FEATURE_LEVEL result;
    // The FL12 pipeline levels came with the WDDM 2.0 interface and are the same at every later one: no
    // interface after it defines a D3D11DDI_3DPIPELINELEVEL of its own.
    const bool fl12_interface=interfaceVersion>=D3DWDDM2_0_DDI_INTERFACE_VERSION;
    switch (D3D11DDI_EXTRACT_3DPIPELINELEVEL_FROM_FLAGS(flags)) {
    case D3D11DDI_3DPIPELINELEVEL_10_0: result=D3D_FEATURE_LEVEL_10_0; break;
    case D3D11DDI_3DPIPELINELEVEL_10_1: result=D3D_FEATURE_LEVEL_10_1; break;
    case D3D11DDI_3DPIPELINELEVEL_11_0: result=D3D_FEATURE_LEVEL_11_0; break;
    case D3D11_1DDI_3DPIPELINELEVEL_11_1: result=D3D_FEATURE_LEVEL_11_1; break;
    case D3DWDDM2_0DDI_3DPIPELINELEVEL_12_0: if (!fl12_interface) return E_INVALIDARG; result=D3D_FEATURE_LEVEL_12_0; break;
    case D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1: if (!fl12_interface) return E_INVALIDARG; result=D3D_FEATURE_LEVEL_12_1; break;
    default: return E_INVALIDARG; // FL9 needs a different implemented contract.
    }
    out=result; return S_OK;
}
}
