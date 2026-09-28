// SPDX-License-Identifier: MIT
#include "ddi-resource.h"
#include <algorithm>
#include <utility>
namespace bc250::umd {
HRESULT convert_resource(const D3D11DDIARG_CREATERESOURCE &s,ResourceDescription &out) {
    // Shared/primary ownership belongs to the runtime allocation/import path.
    // Never silently create an engine-private replacement for these resources.
    if (s.pPrimaryDesc || (s.BindFlags & D3D10_DDI_BIND_PRESENT) ||
        (s.MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED)) return E_NOTIMPL;
    if (!s.pMipInfoList || !s.MipLevels || !s.ArraySize) return E_INVALIDARG;
    D3D11_USAGE usage;
    switch(s.Usage) {
    case D3D10_DDI_USAGE_DEFAULT: usage=D3D11_USAGE_DEFAULT; break;
    case D3D10_DDI_USAGE_IMMUTABLE: usage=D3D11_USAGE_IMMUTABLE; break;
    case D3D10_DDI_USAGE_DYNAMIC: usage=D3D11_USAGE_DYNAMIC; break;
    case D3D10_DDI_USAGE_STAGING: usage=D3D11_USAGE_STAGING; break;
    default: return E_INVALIDARG;
    }
    if (usage==D3D11_USAGE_IMMUTABLE && !s.pInitialDataUP) return E_INVALIDARG;
    UINT bind=0,remaining=s.BindFlags;
#define BIND(ddi,api) if (remaining & ddi) { bind|=api; remaining&=~UINT(ddi); }
    BIND(D3D10_DDI_BIND_VERTEX_BUFFER,D3D11_BIND_VERTEX_BUFFER);
    BIND(D3D10_DDI_BIND_INDEX_BUFFER,D3D11_BIND_INDEX_BUFFER);
    BIND(D3D10_DDI_BIND_CONSTANT_BUFFER,D3D11_BIND_CONSTANT_BUFFER);
    BIND(D3D10_DDI_BIND_SHADER_RESOURCE,D3D11_BIND_SHADER_RESOURCE);
    BIND(D3D10_DDI_BIND_STREAM_OUTPUT,D3D11_BIND_STREAM_OUTPUT);
    BIND(D3D10_DDI_BIND_RENDER_TARGET,D3D11_BIND_RENDER_TARGET);
    BIND(D3D10_DDI_BIND_DEPTH_STENCIL,D3D11_BIND_DEPTH_STENCIL);
    BIND(D3D11_DDI_BIND_UNORDERED_ACCESS,D3D11_BIND_UNORDERED_ACCESS);
#undef BIND
    if (remaining) return E_NOTIMPL;
    if (s.MapFlags & ~UINT(D3D10_DDI_CPU_ACCESS_MASK)) return E_INVALIDARG;
    UINT cpu=0;
    if (s.MapFlags & D3D10_DDI_CPU_ACCESS_READ) cpu|=D3D11_CPU_ACCESS_READ;
    if (s.MapFlags & D3D10_DDI_CPU_ACCESS_WRITE) cpu|=D3D11_CPU_ACCESS_WRITE;
    UINT misc=0; remaining=s.MiscFlags;
#define MISC(ddi,api) if (remaining & ddi) { misc|=api; remaining&=~UINT(ddi); }
    MISC(D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP,D3D11_RESOURCE_MISC_GENERATE_MIPS);
    MISC(D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS,D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS);
    MISC(D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
    MISC(D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
    MISC(D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP,D3D11_RESOURCE_MISC_RESOURCE_CLAMP);
#undef MISC
    if (remaining) return E_NOTIMPL;
    ResourceDescription d; d.dimension=s.ResourceDimension;
    const auto &m=s.pMipInfoList[0];
    if (!m.TexelWidth) return E_INVALIDARG;
    const bool buffer=s.ResourceDimension==D3D10DDIRESOURCE_BUFFER || s.ResourceDimension==D3D11DDIRESOURCE_BUFFEREX;
    UINT subresources=1;
    if (buffer) {
        if (s.MipLevels!=1 || s.ArraySize!=1) return E_INVALIDARG;
        d.buffer={m.TexelWidth,usage,bind,cpu,misc,s.ByteStride};
    } else {
        // DDI supplies logical texels and padded physical dimensions. COM wants
        // logical dimensions; padding belongs to the backend allocation.
        if (s.MipLevels>D3D11_REQ_MIP_LEVELS || s.ArraySize>D3D11_REQ_TEXTURE2D_ARRAY_AXIS_DIMENSION) return E_INVALIDARG;
        subresources=s.MipLevels*s.ArraySize;
        switch(s.ResourceDimension) {
        case D3D10DDIRESOURCE_TEXTURE1D:
            d.texture1d={m.TexelWidth,s.MipLevels,s.ArraySize,s.Format,usage,bind,cpu,misc}; break;
        case D3D10DDIRESOURCE_TEXTURECUBE:
            if (s.ArraySize%6 || m.TexelWidth!=m.TexelHeight) return E_INVALIDARG;
            misc|=D3D11_RESOURCE_MISC_TEXTURECUBE;
            [[fallthrough]];
        case D3D10DDIRESOURCE_TEXTURE2D:
            if (!m.TexelHeight) return E_INVALIDARG;
            d.texture2d={m.TexelWidth,m.TexelHeight,s.MipLevels,s.ArraySize,s.Format,s.SampleDesc,usage,bind,cpu,misc}; break;
        case D3D10DDIRESOURCE_TEXTURE3D:
            if (s.ArraySize!=1 || !m.TexelHeight || !m.TexelDepth) return E_INVALIDARG;
            d.texture3d={m.TexelWidth,m.TexelHeight,m.TexelDepth,s.MipLevels,s.Format,usage,bind,cpu,misc}; break;
        default: return E_INVALIDARG;
        }
        for (UINT level=1;level<s.MipLevels;++level) {
            const auto &a=s.pMipInfoList[level-1]; const auto &b=s.pMipInfoList[level];
            if (b.TexelWidth!=std::max(1u,a.TexelWidth/2)) return E_INVALIDARG;
            if (s.ResourceDimension!=D3D10DDIRESOURCE_TEXTURE1D && b.TexelHeight!=std::max(1u,a.TexelHeight/2)) return E_INVALIDARG;
            if (s.ResourceDimension==D3D10DDIRESOURCE_TEXTURE3D && b.TexelDepth!=std::max(1u,a.TexelDepth/2)) return E_INVALIDARG;
        }
    }
    if (s.pInitialDataUP) {
        d.initial.reserve(subresources);
        for (UINT i=0;i<subresources;++i) {
            const auto &a=s.pInitialDataUP[i];
            if (!a.pSysMem) return E_INVALIDARG;
            d.initial.push_back({a.pSysMem,a.SysMemPitch,a.SysMemSlicePitch});
        }
    }
    // This is the D3D11.1 entry. Do not read later appended TextureLayout or
    // decoder fields merely because the build uses a newer WDK header.
    out=std::move(d); return S_OK;
}
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATERESOURCE *) { return sizeof(DdiResource); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATERESOURCE *desc,
    D3D10DDI_HRESOURCE handle,D3D10DDI_HRTRESOURCE) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
    if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !desc || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        ResourceDescription d;
        HRESULT hr=convert_resource(*desc,d);
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        const auto *initial=d.initial.empty() ? nullptr : d.initial.data();
        switch(d.dimension) {
        case D3D10DDIRESOURCE_BUFFER: case D3D11DDIRESOURCE_BUFFEREX: {
            ID3D11Buffer *p=nullptr; hr=owner.device()->CreateBuffer(&d.buffer,initial,&p); s->object=p; break;
        }
        case D3D10DDIRESOURCE_TEXTURE1D: {
            ID3D11Texture1D *p=nullptr; hr=owner.device()->CreateTexture1D(&d.texture1d,initial,&p); s->object=p; break;
        }
        case D3D10DDIRESOURCE_TEXTURE2D: case D3D10DDIRESOURCE_TEXTURECUBE: {
            ID3D11Texture2D *p=nullptr; hr=owner.device()->CreateTexture2D(&d.texture2d,initial,&p); s->object=p; break;
        }
        case D3D10DDIRESOURCE_TEXTURE3D: {
            ID3D11Texture3D *p=nullptr; hr=owner.device()->CreateTexture3D(&d.texture3d,initial,&p); s->object=p; break;
        }
        default: hr=E_INVALIDARG;
        }
        if (FAILED(hr)) {
            if (s->object) { s->object->Release(); s->object=nullptr; }
            report_ddi_error(owner,hr);
        } else if (!s->object) report_ddi_error(owner,E_FAIL);
        else s->dimension=d.dimension;
    });
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
}
void install_resource_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateResourceSize=size; t.pfnCreateResource=create; t.pfnDestroyResource=destroy;
}
}
