// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
namespace bc250::umd {
// The D3D11 DDI interfaces whose tables this shell fills, oldest first. A level is one row of kDdiInterfaces.
// No older or intermediate table may be written through the runtime's union pointer merely because its prefix
// looks similar, so an interface and build that have no row get no device.
enum class DdiLevel : unsigned { d3d11_1, wddm2_0, wddm2_2 };
struct DdiInterface {
    DdiLevel level;
    UINT interface_version;
    UINT build; // D3D10DDIARG_CALCPRIVATEDEVICESIZE::Version >> 16 (D3DDDI_BUILD16_FROM_VERSION32)
    UINT64 supported; // the value GetSupportedVersions writes
    const char *name; // for the log of ddi-adapter.cpp
};
// BD-099: the WDDM 2.2 row is the interface at build 5. The runtime gives the whole DXGIDDICB_PRESENT, with
// SyncIntervalOverride in it, only from interface 0xB0023 build 5 (IS_DXGI1_6_1_BASE_FUNCTIONS in d3d10umddi.h,
// and the version byte of d3d11.dll in docs/design/per-app-graphics-settings.md). A build this shell did not
// advertise is declined, and the runtime then takes the WDDM 2.0 table, which the adapter advertises as well.
inline constexpr DdiInterface kDdiInterfaces[]={
    {DdiLevel::d3d11_1,D3D11_1_DDI_INTERFACE_VERSION,D3D11_1_DDI_BUILD_VERSION,D3D11_1_DDI_SUPPORTED,"11_1"},
    {DdiLevel::wddm2_0,D3DWDDM2_0_DDI_INTERFACE_VERSION,D3DWDDM2_0_DDI_BUILD_VERSION,D3DWDDM2_0_DDI_SUPPORTED,"WDDM2_0"},
    {DdiLevel::wddm2_2,D3DWDDM2_2_DDI_INTERFACE_VERSION,D3DWDDM2_2_DDI_BUILD_VERSION,D3DWDDM2_2_DDI_SUPPORTED,"WDDM2_2"},
};
inline constexpr size_t kDdiInterfaceCount=sizeof(kDdiInterfaces)/sizeof(kDdiInterfaces[0]);
inline constexpr bool ddi_rows_ordered() {
    for (size_t i=0;i<kDdiInterfaceCount;++i) {
        if (kDdiInterfaces[i].level!=static_cast<DdiLevel>(i)) return false;
        if (i && kDdiInterfaces[i].supported<=kDdiInterfaces[i-1].supported) return false;
    }
    return true;
}
static_assert(ddi_rows_ordered(),"one row per level, oldest first, in the order GetSupportedVersions writes them");
inline constexpr const DdiInterface &ddi_interface(DdiLevel level) { return kDdiInterfaces[static_cast<unsigned>(level)]; }
// The level of a negotiated interface: the row whose interface and build match exactly. False for any other pair.
inline constexpr bool level_of(UINT interfaceVersion,UINT version,DdiLevel &level) {
    for (const auto &row:kDdiInterfaces)
        if (row.interface_version==interfaceVersion && (version>>16)==row.build) { level=row.level; return true; }
    return false;
}
// What caps the newest offer of one adapter.
struct DdiLevelInputs {
    D3D_FEATURE_LEVEL maximum; // the adapter's advertised maximum feature level
    bool kmt_known; // the KMT driver version query succeeded
    D3DKMT_DRIVERVERSION kmt; // its answer (d3dkmthk.h KMT_DRIVERVERSION_*)
    bool wddm2_2_allowed; // the process switch wddm22-ddi-off is not set (ddi-experiment.h)
};
// The newest interface the adapter offers. FL12_x exists only at the WDDM 2.0 interface and later, so an FL11
// adapter offers the D3D11.1 table alone. BD-153: an FL12 pipeline does not establish the KMD's WDDM version, so
// an interface newer than WDDM 2.0 needs the KMD to report that WDDM version or a later one; an unknown version
// keeps WDDM 2.0. A switch only lowers the result, never raises it.
inline constexpr DdiLevel newest_ddi_level(const DdiLevelInputs &in) {
    if (in.maximum<D3D_FEATURE_LEVEL_12_0) return DdiLevel::d3d11_1;
    if (in.kmt_known && in.kmt>=KMT_DRIVERVERSION_WDDM_2_2 && in.wddm2_2_allowed) return DdiLevel::wddm2_2;
    return DdiLevel::wddm2_0;
}
// Every table up to the newest level, oldest first. Follow the version-discovery protocol: a short array leaves
// both output arguments untouched. A null array is the count-only query.
inline HRESULT supported_ddi_versions(DdiLevel newest,UINT32 *entries,UINT64 *versions) {
    if (!entries) return E_INVALIDARG;
    const UINT32 count=static_cast<UINT32>(newest)+1;
    if (versions && *entries<count) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    *entries=count;
    if (!versions) return S_OK;
    for (UINT32 i=0;i<count;++i) versions[i]=kDdiInterfaces[i].supported;
    return S_OK;
}
// The name of one offered version, for the log of ddi-adapter.cpp. The log must name the set the shell really
// offered: an FL11 adapter offers the D3D11.1 table alone, and a fixed text would state an offer it never made.
inline const char *ddi_version_name(UINT64 supported) {
    for (const auto &row:kDdiInterfaces) if (row.supported==supported) return row.name;
    return "?";
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
