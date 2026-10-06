// SPDX-License-Identifier: MIT
// WDDM 2.0 D3D11 interface of an FL12 adapter: negotiation, the device and DXGI 1.4 tables, the sparse
// policy's hold on FL12, and the converting and tiled entries on an owner without an engine (no GPU).
#include "ddi-wddm2.h"
#include "ddi-table.h"
#include "ddi-dxgi-table.h"
#include "ddi-adapter.h"
#include "ddi-negotiation.h"
#include "adapter-identity.h"
#include "ddi-srv.h"
#include "ddi-rtv.h"
#include "ddi-uav.h"
#include "ddi-query.h"
#include "ddi-direct-flip.h"
#include "ddi-resource.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace bc250::umd;
// The line is flushed before the abort: a redirected stdout is fully buffered, and a lost message
// turns a one-line failure into a debugger session.
#define CHECK(x) do { if(!(x)) { std::printf("FAIL wddm2 line %d\n",__LINE__);std::fflush(stdout);std::abort(); } } while(0)
namespace {
unsigned reported; HRESULT lastReported;
void APIENTRY count_error(D3D10DDI_HRTCORELAYER,HRESULT hr) { ++reported; lastReported=hr; }
int adapterIdentity;
HRESULT APIENTRY adapter_query(HANDLE,const D3DDDICB_QUERYADAPTERINFO *args) {
    // The kernel driver writes an optional trailer only when all of it fits, so the asked-for size is the
    // whole contract between the two halves. This stands in for a driver that writes the last trailer the
    // contract defines: the check above refuses a buffer the write below would overrun.
    CHECK(args && args->PrivateDriverDataSize==BC250_SCANOUT_CAPS_TOTAL);
    bc250_adapter_identity identity{BC250_ADAPTER_IDENTITY_MAGIC,BC250_ADAPTER_IDENTITY_VERSION,
        sizeof(bc250_adapter_identity),77,0,0};
    std::memcpy(static_cast<unsigned char *>(args->pPrivateDriverData)+BC250_ADAPTER_IDENTITY_OFFSET,&identity,sizeof(identity));
    bc250_scanout_caps scanout{BC250_SCANOUT_CAPS_MAGIC,BC250_SCANOUT_CAPS_VERSION,
        sizeof(bc250_scanout_caps),BC250_SCANOUT_CAPS_DIRECT_FLIP,1920,1200};
    std::memcpy(static_cast<unsigned char *>(args->pPrivateDriverData)+BC250_SCANOUT_CAPS_OFFSET,&scanout,sizeof(scanout));
    return S_OK;
}
bool policySparse; HRESULT policyResult; UINT64 policyLuid;
HRESULT policy(UINT64 luid,bool &sparse,D3DKMT_HANDLE &unresolved) noexcept {
    policyLuid=luid; sparse=policySparse; unresolved=0; return policyResult;
}
AdapterCaps fl12_1_caps() {
    AdapterCaps caps{}; caps.maximum=D3D_FEATURE_LEVEL_12_1;
    caps.doubles.DoublePrecisionFloatShaderOps=TRUE;
    caps.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=TRUE;
    caps.options.OutputMergerLogicOp=TRUE;
    caps.options2.TiledResourcesTier=D3D11_TILED_RESOURCES_TIER_2;
    caps.options2.TypedUAVLoadAdditionalFormats=TRUE;caps.options2.ROVsSupported=TRUE;
    caps.options2.ConservativeRasterizationTier=D3D11_CONSERVATIVE_RASTERIZATION_TIER_1;
    caps.options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer=TRUE;
    return caps;
}
UINT flags_for(UINT pipeline) {
    return ((pipeline&7)<<D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT)|
        ((pipeline&24)<<D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT2);
}
void negotiation() {
    UINT32 count=0; UINT64 versions[3]={1,2,3};
    CHECK(supported_ddi_versions(D3D_FEATURE_LEVEL_12_1,&count,nullptr)==S_OK && count==2);
    count=1;
    CHECK(supported_ddi_versions(D3D_FEATURE_LEVEL_12_0,&count,versions)==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) &&
          count==1 && versions[0]==1);
    count=3;
    CHECK(supported_ddi_versions(D3D_FEATURE_LEVEL_12_1,&count,versions)==S_OK && count==2 &&
          versions[0]==D3D11_1_DDI_SUPPORTED && versions[1]==D3DWDDM2_0_DDI_SUPPORTED && versions[2]==3);
    CHECK(wddm2_0_ddi(D3DWDDM2_0_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_0_DDI_BUILD_VERSION)<<16));
    CHECK(!wddm2_0_ddi(D3DWDDM2_0_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_0_DDI_BUILD_VERSION-1)<<16));
    CHECK(!wddm2_0_ddi(D3D11_1_DDI_INTERFACE_VERSION,UINT(D3DWDDM2_0_DDI_BUILD_VERSION)<<16));
    const D3D_FEATURE_LEVEL expected[]={D3D_FEATURE_LEVEL_10_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_9_1,D3D_FEATURE_LEVEL_9_1,D3D_FEATURE_LEVEL_9_1,
        D3D_FEATURE_LEVEL_12_0,D3D_FEATURE_LEVEL_12_1};
    for (UINT pipeline=0;pipeline<32;++pipeline) {
        D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_9_1;
        const HRESULT at2=requested_feature_level(flags_for(pipeline),level,D3DWDDM2_0_DDI_INTERFACE_VERSION);
        const bool valid=pipeline<9 && expected[pipeline]!=D3D_FEATURE_LEVEL_9_1;
        CHECK(valid ? at2==S_OK && level==expected[pipeline] : at2==E_INVALIDARG && level==D3D_FEATURE_LEVEL_9_1);
        level=D3D_FEATURE_LEVEL_9_1;
        const HRESULT at1=requested_feature_level(flags_for(pipeline),level);
        CHECK(pipeline<4 ? at1==S_OK : at1==E_INVALIDARG);
    }
}
// M15.14: the rule behind pfnCheckDirectFlipSupport, and the table entry that resolves the handles.
void ready_primary(RuntimeSurface &s,DXGI_FORMAT format,UINT width,UINT height,UINT pitch,UINT source) {
    s.phase=SurfacePhase::ready; s.allocation.allocation=0x1000+source;
    s.primary=true; s.vidpn_source=source; s.scanout=true;
    s.desc.Format=format; s.desc.Width=width; s.desc.Height=height; s.pitch=pitch;
}
void direct_flip_rule() {
    RuntimeSurface front,back;
    ready_primary(front,DXGI_FORMAT_B8G8R8A8_UNORM,1920,1200,7680,0);
    ready_primary(back,DXGI_FORMAT_B8G8R8A8_UNORM,1920,1200,7680,0);
    back.allocation.allocation=0x2000;
    // Two 8-bit primaries of one source, identical layout: the one case that may flip.
    CHECK(direct_flip_supported(&front,&back) && direct_flip_supported(&back,&front));
    // The same buffer is not a flip, and a missing surface is not one either.
    CHECK(!direct_flip_supported(&front,&front) && !direct_flip_supported(&front,nullptr) &&
          !direct_flip_supported(nullptr,&back) && !direct_flip_supported(nullptr,nullptr));
    // Every single difference refuses: phase, storage, primary, source, geometry, pitch.
    const auto refuses=[&](auto &&mutate) {
        RuntimeSurface a,b; ready_primary(a,DXGI_FORMAT_B8G8R8A8_UNORM,1920,1200,7680,0);
        ready_primary(b,DXGI_FORMAT_B8G8R8A8_UNORM,1920,1200,7680,0); b.allocation.allocation=0x2000;
        mutate(a,b);
        return !direct_flip_supported(&a,&b) && !direct_flip_supported(&b,&a);
    };
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.phase=SurfacePhase::paging; }));
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.phase=SurfacePhase::quarantined; }));
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.allocation.allocation=0; }));
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.primary=false; }));
    // The scan-out bit of the resource record. This is the clause that keeps the shell's answer FALSE
    // while its primaries are still aperture-resident: an aperture address is one the display core
    // cannot read and SetVidPnSourceAddress refuses, and by then the runtime has skipped the copy.
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.scanout=false; }));
    CHECK(refuses([](RuntimeSurface &a,RuntimeSurface &b) { a.scanout=false; b.scanout=false; }));
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.vidpn_source=1; }));
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.desc.Width=1280; }));
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.desc.Height=1080; }));
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.pitch=5120; }));
    CHECK(refuses([](RuntimeSurface &a,RuntimeSurface &b) { a.pitch=0; b.pitch=0; }));
    // A format difference refuses even between two scan-out rows: BGRA8 and X8 are not one mode.
    CHECK(refuses([](RuntimeSurface &,RuntimeSurface &b) { b.desc.Format=DXGI_FORMAT_B8G8R8X8_UNORM; }));
    // The sRGB view of the same storage row is the same scan-out row.
    CHECK(!refuses([](RuntimeSurface &,RuntimeSurface &b) { b.desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; }));
    // The composed-only rows are never candidates, so a 10-bit or FP16 swap chain keeps its copy
    // (HDR-ready Present architecture, owner 2026-09-29), and so does a format outside the table.
    for (auto format:{DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,
                      DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_A8_UNORM,DXGI_FORMAT_R32G32B32A32_FLOAT}) {
        RuntimeSurface a,b; ready_primary(a,format,1920,1200,7680,0);
        ready_primary(b,format,1920,1200,7680,0); b.allocation.allocation=0x2000;
        CHECK(!direct_flip_supported(&a,&b));
    }
    // The table entry answers from the resources' surfaces, and a resource without one refuses.
    const auto t=make_wddm2_0_device_table();
    CHECK(t.pfnCheckDirectFlipSupport);
    DdiResource a{},b{}; a.runtime_surface=&front; b.runtime_surface=&back;
    D3D10DDI_HDEVICE device{}; D3D10DDI_HRESOURCE ha{},hb{};
    ha.pDrvPrivate=&a; hb.pDrvPrivate=&b;
    BOOL supported=FALSE;
    t.pfnCheckDirectFlipSupport(device,ha,hb,0,&supported); CHECK(supported==TRUE);
    supported=FALSE;
    t.pfnCheckDirectFlipSupport(device,ha,hb,D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE,&supported);
    CHECK(supported==TRUE);
    DdiResource plain{}; D3D10DDI_HRESOURCE hplain{}; hplain.pDrvPrivate=&plain;
    supported=TRUE; t.pfnCheckDirectFlipSupport(device,ha,hplain,0,&supported); CHECK(supported==FALSE);
    supported=TRUE; t.pfnCheckDirectFlipSupport(device,ha,{},0,&supported); CHECK(supported==FALSE);
    t.pfnCheckDirectFlipSupport(device,ha,hb,0,nullptr); // No output pointer: nothing is written.
    // What a surface built the way the shipped shell builds one answers today: decode_create_resource
    // writes PRIMARY and never SCANOUT, so a pair of this shell's own primaries is refused, which is
    // the pre-M15.14 answer. The day the bit is written, this is the check that has to change -
    // together with the placement, and not before it.
    {
        RuntimeSurface shipped,shipped2;
        ready_primary(shipped,DXGI_FORMAT_B8G8R8A8_UNORM,1920,1200,7680,0);
        ready_primary(shipped2,DXGI_FORMAT_B8G8R8A8_UNORM,1920,1200,7680,0);
        shipped2.allocation.allocation=0x2000;
        BC250_SURFACE_RESOURCE_PRIVATE record{};
        record.Access=BC250_SURFACE_RESOURCE_PRIMARY;      // ddi-resource.cpp's own value for a primary
        shipped.scanout=shipped2.scanout=(record.Access&BC250_SURFACE_RESOURCE_SCANOUT)!=0;
        CHECK(!direct_flip_supported(&shipped,&shipped2));
        record.Access|=BC250_SURFACE_RESOURCE_SCANOUT;     // and what the next increment must write
        shipped.scanout=shipped2.scanout=(record.Access&BC250_SURFACE_RESOURCE_SCANOUT)!=0;
        CHECK(direct_flip_supported(&shipped,&shipped2));
    }
}
// BD-071: the plane slice of a WDDM 2.0 view argument must reach the engine. A planar resource (NV12
// and the other video formats) is viewed one plane at a time, and the runtime names the plane next to
// the view format: plane 0 with R8_UNORM is the luma of an NV12 texture, plane 1 with R8G8_UNORM its
// chroma. The three create entries may report only E_OUTOFMEMORY or D3DDDIERR_DEVICEREMOVED, so a
// refusal here costs the device, and the encoder input lost it on every NV12 chroma view. Both halves
// are pure functions, so the whole path is checkable without an engine: the DDI argument becomes the
// D3D11.1 argument plus the plane, and the plane becomes DESC1.PlaneSlice for the engine.
void planar_views() {
    D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW srv{};
    srv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; srv.Format=DXGI_FORMAT_R8G8_UNORM;
    srv.Tex2D={0,0,1,1,1}; // MostDetailedMip, FirstArraySlice, MipLevels, ArraySize, PlaneSlice
    D3D11DDIARG_CREATESHADERRESOURCEVIEW srv11{}; UINT plane=7;
    convert_wddm2_srv(srv,srv11,plane);
    CHECK(plane==1 && srv11.ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D &&
          srv11.Format==DXGI_FORMAT_R8G8_UNORM && srv11.Tex2D.MipLevels==1 && srv11.Tex2D.ArraySize==1);
    D3D11_SHADER_RESOURCE_VIEW_DESC1 view{};
    CHECK(convert_srv(srv11,1,1,plane,view)==S_OK && view.Format==DXGI_FORMAT_R8G8_UNORM &&
          view.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D && view.Texture2D.PlaneSlice==1 &&
          view.Texture2D.MipLevels==1);
    // The luma plane of the same texture: plane 0 is not a special case, only the common one.
    srv.Tex2D.PlaneSlice=0; srv.Format=DXGI_FORMAT_R8_UNORM;
    convert_wddm2_srv(srv,srv11,plane);
    CHECK(!plane && convert_srv(srv11,1,1,plane,view)==S_OK && !view.Texture2D.PlaneSlice &&
          view.Format==DXGI_FORMAT_R8_UNORM);
    // An array of planar textures keeps the plane next to the array range.
    srv.Tex2D={0,1,1,2,1}; srv.Format=DXGI_FORMAT_R8G8_UNORM;
    convert_wddm2_srv(srv,srv11,plane);
    CHECK(plane==1 && convert_srv(srv11,3,1,plane,view)==S_OK &&
          view.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY && view.Texture2DArray.PlaneSlice==1 &&
          view.Texture2DArray.FirstArraySlice==1 && view.Texture2DArray.ArraySize==2);
    // No other view dimension has a plane field, so no plane leaves such an argument, and a plane
    // the runtime cannot have sent is refused before the engine sees it.
    srv.ResourceDimension=D3D11DDIRESOURCE_BUFFEREX; srv.BufferEx={0,4,0};
    convert_wddm2_srv(srv,srv11,plane);
    CHECK(!plane && convert_srv(srv11,1,1,1,view)==E_INVALIDARG);
    // A planar resource is never multisampled, and a multisampled view has no plane field.
    srv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; srv.Tex2D={0,0,1,1,1};
    convert_wddm2_srv(srv,srv11,plane);
    CHECK(plane==1 && convert_srv(srv11,1,4,plane,view)==E_INVALIDARG);

    D3DWDDM2_0DDIARG_CREATERENDERTARGETVIEW rtv{};
    rtv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; rtv.Format=DXGI_FORMAT_R8G8_UNORM;
    rtv.Tex2D={0,0,1,1}; // MipSlice, FirstArraySlice, ArraySize, PlaneSlice
    D3D10DDIARG_CREATERENDERTARGETVIEW rtv11{}; plane=7;
    convert_wddm2_rtv(rtv,rtv11,plane);
    D3D11_RENDER_TARGET_VIEW_DESC1 target{};
    CHECK(plane==1 && rtv11.Format==DXGI_FORMAT_R8G8_UNORM && convert_rtv(rtv11,1,1,plane,target)==S_OK &&
          target.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && target.Texture2D.PlaneSlice==1);
    CHECK(convert_rtv(rtv11,4,1,plane,target)==S_OK &&
          target.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY && target.Texture2DArray.PlaneSlice==1);
    rtv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE3D; rtv.Tex3D={0,0,1};
    convert_wddm2_rtv(rtv,rtv11,plane);
    CHECK(!plane && convert_rtv(rtv11,1,1,1,target)==E_INVALIDARG);

    D3DWDDM2_0DDIARG_CREATEUNORDEREDACCESSVIEW uav{};
    uav.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; uav.Format=DXGI_FORMAT_R8G8_UNORM;
    uav.Tex2D={0,0,1,1}; // MipSlice, FirstArraySlice, ArraySize, PlaneSlice
    D3D11DDIARG_CREATEUNORDEREDACCESSVIEW uav11{}; plane=7;
    convert_wddm2_uav(uav,uav11,plane);
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 access{};
    CHECK(plane==1 && uav11.Format==DXGI_FORMAT_R8G8_UNORM && convert_uav(uav11,1,1,plane,access)==S_OK &&
          access.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2D && access.Texture2D.PlaneSlice==1);
    CHECK(convert_uav(uav11,4,1,plane,access)==S_OK &&
          access.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2DARRAY && access.Texture2DArray.PlaneSlice==1);
    uav.ResourceDimension=D3D11DDIRESOURCE_BUFFEREX; uav.Buffer={0,4,0};
    convert_wddm2_uav(uav,uav11,plane);
    CHECK(!plane && convert_uav(uav11,1,1,1,access)==E_INVALIDARG);
}
// The other table. A D3D11.1 create argument has no plane field, so the view format is the only thing
// that can name a chroma plane there, and the engine derives the plane from it. The plan carries that
// decision: ddi_plane_from_view_format produces the legacy description, which has no plane field, and
// the engine entry that reads it derives the plane. Sending plane 0 instead would refuse every planar
// view on that table and cost the device there, which is BD-071 moved one table across.
void plane_from_view_format() {
    D3D11DDIARG_CREATESHADERRESOURCEVIEW srv{};
    srv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; srv.Format=DXGI_FORMAT_R8G8_UNORM;
    srv.Tex2D={0,0,1,1}; // MostDetailedMip, FirstArraySlice, MipLevels, ArraySize
    SrvRequest request{};
    CHECK(plan_srv(srv,1,1,ddi_plane_from_view_format,request)==S_OK && request.derive_plane &&
          request.legacy.Format==DXGI_FORMAT_R8G8_UNORM &&
          request.legacy.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D &&
          request.legacy.Texture2D.MipLevels==1 && !request.legacy.Texture2D.MostDetailedMip);
    // A named plane keeps the D3D11.1 description with the plane in it, and derives nothing.
    CHECK(plan_srv(srv,1,1,1,request)==S_OK && !request.derive_plane &&
          request.desc1.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2D &&
          request.desc1.Texture2D.PlaneSlice==1);
    CHECK(plan_srv(srv,1,1,0,request)==S_OK && !request.derive_plane && !request.desc1.Texture2D.PlaneSlice);
    // An array view and a multisampled view travel the same way, and the array range survives.
    srv.Tex2D={0,1,1,2};
    CHECK(plan_srv(srv,3,1,ddi_plane_from_view_format,request)==S_OK && request.derive_plane &&
          request.legacy.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DARRAY &&
          request.legacy.Texture2DArray.FirstArraySlice==1 && request.legacy.Texture2DArray.ArraySize==2);
    srv.Tex2D={0,0,1,1};
    CHECK(plan_srv(srv,1,4,ddi_plane_from_view_format,request)==S_OK && request.derive_plane &&
          request.legacy.ViewDimension==D3D11_SRV_DIMENSION_TEXTURE2DMS);
    // A buffer view has no plane field in either description, and the derive request still works.
    srv.ResourceDimension=D3D11DDIRESOURCE_BUFFEREX; srv.BufferEx={2,4,D3D11_DDI_BUFFEREX_SRV_FLAG_RAW};
    CHECK(plan_srv(srv,1,1,ddi_plane_from_view_format,request)==S_OK && request.derive_plane &&
          request.legacy.ViewDimension==D3D11_SRV_DIMENSION_BUFFEREX &&
          request.legacy.BufferEx.FirstElement==2 && request.legacy.BufferEx.NumElements==4 &&
          request.legacy.BufferEx.Flags==UINT(D3D11_BUFFEREX_SRV_FLAG_RAW));
    // A refusal is still a refusal, whoever names the plane.
    srv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; srv.Tex2D={0,0,1,0};
    CHECK(plan_srv(srv,1,1,ddi_plane_from_view_format,request)==E_INVALIDARG);

    D3D10DDIARG_CREATERENDERTARGETVIEW rtv{};
    rtv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; rtv.Format=DXGI_FORMAT_R8G8_UNORM;
    rtv.Tex2D={2,0,1}; // MipSlice, FirstArraySlice, ArraySize
    RtvRequest target{};
    CHECK(plan_rtv(rtv,1,1,ddi_plane_from_view_format,target)==S_OK && target.derive_plane &&
          target.legacy.Format==DXGI_FORMAT_R8G8_UNORM &&
          target.legacy.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2D && target.legacy.Texture2D.MipSlice==2);
    CHECK(plan_rtv(rtv,1,1,1,target)==S_OK && !target.derive_plane && target.desc1.Texture2D.PlaneSlice==1);
    CHECK(plan_rtv(rtv,4,1,ddi_plane_from_view_format,target)==S_OK && target.derive_plane &&
          target.legacy.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE2DARRAY &&
          target.legacy.Texture2DArray.MipSlice==2 && target.legacy.Texture2DArray.ArraySize==1);
    rtv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE3D; rtv.Tex3D={1,2,3};
    CHECK(plan_rtv(rtv,1,1,ddi_plane_from_view_format,target)==S_OK && target.derive_plane &&
          target.legacy.ViewDimension==D3D11_RTV_DIMENSION_TEXTURE3D &&
          target.legacy.Texture3D.MipSlice==1 && target.legacy.Texture3D.FirstWSlice==2 &&
          target.legacy.Texture3D.WSize==3);

    D3D11DDIARG_CREATEUNORDEREDACCESSVIEW uav{};
    uav.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; uav.Format=DXGI_FORMAT_R8G8_UNORM;
    uav.Tex2D={3,0,1}; // MipSlice, FirstArraySlice, ArraySize
    UavRequest access{};
    CHECK(plan_uav(uav,1,1,ddi_plane_from_view_format,access)==S_OK && access.derive_plane &&
          access.legacy.Format==DXGI_FORMAT_R8G8_UNORM &&
          access.legacy.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE2D && access.legacy.Texture2D.MipSlice==3);
    CHECK(plan_uav(uav,1,1,1,access)==S_OK && !access.derive_plane && access.desc1.Texture2D.PlaneSlice==1);
    uav.ResourceDimension=D3D10DDIRESOURCE_BUFFER;
    uav.Buffer={1,5,D3D11_DDI_BUFFER_UAV_FLAG_COUNTER};
    CHECK(plan_uav(uav,1,1,ddi_plane_from_view_format,access)==S_OK && access.derive_plane &&
          access.legacy.ViewDimension==D3D11_UAV_DIMENSION_BUFFER &&
          access.legacy.Buffer.FirstElement==1 && access.legacy.Buffer.NumElements==5 &&
          access.legacy.Buffer.Flags==UINT(D3D11_BUFFER_UAV_FLAG_COUNTER));
}
void device_table() {
    const auto t=make_wddm2_0_device_table();
    const auto b=make_render_device_table();
    const size_t retyped[]={offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnFlush),offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnRelocateDeviceFuncs),
        offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCalcPrivateShaderResourceViewSize),offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCreateShaderResourceView),
        offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCalcPrivateRenderTargetViewSize),offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCreateRenderTargetView),
        offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCalcPrivateRasterizerStateSize),offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCreateRasterizerState),
        offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCalcPrivateQuerySize),offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCreateQuery),
        offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCalcPrivateUnorderedAccessViewSize),offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCreateUnorderedAccessView),
        offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,pfnCheckMultisampleQualityLevels)};
    void *slots[sizeof(t)/sizeof(void *)]; std::memcpy(slots,&t,sizeof(t));
    void *baseSlots[sizeof(b)/sizeof(void *)]; std::memcpy(baseSlots,&b,sizeof(b));
    unsigned changed=0,appended=0;
    for (size_t i=0;i<sizeof(t)/sizeof(void *);++i) {
        bool isRetyped=false;
        for (auto offset:retyped) isRetyped|=offset==i*sizeof(void *);
        if (i>=sizeof(b)/sizeof(void *)) { CHECK(slots[i]); ++appended; }
        else if (isRetyped) { CHECK(slots[i] && slots[i]!=baseSlots[i]); ++changed; }
        else CHECK(slots[i]==baseSlots[i]);
    }
    CHECK(changed==13 && appended==13);
    // Every entry the D3D11.1 table fills is filled here too, and the WDDM 2.0 relocation rebuilds this table.
    D3DWDDM2_0DDI_DEVICEFUNCS moved{}; std::memset(&moved,0xA5,sizeof(moved));
    DeviceOwner owner; owner.runtime().UMCallbacks.pfnSetErrorCb=count_error;
    DdiDeviceHandle storage{&owner}; D3D10DDI_HDEVICE h{}; h.pDrvPrivate=&storage;
    t.pfnRelocateDeviceFuncs(h,&moved);
    CHECK(!std::memcmp(&moved,&t,sizeof(t)) && !owner.runtime().domain.entered());

    // No engine behind this owner: every entry reports once and changes nothing else.
    reported=0;
    CHECK(!t.pfnFlush(h,0,0) && reported==1);
    CHECK(!t.pfnFlush(h,16,0) && reported==2);
    DdiShaderResourceView view{reinterpret_cast<ID3D11ShaderResourceView *>(1)};
    D3D10DDI_HSHADERRESOURCEVIEW viewHandle{}; viewHandle.pDrvPrivate=&view;
    D3DWDDM2_0DDIARG_CREATESHADERRESOURCEVIEW srv{}; srv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;
    srv.Tex2D.ArraySize=1; srv.Tex2D.MipLevels=1; srv.Tex2D.PlaneSlice=1;
    CHECK(t.pfnCalcPrivateShaderResourceViewSize(h,&srv)==sizeof(DdiShaderResourceView));
    t.pfnCreateShaderResourceView(h,&srv,viewHandle,{});
    // BD-071 at the table: a plane slice is no longer a refusal of its own, and the only status this
    // entry may report for a view it cannot create is device removal. E_INVALIDARG here is what the
    // runtime used to answer with Dr. Watson and a deliberate device loss.
    CHECK(!view.object && reported==3 && lastReported==D3DDDIERR_DEVICEREMOVED);
    DdiQuery query{}; D3D10DDI_HQUERY queryHandle{}; queryHandle.pDrvPrivate=&query;
    D3DWDDM2_0DDIARG_CREATEQUERY queryArgs{}; queryArgs.Query=D3D10DDI_QUERY_EVENT; queryArgs.ContextType=32;
    CHECK(t.pfnCalcPrivateQuerySize(h,&queryArgs)==sizeof(DdiQuery));
    t.pfnCreateQuery(h,&queryArgs,queryHandle,{});
    CHECK(!query.object && reported==4);
    UINT packed=7,tiles=7; D3D10DDI_HRESOURCE none{};
    t.pfnGetMipPacking(h,none,&packed,&tiles);
    // GetMipPacking allows E_INVALIDARG for a missing argument, and it is not a check-type entry, so
    // a lost device is reported as lost (ddi-error-policy.h, BD-071 review).
    CHECK(!packed && !tiles && reported==5 && lastReported==D3DDDIERR_DEVICEREMOVED);
    D3DWDDM1_3DDI_TILED_RESOURCE_COORDINATE start{}; D3DWDDM1_3DDI_TILE_REGION_SIZE size{}; size.NumTiles=1;
    t.pfnUpdateTileMappings(h,none,1,&start,&size,none,0,nullptr,nullptr,nullptr,0);
    t.pfnCopyTileMappings(h,none,&start,none,&start,&size,0);
    t.pfnCopyTiles(h,none,&start,&size,none,0,0);
    t.pfnUpdateTiles(h,none,&start,&size,&packed,0);
    t.pfnTiledResourceBarrier(h,D3D10DDI_HT_RESOURCE,nullptr,D3D10DDI_HT_RESOURCE,nullptr);
    t.pfnResizeTilePool(h,none,65536);
    CHECK(reported==11 && !owner.runtime().domain.entered());
    UINT quality=9;
    t.pfnCheckMultisampleQualityLevels(h,DXGI_FORMAT_R8G8B8A8_UNORM,4,D3DWDDM1_3DDI_CHECK_MULTISAMPLE_QUALITY_LEVELS_TILED_RESOURCE,&quality);
    CHECK(!quality && reported==12 && lastReported==E_INVALIDARG);
    quality=9; t.pfnCheckMultisampleQualityLevels(h,DXGI_FORMAT_R8G8B8A8_UNORM,1,0,&quality);
    CHECK(quality==1 && reported==12);
    t.pfnSetMarker(h); t.pfnSetMarkerMode(h,D3DWDDM1_3DDI_MARKER_TYPE_NONE,0);
    CHECK(reported==12);
    t.pfnSetHardwareProtection(h,none,TRUE);
    CHECK(reported==13 && t.pfnRetrieveShaderComment(h,{},nullptr,nullptr)==E_NOTIMPL);
    DdiDeviceHandle empty{}; D3D10DDI_HDEVICE emptyHandle{}; emptyHandle.pDrvPrivate=&empty;
    CHECK(!t.pfnFlush(emptyHandle,0,D3DWDDM1_3DDI_TRIM_MEMORY) && reported==13);
}
void dxgi_table() {
    const auto t=make_dxgi1_4_device_table();
    const auto b=make_dxgi_device_table();
    CHECK(!std::memcmp(&t,&b,offsetof(DXGI1_2_DDI_BASE_FUNCTIONS,pfnGetMultiplaneOverlayCaps)));
    CHECK(t.pfnPresent1 && t.pfnCheckPresentDurationSupport && t.pfnTrimResidencySet && !t.pfnReserved1 && !t.pfnReserved2);
    CHECK(t.pfnGetMultiplaneOverlayCaps(nullptr)==DXGI_ERROR_UNSUPPORTED &&
          t.pfnGetMultiplaneOverlayGroupCaps(nullptr)==DXGI_ERROR_UNSUPPORTED &&
          t.pfnPresentMultiplaneOverlay(nullptr)==DXGI_ERROR_UNSUPPORTED &&
          t.pfnCheckMultiplaneOverlayColorSpaceSupport(nullptr)==DXGI_ERROR_UNSUPPORTED &&
          t.pfnPresentMultiplaneOverlay1(nullptr)==DXGI_ERROR_UNSUPPORTED);
    int device=0;
    DXGI_DDI_ARG_PRESENTSURFACE surfaces[2]{};
    DXGI_DDI_ARG_PRESENT1 present{}; present.hDevice=reinterpret_cast<DXGI_DDI_HDEVICE>(&device);
    present.phSurfacesToPresent=surfaces; present.SurfacesToPresent=2;
    CHECK(t.pfnPresent1(nullptr)==E_INVALIDARG && t.pfnPresent1(&present)==DXGI_ERROR_UNSUPPORTED);
    DXGI_DDI_ARG_CHECKPRESENTDURATIONSUPPORT duration{}; duration.hDevice=present.hDevice;
    duration.ClosestSmallerDuration=5; duration.ClosestLargerDuration=6; duration.DesiredPresentDuration=100000;
    CHECK(t.pfnCheckPresentDurationSupport(&duration)==S_OK && !duration.ClosestSmallerDuration && !duration.ClosestLargerDuration);
    DXGI_DDI_ARG_TRIMRESIDENCYSET trim{}; trim.hDevice=present.hDevice;
    CHECK(t.pfnTrimResidencySet(&trim)==S_OK && t.pfnTrimResidencySet(nullptr)==E_INVALIDARG);
}
void adapter_policy() {
    for (int sparse=0;sparse<3;++sparse) {
        policySparse=sparse!=0; policyResult=sparse==2 ? E_FAIL : S_OK; policyLuid=0;
        AdapterConfiguration configuration{L"P:/BC-250/scratch/m14/missing-adapter-engine.dll",
            L"P:/BC-250/scratch/m14/missing-adapter-icd.dll",fl12_1_caps()};
        configuration.sparse_policy=policy;
        D3D10_2DDI_ADAPTERFUNCS table{};
        D3DDDI_ADAPTERCALLBACKS callbacks{}; callbacks.pfnQueryAdapterInfoCb=adapter_query;
        D3D10DDIARG_OPENADAPTER args{}; args.pAdapterFuncs_2=&table; args.pAdapterCallbacks=&callbacks;
        args.hRTAdapter.handle=&adapterIdentity;
        CHECK(open_render_adapter(args,configuration)==S_OK && policyLuid==77);
        // FL12 needs the policy's sparse binding; off or unreadable leaves the adapter at FL11_1.
        const bool fl12=sparse==1;
        UINT32 count=0;
        CHECK(table.pfnGetSupportedVersions(args.hAdapter,&count,nullptr)==S_OK && count==(fl12 ? 2u : 1u));
        D3D11DDI_3DPIPELINESUPPORT_CAPS pipelines{}; D3D10_2DDIARG_GETCAPS caps{};
        caps.Type=D3D11DDICAPS_3DPIPELINESUPPORT; caps.pData=&pipelines; caps.DataSize=sizeof(pipelines);
        CHECK(table.pfnGetCaps(args.hAdapter,&caps)==S_OK && pipelines.Caps==(fl12 ? 0x18Fu : 15u));
        D3DWDDM1_3DDI_D3D11_OPTIONS_DATA1 options1{}; caps.Type=D3DWDDM1_3DDICAPS_D3D11_OPTIONS1;
        caps.pData=&options1; caps.DataSize=sizeof(options1);
        CHECK(table.pfnGetCaps(args.hAdapter,&caps)==(fl12 ? S_OK : E_NOTIMPL) && options1.TiledResourcesSupportFlags==(fl12 ? 3u : 0u));
        D3D10DDIARG_CALCPRIVATEDEVICESIZE size{}; size.Interface=D3DWDDM2_0_DDI_INTERFACE_VERSION;
        size.Version=UINT(D3DWDDM2_0_DDI_BUILD_VERSION)<<16; size.Flags=flags_for(D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1);
        CHECK(table.pfnCalcPrivateDeviceSize(args.hAdapter,&size)==(fl12 ? sizeof(DdiDeviceHandle) : 0u));
        size.Flags=flags_for(D3D11_1DDI_3DPIPELINELEVEL_11_1);
        CHECK(table.pfnCalcPrivateDeviceSize(args.hAdapter,&size)==(fl12 ? sizeof(DdiDeviceHandle) : 0u));
        size.Interface=D3D11_1_DDI_INTERFACE_VERSION; size.Version=UINT(D3D11_1_DDI_BUILD_VERSION)<<16;
        CHECK(table.pfnCalcPrivateDeviceSize(args.hAdapter,&size)==sizeof(DdiDeviceHandle));
        size.Flags=flags_for(D3DWDDM2_0DDI_3DPIPELINELEVEL_12_0);
        CHECK(!table.pfnCalcPrivateDeviceSize(args.hAdapter,&size));
        CHECK(table.pfnCloseAdapter(args.hAdapter)==S_OK);
    }
    // The WDDM 2.0 interface is refused for an FL11 snapshot before any callback is used.
    D3DWDDM2_0DDI_DEVICEFUNCS funcs{}; DXGI1_4_DDI_BASE_FUNCTIONS dxgi{};
    DdiDeviceHandle handle{}, failed{};
    D3D10DDIARG_CREATEDEVICE create{}; create.Interface=D3DWDDM2_0_DDI_INTERFACE_VERSION;
    create.Flags=flags_for(D3D11_1DDI_3DPIPELINELEVEL_11_1); create.hDrvDevice.pDrvPrivate=&handle;
    create.pWDDM2_0DeviceFuncs=&funcs; create.DXGIBaseDDI.pDXGIDDIBaseFunctions5=&dxgi;
    BC250_DXVK_ENGINE_FUNCS engine{}; BC250_DXVK_SHELL_SERVICES services{};
    CHECK(create_render_device(create,1,nullptr,engine,D3D_FEATURE_LEVEL_11_1,services,failed,
        without_fl12(fl12_1_caps()),BC250_HOST_POLICY_SPARSE)==E_INVALIDARG && !handle.owner && !failed.owner);
}
}
void test_wddm2_0_ddi() {
    negotiation();
    direct_flip_rule();
    planar_views();
    plane_from_view_format();
    device_table();
    dxgi_table();
    adapter_policy();
    std::cout << "PASS WDDM 2.0 DDI: negotiation, device and DXGI 1.4 tables, sparse policy gate, direct-flip rule, plane views, plane from the view format, tiled entries (no GPU)\n";
}
