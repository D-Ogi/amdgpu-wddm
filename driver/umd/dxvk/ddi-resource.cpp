// SPDX-License-Identifier: MIT
#include "ddi-resource.h"
#include "diagnostics.h"
#include "runtime-surface-format.h"
#include <algorithm>
#include <utility>
#include <cstring>
namespace bc250::umd {
HRESULT convert_resource(const D3D11DDIARG_CREATERESOURCE &s,ResourceDescription &out) {
    // Shared/primary ownership belongs to the runtime allocation/import path.
    // Never silently create an engine-private replacement for these resources.
    if (s.pPrimaryDesc || (s.BindFlags & D3D10_DDI_BIND_PRESENT) ||
        (s.MiscFlags & (D3D10_DDI_RESOURCE_MISC_SHARED|D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE))) return E_NOTIMPL;
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
    // Tiled resources (WDDM 1.3 flags, sent only once an FL12 adapter reports tiled tiers).
    MISC(D3DWDDM1_3DDI_RESOURCE_MISC_TILED,D3D11_RESOURCE_MISC_TILED);
    MISC(D3DWDDM1_3DDI_RESOURCE_MISC_TILE_POOL,D3D11_RESOURCE_MISC_TILE_POOL);
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
HRESULT convert_runtime_resource(const D3D11DDIARG_CREATERESOURCE &input,HANDLE runtimeHandle,
    RuntimeSurfaceRequest &request,D3D11_TEXTURE2D_DESC1 &desc) {
    if (!runtimeHandle || input.ResourceDimension!=D3D10DDIRESOURCE_TEXTURE2D ||
        input.MipLevels!=1 || input.ArraySize!=1 || input.SampleDesc.Count!=1 || input.SampleDesc.Quality ||
        input.Usage!=D3D10_DDI_USAGE_DEFAULT || input.MapFlags) return E_NOTIMPL;
    auto ordinary=input; ordinary.pPrimaryDesc=nullptr;
    ordinary.BindFlags&=~UINT(D3D10_DDI_BIND_PRESENT);
    // DISPLAYABLE is runtime allocation intent, not a COM texture misc flag.
    // LB7A runtime surfaces already use linear, 256-byte-pitch scanout storage.
    // A window buffer with no primary descriptor is not a VidPn primary.
    ordinary.MiscFlags&=~UINT(D3D10_DDI_RESOURCE_MISC_SHARED|D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT|
        D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE);
    ResourceDescription converted;
    HRESULT hr=convert_resource(ordinary,converted);
    if (FAILED(hr)) return hr;
    const auto &d=converted.texture2d;
    if (d.Width>16384 || d.Height>16384) return E_INVALIDARG;
    // The storage formats the kernel driver and the compositor's UMD admit by the same table: BGRA8
    // and RGBA8 with their sRGB views, RGB10A2, RGBA16F, and A8 for the atlases DirectComposition
    // shares (M14.1: Task Manager's 32x32 A8 render target). A primary is the buffer of a flip-model
    // or fullscreen swap chain; A8 is never one. Every primary keeps the descriptor's VidPn source:
    // dxgkrnl refused an RGB10A2 primary allocation with D3DDDI_ID_UNINITIALIZED (343: AllocateCb
    // E_INVALIDARG, the kernel driver saw no request). Admission as a primary is not permission to
    // scan out: the kernel driver's SetVidPnSourceAddress still takes only the SCANOUT_PRIMARY rows
    // (8-bit), and direct scan-out of RGB10A2/RGBA16F stays unsupported. The Ascent's UE 4.26
    // borderless swap chain is RGB10A2 with a primary descriptor; refusing it removed the device at
    // startup (339-342).
    const auto *row=runtime_surface_format(d.Format);
    if (!row || (input.pPrimaryDesc && row->dxgi==AMDGPU_WDDM_DXGI_A8_UNORM)) return E_NOTIMPL;
    RuntimeSurfaceRequest r{};
    r.runtime_resource=runtimeHandle; r.primary=input.pPrimaryDesc!=nullptr;
    r.displayable=(input.MiscFlags&D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE)!=0;
    r.shared=(input.MiscFlags&D3D10_DDI_RESOURCE_MISC_SHARED)!=0;
    if (input.pPrimaryDesc) r.vidpn_source=input.pPrimaryDesc->VidPnSourceId;
    const UINT pitch=runtime_surface_pitch(d.Width,row->bytes_per_pixel);
    const UINT64 bytes=(UINT64(pitch)*((d.Height+3)&~3u)+4095)&~UINT64(4095);
    r.surface={BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,1,d.Width,d.Height,pitch,row->d3dddi,bytes};
    D3D11_TEXTURE2D_DESC1 result{d.Width,d.Height,d.MipLevels,d.ArraySize,d.Format,d.SampleDesc,
        d.Usage,d.BindFlags,d.CPUAccessFlags,d.MiscFlags,D3D11_TEXTURE_LAYOUT_UNDEFINED};
    r.texture={BC250_SURFACE_RESOURCE_MAGIC,BC250_SURFACE_RESOURCE_TEXTURE_VERSION,UINT(r.shared),r.primary ? 1u : 0u,
        result.Width,result.Height,result.MipLevels,result.ArraySize,UINT(result.Format),result.SampleDesc.Count,
        result.SampleDesc.Quality,UINT(result.Usage),result.BindFlags,result.CPUAccessFlags,result.MiscFlags,UINT(result.TextureLayout)};
    request=r; desc=result; return S_OK;
}
HRESULT decode_open_resource(const D3D10DDIARG_OPENRESOURCE &input,BC250_WDDM_ALLOCATION_PRIVATE &metadata,
    D3D11_TEXTURE2D_DESC1 &desc) {
    if (input.NumAllocations!=1 || !input.pOpenAllocationInfo2) return E_NOTIMPL;
    const auto &a=input.pOpenAllocationInfo2[0];
    if (!a.hAllocation || !a.pPrivateDriverData || a.PrivateDriverDataSize!=sizeof(metadata) ||
        !input.pPrivateDriverData || input.PrivateDriverDataSize!=sizeof(BC250_SURFACE_RESOURCE_PRIVATE)) return E_INVALIDARG;
    BC250_WDDM_ALLOCATION_PRIVATE m{}; BC250_SURFACE_RESOURCE_PRIVATE p{};
    std::memcpy(&m,a.pPrivateDriverData,sizeof(m)); std::memcpy(&p,input.pPrivateDriverData,sizeof(p));
    int shared=0,cached=0;
    if (p.Magic!=BC250_SURFACE_RESOURCE_MAGIC || p.Version!=BC250_SURFACE_RESOURCE_TEXTURE_VERSION ||
        !Bc250SurfaceResourcePolicy(&p,sizeof(p),&shared,&cached) ||
        m.Magic!=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || m.Version!=1 ||
        p.Width!=m.Width || p.Height!=m.Height || p.Width>16384 || p.Height>16384 ||
        p.MipLevels!=1 || p.ArraySize!=1 || p.SampleCount!=1 || p.SampleQuality ||
        p.Usage!=D3D11_USAGE_DEFAULT || p.CpuAccessFlags || p.TextureLayout!=D3D11_TEXTURE_LAYOUT_UNDEFINED ||
        (p.BindFlags&~UINT(D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS)) ||
        (p.MiscFlags&~UINT(D3D11_RESOURCE_MISC_GENERATE_MIPS|D3D11_RESOURCE_MISC_RESOURCE_CLAMP))) return E_INVALIDARG;
    // The row the creator took the LB7A format and pitch from; its pixel size bounds the geometry.
    const auto *row=runtime_surface_format(DXGI_FORMAT(p.Format));
    if (!row) return E_NOTIMPL;
    if (row->d3dddi!=m.Format || !runtime_surface_geometry(m,row->bytes_per_pixel)) return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC1 d{p.Width,p.Height,p.MipLevels,p.ArraySize,DXGI_FORMAT(p.Format),
        {p.SampleCount,p.SampleQuality},D3D11_USAGE(p.Usage),p.BindFlags,p.CpuAccessFlags,p.MiscFlags,D3D11_TEXTURE_LAYOUT(p.TextureLayout)};
    metadata=m; desc=d; return S_OK;
}
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATERESOURCE *) { return sizeof(DdiResource); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATERESOURCE *desc,
    D3D10DDI_HRESOURCE handle,D3D10DDI_HRTRESOURCE runtimeHandle) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
    if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !desc || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        if (desc->pPrimaryDesc || (desc->BindFlags&D3D10_DDI_BIND_PRESENT) ||
            (desc->MiscFlags&(D3D10_DDI_RESOURCE_MISC_SHARED|D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE))) {
            RuntimeSurfaceRequest request{}; D3D11_TEXTURE2D_DESC1 texture{};
            HRESULT hr=convert_runtime_resource(*desc,reinterpret_cast<HANDLE>(runtimeHandle.handle),request,texture);
            if (FAILED(hr)) {
                // Record the rejected runtime descriptor, not pointers or memory.
                // Swap-chain admission must be fixed against the actual request.
                static std::atomic_uint reports{0};
                if (reports.fetch_add(1,std::memory_order_relaxed)<8) {
                    char text[512];
                    const auto *m=desc->pMipInfoList;
                    std::snprintf(text,sizeof(text),
                        "M14 runtime resource rejected hr=%08X handle=%u primary=%u dimension=%u format=%u bind=%08X misc=%08X usage=%u map=%08X mips=%u array=%u samples=%u quality=%u width=%u height=%u\n",
                        unsigned(hr),runtimeHandle.handle ? 1u : 0u,desc->pPrimaryDesc ? 1u : 0u,
                        unsigned(desc->ResourceDimension),unsigned(desc->Format),desc->BindFlags,desc->MiscFlags,
                        unsigned(desc->Usage),desc->MapFlags,desc->MipLevels,desc->ArraySize,
                        desc->SampleDesc.Count,desc->SampleDesc.Quality,m ? m->TexelWidth : 0u,m ? m->TexelHeight : 0u);
                    OutputDebugStringA(text);
                }
                report_ddi_error(owner,hr); return;
            }
            RuntimeSurface *surface=nullptr;
            hr=owner.begin_surface(request,texture,surface);
            if (SUCCEEDED(hr)) hr=owner.wait_surface(*surface);
            if (hr!=S_OK) {
                if (surface) owner.release_surface_handle(*surface);
                report_ddi_error(owner,FAILED(hr) ? hr : E_FAIL); return;
            }
            s->runtime_surface=surface; s->object=surface->texture.texture;
            s->dimension=D3D10DDIRESOURCE_TEXTURE2D;
            s->present_allocation=surface->allocation.allocation; s->present_subresource=0;
            if (desc->pInitialDataUP) {
                const auto &initial=desc->pInitialDataUP[0];
                owner.context()->UpdateSubresource(s->object,0,nullptr,initial.pSysMem,initial.SysMemPitch,initial.SysMemSlicePitch);
            }
            return;
        }
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
SIZE_T APIENTRY opened_size(D3D10DDI_HDEVICE,const D3D10DDIARG_OPENRESOURCE *) { return sizeof(DdiResource); }
void APIENTRY open(D3D10DDI_HDEVICE h,const D3D10DDIARG_OPENRESOURCE *args,
    D3D10DDI_HRESOURCE handle,D3D10DDI_HRTRESOURCE runtimeHandle) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate); if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !args || !runtimeHandle.handle) { report_ddi_error(owner,E_INVALIDARG); return; }
        BC250_WDDM_ALLOCATION_PRIVATE metadata{}; D3D11_TEXTURE2D_DESC1 desc{};
        HRESULT hr=decode_open_resource(*args,metadata,desc);
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        RuntimeSurfaceAllocation allocation{reinterpret_cast<HANDLE>(runtimeHandle.handle),args->pOpenAllocationInfo2[0].hAllocation,args->hKMResource.handle};
        RuntimeSurface *surface=nullptr;
        hr=owner.adopt_surface(allocation,metadata,desc,surface);
        if (SUCCEEDED(hr)) hr=owner.wait_surface(*surface);
        if (hr!=S_OK) {
            if (surface) owner.release_surface_handle(*surface);
            report_ddi_error(owner,FAILED(hr) ? hr : E_FAIL); return;
        }
        s->runtime_surface=surface; s->object=surface->texture.texture;
        s->dimension=D3D10DDIRESOURCE_TEXTURE2D; s->present_allocation=surface->allocation.allocation;
    });
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
        if (s && s->runtime_surface) {
            auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
            HRESULT hr=owner.release_surface_handle(*s->runtime_surface);
            // Runtime private storage may disappear now. DeviceOwner retains any
            // unfinished cleanup independently of this DDI handle.
            *s={};
            if (ddi_device_status(hr)==D3DDDIERR_DEVICEREMOVED) report_ddi_error(owner,hr);
        } else if (s && s->object) { s->object->Release(); *s={}; }
    });
}
}
void install_resource_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateResourceSize=size; t.pfnCreateResource=create; t.pfnDestroyResource=destroy;
    t.pfnCalcPrivateOpenedResourceSize=opened_size; t.pfnOpenResource=open;
}
}
