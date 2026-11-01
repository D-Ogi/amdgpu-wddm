// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-present.h"
#include "ddi-blt.h"
#include "ddi-dxgi-resources.h"
namespace bc250::umd {
inline constexpr bool dxgi_table_layout_supported(UINT interfaceVersion,UINT buildVersion) {
    return IS_DXGI1_2_BASE_FUNCTIONS(interfaceVersion,buildVersion) &&
           IS_DXGI_MULTIPLANE_OVERLAY_FUNCTIONS(interfaceVersion,buildVersion);
}

// BD-198: a query that has no capability to report answers "none" in its output and returns S_OK. d3d11.dll
// removes the device for any other status of these entries (NDXGI::CDevice::GetGammaCaps and
// GetInternalMultiplaneOverlayCaps call DriverInternalError, d3d11.dll 10.0.26100), so DXGI_ERROR_UNSUPPORTED here
// was a device removal, not a refusal. GetGammaCaps has no gate: IDXGIOutput::GetGammaControlCapabilities reaches it
// on every device. The overlay queries run only when the KMD reports multiplane-overlay support (lab flavour with
// LabScaledPlane bit 0); this KMD reports one plane, the primary.
inline HRESULT APIENTRY unsupported_gamma(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *args) {
    if (!args || !args->hDevice || !args->pGammaCapabilities) return E_INVALIDARG;
    // The KMD does not implement a gamma-ramp DDI: no scale and offset, no control point. This is also the
    // answer the runtime itself gives after a failed call, but then without the removal.
    *args->pGammaCapabilities=DXGI_GAMMA_CONTROL_CAPABILITIES{};
    return S_OK;
}
inline HRESULT APIENTRY unsupported_overlay_caps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *args) {
    if (!args) return E_INVALIDARG;
    args->MultiplaneOverlayCaps.MaxPlanes=1; args->MultiplaneOverlayCaps.NumCapabilityGroups=0;
    return S_OK;
}
inline HRESULT APIENTRY unsupported_overlay_check(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYSUPPORT *args) {
    if (!args) return E_INVALIDARG;
    args->Supported=FALSE; return S_OK;
}
// A present of overlay planes is not a query: with one plane reported, the runtime has no plane set to present.
inline HRESULT APIENTRY unsupported_overlay_present(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *) { return DXGI_ERROR_UNSUPPORTED; }
inline DXGI1_2_DDI_BASE_FUNCTIONS make_dxgi_device_table() {
    DXGI1_2_DDI_BASE_FUNCTIONS table{};
    install_present_ddi(table);
    install_blt_ddi(table);
    install_dxgi_resource_ddi(table);
    table.pfnGetGammaCaps=unsupported_gamma;
    // No MPO support is advertised by this KMD. Do not silently report success.
    table.pfnGetMultiplaneOverlayCaps=unsupported_overlay_caps;
    table.pfnGetMultiplaneOverlayFilterRange=nullptr; // Reserved by the DXGI DDI.
    table.pfnCheckMultiplaneOverlaySupport=unsupported_overlay_check;
    table.pfnPresentMultiplaneOverlay=unsupported_overlay_present;
    return table;
}
}
