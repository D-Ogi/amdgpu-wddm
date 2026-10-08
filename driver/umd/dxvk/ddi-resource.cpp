// SPDX-License-Identifier: MIT
#include "ddi-resource.h"
#include "diagnostics.h"
#include "runtime-surface-format.h"
#include <algorithm>
#include <utility>
#include <cstring>
namespace bc250::umd {
namespace {
// One line for every change of the scan-out answer, as the D3D12 shell writes it (heap-import.cpp,
// scanout_note): a trial must be able to read which clause decided each chain a game made. A game that
// recreates the same chain at the same mode adds nothing; the budget bounds a game that alternates forever,
// and the last line of the budget says that it ran out.
void scanout_primary_note(const ScanoutPrimaryDecision &decision,const ScanoutSource *source,bool displayable,
    UINT vidpn_source,unsigned dxgi,unsigned width,unsigned height,unsigned pitch) noexcept {
    const unsigned long force_cpu=source ? source->force_cpu : 0;
    const unsigned long long words[]={unsigned(decision.reason),unsigned(decision.switch_state),width,height,pitch,
        dxgi,decision.caps.flags,decision.caps.post_width,decision.caps.post_height,force_cpu,displayable ? 1u : 0u,
        vidpn_source};
    unsigned long long key=1469598103934665603ull;                    // FNV-1a over the fields the line prints
    for (const unsigned long long word:words)
        for (unsigned i=0;i<8;++i) key=(key^((word>>(i*8))&0xFFu))*1099511628211ull;
    static std::atomic<unsigned long long> last{0};
    static std::atomic_int budget{64};
    if (last.exchange(key,std::memory_order_relaxed)==key) return;
    const int left=budget.fetch_sub(1,std::memory_order_relaxed);
    if (left<=0) return;
    char text[256];
    std::snprintf(text,sizeof(text),
        "M15.14 d3d11 scanout %s switch=%s chain=%ux%u pitch=%u format=%u displayable=%u vidpn=%u caps=%08X "
        "source=%ux%u forcecpu=%lu%s\n",
        scanout_primary_reason_text(decision.reason),scanout_primary_switch_text(decision.switch_state),width,
        height,pitch,dxgi,displayable ? 1u : 0u,vidpn_source,decision.caps.flags,decision.caps.post_width,
        decision.caps.post_height,force_cpu,left==1 ? " budget-spent" : "");
    OutputDebugStringA(text);
}
}
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
    // The video bind flags. DECODER and VIDEO_ENCODER have the same value in the DDI and in the API,
    // and the engine carries them without a usage of their own, so a planar image is still sampled
    // and written the way its other bind flags ask. The media pipeline asks for NV12 with
    // SHADER_RESOURCE|DECODER, and E_NOTIMPL for that costs the device, because CreateResource may
    // not report it (BD-071). D3D11_DDI_BIND_CAPTURE has no API flag at all, so the engine cannot be
    // asked for a capture-capable resource: the request falls through to the answer below, which
    // fails that one call. Consuming the bit would hand the video capture engine a resource it cannot
    // write (BD-071 review). This driver reports no CAPTURE format support, so the runtime does not
    // ask.
    BIND(D3D11_DDI_BIND_DECODER,D3D11_BIND_DECODER);
    BIND(D3D11_DDI_BIND_VIDEO_ENCODER,D3D11_BIND_VIDEO_ENCODER);
#undef BIND
    // A bind flag this driver does not know is a request it cannot serve, which is what
    // DXGI_DDI_ERR_UNSUPPORTED says. The runtime then fails the caller instead of losing the device.
    if (remaining) return DXGI_DDI_ERR_UNSUPPORTED;
    if (s.MapFlags & ~UINT(D3D10_DDI_CPU_ACCESS_MASK)) return E_INVALIDARG;
    UINT cpu=0;
    if (s.MapFlags & D3D10_DDI_CPU_ACCESS_READ) cpu|=D3D11_CPU_ACCESS_READ;
    if (s.MapFlags & D3D10_DDI_CPU_ACCESS_WRITE) cpu|=D3D11_CPU_ACCESS_WRITE;
    UINT misc=0; remaining=s.MiscFlags;
#define MISC(ddi,api) if (remaining & ddi) { misc|=api; remaining&=~UINT(ddi); }
    MISC(D3D10_DDI_RESOURCE_AUTO_GEN_MIP_MAP,D3D11_RESOURCE_MISC_GENERATE_MIPS);
    // A present hint with no API flag of its own. The present path above answers E_NOTIMPL before
    // this loop, so the bit only arrives here without BIND_PRESENT, and then it asks for nothing.
    MISC(D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT,0u);
    MISC(D3D11_DDI_RESOURCE_MISC_DRAWINDIRECT_ARGS,D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS);
    MISC(D3D11_DDI_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS);
    MISC(D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED);
    MISC(D3D11_DDI_RESOURCE_MISC_RESOURCE_CLAMP,D3D11_RESOURCE_MISC_RESOURCE_CLAMP);
    // Tiled resources (WDDM 1.3 flags, sent only once an FL12 adapter reports tiled tiers).
    MISC(D3DWDDM1_3DDI_RESOURCE_MISC_TILED,D3D11_RESOURCE_MISC_TILED);
    MISC(D3DWDDM1_3DDI_RESOURCE_MISC_TILE_POOL,D3D11_RESOURCE_MISC_TILE_POOL);
#undef MISC
    if (remaining) return DXGI_DDI_ERR_UNSUPPORTED;
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
    RuntimeSurfaceRequest &request,D3D11_TEXTURE2D_DESC1 &desc,const ScanoutSource *scanout) {
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
    // M15.14 increment 3: a swap-chain buffer asks for scan-out when every clause of scanout-primary.h holds.
    // The LB7A description does not change: its pitch is already the scan-out pitch for a 4-byte row
    // (runtime_surface_pitch rounds a row up to 256 bytes, as bc250_scanout_primary_pitch does), and the
    // rule checks that equality instead of assuming it. Only the record's SCANOUT bit is added, which moves
    // the allocation to the local segment; every other answer keeps the composed primary.
    if (r.primary || r.displayable) {
        const ScanoutPrimaryDecision decision=scanout_primary_decide(scanout,r.primary,r.vidpn_source,
            UINT(d.Format),d.Width,d.Height,pitch);
        if (decision.admitted) { r.scanout=true; r.texture.Access|=BC250_SURFACE_RESOURCE_SCANOUT; }
        scanout_primary_note(decision,scanout,r.displayable,r.vidpn_source,UINT(d.Format),d.Width,d.Height,pitch);
    }
    request=r; desc=result; return S_OK;
}
// The shared-surface wire format has one reader (driver/contract/bc250_shared_surface.h); this is the
// D3D11 half of it, which maps the neutral record to a D3D11_TEXTURE2D_DESC1 and nothing more.
// BD-075: the D3D12 shell maps the same neutral record to a D3D12_RESOURCE_DESC1, so a surface either
// shell creates is one either shell opens, and the admission rules exist once.
// The admission policy here is this shell's own and is wider than the D3D12 one on purpose: a D3D11
// opener is handed the compositor's own buffers, which are primaries (E26R Access PRIMARY) and may ask
// for a cached CPU mapping (CPU_READ), and refusing those would close the desktop route. Which access
// bits are coherent at all is still the kernel driver's parser's answer, inside the decode.
static_assert(BC250_SHARED_LAYOUT_UNDEFINED==D3D11_TEXTURE_LAYOUT_UNDEFINED);
HRESULT decode_open_resource(const D3D10DDIARG_OPENRESOURCE &input,BC250_WDDM_ALLOCATION_PRIVATE &metadata,
    D3D11_TEXTURE2D_DESC1 &desc) {
    if (input.NumAllocations!=1 || !input.pOpenAllocationInfo2) return E_NOTIMPL;
    const auto &a=input.pOpenAllocationInfo2[0];
    if (!a.hAllocation) return E_INVALIDARG;
    BC250_SHARED_SURFACE_ADMIT admit{};
    admit.RequireShared=0;
    admit.AccessMask=BC250_SURFACE_RESOURCE_ACCESS_MASK;
    BC250_SHARED_SURFACE surface{};
    switch (Bc250SharedSurfaceDecodeAdmitted(input.pPrivateDriverData,input.PrivateDriverDataSize,
                                             a.pPrivateDriverData,a.PrivateDriverDataSize,&admit,&surface)) {
    case BC250_SHARED_SURFACE_OK: break;
    case BC250_SHARED_SURFACE_FORMAT: return E_NOTIMPL;
    default: return E_INVALIDARG;
    }
    BC250_WDDM_ALLOCATION_PRIVATE m{BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,1,surface.Width,surface.Height,
        surface.Pitch,surface.D3dDdiFormat,surface.Size};
    D3D11_TEXTURE2D_DESC1 d{surface.Width,surface.Height,1,1,DXGI_FORMAT(surface.DxgiFormat),{1,0},
        D3D11_USAGE_DEFAULT,surface.BindFlags,0,surface.MiscFlags,D3D11_TEXTURE_LAYOUT_UNDEFINED};
    metadata=m; desc=d; return S_OK;
}
namespace {
// CreateResource may report E_OUTOFMEMORY, D3DDDIERR_DEVICEREMOVED or DXGI_DDI_ERR_UNSUPPORTED. A
// descriptor this driver cannot serve is the third case, not the second: the runtime then fails that
// one call and its caller can choose another format, while device removal ends every device of the
// process. The Ascent's RGB10A2 primary (339-342) is the measured example, and BD-071 is the same
// mistake made with E_NOTIMPL, a status this page does not allow at all.
HRESULT unsupported_request(HRESULT hr) {
    const HRESULT status=ddi_device_status(hr);
    return (status==E_OUTOFMEMORY || status==D3DDDIERR_DEVICEREMOVED) ? status : DXGI_DDI_ERR_UNSUPPORTED;
}
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATERESOURCE *) { return sizeof(DdiResource); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATERESOURCE *desc,
    D3D10DDI_HRESOURCE handle,D3D10DDI_HRTRESOURCE runtimeHandle) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
    if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !desc || !owner.device()) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::unsupported); return; }
        if (desc->pPrimaryDesc || (desc->BindFlags&D3D10_DDI_BIND_PRESENT) ||
            (desc->MiscFlags&(D3D10_DDI_RESOURCE_MISC_SHARED|D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE))) {
            RuntimeSurfaceRequest request{}; D3D11_TEXTURE2D_DESC1 texture{};
            HRESULT hr=convert_runtime_resource(*desc,reinterpret_cast<HANDLE>(runtimeHandle.handle),request,texture,
                &owner.scanout());
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
                report_ddi_error(owner,unsupported_request(hr),DdiErrorClass::unsupported); return;
            }
            RuntimeSurface *surface=nullptr;
            hr=owner.begin_surface(request,texture,surface);
            if (SUCCEEDED(hr)) hr=owner.wait_surface(*surface);
            if (hr!=S_OK) {
                if (surface) owner.release_surface_handle(*surface);
                report_ddi_error(owner,FAILED(hr) ? hr : D3DDDIERR_DEVICEREMOVED,DdiErrorClass::unsupported); return;
            }
            s->runtime_surface=surface; s->object=surface->texture.texture;
            s->dimension=D3D10DDIRESOURCE_TEXTURE2D;
            s->present_allocation=surface->allocation.allocation; s->present_subresource=0;
            s->blt_model_buffer=(desc->BindFlags&D3D10_DDI_BIND_PRESENT) && !request.primary && !request.shared &&
                !request.displayable;
            if (desc->pInitialDataUP) {
                const auto &initial=desc->pInitialDataUP[0];
                owner.context()->UpdateSubresource(s->object,0,nullptr,initial.pSysMem,initial.SysMemPitch,initial.SysMemSlicePitch);
            }
            return;
        }
        ResourceDescription d;
        HRESULT hr=convert_resource(*desc,d);
        if (FAILED(hr)) { report_ddi_error(owner,unsupported_request(hr),DdiErrorClass::unsupported); return; }
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
            // The engine refuses a format or a flag combination it cannot build with E_INVALIDARG.
            report_ddi_error(owner,unsupported_request(hr),DdiErrorClass::unsupported);
        } else if (!s->object) report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::unsupported);
        else s->dimension=d.dimension;
    },DdiErrorClass::unsupported);
}
SIZE_T APIENTRY opened_size(D3D10DDI_HDEVICE,const D3D10DDIARG_OPENRESOURCE *) { return sizeof(DdiResource); }
void APIENTRY open(D3D10DDI_HDEVICE h,const D3D10DDIARG_OPENRESOURCE *args,
    D3D10DDI_HRESOURCE handle,D3D10DDI_HRTRESOURCE runtimeHandle) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate); if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !args || !runtimeHandle.handle) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return; }
        BC250_WDDM_ALLOCATION_PRIVATE metadata{}; D3D11_TEXTURE2D_DESC1 desc{};
        HRESULT hr=decode_open_resource(*args,metadata,desc);
        if (FAILED(hr)) { report_ddi_error(owner,hr,DdiErrorClass::out_of_memory); return; }
        RuntimeSurfaceAllocation allocation{reinterpret_cast<HANDLE>(runtimeHandle.handle),args->pOpenAllocationInfo2[0].hAllocation,args->hKMResource.handle};
        RuntimeSurface *surface=nullptr;
        hr=owner.adopt_surface(allocation,metadata,desc,surface);
        if (SUCCEEDED(hr)) hr=owner.wait_surface(*surface);
        if (hr!=S_OK) {
            if (surface) owner.release_surface_handle(*surface);
            report_ddi_error(owner,FAILED(hr) ? hr : D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return;
        }
        s->runtime_surface=surface; s->object=surface->texture.texture;
        s->dimension=D3D10DDIRESOURCE_TEXTURE2D; s->present_allocation=surface->allocation.allocation;
    },DdiErrorClass::out_of_memory);
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
