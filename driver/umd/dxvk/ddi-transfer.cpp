// SPDX-License-Identifier: MIT
#include "ddi-transfer.h"
namespace bc250::umd {
HRESULT convert_copy_box(const D3D10_DDI_BOX *s,D3D11_BOX &out,bool &empty) {
    empty=false;
    if (!s) return S_OK;
    // WDK boxes are signed; COM boxes are unsigned. Never reinterpret a
    // negative coordinate as a large positive texture/buffer address.
    if (s->left<0 || s->top<0 || s->front<0 || s->right<0 || s->bottom<0 || s->back<0) return E_INVALIDARG;
    out={UINT(s->left),UINT(s->top),UINT(s->front),UINT(s->right),UINT(s->bottom),UINT(s->back)};
    empty=s->left>=s->right || s->top>=s->bottom || s->front>=s->back;
    return S_OK;
}
HRESULT convert_copy_flags(UINT flags,UINT &out) {
    constexpr UINT known=D3D11_1DDI_COPY_NO_OVERWRITE|D3D11_1DDI_COPY_DISCARD|D3D11_1DDI_COPY_TILEABLE;
    if (flags & ~known) return E_INVALIDARG;
    if ((flags & D3D11_1DDI_COPY_NO_OVERWRITE) && (flags & D3D11_1DDI_COPY_DISCARD)) return E_INVALIDARG;
    UINT converted=0;
    if (flags & D3D11_1DDI_COPY_NO_OVERWRITE) converted|=D3D11_COPY_NO_OVERWRITE;
    if (flags & D3D11_1DDI_COPY_DISCARD) converted|=D3D11_COPY_DISCARD;
    // TILEABLE permits an optimization on tile-based deferred renderers. It
    // is not a tiled-resource copy command. Use the normal full operation.
    out=converted; return S_OK;
}
namespace {
DeviceOwner &owner(D3D10DDI_HDEVICE h) { return *static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner; }
ID3D11Resource *resource(D3D10DDI_HRESOURCE h) {
    auto *s=static_cast<DdiResource *>(h.pDrvPrivate); return s ? s->object : nullptr;
}
void APIENTRY copy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,D3D10DDI_HRESOURCE src) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *d=resource(dst); auto *s=resource(src);
        if (!d || !s) { report_ddi_error(owner(h),E_INVALIDARG); return; }
        c.CopyResource(d,s);
    });
}
void APIENTRY region(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,UINT dstSub,UINT x,UINT y,UINT z,
    D3D10DDI_HRESOURCE src,UINT srcSub,const D3D10_DDI_BOX *box,UINT flags) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *d=resource(dst); auto *s=resource(src);
        D3D11_BOX converted{}; bool empty=false; UINT copyFlags=0;
        HRESULT hr=convert_copy_box(box,converted,empty);
        if (SUCCEEDED(hr)) hr=convert_copy_flags(flags,copyFlags);
        if (FAILED(hr) || !d || !s) { report_ddi_error(owner(h),FAILED(hr) ? hr : E_INVALIDARG); return; }
        if (!empty) c.CopySubresourceRegion1(d,dstSub,x,y,z,s,srcSub,box ? &converted : nullptr,copyFlags);
    });
}
void APIENTRY update(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,UINT sub,const D3D10_DDI_BOX *box,
    const void *data,UINT rowPitch,UINT depthPitch,UINT flags) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *d=resource(dst);
        D3D11_BOX converted{}; bool empty=false; UINT copyFlags=0;
        HRESULT hr=convert_copy_box(box,converted,empty);
        if (SUCCEEDED(hr)) hr=convert_copy_flags(flags,copyFlags);
        if (FAILED(hr) || !d || (!empty && !data)) { report_ddi_error(owner(h),FAILED(hr) ? hr : E_INVALIDARG); return; }
        if (!empty) {
            hr=owner(h).take_deferred_error();
            if (FAILED(hr)) { report_ddi_error(owner(h),hr); return; }
            c.UpdateSubresource1(d,sub,box ? &converted : nullptr,data,rowPitch,depthPitch,copyFlags);
            hr=owner(h).take_deferred_error();
            if (FAILED(hr)) report_ddi_error(owner(h),hr);
        }
    });
}
void APIENTRY resolve(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,UINT dstSub,
    D3D10DDI_HRESOURCE src,UINT srcSub,DXGI_FORMAT format) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *d=resource(dst); auto *s=resource(src);
        if (!d || !s) { report_ddi_error(owner(h),E_INVALIDARG); return; }
        c.ResolveSubresource(d,dstSub,s,srcSub,format);
    });
}
}
void install_transfer_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnResourceCopy=copy; t.pfnResourceCopyRegion=region;
    t.pfnDefaultConstantBufferUpdateSubresourceUP=update; t.pfnResourceUpdateSubresourceUP=update; t.pfnResourceResolveSubresource=resolve;
}
}
