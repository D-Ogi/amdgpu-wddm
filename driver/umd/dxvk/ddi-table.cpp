// SPDX-License-Identifier: MIT
#include "ddi-table.h"
#include "ddi-clear-view.h"
#include "ddi-query.h"
#include "ddi-lifecycle.h"
#include "ddi-resource-status.h"
#include "ddi-format.h"
#include "ddi-draw.h"
#include "ddi-input-layout.h"
#include "ddi-raster.h"
#include "ddi-shader.h"
#include "ddi-sampler.h"
#include "ddi-fixed-state.h"
#include "ddi-blend.h"
#include "ddi-resource.h"
#include "ddi-buffer-binding.h"
#include "ddi-transfer.h"
#include "ddi-map.h"
#include "ddi-rtv.h"
#include "ddi-dsv.h"
#include "ddi-uav.h"
#include "ddi-output.h"
#include "ddi-srv.h"
#include "ddi-flush.h"
#include "ddi-direct-flip.h"
#include <type_traits>
namespace bc250::umd {
namespace {
// DXVK inserts resource barriers at actual use; these runtime notifications
// need no additional submission or CPU wait in the shell.
void APIENTRY resource_hazard(D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE) {}
void APIENTRY view_hazard(D3D10DDI_HDEVICE,D3D10DDI_HSHADERRESOURCEVIEW,D3D10DDI_HRESOURCE) {}
// Discard makes contents undefined. Retaining the contents is legal and does
// not release backing before GPU retirement. An optimization can forward later.
void APIENTRY discard(D3D10DDI_HDEVICE,D3D11DDI_HANDLETYPE,void *,const D3D10_DDI_RECT *,UINT) {}
void APIENTRY debug_binary(D3D10DDI_HDEVICE,D3D10DDI_HSHADER,UINT,const void *) {}
// TEXT_1BIT sampling is not exposed by this D3D11 shell.
void APIENTRY text_filter(D3D10DDI_HDEVICE,UINT,UINT) {}
// CheckDirectFlipSupport: may the runtime hand the application's back buffer straight to the window's
// front buffer, with no copy in between. The rule is in ddi-direct-flip.h; here only the handles are
// resolved. CheckDirectFlipFlags carries IMMEDIATE, which does not enter the rule (see the header).
void APIENTRY direct_flip(D3D10DDI_HDEVICE,D3D10DDI_HRESOURCE front,D3D10DDI_HRESOURCE back,
                          UINT,BOOL *supported) {
    if(!supported)return;
    auto *a=static_cast<DdiResource *>(front.pDrvPrivate);
    auto *b=static_cast<DdiResource *>(back.pDrvPrivate);
    *supported=a&&b&&direct_flip_supported(a->runtime_surface,b->runtime_surface)?TRUE:FALSE;
}
// Threading caps do not advertise command lists or deferred contexts. Keep
// exact typed entries so an unexpected runtime call is diagnosed, not NULL.
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
void APIENTRY deferred_sizes(D3D10DDI_HDEVICE,UINT *count,D3D11DDI_HANDLESIZE *) {
    if(count)*count=0;
}
void APIENTRY counter(D3D10DDI_HDEVICE h,D3D10DDI_QUERY,D3D10DDI_COUNTER_TYPE *type,UINT *active,
    LPSTR,UINT *name,LPSTR,UINT *units,LPSTR,UINT *description) {
    if(type)*type={};if(active)*active=0;
    if(name)*name=0;if(units)*units=0;if(description)*description=0;
    auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if(storage && storage->owner) {
        RuntimeDomain::Scope scope(storage->owner->runtime().domain);
        report_ddi_error(*storage->owner,E_INVALIDARG,DdiErrorClass::unsupported_check);
    }
}
template<auto Bind> void APIENTRY shader_ifaces(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER shader,
    UINT count,const UINT *,const D3D11DDIARG_POINTERDATA *) {
    if(count) {
        auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
        if(storage && storage->owner) {
            RuntimeDomain::Scope scope(storage->owner->runtime().domain);
            report_ddi_error(*storage->owner,D3DDDIERR_DEVICEREMOVED);
        }
        return;
    }
    static const auto table=make_render_device_table();
    (table.*Bind)(h,shader);
}
void APIENTRY relocate(D3D10DDI_HDEVICE h,D3D11_1DDI_DEVICEFUNCS *destination) {
    auto *storage=static_cast<DdiDeviceHandle *>(h.pDrvPrivate);
    if (!storage || !storage->owner) return;
    RuntimeDomain::Scope scope(storage->owner->runtime().domain);
    // The shell caches no runtime table pointer and uses no per-device patches.
    // Rebuild the same dispatch table at the destination supplied by runtime.
    if (destination) *destination=make_render_device_table();
}
}
D3D11_1DDI_DEVICEFUNCS make_render_device_table() {
    D3D11_1DDI_DEVICEFUNCS table{};
    install_query_ddi(table);
    table.pfnRelocateDeviceFuncs=relocate;
    install_clear_view_ddi(table);
    install_draw_ddi(table);
    install_input_layout_ddi(table);
    install_raster_ddi(table);
    install_shader_ddi(table);
    install_sampler_ddi(table);
    install_fixed_state_ddi(table);
    install_blend_ddi(table);
    install_resource_ddi(table);
    install_buffer_binding_ddi(table);
    install_transfer_ddi(table);
    install_map_ddi(table);
    install_rtv_ddi(table);
    install_dsv_ddi(table);
    install_uav_ddi(table);
    install_output_ddi(table);
    install_srv_ddi(table);
    install_flush_ddi(table);
    install_format_ddi(table);
    install_resource_status_ddi(table);
    install_lifecycle_ddi(table);
    table.pfnResourceReadAfterWriteHazard=resource_hazard;
    table.pfnShaderResourceViewReadAfterWriteHazard=view_hazard;
    table.pfnResourceConvert=table.pfnResourceCopy;
    table.pfnResourceConvertRegion=table.pfnResourceCopyRegion;
    table.pfnDiscard=discard;
    table.pfnAssignDebugBinary=debug_binary;
    table.pfnSetTextFilterSize=text_filter;
    table.pfnCheckDirectFlipSupport=direct_flip;
    table.pfnCheckCounter=counter;
    table.pfnCheckDeferredContextHandleSizes=deferred_sizes;
#define IFACES(stage) table.pfn##stage##SetShaderWithIfaces=shader_ifaces<&D3D11_1DDI_DEVICEFUNCS::pfn##stage##SetShader>
    IFACES(Vs);IFACES(Ps);IFACES(Gs);IFACES(Hs);IFACES(Ds);IFACES(Cs);
#undef IFACES
#define UNEXPECTED(field) static constexpr char name_##field[]=#field; table.field=Unexpected<decltype(table.field),name_##field>::call
    UNEXPECTED(pfnCommandListExecute);
    UNEXPECTED(pfnCalcDeferredContextHandleSize);
    UNEXPECTED(pfnCalcPrivateDeferredContextSize);
    UNEXPECTED(pfnCreateDeferredContext);
    UNEXPECTED(pfnAbandonCommandList);
    UNEXPECTED(pfnCalcPrivateCommandListSize);
    UNEXPECTED(pfnCreateCommandList);
    UNEXPECTED(pfnDestroyCommandList);
    UNEXPECTED(pfnRecycleCommandList);
    UNEXPECTED(pfnRecycleCreateCommandList);
    UNEXPECTED(pfnRecycleCreateDeferredContext);
    UNEXPECTED(pfnRecycleDestroyCommandList);
#undef UNEXPECTED
    return table;
}
}
