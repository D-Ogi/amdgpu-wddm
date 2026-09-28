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

inline HRESULT APIENTRY unsupported_gamma(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *args) {
    if (!args || !args->hDevice || !args->pGammaCapabilities) return E_INVALIDARG;
    // The KMD does not implement a gamma-ramp DDI. Leave output untouched.
    return DXGI_ERROR_UNSUPPORTED;
}
inline HRESULT APIENTRY unsupported_overlay_caps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYCAPS *) { return DXGI_ERROR_UNSUPPORTED; }
inline HRESULT APIENTRY unsupported_overlay_filter(void *) { return DXGI_ERROR_UNSUPPORTED; }
inline HRESULT APIENTRY unsupported_overlay_check(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYSUPPORT *) { return DXGI_ERROR_UNSUPPORTED; }
inline HRESULT APIENTRY unsupported_overlay_present(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY *) { return DXGI_ERROR_UNSUPPORTED; }
inline DXGI1_2_DDI_BASE_FUNCTIONS make_dxgi_device_table() {
    DXGI1_2_DDI_BASE_FUNCTIONS table{};
    install_present_ddi(table);
    install_blt_ddi(table);
    install_dxgi_resource_ddi(table);
    table.pfnGetGammaCaps=unsupported_gamma;
    // No MPO support is advertised by this KMD. Do not silently report success.
    table.pfnGetMultiplaneOverlayCaps=unsupported_overlay_caps;
    table.pfnGetMultiplaneOverlayFilterRange=unsupported_overlay_filter;
    table.pfnCheckMultiplaneOverlaySupport=unsupported_overlay_check;
    table.pfnPresentMultiplaneOverlay=unsupported_overlay_present;
    return table;
}
}
