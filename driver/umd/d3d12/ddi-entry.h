// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <atomic>
#include <new>
#include <tuple>
#include <type_traits>

namespace native12 {
// Numeric sizes, counts and nullable handles have no universal failure code;
// their zero value is accompanied by Policy::failure. Status enums need an
// explicit mapping so a denied call cannot return a successful status by
// default initialization. New enum return types must be reviewed explicitly.
template<class R> struct EntryFailureValue {
    static_assert(!std::is_enum_v<R>, "DDI status enum requires an explicit failure value");
    static R value() noexcept {return R{};}
};
template<> struct EntryFailureValue<D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS> {
    static D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS value() noexcept {
        // WDK10.0.26100 d3d12umddi.h:8118: zero means COMPATIBLE_WITH_DEVICE.
        return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
    }
};
// Policy supplies resolve(first DDI handle) noexcept -> Owner*, Scope(Owner&)
// noexcept with entered(), and failure(Owner*, HRESULT) noexcept. Scope holds
// both runtime callback authority and hosted GIPA authority. It must restore
// its previous thread state on destruction. failure may run without an entered
// scope (or with nullptr owner); it must not invoke unauthorized runtime callbacks.
// The caller owns handle/device lifetime. No resolver pins arbitrary stale memory.
// Binding::original() noexcept returns the exact typed original function pointer.
template<class Function,class Binding,class Policy> struct EntryThunk;
template<class R,class... A,class Binding,class Policy>
struct EntryThunk<R(APIENTRY*)(A...),Binding,Policy> {
    static_assert(sizeof...(A)>0,"A DDI entry must carry an owner handle");
    template<class Owner> static R denied(Owner* owner,HRESULT hr) noexcept {
        static_assert(noexcept(Policy::failure(owner,hr)),"failure must not throw");
        Policy::failure(owner,hr);
        if constexpr(std::is_same_v<R,HRESULT>) return hr;
        else if constexpr(!std::is_void_v<R>) return EntryFailureValue<R>::value();
    }
    static R APIENTRY call(A... args) noexcept {
        auto first=std::get<0>(std::tuple<A...>(args...));
        static_assert(noexcept(Policy::resolve(first)),"resolve must not throw");
        auto* owner=Policy::resolve(first);
        if(!owner) return denied(owner,E_INVALIDARG);
        using Scope=typename Policy::Scope;
        static_assert(std::is_nothrow_constructible_v<Scope,decltype(*owner)>);
        static_assert(std::is_nothrow_destructible_v<Scope>);
        Scope scope(*owner);
        static_assert(noexcept(scope.entered()),"scope authority query must not throw");
        if(!scope.entered()) return denied(owner,E_UNEXPECTED);
        static_assert(noexcept(Binding::original()),"original lookup must not throw");
        auto fn=Binding::original();
        if(!fn) return denied(owner,E_UNEXPECTED);
        try {
            return fn(args...);
        } catch(const std::bad_alloc&) {
            return denied(owner,E_OUTOFMEMORY);
        } catch(...) {
            return denied(owner,E_FAIL);
        }
    }
};

// Exact member names from WDK10.0.26100 d3d12umddi.h, core0088/list0092.
// Compiler assignment checks every signature; counts and byte sizes reject drift.
#define NATIVE12_CORE_0088_MEMBERS(X) \
    X(pfnCheckFormatSupport) \
    X(pfnCheckMultisampleQualityLevels) \
    X(pfnGetMipPacking) \
    X(pfnCalcPrivateElementLayoutSize) \
    X(pfnCreateElementLayout) \
    X(pfnDestroyElementLayout) \
    X(pfnCalcPrivateBlendStateSize) \
    X(pfnCreateBlendState) \
    X(pfnDestroyBlendState) \
    X(pfnCalcPrivateDepthStencilStateSize) \
    X(pfnCreateDepthStencilState) \
    X(pfnDestroyDepthStencilState) \
    X(pfnCalcPrivateRasterizerStateSize) \
    X(pfnCreateRasterizerState) \
    X(pfnDestroyRasterizerState) \
    X(pfnCalcPrivateShaderSize) \
    X(pfnCreateVertexShader) \
    X(pfnCreatePixelShader) \
    X(pfnCreateGeometryShader) \
    X(pfnCreateComputeShader) \
    X(pfnCalcPrivateGeometryShaderWithStreamOutput) \
    X(pfnCreateGeometryShaderWithStreamOutput) \
    X(pfnCalcPrivateTessellationShaderSize) \
    X(pfnCreateHullShader) \
    X(pfnCreateDomainShader) \
    X(pfnDestroyShader) \
    X(pfnCalcPrivateCommandQueueSize) \
    X(pfnCreateCommandQueue) \
    X(pfnDestroyCommandQueue) \
    X(pfnCalcPrivateCommandPoolSize) \
    X(pfnCreateCommandPool) \
    X(pfnDestroyCommandPool) \
    X(pfnResetCommandPool) \
    X(pfnCalcPrivatePipelineStateSize) \
    X(pfnCreatePipelineState) \
    X(pfnDestroyPipelineState) \
    X(pfnCalcPrivateCommandListSize) \
    X(pfnCreateCommandList) \
    X(pfnDestroyCommandList) \
    X(pfnCalcPrivateFenceSize) \
    X(pfnCreateFence) \
    X(pfnDestroyFence) \
    X(pfnCalcPrivateDescriptorHeapSize) \
    X(pfnCreateDescriptorHeap) \
    X(pfnDestroyDescriptorHeap) \
    X(pfnGetDescriptorSizeInBytes) \
    X(pfnGetCPUDescriptorHandleForHeapStart) \
    X(pfnGetGPUDescriptorHandleForHeapStart) \
    X(pfnCreateShaderResourceView) \
    X(pfnCreateConstantBufferView) \
    X(pfnCreateSampler) \
    X(pfnCreateUnorderedAccessView) \
    X(pfnCreateRenderTargetView) \
    X(pfnCreateDepthStencilView) \
    X(pfnCalcPrivateRootSignatureSize) \
    X(pfnCreateRootSignature) \
    X(pfnDestroyRootSignature) \
    X(pfnMapHeap) \
    X(pfnUnmapHeap) \
    X(pfnCalcPrivateHeapAndResourceSizes) \
    X(pfnCreateHeapAndResource) \
    X(pfnDestroyHeapAndResource) \
    X(pfnMakeResident) \
    X(pfnEvict) \
    X(pfnCalcPrivateOpenedHeapAndResourceSizes) \
    X(pfnOpenHeapAndResource) \
    X(pfnCopyDescriptors) \
    X(pfnCopyDescriptorsSimple) \
    X(pfnCalcPrivateQueryHeapSize) \
    X(pfnCreateQueryHeap) \
    X(pfnDestroyQueryHeap) \
    X(pfnCalcPrivateCommandSignatureSize) \
    X(pfnCreateCommandSignature) \
    X(pfnDestroyCommandSignature) \
    X(pfnCheckResourceVirtualAddress) \
    X(pfnCheckResourceAllocationInfo) \
    X(pfnCheckSubresourceInfo) \
    X(pfnCheckExistingResourceAllocationInfo) \
    X(pfnOfferResources) \
    X(pfnReclaimResources) \
    X(pfnGetImplicitPhysicalAdapterMask) \
    X(pfnGetPresentPrivateDriverDataSize) \
    X(pfnQueryNodeMap) \
    X(pfnRetrieveShaderComment) \
    X(pfnCheckResourceAllocationHandle) \
    X(pfnCalcPrivatePipelineLibrarySize) \
    X(pfnCreatePipelineLibrary) \
    X(pfnDestroyPipelineLibrary) \
    X(pfnAddPipelineStateToLibrary) \
    X(pfnCalcSerializedLibrarySize) \
    X(pfnSerializeLibrary) \
    X(pfnGetDebugAllocationInfo) \
    X(pfnCalcPrivateCommandRecorderSize) \
    X(pfnCreateCommandRecorder) \
    X(pfnDestroyCommandRecorder) \
    X(pfnCommandRecorderSetCommandPoolAsTarget) \
    X(pfnCalcPrivateSchedulingGroupSize) \
    X(pfnCreateSchedulingGroup) \
    X(pfnDestroySchedulingGroup) \
    X(pfnEnumerateMetaCommands) \
    X(pfnEnumerateMetaCommandParameters) \
    X(pfnCalcPrivateMetaCommandSize) \
    X(pfnCreateMetaCommand) \
    X(pfnDestroyMetaCommand) \
    X(pfnGetMetaCommandRequiredParameterInfo) \
    X(pfnCalcPrivateStateObjectSize) \
    X(pfnCreateStateObject) \
    X(pfnDestroyStateObject) \
    X(pfnGetRaytracingAccelerationStructurePrebuildInfo) \
    X(pfnCheckDriverMatchingIdentifier) \
    X(pfnGetShaderIdentifier) \
    X(pfnGetShaderStackSize) \
    X(pfnGetPipelineStackSize) \
    X(pfnSetPipelineStackSize) \
    X(pfnSetBackgroundProcessingMode) \
    X(pfnCalcPrivateAddToStateObjectSize) \
    X(pfnAddToStateObject) \
    X(pfnCreateSamplerFeedbackUnorderedAccessView) \
    X(pfnCreateAmplificationShader) \
    X(pfnCreateMeshShader) \
    X(pfnCalcPrivateMeshShaderSize) \
    X(pfnImplicitShaderCacheControl)
#define NATIVE12_LIST_0092_MEMBERS(X) \
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

// One immutable original table per Policy and table number in this UMD module.
// Refill with identical entries is permitted. A changed entry refuses the whole
// refill without publishing any output. No mutable current adapter/device global.
// Wrap before publishing to the runtime; the caller owns publication of *out.
// The source must already contain all shell and engine entries; never wrap twice.
template<class Policy> class DdiEntryTables final {
    using Core=D3D12DDI_DEVICE_FUNCS_CORE_0088;
    using List=D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
    inline static Core core_{};
    inline static List lists_[2]{};
    inline static std::atomic<bool> core_ready_{};
    inline static std::atomic<bool> list_ready_[2]{};
    inline static SRWLOCK lock_=SRWLOCK_INIT;
    class Lock final {
    public:
        Lock() noexcept {AcquireSRWLockExclusive(&lock_);}
        ~Lock(){ReleaseSRWLockExclusive(&lock_);}
        Lock(const Lock&)=delete;
        Lock& operator=(const Lock&)=delete;
    };
    template<auto Member> struct CoreBinding {
        static auto original() noexcept {
            using Fn=std::remove_reference_t<decltype(core_.*Member)>;
            return core_ready_.load(std::memory_order_acquire)?core_.*Member:Fn{};
        }
    };
    template<unsigned Index,auto Member> struct ListBinding {
        static auto original() noexcept {
            using Fn=std::remove_reference_t<decltype(lists_[Index].*Member)>;
            return list_ready_[Index].load(std::memory_order_acquire)?lists_[Index].*Member:Fn{};
        }
    };
    template<unsigned Index> static HRESULT wrap_list_at(const List& source,List* out) noexcept {
        if(!out) return E_INVALIDARG;
        List wrapped{};
#define N12_WRAP_LIST(m) wrapped.m=&EntryThunk<decltype(source.m),ListBinding<Index,&List::m>,Policy>::call;
        NATIVE12_LIST_0092_MEMBERS(N12_WRAP_LIST)
#undef N12_WRAP_LIST
#define N12_VALIDATE_LIST(m) if(!source.m || source.m==wrapped.m) return E_INVALIDARG;
        NATIVE12_LIST_0092_MEMBERS(N12_VALIDATE_LIST)
#undef N12_VALIDATE_LIST
        Lock guard;
        if(list_ready_[Index].load(std::memory_order_acquire)) {
#define N12_COMPARE_LIST(m) if(lists_[Index].m!=source.m) return E_UNEXPECTED;
            NATIVE12_LIST_0092_MEMBERS(N12_COMPARE_LIST)
#undef N12_COMPARE_LIST
        } else {
            lists_[Index]=source;
            list_ready_[Index].store(true,std::memory_order_release);
        }
        *out=wrapped;return S_OK;
    }
public:
#define N12_COUNT(m) +1
    static constexpr unsigned core_slots=0 NATIVE12_CORE_0088_MEMBERS(N12_COUNT);
    static constexpr unsigned list_slots=0 NATIVE12_LIST_0092_MEMBERS(N12_COUNT);
#undef N12_COUNT
    static_assert(sizeof(void*)==8 && core_slots==122 && list_slots==70);
    static_assert(sizeof(Core)==core_slots*sizeof(void*) && sizeof(List)==list_slots*sizeof(void*));
    static HRESULT wrap_core(const Core& source,Core* out) noexcept {
        if(!out) return E_INVALIDARG;
        Core wrapped{};
#define N12_WRAP_CORE(m) wrapped.m=&EntryThunk<decltype(source.m),CoreBinding<&Core::m>,Policy>::call;
        NATIVE12_CORE_0088_MEMBERS(N12_WRAP_CORE)
#undef N12_WRAP_CORE
#define N12_VALIDATE_CORE(m) if(!source.m || source.m==wrapped.m) return E_INVALIDARG;
        NATIVE12_CORE_0088_MEMBERS(N12_VALIDATE_CORE)
#undef N12_VALIDATE_CORE
        Lock guard;
        if(core_ready_.load(std::memory_order_acquire)) {
#define N12_COMPARE_CORE(m) if(core_.m!=source.m) return E_UNEXPECTED;
            NATIVE12_CORE_0088_MEMBERS(N12_COMPARE_CORE)
#undef N12_COMPARE_CORE
        } else {
            core_=source;
            core_ready_.store(true,std::memory_order_release);
        }
        *out=wrapped;return S_OK;
    }
    static HRESULT wrap_list(unsigned index,const List& source,List* out) noexcept {
        if(index==0) return wrap_list_at<0>(source,out);
        if(index==1) return wrap_list_at<1>(source,out);
        return E_INVALIDARG;
    }
};
#undef NATIVE12_CORE_0088_MEMBERS
#undef NATIVE12_LIST_0092_MEMBERS
}
