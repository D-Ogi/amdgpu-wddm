// SPDX-License-Identifier: MIT
#include "ddi-wddm26.h"
#include "ddi-wddm22.h"
#include "ddi-unexpected.h"
#include <cstddef>
#include <cstring>
#include <type_traits>
namespace bc250::umd {
namespace {
// The WDDM 2.6 table is the WDDM 2.2 table plus two scan-out entries. Only RelocateDeviceFuncs changes type,
// and it carries this table in place of the WDDM 2.2 one.
static_assert(offsetof(D3DWDDM2_6DDI_DEVICEFUNCS,pfnQueryScanoutCaps)==sizeof(D3DWDDM2_2DDI_DEVICEFUNCS));
static_assert(sizeof(D3DWDDM2_6DDI_DEVICEFUNCS)==sizeof(D3DWDDM2_2DDI_DEVICEFUNCS)+2*sizeof(void *));
#define SAME_OFFSET(field) static_assert(offsetof(D3DWDDM2_6DDI_DEVICEFUNCS,field)==offsetof(D3DWDDM2_2DDI_DEVICEFUNCS,field))
SAME_OFFSET(pfnDefaultConstantBufferUpdateSubresourceUP); SAME_OFFSET(pfnFlush); SAME_OFFSET(pfnRelocateDeviceFuncs);
SAME_OFFSET(pfnCreateShaderResourceView); SAME_OFFSET(pfnCreateRenderTargetView); SAME_OFFSET(pfnCreateRasterizerState);
SAME_OFFSET(pfnCreateQuery); SAME_OFFSET(pfnCreateUnorderedAccessView); SAME_OFFSET(pfnCheckMultisampleQualityLevels);
SAME_OFFSET(pfnCheckDirectFlipSupport); SAME_OFFSET(pfnClearView); SAME_OFFSET(pfnUpdateTileMappings);
SAME_OFFSET(pfnSetHardwareProtectionState); SAME_OFFSET(pfnAcquireResource); SAME_OFFSET(pfnReleaseResource);
SAME_OFFSET(pfnCalcPrivateShaderCacheSessionSize); SAME_OFFSET(pfnSetShaderCacheSession);
#undef SAME_OFFSET
// Only the relocation entry differs in type. tools/quality/ddi_table_versions.py checks the whole list of entries
// against the WDK header.
static_assert(!std::is_same_v<decltype(D3DWDDM2_6DDI_DEVICEFUNCS::pfnRelocateDeviceFuncs),
                              decltype(D3DWDDM2_2DDI_DEVICEFUNCS::pfnRelocateDeviceFuncs)>);
static_assert(std::is_same_v<decltype(D3DWDDM2_6DDI_DEVICEFUNCS::pfnSetShaderCacheSession),
                             decltype(D3DWDDM2_2DDI_DEVICEFUNCS::pfnSetShaderCacheSession)>);
// The UM callbacks of WDDM 2.6 extend the WDDM 2.2 ones; the shell reads only SetErrorCb, at the offset the
// D3D10 structure gives it (device-owner.cpp copies that prefix).
static_assert(offsetof(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS,pfnSetErrorCb)==offsetof(D3D10DDI_CORELAYER_DEVICECALLBACKS,pfnSetErrorCb) &&
    sizeof(D3DWDDM2_6DDI_CORELAYER_DEVICECALLBACKS)>=sizeof(D3DWDDM2_2DDI_CORELAYER_DEVICECALLBACKS));

// The WDDM 2.2 implementation every copied entry comes from.
const D3DWDDM2_2DDI_DEVICEFUNCS &wddm2_2_base() { static const auto table=make_wddm2_2_device_table(); return table; }
constexpr char kPrepareScanoutTransformation[]="PrepareScanoutTransformation";

void APIENTRY relocate26(D3D10DDI_HDEVICE h,D3DWDDM2_6DDI_DEVICEFUNCS *destination) {
    auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if (!storage || !storage->owner) return;
    RuntimeDomain::Scope scope(storage->owner->runtime().domain);
    // The shell caches no runtime table pointer and uses no per-device patches.
    if (destination) *destination=make_wddm2_6_device_table();
}
// The runtime calls this entry before a full-screen or multiplane-overlay flip (the PFND3DWDDM2_6DDI_QUERY_SCANOUT_CAPS
// page in ref/ddi-display/d3d10umddi.md). No surface of this shell needs a scan-out transformation, and the shell
// asks for none. Without UNPREDICTABLE_TIMING the answer also states that the display engine reads the surface at
// the ordinary scan-out rate, which is what it does. The answer depends on no argument, so the entry reads none and
// touches no device state. The entry returns nothing and its page names no status, so there is nothing to report.
void APIENTRY query_scanout_caps(D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE,UINT,D3DDDI_VIDEO_PRESENT_SOURCE_ID,UINT,
    D3DWDDM2_6DDI_SCANOUT_FLAGS *flags) {
    if (flags) *flags=kScanoutFlags;
}
}
D3DWDDM2_6DDI_DEVICEFUNCS make_wddm2_6_device_table() {
    D3DWDDM2_6DDI_DEVICEFUNCS t{};
    const auto &b=wddm2_2_base();
    std::memcpy(&t,&b,sizeof(b));
    // The one retyped entry; the copied pointer there has the WDDM 2.2 signature.
    t.pfnRelocateDeviceFuncs=relocate26;
    t.pfnQueryScanoutCaps=query_scanout_caps;
    t.pfnPrepareScanoutTransformation=
        Unexpected<PFND3DWDDM2_6DDI_PREPARE_SCANOUT_TRANSFORMATION,kPrepareScanoutTransformation>::call;
    return t;
}
}
