// SPDX-License-Identifier: MIT
#include "ddi-wddm22.h"
#include "ddi-wddm2.h"
#include "ddi-dxgi-table.h"
#include "ddi-dxgi-resources.h"
#include <cstddef>
#include <cstring>
#include <type_traits>
namespace bc250::umd {
namespace {
// The WDDM 2.2 table is the WDDM 2.0 table plus the WDDM 2.1 and 2.2 appendices. Only RelocateDeviceFuncs
// changes type, and it carries this table in place of the WDDM 2.0 one.
static_assert(offsetof(D3DWDDM2_2DDI_DEVICEFUNCS,pfnAcquireResource)==sizeof(D3DWDDM2_0DDI_DEVICEFUNCS));
static_assert(sizeof(D3DWDDM2_2DDI_DEVICEFUNCS)==sizeof(D3DWDDM2_0DDI_DEVICEFUNCS)+6*sizeof(void *));
#define SAME_OFFSET(field) static_assert(offsetof(D3DWDDM2_2DDI_DEVICEFUNCS,field)==offsetof(D3DWDDM2_0DDI_DEVICEFUNCS,field))
SAME_OFFSET(pfnDefaultConstantBufferUpdateSubresourceUP); SAME_OFFSET(pfnFlush); SAME_OFFSET(pfnRelocateDeviceFuncs);
SAME_OFFSET(pfnCreateShaderResourceView); SAME_OFFSET(pfnCreateRenderTargetView); SAME_OFFSET(pfnCreateRasterizerState);
SAME_OFFSET(pfnCreateQuery); SAME_OFFSET(pfnCreateUnorderedAccessView); SAME_OFFSET(pfnCheckMultisampleQualityLevels);
SAME_OFFSET(pfnCheckDirectFlipSupport); SAME_OFFSET(pfnClearView); SAME_OFFSET(pfnUpdateTileMappings);
SAME_OFFSET(pfnResizeTilePool); SAME_OFFSET(pfnSetMarker); SAME_OFFSET(pfnSetHardwareProtection);
SAME_OFFSET(pfnGetResourceLayout); SAME_OFFSET(pfnRetrieveShaderComment); SAME_OFFSET(pfnSetHardwareProtectionState);
#undef SAME_OFFSET
// Only the relocation entry differs in type. Every entry the table copies therefore keeps its signature;
// tools/quality/ddi_table_versions.py checks the whole list of 174 entries against the WDK header.
static_assert(!std::is_same_v<decltype(D3DWDDM2_2DDI_DEVICEFUNCS::pfnRelocateDeviceFuncs),
                              decltype(D3DWDDM2_0DDI_DEVICEFUNCS::pfnRelocateDeviceFuncs)>);
static_assert(std::is_same_v<decltype(D3DWDDM2_2DDI_DEVICEFUNCS::pfnFlush),
                             decltype(D3DWDDM2_0DDI_DEVICEFUNCS::pfnFlush)>);
// The UM callbacks of WDDM 2.2 extend the WDDM 2.0 ones; the shell reads only SetErrorCb, at the offset the
// D3D10 structure gives it (device-owner.cpp copies that prefix).
static_assert(offsetof(D3DWDDM2_2DDI_CORELAYER_DEVICECALLBACKS,pfnSetErrorCb)==offsetof(D3D10DDI_CORELAYER_DEVICECALLBACKS,pfnSetErrorCb) &&
    sizeof(D3DWDDM2_2DDI_CORELAYER_DEVICECALLBACKS)>=sizeof(D3DWDDM2_0DDI_CORELAYER_DEVICECALLBACKS));

// The WDDM 2.0 implementation every copied entry comes from.
const D3DWDDM2_0DDI_DEVICEFUNCS &wddm2_0_base() { static const auto table=make_wddm2_0_device_table(); return table; }
DeviceOwner &owner22(D3D10DDI_HDEVICE h) { return *static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner; }
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
// The shader-cache session entries. The adapter answers D3DWDDM2_2DDICAPS_SHADERCACHE with
// RequestRuntimeShaderCache FALSE (adapter-caps.h): the engine keeps its own pipeline cache, so the shell
// asks for no session of the runtime's and the runtime creates none. A call to one of these entries is
// therefore a contract break, and the entry says so once and loses the device, as the entries behind the
// zero TEXTURE_LAYOUT and content-protection caps do.
constexpr char kCalcPrivateShaderCacheSessionSize[]="CalcPrivateShaderCacheSessionSize";
constexpr char kCreateShaderCacheSession[]="CreateShaderCacheSession";
constexpr char kDestroyShaderCacheSession[]="DestroyShaderCacheSession";
constexpr char kSetShaderCacheSession[]="SetShaderCacheSession";

void APIENTRY relocate22(D3D10DDI_HDEVICE h,D3DWDDM2_2DDI_DEVICEFUNCS *destination) {
    auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if (!storage || !storage->owner) return;
    RuntimeDomain::Scope scope(storage->owner->runtime().domain);
    // The shell caches no runtime table pointer and uses no per-device patches.
    if (destination) *destination=make_wddm2_2_device_table();
}
// WDDM 2.1 sync tokens. The runtime hands the shell a token for a resource that another process shares with
// this one, a composition surface above all. The kernel serialises the two sides by that token, so the one
// correct answer is to pass it on: AcquireResourceCb before the commands that use the resource, and
// ReleaseResourceCb after they are submitted. Both entries return nothing and their reference pages name no
// status of their own, so a refusal from the kernel costs the device (the baseline of handling-errors.md).
//
// What the release order rests on, so that a later reader can measure it instead of trusting it: the shell
// submits through the engine's own contexts, which the hosted ICD owns and does not name to the shell, and
// D3DDDICB_SYNCTOKEN names no context either (BroadcastContextArray is for a link adapter). The flush below
// therefore gives the right order only while the kernel puts the token release behind the work every context
// of this device has already submitted. No shared-surface client of the lab exercises this path yet, so it is
// untested there. Below the WDDM 2.1 interface the runtime called these callbacks itself with no flush of the
// driver at all, so this is not weaker than what the shell did before.
void APIENTRY acquire_resource(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE,HANDLE token) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &o=owner22(h);
        auto &runtime=o.runtime();
        if (!runtime.KTCallbacks.pfnAcquireResourceCb) { report_ddi_error(o,D3DDDIERR_DEVICEREMOVED); return; }
        // One context, no link adapter: the token goes to no other context.
        D3DDDICB_SYNCTOKEN request{}; request.hSyncToken=token;
        const HRESULT hr=runtime.KTCallbacks.pfnAcquireResourceCb(runtime.hDevice,&request);
        if (FAILED(hr)) report_ddi_error(o,hr);
    });
}
void APIENTRY release_resource(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE,HANDLE token) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto &o=owner22(h);
        auto &runtime=o.runtime();
        if (!runtime.KTCallbacks.pfnReleaseResourceCb) { report_ddi_error(o,D3DDDIERR_DEVICEREMOVED); return; }
        // The other side may start as soon as the token is released, so every command that reads or writes
        // the resource must reach the kernel first. Flush submits the engine's commands; it is not a CPU wait.
        context.Flush();
        D3DDDICB_SYNCTOKEN request{}; request.hSyncToken=token;
        const HRESULT hr=runtime.KTCallbacks.pfnReleaseResourceCb(runtime.hDevice,&request);
        if (FAILED(hr)) report_ddi_error(o,hr);
    });
}

// DXGI 1.6.1. The table keeps the DXGI 1.4 layout and appends one entry, so the copied entries stay where
// they were. Three of them change: OfferResources becomes OfferResources1 with the offer flags, and the two
// Present entries take the 1_6_1 arguments, which carry a rotation hint in place of a reserved field.
static_assert(sizeof(DXGI1_6_1_DDI_BASE_FUNCTIONS)==sizeof(DXGI1_4_DDI_BASE_FUNCTIONS)+sizeof(void *));
static_assert(offsetof(DXGI1_6_1_DDI_BASE_FUNCTIONS,pfnReclaimResources1)==sizeof(DXGI1_4_DDI_BASE_FUNCTIONS));
#define SAME_DXGI_OFFSET(newer,older) static_assert(offsetof(DXGI1_6_1_DDI_BASE_FUNCTIONS,newer)==offsetof(DXGI1_4_DDI_BASE_FUNCTIONS,older))
SAME_DXGI_OFFSET(pfnPresent,pfnPresent); SAME_DXGI_OFFSET(pfnBlt1,pfnBlt1);
SAME_DXGI_OFFSET(pfnOfferResources1,pfnOfferResources); SAME_DXGI_OFFSET(pfnReclaimResources,pfnReclaimResources);
SAME_DXGI_OFFSET(pfnPresent1,pfnPresent1); SAME_DXGI_OFFSET(pfnTrimResidencySet,pfnTrimResidencySet);
SAME_DXGI_OFFSET(pfnPresentMultiplaneOverlay1,pfnPresentMultiplaneOverlay1);
#undef SAME_DXGI_OFFSET
const DXGI1_4_DDI_BASE_FUNCTIONS &dxgi1_4_base() { static const auto table=make_dxgi1_4_device_table(); return table; }
// One surface is the Present of DXGI 1.2. present_arguments_1_6_1 (ddi-wddm22.h) carries the arguments over and
// the host test reads every field it carries.
HRESULT APIENTRY present_1_6_1(DXGI1_6_1_DDI_ARG_PRESENT *a) {
    if (!a || !a->hDevice) return E_INVALIDARG;
    DXGI_DDI_ARG_PRESENT p{};
    if (!present_arguments_1_6_1(*a,p)) return DXGI_ERROR_UNSUPPORTED;
    return dxgi1_4_base().pfnPresent(&p);
}
HRESULT APIENTRY overlay_present_1_6_1(DXGI1_6_1_DDI_ARG_PRESENTMULTIPLANEOVERLAY *) { return DXGI_ERROR_UNSUPPORTED; }
}
D3DWDDM2_2DDI_DEVICEFUNCS make_wddm2_2_device_table() {
    D3DWDDM2_2DDI_DEVICEFUNCS t{};
    const auto &b=wddm2_0_base();
    std::memcpy(&t,&b,sizeof(b));
    // The one retyped entry; the copied pointer there has the WDDM 2.0 signature.
    t.pfnRelocateDeviceFuncs=relocate22;
    t.pfnAcquireResource=acquire_resource;
    t.pfnReleaseResource=release_resource;
    t.pfnCalcPrivateShaderCacheSessionSize=
        Unexpected<PFND3DWDDM2_2DDI_CALCPRIVATE_SHADERCACHE_SESSION_SIZE,kCalcPrivateShaderCacheSessionSize>::call;
    t.pfnCreateShaderCacheSession=
        Unexpected<PFND3DWDDM2_2DDI_CREATE_SHADERCACHE_SESSION,kCreateShaderCacheSession>::call;
    t.pfnDestroyShaderCacheSession=
        Unexpected<PFND3DWDDM2_2DDI_DESTROY_SHADERCACHE_SESSION,kDestroyShaderCacheSession>::call;
    t.pfnSetShaderCacheSession=
        Unexpected<PFND3DWDDM2_2DDI_SET_SHADERCACHE_SESSION,kSetShaderCacheSession>::call;
    return t;
}
DXGI1_6_1_DDI_BASE_FUNCTIONS make_dxgi1_6_1_device_table() {
    DXGI1_6_1_DDI_BASE_FUNCTIONS t{};
    const auto &b=dxgi1_4_base();
    std::memcpy(&t,&b,sizeof(b));
    t.pfnPresent1=present_1_6_1;
    t.pfnPresentMultiplaneOverlay1=overlay_present_1_6_1;
    install_dxgi1_6_1_resource_ddi(t);
    return t;
}
}
