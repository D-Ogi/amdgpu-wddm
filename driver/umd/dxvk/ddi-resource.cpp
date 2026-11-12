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
// and the last line of the budget says that it ran out. mode is the primary descriptor's ModeDesc (0x0
// without a descriptor).
void scanout_primary_note(const ScanoutPrimaryDecision &decision,const ScanoutSource *source,bool displayable,
    UINT vidpn_source,unsigned dxgi,unsigned width,unsigned height,unsigned pitch,unsigned mode_width,
    unsigned mode_height) noexcept {
    const unsigned long force_cpu=source ? source->force_cpu : 0;
    // The compositor's record as this decision read it; "not-read" when an earlier clause decided first.
    const char *desktop=decision.desktop_read ?
        bc250_desktop_route_text(decision.desktop_status,decision.desktop.route) : "not-read";
    const unsigned long long words[]={unsigned(decision.reason),unsigned(decision.switch_state),width,height,pitch,
        dxgi,decision.caps.flags,decision.caps.post_width,decision.caps.post_height,force_cpu,displayable ? 1u : 0u,
        vidpn_source,decision.desktop.pid,mode_width,mode_height};
    unsigned long long key=1469598103934665603ull;                    // FNV-1a over the fields the line prints
    for (const unsigned long long word:words)
        for (unsigned i=0;i<8;++i) key=(key^((word>>(i*8))&0xFFu))*1099511628211ull;
    for (const char *c=desktop;*c;++c) key=(key^static_cast<unsigned char>(*c))*1099511628211ull;
    static std::atomic<unsigned long long> last{0};
    static std::atomic_int budget{64};
    if (last.exchange(key,std::memory_order_relaxed)==key) return;
    const int left=budget.fetch_sub(1,std::memory_order_relaxed);
    if (left<=0) return;
    char text[320];
    std::snprintf(text,sizeof(text),
        "M15.14 d3d11 scanout %s switch=%s chain=%ux%u pitch=%u format=%u displayable=%u vidpn=%u caps=%08X "
        "source=%ux%u mode=%ux%u forcecpu=%lu desktop=%s desktop_pid=%u%s\n",
        scanout_primary_reason_text(decision.reason),scanout_primary_switch_text(decision.switch_state),width,
        height,pitch,dxgi,displayable ? 1u : 0u,vidpn_source,decision.caps.flags,decision.caps.post_width,
        decision.caps.post_height,mode_width,mode_height,force_cpu,desktop,decision.desktop.pid,
        left==1 ? " budget-spent" : "");
    OutputDebugStringA(text);
}
// UMD logging of a resource whose memory the engine owns (BD-373, engine ABI 1.8): the engine keeps its memory and
// writes its events (engine-memory-log.h), with the hResource as the cookie, as the shell's own table has it. Cookie
// 0 ends tracking, before the last release. Logging is optional: a failure leaves the resource complete.
void track_engine_memory(DeviceOwner &owner,const DdiResource &s,bool track) noexcept {
#if BC250_DXVK_ENGINE_ABI_MINOR>=8
    if (s.object) (void)owner.memory_log().track(s.object,track ? umd_etw_resource(s) : 0);
#else
    (void)owner; (void)s; (void)track;
#endif
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
// BD-373 step 3: the LINEAR SURFACE shape (convert_runtime_resource): a single-mip, single-layer, single-sample 2D
// texture of DEFAULT usage without CPU access, in a composed format or NV12. A shared resource of another shape takes
// the ENGINE LAYOUT form. Primaries, present buffers and displayable surfaces always take the LINEAR SURFACE path,
// which refuses what it cannot make: DWM and the display path read them.
bool engine_layout_resource(const D3D11DDIARG_CREATERESOURCE &s) {
    if (s.pPrimaryDesc || (s.BindFlags&D3D10_DDI_BIND_PRESENT) ||
        (s.MiscFlags&D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE) || !(s.MiscFlags&D3D10_DDI_RESOURCE_MISC_SHARED))
        return false;
    const bool linear=s.ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D && s.MipLevels==1 && s.ArraySize==1 &&
        s.SampleDesc.Count==1 && !s.SampleDesc.Quality && s.Usage==D3D10_DDI_USAGE_DEFAULT && !s.MapFlags &&
        (s.Format==DXGI_FORMAT_NV12 || runtime_surface_format(s.Format)!=nullptr);
    return !linear;
}
HRESULT convert_engine_resource(const D3D11DDIARG_CREATERESOURCE &input,EngineResourceDesc &out,
    ResourceDescription &converted,const char **why) {
    const char *ignored=nullptr; if (!why) why=&ignored;
    *why=BC250_SR_WHY_DIMENSION;
    const bool buffer=input.ResourceDimension==D3D10DDIRESOURCE_BUFFER || input.ResourceDimension==D3D11DDIRESOURCE_BUFFEREX;
    // D3D11 shares buffers (the runtime passes them) and Texture2D; 1D, 3D and cube textures are not shareable.
    if (!buffer && input.ResourceDimension!=D3D10DDIRESOURCE_TEXTURE2D) return E_NOTIMPL;
    // The sharing flags are the runtime allocation's, not the engine's. RESTRICT_SHARED_RESOURCE_DRIVER limits the
    // openers to this driver, which is already the only reader of this form. RESTRICTED_CONTENT stays and is refused
    // by convert_resource: this driver has no protected content.
    auto ordinary=input;
    ordinary.MiscFlags&=~UINT(D3D10_DDI_RESOURCE_MISC_SHARED|D3D11_1DDI_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE_DRIVER);
    HRESULT hr=convert_resource(ordinary,converted);
    if (FAILED(hr)) { *why=BC250_SR_WHY_DESCRIPTION; return hr; }
    EngineResourceDesc d{};
    if (buffer) {
        d.buffer=true; d.buffer_desc=converted.buffer;
    } else {
        const auto &t=converted.texture2d;
        d.texture={t.Width,t.Height,t.MipLevels,t.ArraySize,t.Format,t.SampleDesc,t.Usage,t.BindFlags,t.CPUAccessFlags,
            t.MiscFlags,D3D11_TEXTURE_LAYOUT_UNDEFINED};
    }
    BC250_SHARED_RESOURCE record{};
    *why=engine_resource_desc_why(d,record);
    if (!Bc250SharedResourceWhyOk(*why)) return E_NOTIMPL;
    out=d; return S_OK;
}
bool engine_layout_open(const D3D10DDIARG_OPENRESOURCE &args) {
    UINT magic=0;
    if (!args.pPrivateDriverData || args.PrivateDriverDataSize<sizeof(magic)) return false;
    std::memcpy(&magic,args.pPrivateDriverData,sizeof(magic));
    return magic==BC250_SHARED_RESOURCE_MAGIC;
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
    // (BGRA8, and RGBA8 and RGB10A2 from 0.7.216.20 when its trailer carries PLANE_FORMATS), and direct
    // scan-out of RGBA16F stays unsupported. The Ascent's UE 4.26
    // borderless swap chain is RGB10A2 with a primary descriptor; refusing it removed the device at
    // startup (339-342).
    const auto *row=runtime_surface_format(d.Format);
    const bool planar=d.Format==DXGI_FORMAT_NV12;
    if ((!row && !planar) || (input.pPrimaryDesc && row && row->dxgi==AMDGPU_WDDM_DXGI_A8_UNORM)) return E_NOTIMPL;
    if (planar && ((!d.Width || !d.Height) || ((d.Width|d.Height)&1u) || d.Width>16383u || d.Height>16383u)) return E_INVALIDARG;
    RuntimeSurfaceRequest r{};
    r.runtime_resource=runtimeHandle; r.primary=input.pPrimaryDesc!=nullptr;
    r.displayable=(input.MiscFlags&D3DWDDM2_0DDI_RESOURCE_MISC_DISPLAYABLE_SURFACE)!=0;
    r.shared=(input.MiscFlags&D3D10_DDI_RESOURCE_MISC_SHARED)!=0;
    if (input.pPrimaryDesc) r.vidpn_source=input.pPrimaryDesc->VidPnSourceId;
    const UINT pitch=planar ? 0u : runtime_surface_pitch(d.Width,row->bytes_per_pixel);
    const UINT64 bytes=(UINT64(pitch)*((d.Height+3)&~3u)+4095)&~UINT64(4095);
    r.surface={BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,planar ? BC250_WDDM_NV12_VERSION : 1ul,d.Width,d.Height,pitch,
        planar ? BC250_NV12_D3DDDI_FORMAT : row->d3dddi,bytes};
    // NV12 is a pending description, not an allocation. begin_surface queries
    // both planes of its retained image before filling the exact LB7A v2 record.
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
    // C71: the mode the descriptor names, which the runtime sets after this create (scanout-primary.h).
    if (!planar && (r.primary || r.displayable)) {
        const UINT mode_width=input.pPrimaryDesc ? input.pPrimaryDesc->ModeDesc.Width : 0;
        const UINT mode_height=input.pPrimaryDesc ? input.pPrimaryDesc->ModeDesc.Height : 0;
        // BD-223 step R3: a displayable buffer with no descriptor is decided too, while the trailer opens it; an
        // admitted one gets SCANOUT | DISPLAYABLE and keeps Access without PRIMARY.
        const ScanoutPrimaryDecision decision=scanout_primary_decide(scanout,r.primary,r.vidpn_source,
            UINT(d.Format),d.Width,d.Height,pitch,mode_width,mode_height,r.displayable);
        if (decision.admitted) { r.scanout=true; r.texture.Access|=UINT(scanout_primary_record_access(decision)); }
        scanout_primary_note(decision,scanout,r.displayable,r.vidpn_source,UINT(d.Format),d.Width,d.Height,pitch,
                             mode_width,mode_height);
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
// BD-253: a shared surface with no resource record. Its creator gave no E26R blob to the runtime, so the only
// description is the LB7A blob that the kernel driver keeps for the allocation: a standard GDI surface that the
// kernel driver described (GetStandardAllocationDriverData, 48 bytes: LB7A and the GDI1 trailer), or the
// compositor's hosted driver's own surface (32 bytes, LB7A only; driver/umd/router/front-resource.h says the
// desktop's live surfaces are such blobs). Qt's backing store opens one right after it makes its swap chain, and
// a refusal of that open removed every device of Galaxy and of its Notifications Renderer. The description
// that follows from the blob: the storage format of the LB7A format's COMPOSED row, one mip, one array slice,
// no multisampling, DEFAULT usage, shader resource and render target binding, no misc flags. Only a GDI texture
// (D3DKMDT_GDISURFACE_TEXTURE, 1) is a GPU texture; the staging and lookup-table types are refused. The
// geometry rule is the one of every other opened surface (Bc250SharedSurfaceGeometry).
static HRESULT decode_bare_allocation(const D3DDDI_OPENALLOCATIONINFO2 &a,BC250_WDDM_ALLOCATION_PRIVATE &metadata,
    D3D11_TEXTURE2D_DESC1 &desc) {
    constexpr unsigned long gdi_texture=1; // D3DKMDT_GDISURFACE_TEXTURE (d3dkmdt.h)
    BC250_GDI_PRIVATE blob{};
    if (!a.pPrivateDriverData) return E_INVALIDARG;
    if (a.PrivateDriverDataSize==sizeof(BC250_GDI_PRIVATE)) {
        std::memcpy(&blob,a.pPrivateDriverData,sizeof(blob));
        if (blob.Magic!=BC250_GDI_PRIVATE_MAGIC || blob.Type!=gdi_texture || blob.Flags || blob.Reserved)
            return E_INVALIDARG;
    } else if (a.PrivateDriverDataSize==sizeof(BC250_WDDM_ALLOCATION_PRIVATE)) {
        std::memcpy(&blob.Surface,a.pPrivateDriverData,sizeof(blob.Surface));
    } else return E_INVALIDARG;
    const auto &s=blob.Surface;
    if (s.Magic!=BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || s.Version!=1) return E_INVALIDARG;
    const auto *row=amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_d3dddi(unsigned(s.Format)),
        AMDGPU_WDDM_SURFACE_COMPOSED);
    if (!row || !row->dxgi) return E_NOTIMPL;
    if (!s.Width || !s.Height || s.Width>BC250_SHARED_MAX_EDGE || s.Height>BC250_SHARED_MAX_EDGE ||
        !Bc250SharedSurfaceGeometry(&s,row->bytes_per_pixel)) return E_INVALIDARG;
    metadata=s;
    desc=D3D11_TEXTURE2D_DESC1{UINT(s.Width),UINT(s.Height),1,1,DXGI_FORMAT(row->dxgi),{1,0},D3D11_USAGE_DEFAULT,
        D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,0,0,D3D11_TEXTURE_LAYOUT_UNDEFINED};
    return S_OK;
}
HRESULT decode_open_resource(const D3D10DDIARG_OPENRESOURCE &input,BC250_WDDM_ALLOCATION_PRIVATE &metadata,
    D3D11_TEXTURE2D_DESC1 &desc,BC250_WDDM_NV12_PRIVATE *nv12) {
    if (nv12) *nv12={};
    if (input.NumAllocations!=1 || !input.pOpenAllocationInfo2) return E_NOTIMPL;
    const auto &a=input.pOpenAllocationInfo2[0];
    if (!a.hAllocation) return E_INVALIDARG;
    if (a.PrivateDriverDataSize==sizeof(BC250_WDDM_NV12_PRIVATE)) {
        BC250_WDDM_NV12_PRIVATE n{}; BC250_SURFACE_RESOURCE_PRIVATE r{};
        if (!nv12 || !a.pPrivateDriverData || !input.pPrivateDriverData || input.PrivateDriverDataSize!=sizeof(r))
            return E_INVALIDARG;
        std::memcpy(&n,a.pPrivateDriverData,sizeof(n)); std::memcpy(&r,input.pPrivateDriverData,sizeof(r));
        int shared=0,cached=0;
        if (!Bc250Nv12PrivateValid(&n,sizeof(n)) || r.Magic!=BC250_SURFACE_RESOURCE_MAGIC ||
            r.Version!=BC250_SURFACE_RESOURCE_TEXTURE_VERSION || !Bc250SurfaceResourcePolicy(&r,sizeof(r),&shared,&cached) ||
            r.Width!=n.Surface.Width || r.Height!=n.Surface.Height || r.Format!=DXGI_FORMAT_NV12 ||
            r.MipLevels!=1 || r.ArraySize!=1 || r.SampleCount!=1 || r.SampleQuality || r.Usage!=D3D11_USAGE_DEFAULT ||
            r.CpuAccessFlags || r.TextureLayout!=D3D11_TEXTURE_LAYOUT_UNDEFINED ||
            (r.BindFlags&~BC250_SHARED_BIND_MASK) || (r.MiscFlags&~BC250_SHARED_MISC_MASK) ||
            (r.Access&(BC250_SURFACE_RESOURCE_CPU_READ|BC250_SURFACE_RESOURCE_SCANOUT))) return E_INVALIDARG;
        metadata=n.Surface; *nv12=n;
        desc={r.Width,r.Height,1,1,DXGI_FORMAT_NV12,{1,0},D3D11_USAGE_DEFAULT,r.BindFlags,0,r.MiscFlags,D3D11_TEXTURE_LAYOUT_UNDEFINED};
        return S_OK;
    }
    if (!input.PrivateDriverDataSize) return decode_bare_allocation(a,metadata,desc);
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
// OpenResource may report only E_OUTOFMEMORY or D3DDDIERR_DEVICEREMOVED; the runtime takes every other status as
// critical (d3d10umddi.md, PFND3D10DDI_OPENRESOURCE remarks). A surface that this driver cannot describe or adopt is
// therefore E_OUTOFMEMORY: the runtime fails that one OpenSharedResource and keeps the device. Before BD-253 such a
// refusal reported E_INVALIDARG, which the error class made device removal, and the opener lost its device. Only a
// status that reports a lost device stays device removal.
HRESULT unopenable_resource(HRESULT hr) {
    return ddi_device_status(hr)==D3DDDIERR_DEVICEREMOVED ? D3DDDIERR_DEVICEREMOVED : E_OUTOFMEMORY;
}
}
// Every CreateResource that this shell does not complete reports through this one rule (ddi_create_resource_status):
// DXGI_DDI_ERR_UNSUPPORTED for a primary, which the runtime then makes in another way (the Ascent's RGB10A2 primary,
// 339-342), and E_OUTOFMEMORY for every other resource. Neither status ends the device. The failed step does not
// matter: descriptor conversion, the engine create and the runtime surface all report through this rule.
HRESULT create_refusal(const D3D11DDIARG_CREATERESOURCE &desc,HRESULT hr) {
    return ddi_create_resource_status(hr,desc.pPrimaryDesc && (desc.BindFlags&D3D10_DDI_BIND_PRESENT));
}
namespace {
// BD-168: one line for each OpenResource that this shell refuses, in both flavours, at most 16 for each process. Every
// refusal reaches the runtime as E_OUTOFMEMORY (unopenable_resource), the status that a real allocation failure also
// has, so only this line tells them apart: the step (decode, adopt, wait), the status of that step, the status that
// the runtime gets, and the allocation the open named with its LB7A description (magic and version, size, pitch,
// D3DDDIFORMAT, bytes) and the GDI1 type when the blob has the trailer.
void open_refused_note(const D3D10DDIARG_OPENRESOURCE &args,HANDLE runtime,const char *step,HRESULT hr) noexcept {
    static std::atomic_uint notes{16};
    const D3DDDI_OPENALLOCATIONINFO2 none{};
    const auto &info=args.NumAllocations && args.pOpenAllocationInfo2 ? args.pOpenAllocationInfo2[0] : none;
    unsigned long w[10]{};
    if (info.pPrivateDriverData)
        std::memcpy(w,info.pPrivateDriverData,std::min<size_t>(info.PrivateDriverDataSize,sizeof(w)));
    device_life_note(notes,"BC250 BD-168: OpenResource refused: step=%s hr=%08X reported=%08X runtime=%p allocations=%u "
        "allocation=%u km-resource=%u private=%u allocation-private=%u lb7a=%08lX/%lu %lux%lu pitch=%lu d3dddi=%lu "
        "bytes=%llu gdi=%08lX/%lu thread=%lu\n",step,unsigned(hr),unsigned(unopenable_resource(hr)),runtime,
        args.NumAllocations,unsigned(info.hAllocation),unsigned(args.hKMResource.handle),args.PrivateDriverDataSize,
        info.PrivateDriverDataSize,w[0],w[1],w[2],w[3],w[4],w[5],(unsigned long long)w[7]<<32|w[6],w[8],w[9],
        (unsigned long)GetCurrentThreadId());
}
// BD-168: one line for each runtime surface (a swap-chain buffer, a shared or displayable texture) whose creation
// failed after its descriptor was admitted, in both flavours, at most 16 for each process: the step, the status, the
// surface that was asked for, and the allocation when the kernel had made one.
void surface_failed_note(const char *step,HRESULT hr,const RuntimeSurfaceRequest &request,
    const RuntimeSurface *surface,HANDLE runtime) noexcept {
    static std::atomic_uint notes{16};
    device_life_note(notes,"BC250 BD-168: runtime surface not created: step=%s hr=%08X runtime=%p primary=%u shared=%u "
        "displayable=%u scanout=%u d3dddi=%u %ux%u pitch=%u bytes=%llu allocation=%u thread=%lu\n",step,unsigned(hr),
        runtime,unsigned(request.primary),unsigned(request.shared),unsigned(request.displayable),unsigned(request.scanout),
        unsigned(request.surface.Format),unsigned(request.surface.Width),unsigned(request.surface.Height),
        unsigned(request.surface.Pitch),(unsigned long long)request.surface.Size,
        surface ? unsigned(surface->allocation.allocation) : 0u,(unsigned long)GetCurrentThreadId());
}
// BD-373 step 3: one line for each shared resource of the ENGINE LAYOUT form that this shell refuses to create or to
// open, in both flavours, at most 32 for each process. The status the runtime gets (reported) is the only one the DDI
// page allows for a refusal that keeps the device, so this line is what tells a refused description, a refused record
// and a real allocation failure apart: the step, the reason (a BC250_SR_WHY_* string of
// driver/contract/bc250_shared_resource.h or a reason of runtime-engine-resource.cpp) and the description.
void engine_layout_note(const char *entry,const char *step,const char *why,HRESULT hr,HRESULT reported,UINT dimension,
    UINT format,UINT width,UINT height,UINT mips,UINT array,UINT bind,UINT misc,UINT usage) noexcept {
    static std::atomic_uint notes{32};
    device_life_note(notes,"BC250 BD-373: shared resource %s refused: step=%s why=%s hr=%08X reported=%08X dimension=%u "
        "format=%u %ux%u mips=%u array=%u bind=%08X misc=%08X usage=%u thread=%lu\n",entry,step,why ? why : "-",
        unsigned(hr),unsigned(reported),dimension,format,width,height,mips,array,bind,misc,usage,
        (unsigned long)GetCurrentThreadId());
}
void create_engine_layout(DeviceOwner &owner,const D3D11DDIARG_CREATERESOURCE &desc,HANDLE runtime,DdiResource &s) {
    const auto *m=desc.pMipInfoList;
    auto refuse=[&](const char *step,const char *why,HRESULT hr) {
        const HRESULT reported=FAILED(hr) ? create_refusal(desc,hr) : D3DDDIERR_DEVICEREMOVED;
        engine_layout_note("create",step,why,hr,reported,unsigned(desc.ResourceDimension),unsigned(desc.Format),
            m ? m->TexelWidth : 0u,m ? m->TexelHeight : 0u,desc.MipLevels,desc.ArraySize,desc.BindFlags,desc.MiscFlags,
            unsigned(desc.Usage));
        report_ddi_error(owner,reported,DdiErrorClass::unsupported);
    };
    EngineResourceDesc engineDesc{}; ResourceDescription converted; const char *why=nullptr;
    HRESULT hr=convert_engine_resource(desc,engineDesc,converted,&why);
    if (FAILED(hr)) { refuse("convert",why,hr); return; }
    if (!runtime) { refuse("convert","runtime handle",E_INVALIDARG); return; }
    RuntimeSurface *surface=nullptr;
    const char *step="begin";
    hr=owner.begin_engine_surface(runtime,engineDesc,surface,&why);
    if (SUCCEEDED(hr)) { step="wait"; why=BC250_SR_WHY_OK; hr=owner.wait_surface(*surface); }
    if (hr!=S_OK) {
        if (surface) owner.release_surface_handle(*surface);
        refuse(step,why,hr); return;
    }
    s.runtime_surface=surface; s.object=surface->resource(); s.dimension=desc.ResourceDimension;
    // The initial content of a DEFAULT resource, one subresource after another in the runtime's order (mip first).
    for (UINT i=0;i<UINT(converted.initial.size());++i) {
        const auto &initial=converted.initial[i];
        owner.context()->UpdateSubresource(s.object,i,nullptr,initial.pSysMem,initial.SysMemPitch,initial.SysMemSlicePitch);
    }
    if (auto *log=etw::process_allocation_log()) (void)log->map(runtime_surface_mapping(s));
}
void open_engine_layout(DeviceOwner &owner,const D3D10DDIARG_OPENRESOURCE &args,HANDLE runtime,DdiResource &s) {
    BC250_SHARED_RESOURCE record{};
    const char *why=Bc250SharedResourceDecodeWhy(args.pPrivateDriverData,args.PrivateDriverDataSize,&record);
    auto refuse=[&](const char *step,HRESULT hr) {
        const HRESULT reported=FAILED(hr) ? unopenable_resource(hr) : D3DDDIERR_DEVICEREMOVED;
        engine_layout_note("open",step,why,hr,reported,record.Dimension,record.Format,record.Width,record.Height,
            record.MipLevels,record.ArraySize,record.BindFlags,record.MiscFlags,record.Usage);
        report_ddi_error(owner,reported,DdiErrorClass::out_of_memory);
    };
    if (!Bc250SharedResourceWhyOk(why)) { refuse("decode",E_INVALIDARG); return; }
    if (args.NumAllocations!=1 || !args.pOpenAllocationInfo2 || !args.pOpenAllocationInfo2[0].hAllocation) {
        why="allocations"; refuse("decode",E_INVALIDARG); return;
    }
    const auto &a=args.pOpenAllocationInfo2[0];
    RuntimeSurfaceAllocation allocation{runtime,a.hAllocation,args.hKMResource.handle};
    RuntimeSurface *surface=nullptr;
    const char *step="adopt";
    HRESULT hr=owner.adopt_engine_surface(allocation,record,a.pPrivateDriverData,a.PrivateDriverDataSize,surface,&why);
    if (SUCCEEDED(hr)) { step="wait"; why=BC250_SR_WHY_OK; hr=owner.wait_surface(*surface); }
    if (hr!=S_OK) {
        if (surface) owner.release_surface_handle(*surface);
        refuse(step,hr); return;
    }
    s.runtime_surface=surface; s.object=surface->resource();
    s.dimension=record.Dimension==BC250_SHARED_RESOURCE_BUFFER ? D3D11DDIRESOURCE_BUFFEREX : D3D10DDIRESOURCE_TEXTURE2D;
    if (auto *log=etw::process_allocation_log()) (void)log->map(runtime_surface_mapping(s));
}
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATERESOURCE *) { return sizeof(DdiResource); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATERESOURCE *desc,
    D3D10DDI_HRESOURCE handle,D3D10DDI_HRTRESOURCE runtimeHandle) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
    if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !desc || !owner.device()) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::unsupported); return; }
        if (engine_layout_resource(*desc)) {
            create_engine_layout(owner,*desc,reinterpret_cast<HANDLE>(runtimeHandle.handle),*s); return;
        }
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
                    amdgpu_wddm_log::print("%s",text);   // BD-253: also in the game log
                }
                report_ddi_error(owner,create_refusal(*desc,hr),DdiErrorClass::unsupported); return;
            }
            RuntimeSurface *surface=nullptr;
            const char *failedStep="begin";
            hr=owner.begin_surface(request,texture,surface);
            if (SUCCEEDED(hr)) { failedStep="wait"; hr=owner.wait_surface(*surface); }
            if (hr!=S_OK) {
                surface_failed_note(failedStep,hr,request,surface,owner.runtime().hDevice);
                if (surface) owner.release_surface_handle(*surface);
                report_ddi_error(owner,FAILED(hr) ? create_refusal(*desc,hr) : D3DDDIERR_DEVICEREMOVED,DdiErrorClass::unsupported); return;
            }
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
            // BD-253, lab flavour: each runtime surface that this device creates, at most 32 lines per process.
            // A shared texture of another device of the process (Qt and Chromium) starts here.
            static std::atomic_uint created{32};
            device_life_note(created,"BC250 BD-253: runtime surface created: runtime=%p primary=%u shared=%u "
                "displayable=%u format=%u %ux%u bind=%08X misc=%08X allocation=%u thread=%lu\n",
                owner.runtime().hDevice,unsigned(request.primary),unsigned(request.shared),unsigned(request.displayable),
                unsigned(texture.Format),texture.Width,texture.Height,desc->BindFlags,desc->MiscFlags,
                unsigned(surface->allocation.allocation),(unsigned long)GetCurrentThreadId());
#endif
            s->runtime_surface=surface; s->object=surface->texture.texture;
            s->dimension=D3D10DDIRESOURCE_TEXTURE2D;
            s->present_allocation=surface->allocation.allocation; s->present_subresource=0;
            s->blt_model_buffer=(desc->BindFlags&D3D10_DDI_BIND_PRESENT) && !request.primary && !request.shared &&
                !request.displayable;
            if (desc->pInitialDataUP) {
                const auto &initial=desc->pInitialDataUP[0];
                owner.context()->UpdateSubresource(s->object,0,nullptr,initial.pSysMem,initial.SysMemPitch,initial.SysMemSlicePitch);
            }
            // UMD logging (umd-etw.h): the resource holds its whole allocation. A table without memory for the entry
            // writes no event, and the resource is still complete: the mapping is only missing from the trace.
            if (auto *log=etw::process_allocation_log()) (void)log->map(runtime_surface_mapping(*s));
            return;
        }
        ResourceDescription d;
        HRESULT hr=convert_resource(*desc,d);
        if (FAILED(hr)) { report_ddi_error(owner,create_refusal(*desc,hr),DdiErrorClass::unsupported); return; }
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
            // The engine refuses a format or a flag combination it cannot build with E_INVALIDARG (BD-330).
            report_ddi_error(owner,create_refusal(*desc,hr),DdiErrorClass::unsupported);
        } else if (!s->object) report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::unsupported);
        else { s->dimension=d.dimension; track_engine_memory(owner,*s,true); }
    },DdiErrorClass::unsupported);
}
SIZE_T APIENTRY opened_size(D3D10DDI_HDEVICE,const D3D10DDIARG_OPENRESOURCE *) { return sizeof(DdiResource); }
void APIENTRY open(D3D10DDI_HDEVICE h,const D3D10DDIARG_OPENRESOURCE *args,
    D3D10DDI_HRESOURCE handle,D3D10DDI_HRTRESOURCE runtimeHandle) {
    auto *s=static_cast<DdiResource *>(handle.pDrvPrivate); if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !args || !runtimeHandle.handle) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return; }
        // BD-373 step 3: the record names the form. An S26R record is the ENGINE LAYOUT form; every other record, and
        // none, is the LINEAR SURFACE decode below, unchanged.
        if (engine_layout_open(*args)) {
            open_engine_layout(owner,*args,reinterpret_cast<HANDLE>(runtimeHandle.handle),*s); return;
        }
        BC250_WDDM_ALLOCATION_PRIVATE metadata{}; BC250_WDDM_NV12_PRIVATE nv12{}; D3D11_TEXTURE2D_DESC1 desc{};
        HRESULT hr=decode_open_resource(*args,metadata,desc,&nv12);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
        // BD-253, lab flavour: each OpenResource of this device that succeeds, at most 32 lines per process for opens
        // with a resource record and 16 for opens without one (window redirection surfaces; Chromium's texture pool
        // used all 32 lines in arm 5 before the main window's surfaces were opened): its status,
        // and what the opener asked for (Qt opens Chromium's shared texture here). The allocation blob is named by
        // its size and its first two words (the LB7A magic and version), and by the GDI1 type when it has the
        // trailer. The allocation handle is the one the Present lines name.
        auto opened=[&](const char *step,HRESULT status) {
            static std::atomic_uint recorded{32},bare{16};
            auto &notes=args->PrivateDriverDataSize ? recorded : bare;
            const auto &info=args->pOpenAllocationInfo2 ? args->pOpenAllocationInfo2[0] : D3DDDI_OPENALLOCATIONINFO2{};
            unsigned long words[12]{};
            if (info.pPrivateDriverData)
                std::memcpy(words,info.pPrivateDriverData,std::min<size_t>(info.PrivateDriverDataSize,sizeof(words)));
            device_life_note(notes,"BC250 BD-253: OpenResource %s hr=%08X runtime=%p allocations=%u private=%u "
                "allocation-private=%u %08lX/%lu d3dddi=%lu gdi=%08lX/%lu km-resource=%u format=%u %ux%u bind=%08X "
                "misc=%08X allocation=%u thread=%lu\n",step,unsigned(status),owner.runtime().hDevice,args->NumAllocations,
                args->PrivateDriverDataSize,info.PrivateDriverDataSize,words[0],words[1],words[5],words[8],words[9],
                unsigned(args->hKMResource.handle),unsigned(desc.Format),desc.Width,desc.Height,desc.BindFlags,
                desc.MiscFlags,unsigned(info.hAllocation),(unsigned long)GetCurrentThreadId());
        };
#endif
        // BD-168: a refused open has its own line in both flavours (open_refused_note); the lab line above is for the
        // opens that succeed.
        if (FAILED(hr)) {
            open_refused_note(*args,owner.runtime().hDevice,"decode",hr);
            report_ddi_error(owner,unopenable_resource(hr),DdiErrorClass::out_of_memory); return;
        }
        RuntimeSurfaceAllocation allocation{reinterpret_cast<HANDLE>(runtimeHandle.handle),args->pOpenAllocationInfo2[0].hAllocation,args->hKMResource.handle};
        RuntimeSurface *surface=nullptr;
        hr=owner.adopt_surface(allocation,metadata,desc,surface,nv12.Surface.Version ? &nv12 : nullptr);
        if (FAILED(hr)) open_refused_note(*args,owner.runtime().hDevice,"adopt",hr);
        if (SUCCEEDED(hr)) {
            hr=owner.wait_surface(*surface);
            if (hr!=S_OK) open_refused_note(*args,owner.runtime().hDevice,"wait",FAILED(hr) ? hr : D3DDDIERR_DEVICEREMOVED);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
            else opened("done",hr);
#endif
        }
        if (hr!=S_OK) {
            if (surface) owner.release_surface_handle(*surface);
            report_ddi_error(owner,FAILED(hr) ? unopenable_resource(hr) : D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return;
        }
        s->runtime_surface=surface; s->object=surface->texture.texture;
        s->dimension=D3D10DDIRESOURCE_TEXTURE2D; s->present_allocation=surface->allocation.allocation;
        // UMD logging: the opened resource holds the whole allocation it opened, as a created one does.
        if (auto *log=etw::process_allocation_log()) (void)log->map(runtime_surface_mapping(*s));
    },DdiErrorClass::out_of_memory);
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
        if (s && s->runtime_surface) {
            auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
            // UMD logging: the resource stops using its allocation now, also when the allocation itself lives on
            // until its queued work retires (nf-umdprovider-umdetwlogunmapallocation.md 48).
            if (auto *log=etw::process_allocation_log()) log->unmap_resource(umd_etw_resource(*s));
            HRESULT hr=owner.release_surface_handle(*s->runtime_surface);
            // Runtime private storage may disappear now. DeviceOwner retains any
            // unfinished cleanup independently of this DDI handle.
            *s={};
            if (ddi_device_status(hr)==D3DDDIERR_DEVICEREMOVED) report_ddi_error(owner,hr);
        } else if (s && s->object) {
            track_engine_memory(*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner,*s,false);
            s->object->Release(); *s={};
        }
    });
}
}
void install_resource_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateResourceSize=size; t.pfnCreateResource=create; t.pfnDestroyResource=destroy;
    t.pfnCalcPrivateOpenedResourceSize=opened_size; t.pfnOpenResource=open;
}
}
