// SPDX-License-Identifier: MIT
#include "ddi-wddm2.h"
#include "ddi-table.h"
#include "ddi-dxgi-table.h"
#include "ddi-srv.h"
#include "ddi-rtv.h"
#include "ddi-dsv.h"
#include "ddi-uav.h"
#include "ddi-query.h"
#include "ddi-fixed-state.h"
#include <cstddef>
#include <cstring>
#include <type_traits>
namespace bc250::umd {
namespace {
// The WDDM 2.0 table is the D3D11.1 table plus the WDDM 1.3 and 2.0 appendices.
static_assert(offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnUpdateTileMappings)==sizeof(D3D11_1DDI_DEVICEFUNCS));
static_assert(sizeof(D3DWDDM2_0DDI_DEVICEFUNCS)==sizeof(D3D11_1DDI_DEVICEFUNCS)+13*sizeof(void *));
#define SAME_OFFSET(field) static_assert(offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,field)==offsetof(D3D11_1DDI_DEVICEFUNCS,field))
SAME_OFFSET(pfnDefaultConstantBufferUpdateSubresourceUP); SAME_OFFSET(pfnFlush); SAME_OFFSET(pfnRelocateDeviceFuncs);
SAME_OFFSET(pfnCreateShaderResourceView); SAME_OFFSET(pfnCreateRenderTargetView); SAME_OFFSET(pfnCreateRasterizerState);
SAME_OFFSET(pfnCreateQuery); SAME_OFFSET(pfnCreateUnorderedAccessView); SAME_OFFSET(pfnCheckMultisampleQualityLevels);
SAME_OFFSET(pfnCheckDirectFlipSupport); SAME_OFFSET(pfnClearView);
#undef SAME_OFFSET
// The tiled DDI structures are the D3D11 API structures, field for field.
static_assert(sizeof(D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE)==sizeof(D3D11_TILED_RESOURCE_COORDINATE) &&
    offsetof(D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE,Subresource)==offsetof(D3D11_TILED_RESOURCE_COORDINATE,Subresource));
static_assert(sizeof(D3DWDDM1_3DDI_TILE_REGION_SIZE)==sizeof(D3D11_TILE_REGION_SIZE) &&
    offsetof(D3DWDDM1_3DDI_TILE_REGION_SIZE,bUseBox)==offsetof(D3D11_TILE_REGION_SIZE,bUseBox) &&
    offsetof(D3DWDDM1_3DDI_TILE_REGION_SIZE,Width)==offsetof(D3D11_TILE_REGION_SIZE,Width) &&
    offsetof(D3DWDDM1_3DDI_TILE_REGION_SIZE,Height)==offsetof(D3D11_TILE_REGION_SIZE,Height) &&
    offsetof(D3DWDDM1_3DDI_TILE_REGION_SIZE,Depth)==offsetof(D3D11_TILE_REGION_SIZE,Depth));
static_assert(UINT(D3DWDDM1_3DDI_TILE_MAPPING_NO_OVERWRITE)==UINT(D3D11_TILE_MAPPING_NO_OVERWRITE));
static_assert(UINT(D3DWDDM_1_3DDI_TILE_RANGE_NULL)==UINT(D3D11_TILE_RANGE_NULL) &&
    UINT(D3DWDDM_1_3DDI_TILE_RANGE_SKIP)==UINT(D3D11_TILE_RANGE_SKIP) &&
    UINT(D3DWDDM_1_3DDI_TILE_RANGE_REUSE_SINGLE_TILE)==UINT(D3D11_TILE_RANGE_REUSE_SINGLE_TILE));
static_assert(UINT(D3DWDDM1_3DDI_TILE_COPY_NO_OVERWRITE)==UINT(D3D11_TILE_COPY_NO_OVERWRITE) &&
    UINT(D3DWDDM1_3DDI_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE)==UINT(D3D11_TILE_COPY_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) &&
    UINT(D3DWDDM1_3DDI_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER)==UINT(D3D11_TILE_COPY_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER));
static_assert(UINT(D3DWDDM1_3DDI_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE)==
    UINT(D3D11_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE));
// The UM callbacks of WDDM 2.0 extend the D3D10 core-layer ones; the shell reads only SetErrorCb.
static_assert(offsetof(D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS,pfnSetErrorCb)==offsetof(D3D10DDI_CORELAYER_DEVICECALLBACKS,pfnSetErrorCb) &&
    sizeof(D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS)>=sizeof(D3D10DDI_CORELAYER_DEVICECALLBACKS));

// The D3D11.1 implementation every converting entry forwards to.
const D3D11_1DDI_DEVICEFUNCS &base() { static const auto table=make_render_device_table(); return table; }
DeviceOwner &owner(D3D10DDI_HDEVICE h) { return *static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner; }
// An argument the runtime should never send. The status must be one the entry's page allows, so a
// refusal here costs the device: an argument a conforming application can produce belongs in the
// conversion below, not here (BD-071).
void reject(D3D10DDI_HDEVICE h,DdiErrorClass policy) {
    enter_context(h,[&](ID3D11DeviceContext4 &) { report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED,policy); },policy);
}
ID3D11Resource *resource(D3D10DDI_HRESOURCE h) {
    auto *s=static_cast<DdiResource *>(h.pDrvPrivate); return s ? s->object : nullptr;
}
// A tile pool, or the buffer of CopyTiles: a buffer resource or nothing.
bool buffer(D3D10DDI_HRESOURCE h,ID3D11Buffer *&out) {
    out=nullptr;
    if (!h.pDrvPrivate) return true;
    auto *r=resource(h); if (!r) return false;
    D3D11_RESOURCE_DIMENSION type{}; r->GetType(&type);
    if (type!=D3D11_RESOURCE_DIMENSION_BUFFER) return false;
    out=static_cast<ID3D11Buffer *>(r); return true;
}
template<typename Callback,const char *Name> struct Unexpected;
template<typename R,typename... Args,const char *Name>
struct Unexpected<R (APIENTRY *)(D3D10DDI_HDEVICE,Args...),Name> {
    static R APIENTRY call(D3D10DDI_HDEVICE h,Args...) {
        static std::atomic_bool logged{false};
        if(!logged.exchange(true))failure_diagnostic(Name,E_NOTIMPL);
        if constexpr(std::is_void_v<R>) {
            auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
            if(storage && storage->owner) {
                RuntimeDomain::Scope scope(storage->owner->runtime().domain);
                report_ddi_error(*storage->owner,D3DDDIERR_DEVICEREMOVED);
            }
        }else if constexpr(std::is_same_v<R,HRESULT>)return E_NOTIMPL;
        else return 0;
    }
};
constexpr char kSetHardwareProtection[]="SetHardwareProtection";
constexpr char kGetResourceLayout[]="GetResourceLayout";
constexpr char kRetrieveShaderComment[]="RetrieveShaderComment";
constexpr char kSetHardwareProtectionState[]="SetHardwareProtectionState";

// One queue: every context type flushes and queries the same immediate context.
constexpr UINT known_context_types=D3DWDDM2_0DDI_CONTEXTTYPE_3D|D3DWDDM2_0DDI_CONTEXTTYPE_COMPUTE|
    D3DWDDM2_0DDI_CONTEXTTYPE_COPY|D3DWDDM2_0DDI_CONTEXTTYPE_VIDEO;
BOOL APIENTRY flush(D3D10DDI_HDEVICE h,UINT contextType,UINT flags) {
    if (contextType & ~known_context_types) { reject(h,DdiErrorClass::removed_only); return FALSE; }
    if (!base().pfnFlush(h,flags & ~UINT(D3DWDDM1_3DDI_TRIM_MEMORY))) return FALSE;
    if (!(flags & D3DWDDM1_3DDI_TRIM_MEMORY)) return TRUE;
    // IDXGIDevice3::Trim: engine ABI 1.4 TrimMemory waits for the GPU and frees what it keeps without need.
    BOOL trimmed=FALSE;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *engine=owner(h).engine4();
        const HRESULT hr=engine ? engine->TrimMemory() : E_UNEXPECTED;
        if (FAILED(hr)) report_ddi_error(owner(h),hr); else trimmed=TRUE;
    });
    return trimmed;
}
void APIENTRY relocate(D3D10DDI_HDEVICE h,D3DWDDM2_0DDI_DEVICEFUNCS *destination) {
    auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if (!storage || !storage->owner) return;
    RuntimeDomain::Scope scope(storage->owner->runtime().domain);
    if (destination) *destination=make_wddm2_0_device_table();
}
// Views: the WDDM 2.0 descriptions add a plane slice to Tex2D (d3d10umddi.h 10.0.26100 :5287-5355).
// A planar format has one view per plane, so the plane travels with the argument to the engine, which
// implements plane views. Refusing it here would cost the device: CreateShaderResourceView and its two
// neighbours may report only E_OUTOFMEMORY or D3DDDIERR_DEVICEREMOVED (BD-071).
SIZE_T APIENTRY srv_size(D3D10DDI_HDEVICE,const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *) { return sizeof(DdiShaderResourceView); }
void APIENTRY srv_create(D3D10DDI_HDEVICE h,const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW *s,
    D3D10DDI_HSHADERRESOURCEVIEW view,D3D10DDI_HRTSHADERRESOURCEVIEW rt) {
    if (auto *storage=static_cast<DdiShaderResourceView *>(view.pDrvPrivate)) storage->object=nullptr;
    if (!s) { reject(h,DdiErrorClass::out_of_memory); return; }
    D3D11DDIARG_CREATESHADERRESOURCEVIEW d{}; UINT plane=0;
    convert_wddm2_srv(*s,d,plane);
    create_shader_resource_view(h,&d,plane,view,rt);
}
SIZE_T APIENTRY rtv_size(D3D10DDI_HDEVICE,const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *) { return sizeof(DdiRenderTargetView); }
void APIENTRY rtv_create(D3D10DDI_HDEVICE h,const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW *s,
    D3D10DDI_HRENDERTARGETVIEW view,D3D10DDI_HRTRENDERTARGETVIEW rt) {
    if (auto *storage=static_cast<DdiRenderTargetView *>(view.pDrvPrivate)) storage->object=nullptr;
    if (!s) { reject(h,DdiErrorClass::out_of_memory); return; }
    D3D10DDIARG_CREATERENDERTARGETVIEW d{}; UINT plane=0;
    convert_wddm2_rtv(*s,d,plane);
    create_render_target_view(h,&d,plane,view,rt);
}
SIZE_T APIENTRY uav_size(D3D10DDI_HDEVICE,const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *) { return sizeof(DdiUnorderedAccessView); }
void APIENTRY uav_create(D3D10DDI_HDEVICE h,const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW *s,
    D3D11DDI_HUNORDEREDACCESSVIEW view,D3D11DDI_HRTUNORDEREDACCESSVIEW rt) {
    if (auto *storage=static_cast<DdiUnorderedAccessView *>(view.pDrvPrivate)) storage->object=nullptr;
    if (!s) { reject(h,DdiErrorClass::out_of_memory); return; }
    D3D11DDIARG_CREATEUNORDEREDACCESSVIEW d{}; UINT plane=0;
    convert_wddm2_uav(*s,d,plane);
    create_unordered_access_view(h,&d,plane,view,rt);
}
SIZE_T APIENTRY query_size(D3D10DDI_HDEVICE,const D3DWDDM2_0DDIARG_CREATEQUERY *) { return sizeof(DdiQuery); }
void APIENTRY query_create(D3D10DDI_HDEVICE h,const D3DWDDM2_0DDIARG_CREATEQUERY *s,D3D10DDI_HQUERY q,D3D10DDI_HRTQUERY rt) {
    if (auto *storage=static_cast<DdiQuery *>(q.pDrvPrivate)) *storage=DdiQuery{};
    if (!s || (s->ContextType & ~known_context_types)) { reject(h,DdiErrorClass::non_exclusive); return; }
    D3D10DDIARG_CREATEQUERY d{}; d.Query=s->Query; d.MiscFlags=s->MiscFlags;
    base().pfnCreateQuery(h,&d,q,rt);
}
void APIENTRY samples(D3D10DDI_HDEVICE h,DXGI_FORMAT format,UINT count,UINT flags,UINT *out) {
    if (!flags) { base().pfnCheckMultisampleQualityLevels(h,format,count,out); return; }
    if (out) *out=0;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &o=owner(h);
        if (!out || !o.device() || (flags & ~UINT(D3DWDDM1_3DDI_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE))) {
            report_ddi_error(o,E_INVALIDARG,DdiErrorClass::invalid_arg); return;
        }
        UINT quality=0;
        const HRESULT hr=o.device()->CheckMultisampleQualityLevels1(format,count,flags,&quality);
        if (hr==E_INVALIDARG) return; // unsupported combination: zero levels, as the D3D11.1 entry
        if (FAILED(hr)) { report_ddi_error(o,hr,DdiErrorClass::invalid_arg); return; }
        *out=quality;
    },DdiErrorClass::invalid_arg);
}
// Tiled resources: DXVK's ID3D11DeviceContext2/ID3D11Device2 implementations (sparse binding in the
// hosted ICD). Runtime-validated arguments; refusals from the engine go to SetErrorCb.
void APIENTRY update_tile_mappings(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE tiled,UINT regions,
    const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *coordinates,const D3DWDDM1_3DDI_TILE_REGION_SIZE *sizes,
    D3D10DDI_HRESOURCE pool,UINT ranges,const UINT *rangeFlags,const UINT *poolStarts,const UINT *rangeCounts,UINT flags) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *t=resource(tiled); ID3D11Buffer *p=nullptr;
        if (!t || !buffer(pool,p)) { report_ddi_error(owner(h),E_INVALIDARG,DdiErrorClass::invalid_arg); return; }
        const HRESULT hr=c.UpdateTileMappings(t,regions,reinterpret_cast<const D3D11_TILED_RESOURCE_COORDINATE *>(coordinates),
            reinterpret_cast<const D3D11_TILE_REGION_SIZE *>(sizes),p,ranges,rangeFlags,poolStarts,rangeCounts,flags);
        if (FAILED(hr)) report_ddi_error(owner(h),hr,DdiErrorClass::invalid_arg);
    },DdiErrorClass::invalid_arg);
}
void APIENTRY copy_tile_mappings(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *dstStart,
    D3D10DDI_HRESOURCE src,const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *srcStart,const D3DWDDM1_3DDI_TILE_REGION_SIZE *size,UINT flags) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *d=resource(dst); auto *s=resource(src);
        if (!d || !s || !dstStart || !srcStart || !size) { report_ddi_error(owner(h),E_INVALIDARG,DdiErrorClass::invalid_arg); return; }
        const HRESULT hr=c.CopyTileMappings(d,reinterpret_cast<const D3D11_TILED_RESOURCE_COORDINATE *>(dstStart),s,
            reinterpret_cast<const D3D11_TILED_RESOURCE_COORDINATE *>(srcStart),reinterpret_cast<const D3D11_TILE_REGION_SIZE *>(size),flags);
        if (FAILED(hr)) report_ddi_error(owner(h),hr,DdiErrorClass::invalid_arg);
    },DdiErrorClass::invalid_arg);
}
// CopyTiles and UpdateTiles return nothing: their errors come back as the engine's deferred error.
template<typename Call> void deferred(D3D10DDI_HDEVICE h,Call &&call) {
    HRESULT hr=owner(h).take_deferred_error();
    if (FAILED(hr)) { report_ddi_error(owner(h),hr); return; }
    call();
    hr=owner(h).take_deferred_error();
    if (FAILED(hr)) report_ddi_error(owner(h),hr);
}
void APIENTRY copy_tiles(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE tiled,const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *start,
    const D3DWDDM1_3DDI_TILE_REGION_SIZE *size,D3D10DDI_HRESOURCE buf,UINT64 offset,UINT flags) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *t=resource(tiled); ID3D11Buffer *b=nullptr;
        if (!t || !start || !size || !buffer(buf,b) || !b) { report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED); return; }
        deferred(h,[&] { c.CopyTiles(t,reinterpret_cast<const D3D11_TILED_RESOURCE_COORDINATE *>(start),
            reinterpret_cast<const D3D11_TILE_REGION_SIZE *>(size),b,offset,flags); });
    });
}
void APIENTRY update_tiles(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,const D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE *start,
    const D3DWDDM1_3DDI_TILE_REGION_SIZE *size,const void *data,UINT flags) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *d=resource(dst);
        if (!d || !start || !size || !data) { report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED); return; }
        deferred(h,[&] { c.UpdateTiles(d,reinterpret_cast<const D3D11_TILED_RESOURCE_COORDINATE *>(start),
            reinterpret_cast<const D3D11_TILE_REGION_SIZE *>(size),data,flags); });
    });
}
// The barrier names a resource or view by its driver handle; nothing stands for "all tiled accesses".
bool barrier_object(D3D11DDI_HANDLETYPE type,void *handle,ID3D11DeviceChild *&out) {
    out=nullptr;
    if (!handle) return true;
    switch (type) {
    case D3D10DDI_HT_RESOURCE: out=static_cast<DdiResource *>(handle)->object; break;
    case D3D10DDI_HT_SHADERRESOURCEVIEW: out=static_cast<DdiShaderResourceView *>(handle)->object; break;
    case D3D10DDI_HT_RENDERTARGETVIEW: out=static_cast<DdiRenderTargetView *>(handle)->object; break;
    case D3D10DDI_HT_DEPTHSTENCILVIEW: out=static_cast<DdiDepthStencilView *>(handle)->object; break;
    case D3D11DDI_HT_UNORDEREDACCESSVIEW: out=static_cast<DdiUnorderedAccessView *>(handle)->object; break;
    default: return false;
    }
    return out!=nullptr;
}
void APIENTRY tiled_barrier(D3D10DDI_HDEVICE h,D3D11DDI_HANDLETYPE beforeType,void *before,
    D3D11DDI_HANDLETYPE afterType,void *after) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        ID3D11DeviceChild *b=nullptr,*a=nullptr;
        if (!barrier_object(beforeType,before,b) || !barrier_object(afterType,after,a)) {
            report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED); return;
        }
        c.TiledResourceBarrier(b,a);
    });
}
void APIENTRY mip_packing(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE tiled,UINT *packedMips,UINT *packedTiles) {
    if (packedMips) *packedMips=0;
    if (packedTiles) *packedTiles=0;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &o=owner(h); auto *t=resource(tiled);
        if (!t || !packedMips || !packedTiles || !o.device()) { report_ddi_error(o,E_INVALIDARG,DdiErrorClass::invalid_arg); return; }
        D3D11_PACKED_MIP_DESC packed{};
        o.device()->GetResourceTiling(t,nullptr,&packed,nullptr,nullptr,0,nullptr);
        *packedMips=packed.NumPackedMips; *packedTiles=packed.NumTilesForPackedMips;
    },DdiErrorClass::invalid_arg);
}
void APIENTRY resize_tile_pool(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE pool,UINT64 bytes) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        ID3D11Buffer *p=nullptr;
        if (!buffer(pool,p) || !p) { report_ddi_error(owner(h),E_INVALIDARG,DdiErrorClass::invalid_arg_oom); return; }
        const HRESULT hr=c.ResizeTilePool(p,bytes);
        if (FAILED(hr)) report_ddi_error(owner(h),hr,DdiErrorClass::invalid_arg_oom);
    },DdiErrorClass::invalid_arg_oom);
}
// MARKER caps report no marker type: the runtime has nothing to record.
void APIENTRY set_marker(D3D10DDI_HDEVICE) {}
void APIENTRY set_marker_mode(D3D10DDI_HDEVICE,D3DWDDM1_3DDI_MARKER_TYPE,UINT) {}

// DXGI 1.4
static_assert(offsetof(DXGI1_4_DDI_BASE_FUNCTIONS,pfnReclaimResources)==offsetof(DXGI1_2_DDI_BASE_FUNCTIONS,pfnReclaimResources));
static_assert(offsetof(DXGI1_4_DDI_BASE_FUNCTIONS,pfnGetMultiplaneOverlayCaps)==offsetof(DXGI1_2_DDI_BASE_FUNCTIONS,pfnGetMultiplaneOverlayCaps));
const DXGI1_2_DDI_BASE_FUNCTIONS &dxgi_base() { static const auto table=make_dxgi_device_table(); return table; }
HRESULT APIENTRY present1(DXGI_DDI_ARG_PRESENT1 *a) {
    if (!a || !a->hDevice) return E_INVALIDARG;
    // One surface is the Present of DXGI 1.2 (dirty rectangles are hints). Several surfaces per Present
    // belong to stereo and multi-plane swap chains, which this driver does not create.
    if (a->SurfacesToPresent!=1 || !a->phSurfacesToPresent) return DXGI_ERROR_UNSUPPORTED;
    DXGI_DDI_ARG_PRESENT p{};
    p.hDevice=a->hDevice; p.hSurfaceToPresent=a->phSurfacesToPresent[0].hSurface;
    p.SrcSubResourceIndex=a->phSurfacesToPresent[0].SubResourceIndex;
    p.hDstResource=a->hDstResource; p.DstSubResourceIndex=a->DstSubResourceIndex;
    p.pDXGIContext=a->pDXGIContext; p.Flags=a->Flags; p.FlipInterval=a->FlipInterval;
    return dxgi_base().pfnPresent(&p);
}
HRESULT APIENTRY present_duration(DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT *a) {
    if (!a || !a->hDevice) return E_INVALIDARG;
    // No custom present durations: both neighbours are zero.
    a->ClosestSmallerDuration=0; a->ClosestLargerDuration=0; return S_OK;
}
HRESULT APIENTRY trim_residency(DXGI_DDI_ARG_TRIMRESIDENCYSET *a) {
    if (!a || !a->hDevice) return E_INVALIDARG;
    // Residency belongs to the hosted ICD's allocations and the KMD; the shell keeps no residency set
    // of its own to trim. The request is satisfied by doing nothing.
    return S_OK;
}
HRESULT APIENTRY unsupported_group_caps(DXGI_DDI_ARG_GETMULTIPLANEOVERLAYGROUPCAPS *) { return DXGI_ERROR_UNSUPPORTED; }
HRESULT APIENTRY unsupported_color_space(DXGI_DDI_ARG_CHECKMULTIPLANEOVERLAYCOLORSPACESUPPORT *) { return DXGI_ERROR_UNSUPPORTED; }
HRESULT APIENTRY unsupported_overlay_present1(DXGI_DDI_ARG_PRESENTMULTIPLANEOVERLAY1 *) { return DXGI_ERROR_UNSUPPORTED; }
}
// The plane slice is the only field the WDDM 2.0 arguments add, and it belongs to Tex2D alone. Every
// other member is the D3D11.1 member, field for field.
void convert_wddm2_srv(const D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW &s,
    D3D11DDIARG_CREATESHADERRESOURCEVIEW &out,UINT &plane) {
    D3D11DDIARG_CREATESHADERRESOURCEVIEW d{};
    d.hDrvResource=s.hDrvResource; d.Format=s.Format; d.ResourceDimension=s.ResourceDimension;
    plane=0;
    switch (s.ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER: d.Buffer=s.Buffer; break;
    case D3D10DDIRESOURCE_TEXTURE1D: d.Tex1D=s.Tex1D; break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        d.Tex2D.MostDetailedMip=s.Tex2D.MostDetailedMip; d.Tex2D.FirstArraySlice=s.Tex2D.FirstArraySlice;
        d.Tex2D.MipLevels=s.Tex2D.MipLevels; d.Tex2D.ArraySize=s.Tex2D.ArraySize;
        plane=s.Tex2D.PlaneSlice; break;
    case D3D10DDIRESOURCE_TEXTURE3D: d.Tex3D=s.Tex3D; break;
    case D3D10DDIRESOURCE_TEXTURECUBE: d.TexCube=s.TexCube; break;
    case D3D11DDIRESOURCE_BUFFEREX: d.BufferEx=s.BufferEx; break;
    default: break; // the D3D11.1 entry refuses the dimension
    }
    out=d;
}
void convert_wddm2_rtv(const D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW &s,
    D3D10DDIARG_CREATERENDERTARGETVIEW &out,UINT &plane) {
    D3D10DDIARG_CREATERENDERTARGETVIEW d{};
    d.hDrvResource=s.hDrvResource; d.Format=s.Format; d.ResourceDimension=s.ResourceDimension;
    plane=0;
    switch (s.ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER: d.Buffer=s.Buffer; break;
    case D3D10DDIRESOURCE_TEXTURE1D: d.Tex1D=s.Tex1D; break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        d.Tex2D.MipSlice=s.Tex2D.MipSlice; d.Tex2D.FirstArraySlice=s.Tex2D.FirstArraySlice;
        d.Tex2D.ArraySize=s.Tex2D.ArraySize;
        plane=s.Tex2D.PlaneSlice; break;
    case D3D10DDIRESOURCE_TEXTURE3D: d.Tex3D=s.Tex3D; break;
    case D3D10DDIRESOURCE_TEXTURECUBE: d.TexCube=s.TexCube; break;
    default: break;
    }
    out=d;
}
void convert_wddm2_uav(const D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW &s,
    D3D11DDIARG_CREATEUNORDEREDACCESSVIEW &out,UINT &plane) {
    D3D11DDIARG_CREATEUNORDEREDACCESSVIEW d{};
    d.hDrvResource=s.hDrvResource; d.Format=s.Format; d.ResourceDimension=s.ResourceDimension;
    plane=0;
    switch (s.ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER: case D3D11DDIRESOURCE_BUFFEREX: d.Buffer=s.Buffer; break;
    case D3D10DDIRESOURCE_TEXTURE1D: d.Tex1D=s.Tex1D; break;
    case D3D10DDIRESOURCE_TEXTURE2D:
        d.Tex2D.MipSlice=s.Tex2D.MipSlice; d.Tex2D.FirstArraySlice=s.Tex2D.FirstArraySlice;
        d.Tex2D.ArraySize=s.Tex2D.ArraySize;
        plane=s.Tex2D.PlaneSlice; break;
    case D3D10DDIRESOURCE_TEXTURE3D: d.Tex3D=s.Tex3D; break;
    default: break;
    }
    out=d;
}
D3DWDDM2_0DDI_DEVICEFUNCS make_wddm2_0_device_table() {
    D3DWDDM2_0DDI_DEVICEFUNCS t{};
    const auto &b=base();
    std::memcpy(&t,&b,sizeof(b));
    // Every retyped entry is replaced; the copied pointers there have the D3D11.1 signatures.
    t.pfnFlush=flush;
    t.pfnRelocateDeviceFuncs=relocate;
    t.pfnCalcPrivateShaderResourceViewSize=srv_size; t.pfnCreateShaderResourceView=srv_create;
    t.pfnCalcPrivateRenderTargetViewSize=rtv_size; t.pfnCreateRenderTargetView=rtv_create;
    t.pfnCalcPrivateUnorderedAccessViewSize=uav_size; t.pfnCreateUnorderedAccessView=uav_create;
    t.pfnCalcPrivateQuerySize=query_size; t.pfnCreateQuery=query_create;
    install_fixed_state_wddm2_0_ddi(t);
    t.pfnCheckMultisampleQualityLevels=samples;
    t.pfnUpdateTileMappings=update_tile_mappings; t.pfnCopyTileMappings=copy_tile_mappings;
    t.pfnCopyTiles=copy_tiles; t.pfnUpdateTiles=update_tiles; t.pfnTiledResourceBarrier=tiled_barrier;
    t.pfnGetMipPacking=mip_packing; t.pfnResizeTilePool=resize_tile_pool;
    t.pfnSetMarker=set_marker; t.pfnSetMarkerMode=set_marker_mode;
    // No content protection or texture layouts are advertised (TEXTURE_LAYOUT caps are zero).
    t.pfnSetHardwareProtection=Unexpected<PFND3DWDDM2_0DDI_SETHARDWAREPROTECTION,kSetHardwareProtection>::call;
    t.pfnGetResourceLayout=Unexpected<PFND3DWDDM2_0DDI_GETRESOURCELAYOUT,kGetResourceLayout>::call;
    t.pfnRetrieveShaderComment=Unexpected<PFND3DWDDM2_0DDI_RETRIEVE_SHADER_COMMENT,kRetrieveShaderComment>::call;
    t.pfnSetHardwareProtectionState=Unexpected<PFND3DWDDM2_0DDI_SETHARDWAREPROTECTIONSTATE,kSetHardwareProtectionState>::call;
    return t;
}
DXGI1_4_DDI_BASE_FUNCTIONS make_dxgi1_4_device_table() {
    DXGI1_4_DDI_BASE_FUNCTIONS t{};
    const auto &b=dxgi_base();
    std::memcpy(&t,&b,offsetof(DXGI1_2_DDI_BASE_FUNCTIONS,pfnGetMultiplaneOverlayCaps));
    // No MPO support is advertised by this KMD. Do not silently report success.
    t.pfnGetMultiplaneOverlayCaps=unsupported_overlay_caps;
    t.pfnGetMultiplaneOverlayGroupCaps=unsupported_group_caps;
    t.pfnReserved1=nullptr; t.pfnReserved2=nullptr;
    t.pfnPresentMultiplaneOverlay=unsupported_overlay_present;
    t.pfnPresent1=present1;
    t.pfnCheckPresentDurationSupport=present_duration;
    t.pfnTrimResidencySet=trim_residency;
    t.pfnCheckMultiplaneOverlayColorSpaceSupport=unsupported_color_space;
    t.pfnPresentMultiplaneOverlay1=unsupported_overlay_present1;
    return t;
}
}
