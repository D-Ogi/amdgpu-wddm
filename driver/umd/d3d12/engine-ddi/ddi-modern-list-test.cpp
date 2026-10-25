// SPDX-License-Identifier: MIT
#include "ddi-modern-list.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(D3D12DDI_BUILD_VERSION_0119)
namespace {
using Source = D3D12DDI_COMMAND_LIST_FUNCS_3D_0094;
using L95 = D3D12DDI_COMMAND_LIST_FUNCS_3D_0095;
using L99 = D3D12DDI_COMMAND_LIST_FUNCS_3D_0099;
using L108 = D3D12DDI_COMMAND_LIST_FUNCS_3D_0108;
using L114 = D3D12DDI_COMMAND_LIST_FUNCS_3D_0114;
unsigned observed = ~0u, calls{}, errors{};
void* observed_list{};
HRESULT last_error{};
int driver_object{}, pipeline_object{}, root_object{}, state_object{};
D3D12DDI_HCOMMANDLIST list{&driver_object};
D3D12DDI_RUNTIME_BYPASS_HEADER api_object{nullptr, &driver_object};
D3D12DDI_API_HCOMMANDLIST api_list{&api_object};

void APIENTRY error(D3D12DDI_HCOMMANDLIST h, HRESULT hr) {
    observed_list = h.pDrvPrivate;
    last_error = hr;
    ++errors;
}
void APIENTRY other_error(D3D12DDI_HCOMMANDLIST, HRESULT) { assert(false); }

template<unsigned Id, class Fn> struct Spy;
template<unsigned Id, class R, class H, class... Args>
struct Spy<Id, R(APIENTRY*)(H, Args...)> {
    static R APIENTRY call(H h, Args...) {
        observed = Id;
        observed_list = h.pDrvPrivate;
        ++calls;
        if constexpr (!std::is_void_v<R>) return R{};
    }
};
template<class H> H command_handle() {
    if constexpr (std::is_same_v<H, D3D12DDI_API_HCOMMANDLIST>) return api_list;
    else return list;
}
template<class Fn> struct Invoke;
template<class R, class H, class... Args>
struct Invoke<R(APIENTRY*)(H, Args...)> {
    static void call(R(APIENTRY* fn)(H, Args...)) {
        fn(command_handle<H>(), Args{}...);
    }
};
#define TEST_SOURCE_MEMBERS(X) \
    X(pfnCloseCommandList, 0) \
    X(pfnResetCommandList, 1) \
    X(pfnDrawInstanced, 2) \
    X(pfnDrawIndexedInstanced, 3) \
    X(pfnDispatch, 4) \
    X(pfnClearUnorderedAccessViewUint, 5) \
    X(pfnClearUnorderedAccessViewFloat, 6) \
    X(pfnClearRenderTargetView, 7) \
    X(pfnClearDepthStencilView, 8) \
    X(pfnDiscardResource, 9) \
    X(pfnCopyTextureRegion, 10) \
    X(pfnResourceCopy, 11) \
    X(pfnCopyTiles, 12) \
    X(pfnCopyBufferRegion, 13) \
    X(pfnResourceResolveSubresource, 14) \
    X(pfnExecuteBundle, 15) \
    X(pfnExecuteIndirect, 16) \
    X(pfnResourceBarrier, 17) \
    X(pfnBlt, 18) \
    X(pfnPresent, 19) \
    X(pfnBeginQuery, 20) \
    X(pfnEndQuery, 21) \
    X(pfnResolveQueryData, 22) \
    X(pfnSetPredication, 23) \
    X(pfnIaSetTopology, 24) \
    X(pfnRsSetViewports, 25) \
    X(pfnRsSetScissorRects, 26) \
    X(pfnOmSetBlendFactor, 27) \
    X(pfnOmSetStencilRef, 28) \
    X(pfnSetPipelineState, 29) \
    X(pfnSetDescriptorHeaps, 30) \
    X(pfnSetComputeRootSignature, 31) \
    X(pfnSetGraphicsRootSignature, 32) \
    X(pfnSetComputeRootDescriptorTable, 33) \
    X(pfnSetGraphicsRootDescriptorTable, 34) \
    X(pfnSetComputeRoot32BitConstant, 35) \
    X(pfnSetGraphicsRoot32BitConstant, 36) \
    X(pfnSetComputeRoot32BitConstants, 37) \
    X(pfnSetGraphicsRoot32BitConstants, 38) \
    X(pfnSetComputeRootConstantBufferView, 39) \
    X(pfnSetGraphicsRootConstantBufferView, 40) \
    X(pfnSetComputeRootShaderResourceView, 41) \
    X(pfnSetGraphicsRootShaderResourceView, 42) \
    X(pfnSetComputeRootUnorderedAccessView, 43) \
    X(pfnSetGraphicsRootUnorderedAccessView, 44) \
    X(pfnIASetIndexBuffer, 45) \
    X(pfnIASetVertexBuffers, 46) \
    X(pfnSOSetTargets, 47) \
    X(pfnOMSetRenderTargets, 48) \
    X(pfnSetMarker, 49) \
    X(pfnClearRootArguments, 50) \
    X(pfnAtomicCopyBufferRegion, 51) \
    X(pfnOMSetDepthBounds, 52) \
    X(pfnSetSamplePositions, 53) \
    X(pfnResourceResolveSubresourceRegion, 54) \
    X(pfnSetProtectedResourceSession, 55) \
    X(pfnWriteBufferImmediate, 56) \
    X(pfnSetViewInstanceMask, 57) \
    X(pfnInitializeMetaCommand, 58) \
    X(pfnExecuteMetaCommand, 59) \
    X(pfnBuildRaytracingAccelerationStructure, 60) \
    X(pfnEmitRaytracingAccelerationStructurePostbuildInfo, 61) \
    X(pfnCopyRaytracingAccelerationStructure, 62) \
    X(pfnSetPipelineState1, 63) \
    X(pfnDispatchRays, 64) \
    X(pfnRSSetShadingRate, 65) \
    X(pfnRSSetShadingRateImage, 66) \
    X(pfnDispatchMesh, 67) \
    X(pfnBarrier, 68) \
    X(pfnOmSetAlphaBlendFactor, 69)

Source source() {
    Source s{};
#define SET(m, id) s.m = &Spy<id, decltype(s.m)>::call;
    TEST_SOURCE_MEMBERS(SET)
#undef SET
    return s;
}
template<class Table> void check_common(const Table& table) {
#define CHECK(m, id) \
    observed = ~0u; \
    Invoke<decltype(table.m)>::call(table.m); \
    assert(observed == id && observed_list == list.pDrvPrivate);
    TEST_SOURCE_MEMBERS(CHECK)
#undef CHECK
}

template<class Table> void check_immutable(const Source& input, Table& output, UINT number,
                                          const native12::ModernListEntries& extras = {}) {
    Table before = output;
    Source changed = input;
    changed.pfnDispatch = &Spy<1000, decltype(changed.pfnDispatch)>::call;
    assert(!native12::upgrade_list(changed, &output, number, error, extras));
    assert(std::memcmp(&output, &before, sizeof(output)) == 0);
    assert(!native12::upgrade_list(input, &output, number, other_error, extras));
    assert(std::memcmp(&output, &before, sizeof(output)) == 0);
    auto changed_extras = extras;
    changed_extras.pfnDispatchGraph = &Spy<1001, decltype(changed_extras.pfnDispatchGraph)>::call;
    assert(!native12::upgrade_list(input, &output, number, error, changed_extras));
    assert(std::memcmp(&output, &before, sizeof(output)) == 0);
}

void APIENTRY pipeline(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HPIPELINESTATE pso) {
    assert(h.pDrvPrivate == list.pDrvPrivate);
    assert(pso.pDrvPrivate == &pipeline_object || !pso.pDrvPrivate);
    observed_list = pso.pDrvPrivate;
}
void APIENTRY root(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HROOTSIGNATURE signature) {
    assert(h.pDrvPrivate == list.pDrvPrivate);
    assert(signature.pDrvPrivate == &root_object || !signature.pDrvPrivate);
    observed_list = signature.pDrvPrivate;
}
void APIENTRY state(D3D12DDI_HCOMMANDLIST h, D3D12DDI_HSTATEOBJECT_0054 object) {
    assert(h.pDrvPrivate == list.pDrvPrivate);
    assert(object.pDrvPrivate == &state_object || !object.pDrvPrivate);
    observed_list = object.pDrvPrivate;
}
void APIENTRY constants(D3D12DDI_HCOMMANDLIST h, UINT index, UINT count, const void* data, UINT offset) {
    assert(h.pDrvPrivate == list.pDrvPrivate);
    assert(index == 7 && count == 3 && data == &root_object && offset == 19);
    ++calls;
}
void check_new_entries(const L108& table, bool supplied) {
    const auto before = errors;
    Invoke<decltype(table.pfnOmSetFrontAndBackStencilRef)>::call(table.pfnOmSetFrontAndBackStencilRef);
    assert(supplied ? observed == 70 : errors == before + 1);
    Invoke<decltype(table.pfnRSSetDepthBias)>::call(table.pfnRSSetDepthBias);
    assert(supplied ? observed == 71 : errors == before + 2);
    Invoke<decltype(table.pfnIASetIndexBufferStripCutValue)>::call(table.pfnIASetIndexBufferStripCutValue);
    assert(supplied ? observed == 72 : errors == before + 3);
    Invoke<decltype(table.pfnSetProgram)>::call(table.pfnSetProgram);
    assert(supplied ? observed == 73 : errors == before + 4);
    Invoke<decltype(table.pfnDispatchGraph)>::call(table.pfnDispatchGraph);
    assert(supplied ? observed == 74 : errors == before + 5);
    assert(observed_list == list.pDrvPrivate);
    if (!supplied) assert(last_error == E_NOTIMPL);
}
native12::ModernListEntries extra_entries() {
    native12::ModernListEntries entries{};
    entries.pfnOmSetFrontAndBackStencilRef = &Spy<70, decltype(entries.pfnOmSetFrontAndBackStencilRef)>::call;
    entries.pfnRSSetDepthBias = &Spy<71, decltype(entries.pfnRSSetDepthBias)>::call;
    entries.pfnIASetIndexBufferStripCutValue = &Spy<72, decltype(entries.pfnIASetIndexBufferStripCutValue)>::call;
    entries.pfnSetProgram = &Spy<73, decltype(entries.pfnSetProgram)>::call;
    entries.pfnDispatchGraph = &Spy<74, decltype(entries.pfnDispatchGraph)>::call;
    return entries;
}
void run() {
    Source input = source();
    L95 t95{};
    L99 t99{};
    L108 t108{};
    L114 t114{};
    assert(!native12::upgrade_list(input, static_cast<L95*>(nullptr), 0, error));
    assert(!native12::upgrade_list(input, &t95, 2, error));
    assert(!native12::upgrade_list(input, &t95, 0, nullptr));

    // Publish newest first; its source must not overwrite older layouts.
    assert(native12::upgrade_list(input, &t114, 0, error));
    assert(native12::upgrade_list(input, &t95, 0, error));
    assert(native12::upgrade_list(input, &t108, 0, error));
    std::thread publishers[8];
    for (auto& thread : publishers) thread = std::thread([&] {
        L99 own{};
        assert(native12::upgrade_list(input, &own, 0, error));
        assert(own.pfnDispatch == input.pfnDispatch);
    });
    for (auto& thread : publishers) thread.join();
    assert(native12::upgrade_list(input, &t99, 0, error));
    check_common(t95);
    check_common(t99);
    check_common(t108);
    check_common(t114);
    assert(calls == 4 * 70);
    check_immutable(input, t95, 0);
    check_immutable(input, t99, 0);
    check_immutable(input, t108, 0);
    check_immutable(input, t114, 0);
    check_new_entries(t108, false);

    const auto previous_errors = errors;
    t114.pfnOmSetFrontAndBackStencilRef(api_list, 1, 2);
    t114.pfnRSSetDepthBias(api_list, 1, 2, 3);
    t114.pfnIASetIndexBufferStripCutValue(api_list, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
    t114.pfnSetProgram(api_list, nullptr);
    t114.pfnDispatchGraph(api_list, nullptr);
    assert(errors == previous_errors + 5 && last_error == E_NOTIMPL);
    assert(observed_list == list.pDrvPrivate);
    t114.pfnDrawInstanced({}, 1, 2, 3, 4);
    assert(errors == previous_errors + 6 && last_error == E_INVALIDARG && !observed_list);

    // Table number 1 has an independent immutable source and engine extension.
    const auto entries = extra_entries();
    assert(native12::upgrade_list(input, &t108, 1, error, entries));
    check_new_entries(t108, true);
    check_immutable(input, t108, 1, entries);

    Source custom = input;
    custom.pfnSetPipelineState = pipeline;
    custom.pfnSetComputeRootSignature = root;
    custom.pfnSetGraphicsRootSignature = root;
    custom.pfnSetPipelineState1 = state;
    custom.pfnSetComputeRoot32BitConstants = constants;
    assert(native12::upgrade_list(custom, &t114, 1, error, entries));
    D3D12DDI_RUNTIME_BYPASS_HEADER pso{nullptr, &pipeline_object};
    D3D12DDI_RUNTIME_BYPASS_HEADER signature{nullptr, &root_object};
    D3D12DDI_RUNTIME_BYPASS_HEADER object{nullptr, &state_object};
    t114.pfnSetPipelineState(api_list, {&pso});
    assert(observed_list == &pipeline_object);
    t114.pfnSetPipelineState(api_list, {});
    assert(!observed_list);
    t114.pfnSetComputeRootSignature(api_list, {&signature});
    assert(observed_list == &root_object);
    t114.pfnSetGraphicsRootSignature(api_list, {});
    assert(!observed_list);
    t114.pfnSetPipelineState1(api_list, {&object});
    assert(observed_list == &state_object);
    t114.pfnSetPipelineState1(api_list, {});
    assert(!observed_list);
    t114.pfnSetComputeRoot32BitConstants(api_list, 7, 3, &root_object, 19);
    t114.pfnOmSetFrontAndBackStencilRef(api_list, 1, 2);
    assert(observed == 70 && observed_list == list.pDrvPrivate);
    t114.pfnRSSetDepthBias(api_list, 1, 2, 3);
    assert(observed == 71 && observed_list == list.pDrvPrivate);
    t114.pfnIASetIndexBufferStripCutValue(api_list, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
    assert(observed == 72 && observed_list == list.pDrvPrivate);
    t114.pfnSetProgram(api_list, nullptr);
    assert(observed == 73 && observed_list == list.pDrvPrivate);
    t114.pfnDispatchGraph(api_list, nullptr);
    assert(observed == 74 && observed_list == list.pDrvPrivate);
    check_immutable(custom, t114, 1, entries);
}
#undef TEST_SOURCE_MEMBERS
}
#endif
int main() {
#if defined(D3D12DDI_BUILD_VERSION_0119)
    run();
    std::puts("ddi-modern-list-test: PASS (typed layouts, all common slots, API handles, refusals, publication)");
#else
    std::puts("ddi-modern-list-test: SKIP (headers do not expose DDI 0119)");
#endif
}
