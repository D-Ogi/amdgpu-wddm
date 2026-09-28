// SPDX-License-Identifier: MIT
#include "ddi-format.h"
namespace bc250::umd {
// WDK CheckFormatSupport permits NOT_SUPPORTED only for this explicit list.
bool format_allows_not_supported(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_A8P8: case DXGI_FORMAT_AI44: case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_IA44: case DXGI_FORMAT_NV11: case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016: case DXGI_FORMAT_P8: case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_Y210: case DXGI_FORMAT_Y216: case DXGI_FORMAT_Y410: case DXGI_FORMAT_Y416:
        return true;
    default: return false;
    }
}
bool defined_dxgi_format(DXGI_FORMAT format) {
    // SDK 10.0.26100 dxgiformat.h: the gaps and FORCE_UINT are not formats.
    const UINT value=static_cast<UINT>(format);
    return value<=DXGI_FORMAT_B4G4R4A4_UNORM ||
        (value>=DXGI_FORMAT_P208 && value<=DXGI_FORMAT_V408) ||
        (value>=DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE && value<=DXGI_FORMAT_A4B4G4R4_UNORM);
}
HRESULT classify_format_result(DXGI_FORMAT format,HRESULT hr,UINT &fallback) {
    if (!defined_dxgi_format(format)) return E_FAIL;
    if (hr!=E_FAIL && hr!=E_INVALIDARG) return hr;
    // CheckFormatSupport DDI reports optional features only. A defined format
    // rejected by the engine does not become a nonexistent format (E_FAIL).
    fallback=format_allows_not_supported(format) ? D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED : 0;
    return S_FALSE;
}
HRESULT classify_format_support2_result(HRESULT hr,UINT flags) {
    // DXVK GetFormatSupportFlags returns E_FAIL when the requested flags2
    // word is zero, even if flags1 supports the format. This query follows a
    // successful CheckFormatSupport, so an empty optional word is valid.
    // Preserve unexpected failures and inconsistent nonzero failure output.
    return hr==E_FAIL && !flags ? S_OK : hr;
}
UINT convert_format_support(UINT s,UINT s2) {
    UINT result=0;
#define MAP(api,ddi) if (s & api) result|=ddi
    MAP(D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE);
    MAP(D3D11_FORMAT_SUPPORT_RENDER_TARGET,D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET);
    if ((s&D3D11_FORMAT_SUPPORT_RENDER_TARGET) && (s&D3D11_FORMAT_SUPPORT_BLENDABLE)) result|=D3D10_DDI_FORMAT_SUPPORT_BLENDABLE;
    MAP(D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET,D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET);
    MAP(D3D11_FORMAT_SUPPORT_MULTISAMPLE_LOAD,D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD);
    MAP(D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER,D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER);
    MAP(D3D11_FORMAT_SUPPORT_BUFFER,D3D11_1DDI_FORMAT_SUPPORT_BUFFER);
    MAP(D3D11_FORMAT_SUPPORT_SHADER_GATHER,D3D11_1DDI_FORMAT_SUPPORT_SHADER_GATHER);
#undef MAP
    if (s2&D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) result|=D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES;
    if (s2&D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP) result|=D3D11_1DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
    // Video, capture and overlay paths are not implemented in this UMD table.
    // Later DDI-version bits must not leak through a D3D11.1 capability query.
    return result;
}
namespace {
void APIENTRY format(D3D10DDI_HDEVICE h,DXGI_FORMAT value,UINT *out) {
    if (out) *out=0;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!out || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        if (!defined_dxgi_format(value)) { report_ddi_error(owner,E_FAIL); return; }
        UINT support=0;
        HRESULT hr=classify_format_result(value,owner.device()->CheckFormatSupport(value,&support),*out);
        if (hr==S_FALSE) return;
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 extra{value,0};
        hr=owner.device()->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,&extra,sizeof(extra));
        hr=classify_format_support2_result(hr,extra.OutFormatSupport2);
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        *out=convert_format_support(support,extra.OutFormatSupport2);
    });
}
void APIENTRY samples(D3D10DDI_HDEVICE h,DXGI_FORMAT value,UINT count,UINT *out) {
    if (out) *out=0;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!out || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        if (!count || count>D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT) return;
        UINT quality=0; HRESULT hr=owner.device()->CheckMultisampleQualityLevels(value,count,&quality);
        if (hr==E_INVALIDARG) return;
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        *out=quality;
    });
}
}
void install_format_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCheckFormatSupport=format; t.pfnCheckMultisampleQualityLevels=samples;
}
}
