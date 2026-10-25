// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>

#if defined(D3D12DDI_BUILD_VERSION_0119)
#include <atomic>
#include <type_traits>

namespace native12 {

// Optional engine entries. A missing entry is a recording error, never a no-op.
// These signatures are the driver's handles, including when the runtime uses 0114.
struct ModernListEntries {
    PFND3D12DDI_OM_SETFRONTANDBACKSTENCILREF_0095 pfnOmSetFrontAndBackStencilRef{};
    PFND3D12DDI_SET_DEPTH_BIAS_STATE_0099 pfnRSSetDepthBias{};
    PFND3D12DDI_SET_INDEX_BUFFER_STRIP_CUT_VALUE_0099 pfnIASetIndexBufferStripCutValue{};
    PFND3D12DDI_SET_PROGRAM_0108 pfnSetProgram{};
    PFND3D12DDI_DISPATCH_GRAPH_0108 pfnDispatchGraph{};
};
using ModernListError = void(APIENTRY*)(D3D12DDI_HCOMMANDLIST, HRESULT);

namespace modern_list_detail {
// WDK 10.0.28000.2526 d3d12umddi.h:11457-11535. Copy each member by name;
// a future header layout cannot silently change the table prefix.
#define N12_MODERN_LIST_0094(X) \
    X(pfnCloseCommandList) \
    X(pfnResetCommandList) \
    X(pfnDrawInstanced) \
    X(pfnDrawIndexedInstanced) \
    X(pfnDispatch) \
    X(pfnClearUnorderedAccessViewUint) \
    X(pfnClearUnorderedAccessViewFloat) \
    X(pfnClearRenderTargetView) \
    X(pfnClearDepthStencilView) \
    X(pfnDiscardResource) \
    X(pfnCopyTextureRegion) \
    X(pfnResourceCopy) \
    X(pfnCopyTiles) \
    X(pfnCopyBufferRegion) \
    X(pfnResourceResolveSubresource) \
    X(pfnExecuteBundle) \
    X(pfnExecuteIndirect) \
    X(pfnResourceBarrier) \
    X(pfnBlt) \
    X(pfnPresent) \
    X(pfnBeginQuery) \
    X(pfnEndQuery) \
    X(pfnResolveQueryData) \
    X(pfnSetPredication) \
    X(pfnIaSetTopology) \
    X(pfnRsSetViewports) \
    X(pfnRsSetScissorRects) \
    X(pfnOmSetBlendFactor) \
    X(pfnOmSetStencilRef) \
    X(pfnSetPipelineState) \
    X(pfnSetDescriptorHeaps) \
    X(pfnSetComputeRootSignature) \
    X(pfnSetGraphicsRootSignature) \
    X(pfnSetComputeRootDescriptorTable) \
    X(pfnSetGraphicsRootDescriptorTable) \
    X(pfnSetComputeRoot32BitConstant) \
    X(pfnSetGraphicsRoot32BitConstant) \
    X(pfnSetComputeRoot32BitConstants) \
    X(pfnSetGraphicsRoot32BitConstants) \
    X(pfnSetComputeRootConstantBufferView) \
    X(pfnSetGraphicsRootConstantBufferView) \
    X(pfnSetComputeRootShaderResourceView) \
    X(pfnSetGraphicsRootShaderResourceView) \
    X(pfnSetComputeRootUnorderedAccessView) \
    X(pfnSetGraphicsRootUnorderedAccessView) \
    X(pfnIASetIndexBuffer) \
    X(pfnIASetVertexBuffers) \
    X(pfnSOSetTargets) \
    X(pfnOMSetRenderTargets) \
    X(pfnSetMarker) \
    X(pfnClearRootArguments) \
    X(pfnAtomicCopyBufferRegion) \
    X(pfnOMSetDepthBounds) \
    X(pfnSetSamplePositions) \
    X(pfnResourceResolveSubresourceRegion) \
    X(pfnSetProtectedResourceSession) \
    X(pfnWriteBufferImmediate) \
    X(pfnSetViewInstanceMask) \
    X(pfnInitializeMetaCommand) \
    X(pfnExecuteMetaCommand) \
    X(pfnBuildRaytracingAccelerationStructure) \
    X(pfnEmitRaytracingAccelerationStructurePostbuildInfo) \
    X(pfnCopyRaytracingAccelerationStructure) \
    X(pfnSetPipelineState1) \
    X(pfnDispatchRays) \
    X(pfnRSSetShadingRate) \
    X(pfnRSSetShadingRateImage) \
    X(pfnDispatchMesh) \
    X(pfnBarrier) \
    X(pfnOmSetAlphaBlendFactor)

#define N12_MODERN_LIST_0095(X) X(pfnOmSetFrontAndBackStencilRef)
#define N12_MODERN_LIST_0099(X) X(pfnRSSetDepthBias) X(pfnIASetIndexBufferStripCutValue)
#define N12_MODERN_LIST_0108(X) X(pfnSetProgram) X(pfnDispatchGraph)

template<class T> inline T driver_argument(T value) noexcept { return value; }
// The WDK helpers dereference the API object's bypass header. Nullable API
// handles must stay null, especially SetPipelineState and SetRootSignature.
inline D3D12DDI_HCOMMANDLIST driver_argument(D3D12DDI_API_HCOMMANDLIST value) noexcept {
    return value.pDrvPrivate ? GetDriverCommandListHandle(value) : D3D12DDI_HCOMMANDLIST{};
}
inline D3D12DDI_HPIPELINESTATE driver_argument(D3D12DDI_API_HPIPELINESTATE value) noexcept {
    return value.pDrvPrivate ? GetDriverPipelineStateHandle(value) : D3D12DDI_HPIPELINESTATE{};
}
inline D3D12DDI_HROOTSIGNATURE driver_argument(D3D12DDI_API_HROOTSIGNATURE value) noexcept {
    return value.pDrvPrivate ? GetDriverRootSignatureHandle(value) : D3D12DDI_HROOTSIGNATURE{};
}
inline D3D12DDI_HSTATEOBJECT_0054 driver_argument(D3D12DDI_API_HSTATEOBJECT value) noexcept {
    return value.pDrvPrivate ? GetDriverStateObjectHandle(value) : D3D12DDI_HSTATEOBJECT_0054{};
}

template<class Table, unsigned Number> struct Store {
    using Base = D3D12DDI_COMMAND_LIST_FUNCS_3D_0108;
    inline static Base source{};
    inline static ModernListEntries entries{};
    inline static ModernListError error{};
    inline static std::atomic<bool> ready{};
    inline static SRWLOCK lock = SRWLOCK_INIT;

    struct Lock {
        Lock() noexcept { AcquireSRWLockExclusive(&lock); }
        ~Lock() { ReleaseSRWLockExclusive(&lock); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;
    };

    static void refuse(D3D12DDI_HCOMMANDLIST list, HRESULT reason = E_NOTIMPL) noexcept {
        if (ready.load(std::memory_order_acquire)) error(list, reason);
    }
    static void APIENTRY stencil(D3D12DDI_HCOMMANDLIST list, UINT, UINT) { refuse(list); }
    static void APIENTRY depth_bias(D3D12DDI_HCOMMANDLIST list, FLOAT, FLOAT, FLOAT) { refuse(list); }
    static void APIENTRY strip_cut(D3D12DDI_HCOMMANDLIST list, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE) {
        refuse(list);
    }
    static void APIENTRY program(D3D12DDI_HCOMMANDLIST list, const D3D12DDI_SET_PROGRAM_DESC_0108*) {
        refuse(list);
    }
    static void APIENTRY graph(D3D12DDI_HCOMMANDLIST list, const D3D12DDI_DISPATCH_GRAPH_DESC_0108*) {
        refuse(list);
    }

    // 0114 changes 39 function signatures. Signature deduction keeps all
    // arguments typed; only the four API handle types undergo conversion.
    template<class Fn, auto Member> struct ApiThunk;
    template<class... Args, auto Member>
    struct ApiThunk<void(APIENTRY*)(D3D12DDI_API_HCOMMANDLIST, Args...), Member> {
        static void APIENTRY call(D3D12DDI_API_HCOMMANDLIST list, Args... args) {
            if (!ready.load(std::memory_order_acquire)) return;
            const auto driver_list = driver_argument(list);
            if (!driver_list.pDrvPrivate) {
                refuse(driver_list, E_INVALIDARG);
                return;
            }
            const auto fn = source.*Member;
            if (!fn) {
                refuse(driver_list);
                return;
            }
            fn(driver_list, driver_argument(args)...);
        }
    };
    template<class Fn, auto Member> static Fn bind(const Base& base) noexcept {
        if constexpr (std::is_same_v<Fn, std::remove_cv_t<std::remove_reference_t<decltype(base.*Member)>>>) {
            return base.*Member;
        } else {
            return &ApiThunk<Fn, Member>::call;
        }
    }

    static bool upgrade(const D3D12DDI_COMMAND_LIST_FUNCS_3D_0094& input, Table* output,
                        ModernListError report, const ModernListEntries& extra) noexcept {
        static_assert(std::is_same_v<Table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0095> ||
                      std::is_same_v<Table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0099> ||
                      std::is_same_v<Table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0108> ||
                      std::is_same_v<Table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0114>,
                      "Unsupported command-list layout");
        if (!output || !report) return false;
        Lock guard;
        if (ready.load(std::memory_order_acquire)) {
#define N12_COMPARE(m) if (source.m != input.m) return false;
            N12_MODERN_LIST_0094(N12_COMPARE)
#undef N12_COMPARE
#define N12_COMPARE(m) if (entries.m != extra.m) return false;
            N12_MODERN_LIST_0095(N12_COMPARE)
            N12_MODERN_LIST_0099(N12_COMPARE)
            N12_MODERN_LIST_0108(N12_COMPARE)
#undef N12_COMPARE
            if (error != report) return false;
        } else {
#define N12_COPY(m) source.m = input.m;
            N12_MODERN_LIST_0094(N12_COPY)
#undef N12_COPY
            source.pfnOmSetFrontAndBackStencilRef = extra.pfnOmSetFrontAndBackStencilRef ?
                extra.pfnOmSetFrontAndBackStencilRef : &stencil;
            source.pfnRSSetDepthBias = extra.pfnRSSetDepthBias ? extra.pfnRSSetDepthBias : &depth_bias;
            source.pfnIASetIndexBufferStripCutValue = extra.pfnIASetIndexBufferStripCutValue ?
                extra.pfnIASetIndexBufferStripCutValue : &strip_cut;
            source.pfnSetProgram = extra.pfnSetProgram ? extra.pfnSetProgram : &program;
            source.pfnDispatchGraph = extra.pfnDispatchGraph ? extra.pfnDispatchGraph : &graph;
            entries = extra;
            error = report;
            ready.store(true, std::memory_order_release);
        }
        Table result{};
#define N12_BIND(m) result.m = bind<decltype(result.m), &Base::m>(source);
        N12_MODERN_LIST_0094(N12_BIND)
        N12_MODERN_LIST_0095(N12_BIND)
        if constexpr (!std::is_same_v<Table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0095>) {
            N12_MODERN_LIST_0099(N12_BIND)
        }
        if constexpr (std::is_same_v<Table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0108> ||
                      std::is_same_v<Table, D3D12DDI_COMMAND_LIST_FUNCS_3D_0114>) {
            N12_MODERN_LIST_0108(N12_BIND)
        }
#undef N12_BIND
        *output = result;
        return true;
    }
};
#undef N12_MODERN_LIST_0094
#undef N12_MODERN_LIST_0095
#undef N12_MODERN_LIST_0099
#undef N12_MODERN_LIST_0108
} // namespace modern_list_detail

// A source and its callback are immutable after first publication, separately
// for each layout and table number. A mismatch fails without changing output.
template<class Table>
bool upgrade_list(const D3D12DDI_COMMAND_LIST_FUNCS_3D_0094& source, Table* output, UINT number,
                  ModernListError error, const ModernListEntries& entries = {}) noexcept {
    if (number == 0) return modern_list_detail::Store<Table, 0>::upgrade(source, output, error, entries);
    if (number == 1) return modern_list_detail::Store<Table, 1>::upgrade(source, output, error, entries);
    return false;
}
} // namespace native12
#endif // D3D12DDI_BUILD_VERSION_0119
