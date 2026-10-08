// SPDX-License-Identifier: MIT
// WDDM 2.2 D3D11 interface of an FL12 adapter (BD-099): negotiation, the build that carries the whole present
// callback, the device and DXGI 1.6.1 tables, the sync tokens, the shader-cache entries that the caps exclude,
// and the two WDDM 2.2 capability answers. No GPU: the owner has no engine, so every entry under test is one
// that answers before the engine is reached.
#include "ddi-wddm22.h"
#include "ddi-wddm2.h"
#include "ddi-table.h"
#include "ddi-dxgi-table.h"
#include "ddi-adapter.h"
#include "ddi-negotiation.h"
#include "ddi-experiment.h"
#include "ddi-resource.h"
#include "adapter-identity.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
using namespace bc250::umd;
// The line is flushed before the abort: a redirected stdout is fully buffered, and a lost message
// turns a one-line failure into a debugger session.
#define CHECK(x) do { if(!(x)) { std::printf("FAIL wddm22 line %d\n",__LINE__);std::fflush(stdout);std::abort(); } } while(0)
namespace {
unsigned reported; HRESULT lastReported;
void APIENTRY count_error(D3D10DDI_HRTCORELAYER,HRESULT hr) { ++reported; lastReported=hr; }
int adapterIdentity;
HRESULT APIENTRY adapter_query(HANDLE,const D3DDDICB_QUERYADAPTERINFO *args) {
    CHECK(args && args->PrivateDriverDataSize==BC250_SCANOUT_CAPS_TOTAL);
    bc250_adapter_identity identity{BC250_ADAPTER_IDENTITY_MAGIC,BC250_ADAPTER_IDENTITY_VERSION,
        sizeof(bc250_adapter_identity),77,0,0};
    std::memcpy(static_cast<unsigned char *>(args->pPrivateDriverData)+BC250_ADAPTER_IDENTITY_OFFSET,&identity,sizeof(identity));
    bc250_scanout_caps scanout{BC250_SCANOUT_CAPS_MAGIC,BC250_SCANOUT_CAPS_VERSION,
        sizeof(bc250_scanout_caps),BC250_SCANOUT_CAPS_DIRECT_FLIP,1920,1200};
    std::memcpy(static_cast<unsigned char *>(args->pPrivateDriverData)+BC250_SCANOUT_CAPS_OFFSET,&scanout,sizeof(scanout));
    return S_OK;
}
HRESULT sparse_policy(UINT64,bool &sparse,D3DKMT_HANDLE &unresolved) noexcept {
    sparse=true; unresolved=0; return S_OK;
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
constexpr UINT build_of(UINT build) { return build<<16; }

// The WDDM 2.2 interface is the newest offer, and only at the build whose present callback carries the whole
// structure. The switch of ddi-experiment.h chooses between the two newest offers.
void negotiation() {
    UINT32 count=0; UINT64 versions[4]={1,2,3,4};
    CHECK(supported_ddi_versions(D3D_FEATURE_LEVEL_12_1,true,&count,nullptr)==S_OK && count==3);
    count=2;
    CHECK(supported_ddi_versions(D3D_FEATURE_LEVEL_12_1,true,&count,versions)==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) &&
          count==2 && versions[0]==1);
    count=4;
    CHECK(supported_ddi_versions(D3D_FEATURE_LEVEL_12_1,true,&count,versions)==S_OK && count==3 &&
          versions[0]==D3D11_1_DDI_SUPPORTED && versions[1]==D3DWDDM2_0_DDI_SUPPORTED &&
          versions[2]==D3DWDDM2_2_DDI_SUPPORTED && versions[3]==4);
    // The three offers are ordered oldest first, and the newest is the WDDM 2.2 interface at build 5.
    CHECK(versions[1]<versions[2] && versions[0]<versions[1]);
    static_assert(D3DWDDM2_2_DDI_BUILD_VERSION==5 && D3DWDDM2_2_DDI_INTERFACE_VERSION==0xB0023);
    // FL11 keeps the D3D11.1 table alone, whichever value the switch has.
    count=4;
    CHECK(supported_ddi_versions(D3D_FEATURE_LEVEL_11_1,true,&count,versions)==S_OK && count==1);
    CHECK(wddm2_2_ddi(D3DWDDM2_2_DDI_INTERFACE_VERSION,build_of(D3DWDDM2_2_DDI_BUILD_VERSION)));
    CHECK(!wddm2_2_ddi(D3DWDDM2_2_DDI_INTERFACE_VERSION,build_of(D3DWDDM2_2_DDI_BUILD_VERSION-1)));
    CHECK(!wddm2_2_ddi(D3DWDDM2_0_DDI_INTERFACE_VERSION,build_of(D3DWDDM2_2_DDI_BUILD_VERSION)));
    CHECK(!wddm2_0_ddi(D3DWDDM2_2_DDI_INTERFACE_VERSION,build_of(D3DWDDM2_0_DDI_BUILD_VERSION)));
    // The build decides the shape of the present callback and of the DXGI table together.
    CHECK(dxgi1_6_1_table_layout_supported(D3DWDDM2_2_DDI_INTERFACE_VERSION,D3DWDDM2_2_DDI_BUILD_VERSION));
    CHECK(!dxgi1_6_1_table_layout_supported(D3DWDDM2_2_DDI_INTERFACE_VERSION,D3DWDDM2_2_DDI_BUILD_VERSION-1));
    CHECK(!dxgi1_6_1_table_layout_supported(D3DWDDM2_0_DDI_INTERFACE_VERSION,D3DWDDM2_2_DDI_BUILD_VERSION));
    // FL12_x needs the WDDM 2.0 interface or a later one; the WDDM 2.2 interface is one of those.
    D3D_FEATURE_LEVEL level=D3D_FEATURE_LEVEL_9_1;
    CHECK(requested_feature_level(flags_for(D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1),level,
        D3DWDDM2_2_DDI_INTERFACE_VERSION)==S_OK && level==D3D_FEATURE_LEVEL_12_1);
    // The switch parser, on lists the resolver may give.
    CHECK(native12::ddi_experiment_listed("d3d11-wddm20-ddi","d3d11-wddm20-ddi"));
    CHECK(native12::ddi_experiment_listed("a,d3d11-wddm20-ddi,b","d3d11-wddm20-ddi"));
    CHECK(!native12::ddi_experiment_listed("d3d11-wddm20-ddi-x","d3d11-wddm20-ddi"));
    CHECK(!native12::ddi_experiment_listed("","d3d11-wddm20-ddi"));
    // The recommended default: with no switch named, the shell offers the WDDM 2.2 interface.
    if (!d3d11_experiment_name()[0]) CHECK(wddm2_2_offered());
    CHECK(wddm2_2_offered()!=d3d11_experiment("d3d11-wddm20-ddi"));
}

// The WDDM 2.2 device table is the WDDM 2.0 table with one entry retyped and six appended.
void device_table() {
    const auto t=make_wddm2_2_device_table();
    const auto b=make_wddm2_0_device_table();
    const size_t retyped[]={offsetof(D3DWDDM2_2DDI_DEVICEFUNCS,pfnRelocateDeviceFuncs)};
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
    CHECK(changed==1 && appended==6);
    // The six appended entries are the two sync tokens and the four shader-cache sessions, in that order.
    static_assert(offsetof(D3DWDDM2_2DDI_DEVICEFUNCS,pfnAcquireResource)==sizeof(D3DWDDM2_0DDI_DEVICEFUNCS) &&
        offsetof(D3DWDDM2_2DDI_DEVICEFUNCS,pfnSetShaderCacheSession)+sizeof(void *)==sizeof(D3DWDDM2_2DDI_DEVICEFUNCS));
    // The relocation rebuilds this table, not the WDDM 2.0 one.
    D3DWDDM2_2DDI_DEVICEFUNCS moved{}; std::memset(&moved,0xA5,sizeof(moved));
    DeviceOwner owner; owner.runtime().UMCallbacks.pfnSetErrorCb=count_error;
    DdiDeviceHandle storage{&owner}; D3D10DDI_HDEVICE h{}; h.pDrvPrivate=&storage;
    reported=0; lastReported=S_OK;
    t.pfnRelocateDeviceFuncs(h,&moved);
    CHECK(!std::memcmp(&moved,&t,sizeof(t)) && !owner.runtime().domain.entered() && !reported);
    // The sync tokens reach the kernel callbacks. This owner has no context, so each one reports the loss of
    // the device, which is the only status their pages allow. No callback is installed, so none is called.
    t.pfnAcquireResource(h,{},reinterpret_cast<HANDLE>(0x20));
    CHECK(reported==1 && lastReported==D3DDDIERR_DEVICEREMOVED);
    t.pfnReleaseResource(h,{},reinterpret_cast<HANDLE>(0x20));
    CHECK(reported==2 && lastReported==D3DDDIERR_DEVICEREMOVED);
    CHECK(!owner.runtime().domain.entered());
    // The shader-cache entries are unreachable: the adapter answers RequestRuntimeShaderCache FALSE. A call is
    // a broken contract, so the size entry answers zero and the three void entries lose the device.
    reported=0;
    CHECK(t.pfnCalcPrivateShaderCacheSessionSize(h)==0 && !reported);
    t.pfnCreateShaderCacheSession(h,{},{});
    CHECK(reported==1 && lastReported==D3DDDIERR_DEVICEREMOVED);
    t.pfnDestroyShaderCacheSession(h,{});
    CHECK(reported==2);
    t.pfnSetShaderCacheSession(h,{});
    CHECK(reported==3 && !owner.runtime().domain.entered());
}

// The DXGI 1.6.1 table is the DXGI 1.4 table with three entries retyped and one appended.
void dxgi_table() {
    const auto t=make_dxgi1_6_1_device_table();
    const auto b=make_dxgi1_4_device_table();
    const size_t retyped[]={offsetof(DXGI1_6_1_DDI_BASE_FUNCTIONS,pfnOfferResources1),
        offsetof(DXGI1_6_1_DDI_BASE_FUNCTIONS,pfnPresent1),
        offsetof(DXGI1_6_1_DDI_BASE_FUNCTIONS,pfnPresentMultiplaneOverlay1)};
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
    CHECK(changed==3 && appended==1);
    int device=0;
    DXGI_DDI_ARG_PRESENTSURFACE surfaces[2]{};
    DXGI1_6_1_DDI_ARG_PRESENT present{}; present.hDevice=reinterpret_cast<DXGI_DDI_HDEVICE>(&device);
    present.phSurfacesToPresent=surfaces; present.SurfacesToPresent=2;
    present.RotationHint=DXGI_DDI_MODE_ROTATION_ROTATE90;
    CHECK(t.pfnPresent1(nullptr)==E_INVALIDARG && t.pfnPresent1(&present)==DXGI_ERROR_UNSUPPORTED);
    CHECK(t.pfnPresentMultiplaneOverlay1(nullptr)==DXGI_ERROR_UNSUPPORTED);
    CHECK(t.pfnOfferResources1(nullptr)==E_INVALIDARG && t.pfnReclaimResources1(nullptr)==E_INVALIDARG);
    // Offer and Reclaim of an engine-private resource: a hint, as the DXGI 1.2 entries treat it. Its memory
    // stays as it is, and Reclaim reports its content kept. Refusing them would cost the device.
    DeviceOwner owner; owner.runtime().UMCallbacks.pfnSetErrorCb=count_error;
    DdiDeviceHandle storage{&owner};
    int object=0;
    DdiResource resource{}; resource.object=reinterpret_cast<ID3D11Resource *>(&object);
    DXGI_DDI_HRESOURCE resources[1]={reinterpret_cast<DXGI_DDI_HRESOURCE>(&resource)};
    DXGI_DDI_ARG_OFFERRESOURCES1 offer{}; offer.hDevice=reinterpret_cast<DXGI_DDI_HDEVICE>(&storage);
    offer.pResources=resources; offer.Resources=1; offer.Priority=D3DDDI_OFFER_PRIORITY_LOW;
    offer.Flags.AllowDecommit=1; // dropped on purpose: the reclaim below has no "not committed" answer
    reported=0;
    CHECK(t.pfnOfferResources1(&offer)==S_OK && !reported);
    offer.Priority=static_cast<D3DDDI_OFFER_PRIORITY>(0);
    CHECK(t.pfnOfferResources1(&offer)==E_INVALIDARG);
    D3DDDI_RECLAIM_RESULT results[1]={D3DDDI_RECLAIM_RESULT_NOT_COMMITTED};
    DXGI_DDI_ARG_RECLAIMRESOURCES1 reclaim{}; reclaim.hDevice=offer.hDevice;
    reclaim.pResources=resources; reclaim.Resources=1; reclaim.pResults=results;
    CHECK(t.pfnReclaimResources1(&reclaim)==S_OK && results[0]==D3DDDI_RECLAIM_RESULT_OK && !reported);
    CHECK(!owner.runtime().domain.entered());
}

// The two capability answers of the WDDM 2.2 interface, and the adapter that admits the interface.
void adapter_policy() {
    const AdapterCaps caps=fl12_1_caps();
    D3DWDDM2_2DDICAPS_SHADERCACHE_DATA cache{TRUE};
    D3D10_2DDIARG_GETCAPS query{}; query.Type=D3DWDDM2_2DDICAPS_SHADERCACHE;
    query.pData=&cache; query.DataSize=sizeof(cache);
    CHECK(get_adapter_caps(caps,query)==S_OK && !cache.RequestRuntimeShaderCache);
    CHECK(get_adapter_caps(without_fl12(caps),query)==E_NOTIMPL);
    query.DataSize=sizeof(cache)+1;
    CHECK(get_adapter_caps(caps,query)==E_INVALIDARG);
    D3DWDDM2_2DDI_TEXTURE_LAYOUT_CAPS layout{1,1,TRUE,TRUE};
    query.Type=D3DWDDM2_2DDICAPS_TEXTURE_LAYOUT; query.pData=&layout; query.DataSize=sizeof(layout);
    CHECK(get_adapter_caps(caps,query)==S_OK && !layout.DeviceDependentLayoutCount &&
          !layout.DeviceDependentSwizzleCount && !layout.Supports64KStandardSwizzle && !layout.IndexableSwizzlePatterns);
    // No swizzle pattern is ever asked for, and none is invented.
    query.Type=D3DWDDM2_0DDICAPS_SWIZZLE_PATTERN;
    CHECK(get_adapter_caps(caps,query)==E_NOTIMPL);
    // The adapter admits the WDDM 2.2 interface at its build, and nothing else at that interface.
    AdapterConfiguration configuration{L"P:/BC-250/scratch/m14/missing-adapter-engine.dll",
        L"P:/BC-250/scratch/m14/missing-adapter-icd.dll",caps};
    configuration.sparse_policy=sparse_policy;
    D3D10_2DDI_ADAPTERFUNCS table{};
    D3DDDI_ADAPTERCALLBACKS callbacks{}; callbacks.pfnQueryAdapterInfoCb=adapter_query;
    D3D10DDIARG_OPENADAPTER args{}; args.pAdapterFuncs_2=&table; args.pAdapterCallbacks=&callbacks;
    args.hRTAdapter.handle=&adapterIdentity;
    CHECK(open_render_adapter(args,configuration)==S_OK);
    const bool offered=wddm2_2_offered();
    UINT32 count=0;
    CHECK(table.pfnGetSupportedVersions(args.hAdapter,&count,nullptr)==S_OK && count==(offered ? 3u : 2u));
    D3D10DDIARG_CALCPRIVATEDEVICESIZE size{}; size.Interface=D3DWDDM2_2_DDI_INTERFACE_VERSION;
    size.Version=build_of(D3DWDDM2_2_DDI_BUILD_VERSION); size.Flags=flags_for(D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1);
    CHECK(table.pfnCalcPrivateDeviceSize(args.hAdapter,&size)==(offered ? sizeof(DdiDeviceHandle) : 0u));
    size.Version=build_of(D3DWDDM2_2_DDI_BUILD_VERSION-1);
    CHECK(!table.pfnCalcPrivateDeviceSize(args.hAdapter,&size));
    size.Version=build_of(D3DWDDM2_2_DDI_BUILD_VERSION); size.Flags=flags_for(D3D11_1DDI_3DPIPELINELEVEL_11_1);
    CHECK(table.pfnCalcPrivateDeviceSize(args.hAdapter,&size)==(offered ? sizeof(DdiDeviceHandle) : 0u));
    CHECK(table.pfnCloseAdapter(args.hAdapter)==S_OK);
    // The interface is refused for an FL11 snapshot, and refused at a build the adapter never advertised,
    // before any callback is used. Both leave the runtime's private handle empty.
    D3DWDDM2_2DDI_DEVICEFUNCS funcs{}; DXGI1_6_1_DDI_BASE_FUNCTIONS dxgi{};
    DdiDeviceHandle handle{},failed{};
    D3D10DDIARG_CREATEDEVICE create{}; create.Interface=D3DWDDM2_2_DDI_INTERFACE_VERSION;
    create.Version=build_of(D3DWDDM2_2_DDI_BUILD_VERSION);
    create.Flags=flags_for(D3D11_1DDI_3DPIPELINELEVEL_11_1); create.hDrvDevice.pDrvPrivate=&handle;
    create.pWDDM2_2DeviceFuncs=&funcs; create.DXGIBaseDDI.pDXGIDDIBaseFunctions6_1=&dxgi;
    BC250_DXVK_ENGINE_FUNCS engine{}; BC250_DXVK_SHELL_SERVICES services{};
    CHECK(create_render_device(create,1,nullptr,engine,D3D_FEATURE_LEVEL_11_1,services,failed,
        without_fl12(caps),BC250_HOST_POLICY_SPARSE)==E_INVALIDARG && !handle.owner && !failed.owner);
    create.Version=build_of(D3DWDDM2_2_DDI_BUILD_VERSION-1);
    create.Flags=flags_for(D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1);
    CHECK(create_render_device(create,1,nullptr,engine,D3D_FEATURE_LEVEL_12_1,services,failed,
        without_fl12(caps),BC250_HOST_POLICY_SPARSE)==E_INVALIDARG && !handle.owner && !failed.owner);
    const D3DWDDM2_2DDI_DEVICEFUNCS empty{}; const DXGI1_6_1_DDI_BASE_FUNCTIONS emptyDxgi{};
    CHECK(!std::memcmp(&funcs,&empty,sizeof(empty)) && !std::memcmp(&dxgi,&emptyDxgi,sizeof(emptyDxgi)));
}
}
void test_wddm2_2_ddi() {
    negotiation();
    device_table();
    dxgi_table();
    adapter_policy();
    std::cout << "PASS WDDM 2.2 DDI: negotiation and build, device and DXGI 1.6.1 tables, sync tokens, "
                 "shader-cache contract, WDDM 2.2 caps, adapter admission (no GPU)\n";
}
