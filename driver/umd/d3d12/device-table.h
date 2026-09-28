// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-0092-layout.h"
#include "engine-ddi/engine-ddi.h"
#include <tuple>

namespace native12 {
namespace table_detail {
using Core = D3D12DDI_DEVICE_FUNCS_CORE_0088;
// Exact shell ownership for engine-ddi boundary r3, SLOTS.md. Member pointers
// retain each WDK function type; no table slot is accessed through a cast.
inline constexpr auto shell_core_members = std::tuple{
    &Core::pfnCalcPrivateCommandQueueSize,
    &Core::pfnCreateCommandQueue,
    &Core::pfnDestroyCommandQueue,
    &Core::pfnCalcPrivateFenceSize,
    &Core::pfnCreateFence,
    &Core::pfnDestroyFence,
    &Core::pfnMakeResident,
    &Core::pfnEvict,
    &Core::pfnOfferResources,
    &Core::pfnReclaimResources,
    &Core::pfnGetImplicitPhysicalAdapterMask,
    &Core::pfnGetPresentPrivateDriverDataSize,
    &Core::pfnQueryNodeMap,
    &Core::pfnGetDebugAllocationInfo,
    &Core::pfnCalcPrivateSchedulingGroupSize,
    &Core::pfnCreateSchedulingGroup,
    &Core::pfnDestroySchedulingGroup,
    &Core::pfnSetBackgroundProcessingMode
};
static_assert(engine_ddi::kBoundaryRevision == 3,
    "Recheck table ownership when the engine boundary changes");
static_assert(std::tuple_size_v<decltype(shell_core_members)> == 18);
} // namespace table_detail

// Builds a private complete table before touching the destination. The caller
// supplies all 18 shell entries, including honest refusals for unimplemented
// operations. A missing entry fails closed. Engine-owned fields in shell are
// ignored, so stale storage cannot replace an engine implementation.
//
// S_OK proves composition only. Before publishing, the caller must own a live
// engine DeviceContext, working ShellHooks, queue binding/submission and the
// other required DDI tables. Merely supplying a non-null function is not proof
// that its operation works. This helper does not publish to a runtime or change
// the adapter's admission gate. The engine resolver is process-wide and must
// remain identical across adapters and devices.
inline HRESULT compose_core_0092(D3D12DDI_DEVICE_FUNCS_CORE_0088* output,
    SIZE_T size, const D3D12DDI_DEVICE_FUNCS_CORE_0088& shell,
    const engine_ddi::FillInfo& info) noexcept {
    if (!output || size != sizeof(*output) || info.size != sizeof(info) || !info.resolve)
        return E_INVALIDARG;
    const bool complete = std::apply([&](auto... member) {
        return ((shell.*member != nullptr) && ...);
    }, table_detail::shell_core_members);
    if (!complete) return E_NOTIMPL;
    D3D12DDI_DEVICE_FUNCS_CORE_0088 candidate{};
    const HRESULT hr = engine_ddi::fill_device_core(&candidate, sizeof(candidate), &info);
    if (hr != S_OK) return hr;
    std::apply([&](auto... member) {
        ((candidate.*member = shell.*member), ...);
    }, table_detail::shell_core_members);
    *output = candidate;
    return S_OK;
}

// The two tables are compute (0) and graphics (1). Present belongs to the shell
// even though engine-ddi installs a rejecting placeholder. Require its explicit
// implementation so a missing Present cannot accidentally pass composition.
inline HRESULT compose_list_0092(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* output,
    SIZE_T size, UINT number, PFND3D12DDI_PRESENT_0051 present,
    const engine_ddi::FillInfo& info) noexcept {
    if (!output || size != sizeof(*output) || number > 1 ||
        info.size != sizeof(info) || !info.resolve)
        return E_INVALIDARG;
    if (!present) return E_NOTIMPL;
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 candidate{};
    const HRESULT hr = engine_ddi::fill_command_list(&candidate, sizeof(candidate), number, &info);
    if (hr != S_OK) return hr;
    candidate.pfnPresent = present;
    *output = candidate;
    return S_OK;
}
} // namespace native12
