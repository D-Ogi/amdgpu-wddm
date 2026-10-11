// SPDX-License-Identifier: MIT
#pragma once
#include "device-state.h"

namespace native12 {
namespace shell_core_detail {
inline Device* device(D3D12DDI_HDEVICE h) noexcept {
    return static_cast<Device*>(h.pDrvPrivate);
}
inline HRESULT unsupported(D3D12DDI_HDEVICE h) noexcept {
    auto d = device(h);
    if (!d) return E_INVALIDARG;
    return d->lost.load() ? D3DDDIERR_DEVICEREMOVED : E_NOTIMPL;
}
inline void reject_void(D3D12DDI_HDEVICE h) noexcept {
    if (auto d = device(h)) d->remove();
}
} // namespace shell_core_detail

// This shell supports one physical adapter and one logical node. The mask is
// bit0; QueryNodeMap contains physical adapter indices, not bit masks.
// Microsoft display/gpu-paravirtualization.md, "QueryNodeMap"; WDK10.0.26100
// d3d12umddi.h:2710,2722-2724 defines the mask callback and HIDE_NODE sentinel.
inline UINT APIENTRY implicit_physical_adapter_mask(D3D12DDI_HDEVICE h) {
    auto d = shell_core_detail::device(h);
    return d && !d->lost.load() ? 1u : 0u;
}
inline void APIENTRY query_node_map(D3D12DDI_HDEVICE h, UINT count, UINT* map) {
    auto d = shell_core_detail::device(h);
    if (map) {
        // On failure, hide all caller-provided entries rather than exposing a
        // node that this single-adapter device cannot address.
        for (UINT i = 0; i < count; ++i) map[i] = D3D12DDI_NODE_MAP_HIDE_NODE;
    }
    if (!d || d->lost.load() || count != 1 || !map) {
        shell_core_detail::reject_void(h);
        return;
    }
    map[0] = 0;
}

// No offer/reclaim implementation exists for the shell's runtime allocation
// records yet. Refuse without modifying the caller's records or output arrays.
// These HRESULT DDIs do not require pretending that resources were discarded.
inline HRESULT APIENTRY offer_resources(D3D12DDI_HDEVICE h,
    const D3D12DDIARG_OFFERRESOURCES* args) {
    if (!args) return E_INVALIDARG;
    return shell_core_detail::unsupported(h);
}
inline HRESULT APIENTRY reclaim_resources(D3D12DDI_HDEVICE h,
    D3D12DDIARG_RECLAIMRESOURCES_0001* args) {
    if (!args) return E_INVALIDARG;
    return shell_core_detail::unsupported(h);
}

// ComputeQueuesPer3DQueue is advertised as0: "don't use scheduling groups"
// (WDK10.0.26100 d3d12umddi.h:7007). No scheduling-group storage is constructed.
// A zero private size is not used as a successful zero-storage implementation:
// the size call also reports loss, and Create explicitly refuses the request.
inline SIZE_T APIENTRY scheduling_group_size(D3D12DDI_HDEVICE h,
    const D3D12DDIARG_CREATESCHEDULINGGROUP_0050*) {
    shell_core_detail::reject_void(h);
    return 0;
}
inline HRESULT APIENTRY scheduling_group_create(D3D12DDI_HDEVICE h,
    const D3D12DDIARG_CREATESCHEDULINGGROUP_0050* args,
    D3D12DDI_HSCHEDULINGGROUP_0050, D3D12DDI_HRTSCHEDULINGGROUP_0050) {
    if (!args) return E_INVALIDARG;
    return shell_core_detail::unsupported(h);
}
inline void APIENTRY scheduling_group_destroy(D3D12DDI_HDEVICE h,
    D3D12DDI_HSCHEDULINGGROUP_0050) {
    // No object can have been created successfully by this implementation.
    shell_core_detail::reject_void(h);
}

// D91 and D114 are void slots with no error channel at all: the only thing either can
// hand back is its output (WDK10.0.26100 d3d12umddi.h:3898-3904 and :8828-8833). A
// removed device was therefore the wrong answer for both, and the most expensive one.
// Both are debug-layer and tool calls: the debug layer asks for allocation info per
// object, and a profiler sets the background processing mode. Removing the device there
// turns a tool's question into DXGI_ERROR_DRIVER_INTERNAL_ERROR for the application,
// which is the BD-075 class of defect (DEFECTS.md), not a refusal. Each answers its
// output completely, applies nothing, and writes one line per process.
inline std::atomic<bool> debug_allocation_noted{false};
inline void APIENTRY debug_allocation_info(D3D12DDI_HDEVICE h,
    D3D12DDI_HANDLE_AND_TYPE, UINT* va_count,
    D3D12DDI_DEBUG_VIRTUAL_ADDRESS_ALLOCATION_INFO_0012*, UINT* kmt_count,
    D3D12DDI_DEBUG_KMT_ALLOCATION_INFO_0014*) {
    // This driver publishes no allocation inventory to the debug layer, so zero records
    // of each kind is the whole answer. Never invent a resource mapping: the arrays stay
    // untouched, because a count of zero says nothing was written into them.
    if (va_count) *va_count = 0;
    if (kmt_count) *kmt_count = 0;
    if (!shell_core_detail::device(h)) return;
    if (!debug_allocation_noted.exchange(true, std::memory_order_relaxed))
        ddi_refusal("GetDebugAllocationInfo: no allocation info is published; zero records");
}
inline std::atomic<bool> background_processing_noted{false};
inline void APIENTRY background_processing_mode(D3D12DDI_HDEVICE h,
    D3D12DDI_BACKGROUND_PROCESSING_MODE_0062 mode,
    D3D12DDI_MEASUREMENTS_ACTION_0062, BOOL* further_measurements) {
    // BackgroundProcessingSupported is FALSE in the advertised options, which is the
    // statement that no mode is applied; the engine answers that field FALSE itself and
    // its own SetBackgroundProcessingMode is a stub. The call is accepted and dropped,
    // as pfnSetMarker is, and no further measurements are desired.
    if (further_measurements) *further_measurements = FALSE;
    if (!shell_core_detail::device(h)) return;
    if (!background_processing_noted.exchange(true, std::memory_order_relaxed))
        ddi_refusal("SetBackgroundProcessingMode: mode %u accepted and dropped; "
            "BackgroundProcessingSupported is FALSE", static_cast<unsigned>(mode));
}

// Install only these nine slots. Residency and Present private data remain
// unresolved and are deliberately untouched. The caller must still use the
// complete-table composition gate before publishing anything to the runtime.
inline void install_shell_core_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088& table) noexcept {
    table.pfnGetImplicitPhysicalAdapterMask = implicit_physical_adapter_mask;
    table.pfnQueryNodeMap = query_node_map;
    table.pfnOfferResources = offer_resources;
    table.pfnReclaimResources = reclaim_resources;
    table.pfnCalcPrivateSchedulingGroupSize = scheduling_group_size;
    table.pfnCreateSchedulingGroup = scheduling_group_create;
    table.pfnDestroySchedulingGroup = scheduling_group_destroy;
    table.pfnGetDebugAllocationInfo = debug_allocation_info;
    table.pfnSetBackgroundProcessingMode = background_processing_mode;
}
} // namespace native12
