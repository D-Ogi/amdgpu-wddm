// SPDX-License-Identifier: MIT
#include "ddi-buffer-binding.h"
#include <array>
namespace bc250::umd {
HRESULT resource_buffer(D3D10DDI_HRESOURCE handle,ID3D11Buffer *&out) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
    if (!s) { out=nullptr; return S_OK; }
    if (!s->object || (s->dimension!=D3D10DDIRESOURCE_BUFFER && s->dimension!=D3D11DDIRESOURCE_BUFFEREX)) return E_INVALIDARG;
    out=static_cast<ID3D11Buffer *>(s->object); return S_OK;
}
namespace {
DeviceOwner &owner(D3D10DDI_HDEVICE h) { return *static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner; }
template<size_t N> HRESULT buffers(UINT first,UINT count,const D3D10DDI_HRESOURCE *handles,std::array<ID3D11Buffer *,N> &out) {
    if (first>=N || count>N-first || (count && !handles)) return E_INVALIDARG;
    for (UINT i=0;i<count;++i) { const HRESULT hr=resource_buffer(handles[i],out[i]); if (FAILED(hr)) return hr; }
    return S_OK;
}
void APIENTRY vertex(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HRESOURCE *handles,const UINT *strides,const UINT *offsets) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        std::array<ID3D11Buffer *,D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT> b{};
        HRESULT hr=buffers(first,count,handles,b);
        if (FAILED(hr) || (count && (!strides || !offsets))) { report_ddi_error(owner(h),E_INVALIDARG); return; }
        context.IASetVertexBuffers(first,count,b.data(),strides,offsets);
    });
}
void APIENTRY index(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle,DXGI_FORMAT format,UINT offset) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        ID3D11Buffer *b=nullptr;
        HRESULT hr=resource_buffer(handle,b);
        if (FAILED(hr) || (b && format!=DXGI_FORMAT_R16_UINT && format!=DXGI_FORMAT_R32_UINT)) {
            report_ddi_error(owner(h),E_INVALIDARG); return;
        }
        context.IASetIndexBuffer(b,format,offset);
    });
}
template<auto Set> void APIENTRY constants(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HRESOURCE *handles,
    const UINT *firstConstant,const UINT *numConstants) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        std::array<ID3D11Buffer *,D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> b{};
        HRESULT hr=buffers(first,count,handles,b);
        if (FAILED(hr)) { report_ddi_error(owner(h),hr); return; }
        // These are counts of 16-byte constants, not byte offsets. Preserve
        // optional arrays and let COM apply the D3D11.1 range semantics.
        (context.*Set)(first,count,b.data(),firstConstant,numConstants);
    });
}
}
void install_buffer_binding_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnIaSetVertexBuffers=vertex; t.pfnIaSetIndexBuffer=index;
    t.pfnVsSetConstantBuffers=constants<&ID3D11DeviceContext4::VSSetConstantBuffers1>;
    t.pfnPsSetConstantBuffers=constants<&ID3D11DeviceContext4::PSSetConstantBuffers1>;
    t.pfnGsSetConstantBuffers=constants<&ID3D11DeviceContext4::GSSetConstantBuffers1>;
    t.pfnHsSetConstantBuffers=constants<&ID3D11DeviceContext4::HSSetConstantBuffers1>;
    t.pfnDsSetConstantBuffers=constants<&ID3D11DeviceContext4::DSSetConstantBuffers1>;
    t.pfnCsSetConstantBuffers=constants<&ID3D11DeviceContext4::CSSetConstantBuffers1>;
}
}
