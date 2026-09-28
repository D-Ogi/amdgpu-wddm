// SPDX-License-Identifier: MIT
#include "ddi-format.h"
namespace bc250::umd {
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
        UINT support=0; HRESULT hr=owner.device()->CheckFormatSupport(value,&support);
        if (hr==E_INVALIDARG) return; // Unsupported/unknown format: no support.
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 extra{value,0};
        hr=owner.device()->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2,&extra,sizeof(extra));
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        *out=convert_format_support(support,extra.OutFormatSupport2);
    });
}
void APIENTRY samples(D3D10DDI_HDEVICE h,DXGI_FORMAT value,UINT count,UINT *out) {
    if (out) *out=0;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!out || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
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
