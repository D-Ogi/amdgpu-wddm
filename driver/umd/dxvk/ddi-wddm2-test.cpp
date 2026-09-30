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
#include "ddi-query.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace bc250::umd;
#define CHECK(x) do { if(!(x)) { std::printf("FAIL wddm2 line %d\n",__LINE__);std::abort(); } } while(0)
namespace {
unsigned reported; HRESULT lastReported;
void APIENTRY count_error(D3D10DDI_HRTCORELAYER,HRESULT hr) { ++reported; lastReported=hr; }
int adapterIdentity;
HRESULT APIENTRY adapter_query(HANDLE,const D3DDDICB_QUERYADAPTERINFO *args) {
    CHECK(args && args->PrivateDriverDataSize==BC250_ADAPTER_CAPS_BYTES);
    bc250_adapter_identity identity{BC250_ADAPTER_IDENTITY_MAGIC,BC250_ADAPTER_IDENTITY_VERSION,
        sizeof(bc250_adapter_identity),77,0,0};
    std::memcpy(static_cast<unsigned char *>(args->pPrivateDriverData)+BC250_ADAPTER_IDENTITY_OFFSET,&identity,sizeof(identity));
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
    CHECK(!view.object && reported==3);
    DdiQuery query{}; D3D10DDI_HQUERY queryHandle{}; queryHandle.pDrvPrivate=&query;
    D3DWDDM2_0DDIARG_CREATEQUERY queryArgs{}; queryArgs.Query=D3D10DDI_QUERY_EVENT; queryArgs.ContextType=32;
    CHECK(t.pfnCalcPrivateQuerySize(h,&queryArgs)==sizeof(DdiQuery));
    t.pfnCreateQuery(h,&queryArgs,queryHandle,{});
    CHECK(!query.object && reported==4);
    UINT packed=7,tiles=7; D3D10DDI_HRESOURCE none{};
    t.pfnGetMipPacking(h,none,&packed,&tiles);
    CHECK(!packed && !tiles && reported==5);
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
    CHECK(!quality && reported==12);
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
    device_table();
    dxgi_table();
    adapter_policy();
    std::cout << "PASS WDDM 2.0 DDI: negotiation, device and DXGI 1.4 tables, sparse policy gate, tiled entries (no GPU)\n";
}
