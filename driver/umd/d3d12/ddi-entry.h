// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <atomic>
#include <cstdint>
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
// The failures a slot of the AllowOutOfMemory category may report (windows-driver-docs display
// handling-errors.md): E_OUTOFMEMORY and D3DDDIERR_DEVICEREMOVED, nothing else. The runtime treats every other
// code as a critical driver failure and removes the application's device with DXGI_ERROR_DRIVER_INTERNAL_ERROR
// (BD-075). A thunk refusal is a failure of that slot as much as one the slot itself decided - the runtime cannot
// tell them apart - so the bindings that opt in (CoreBinding::allow_out_of_memory) clamp E_INVALIDARG for an
// unresolvable handle, E_UNEXPECTED for a scope that could not be entered or a missing original, and E_FAIL for
// an exception. Policy::failure still sees the real code, and so does the diagnostic line the refusal writes.
// A lost device keeps the one name the runtime admits: DXGI_ERROR_DEVICE_REMOVED/RESET/HUNG are the names the API
// shows the application, not codes this category admits, so they become D3DDDIERR_DEVICEREMOVED here.
// engine_ddi::admitted_create_failure is the same rule inside the engine module; native-tables.cpp
// static_asserts that the two agree, so neither can drift.
inline constexpr HRESULT kDdiDriverDeviceRemoved=static_cast<HRESULT>(0x88760870);  // D3DDDIERR_DEVICEREMOVED
constexpr HRESULT ddi_admitted_create_failure(HRESULT hr) noexcept {
    if(SUCCEEDED(hr) || hr==E_OUTOFMEMORY || hr==kDdiDriverDeviceRemoved) return hr;
    if(hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET || hr==DXGI_ERROR_DEVICE_HUNG)
        return kDdiDriverDeviceRemoved;
    return E_OUTOFMEMORY;
}
// Policy supplies resolve(first DDI handle) noexcept -> Owner*, Scope(Owner&)
// noexcept with entered(), and failure(Owner*, HRESULT) noexcept. Scope holds
// both runtime callback authority and hosted GIPA authority. It must restore
// its previous thread state on destruction. failure may run without an entered
// scope (or with nullptr owner); it must not invoke unauthorized runtime callbacks.
// The caller owns handle/device lifetime. No resolver pins arbitrary stale memory.
// Binding::original() noexcept returns the exact typed original function pointer.
// Optional paired diagnostics. entry runs before resolving the handle, with a
// null owner. leave sees the resolved owner (if any) after Scope has unwound;
// neither hook grants callback authority. Hooks receive no DDI arguments.
// S_OK for a void/non-status return means the function returned normally only.
template<class Owner,class Binding,class Policy> class EntryTrace final {
    static constexpr bool has_entry=requires(Owner* owner,const char* name) {Policy::entry(owner,name);};
    static constexpr bool has_leave=requires(Owner* owner,const char* name,std::uint64_t id,HRESULT hr) {
        Policy::leave(owner,name,id,hr);
    };
    static_assert(has_entry==has_leave,"DDI tracing requires both entry and leave hooks");
    static constexpr const char* name() noexcept {
        if constexpr(requires {Binding::name();}) {
            static_assert(noexcept(Binding::name()),"binding name must not throw");
            return Binding::name();
        } else return "unnamed";
    }
    std::uint64_t id_{};
public:
    Owner* owner{};
    HRESULT outcome{E_UNEXPECTED};
    EntryTrace() noexcept {
        if constexpr(has_entry) {
            static_assert(noexcept(Policy::entry(owner,name())),"trace entry must not throw");
            static_assert(std::is_same_v<decltype(Policy::entry(owner,name())),std::uint64_t>,
                "trace entry must return uint64_t");
            id_=Policy::entry(nullptr,name());
        }
    }
    ~EntryTrace() noexcept {
        if constexpr(has_leave) {
            static_assert(noexcept(Policy::leave(owner,name(),id_,outcome)),"trace leave must not throw");
            Policy::leave(owner,name(),id_,outcome);
        }
    }
    // A separate typed observation hook may inspect selected public outputs.
    // It runs only after normal return, while the owner's Scope is still active.
    // The policy must handle nullable outputs and choose what is safe to record.
    template<class... Args> void observed(Args... args) noexcept {
        if constexpr(requires {Policy::observed(owner,name(),args...);}) {
            static_assert(noexcept(Policy::observed(owner,name(),args...)),"observation must not throw");
            Policy::observed(owner,name(),args...);
        }
    }
    // Sizes and counts are not statuses: a policy may record the returned number.
    void returned(std::uint64_t value) noexcept {
        if constexpr(requires {Policy::returned(owner,name(),id_,value);}) {
            static_assert(noexcept(Policy::returned(owner,name(),id_,value)),"return record must not throw");
            Policy::returned(owner,name(),id_,value);
        }
    }
    EntryTrace(const EntryTrace&)=delete;
    EntryTrace& operator=(const EntryTrace&)=delete;
};

template<class Function,class Binding,class Policy> struct EntryThunk;
template<class R,class... A,class Binding,class Policy>
struct EntryThunk<R(APIENTRY*)(A...),Binding,Policy> {
    static_assert(sizeof...(A)>0,"A DDI entry must carry an owner handle");
    static R failure_value(HRESULT hr) noexcept {
        if constexpr(std::is_same_v<R,HRESULT>) return hr;
        else if constexpr(!std::is_void_v<R>) return EntryFailureValue<R>::value();
    }
    // What this thunk reports for a refusal of its own. A binding of the AllowOutOfMemory category
    // (Binding::allow_out_of_memory, ddi_admitted_create_failure above) admits only two codes, so the thunk's
    // E_INVALIDARG, E_UNEXPECTED and E_FAIL are clamped before they reach the runtime; every other binding
    // reports what it decided. The record given to Policy::failure is always the real code.
    static constexpr bool clamps_failure() noexcept {
        if constexpr(std::is_same_v<R,HRESULT> && requires {Binding::allow_out_of_memory;})
            return Binding::allow_out_of_memory;
        else return false;
    }
    static HRESULT admitted(HRESULT hr) noexcept {
        if constexpr(clamps_failure()) return ddi_admitted_create_failure(hr);
        else return hr;
    }
    template<class Owner> static R denied(Owner* owner,HRESULT hr) noexcept {
        static_assert(noexcept(Policy::failure(owner,hr)),"failure must not throw");
        Policy::failure(owner,hr);
        return failure_value(admitted(hr));
    }
    // Optional fast path, for a binding that opts in (static constexpr bool fast) under a policy that offers
    // one: Policy::fast_binding(Owner&) noexcept returns the owner's precomputed binding, or null for the full
    // path, and Policy::FastScope(*binding) noexcept binds this thread, with entered(). Admitted, the call runs
    // without EntryTrace and without Scope, so the policy admits only where the fast scope grants the same
    // authority as Scope and where the trace hooks would write nothing on success; a refusal, a missing original
    // and an exception are handled as on the full path (denied), inside the fast scope, and then given to the
    // optional Policy::fast_denied(owner,name,hr) noexcept, which stands in for the leave hook's failure record.
    // Everything else takes the full path.
    static constexpr bool fast_path() noexcept {
        if constexpr(requires {Binding::fast;} && requires {typename Policy::FastScope;}) return Binding::fast;
        else return false;
    }
    static constexpr const char* binding_name() noexcept {
        if constexpr(requires {Binding::name();}) return Binding::name();
        else return "unnamed";
    }
    // Whether the policy observes this slot's arguments (EntryTrace::observed); a template, so that a policy
    // without a matching hook answers false instead of failing to compile.
    template<class Owner> static constexpr bool observed_slot() noexcept {
        return requires(Owner* owner,A... args) {Policy::observed(owner,binding_name(),args...);};
    }
    template<class Owner> static R fast_denied(Owner* owner,HRESULT hr) noexcept {
        static_assert(noexcept(Policy::failure(owner,hr)),"failure must not throw");
        Policy::failure(owner,hr);
        if constexpr(requires {Policy::fast_denied(owner,binding_name(),hr);}) {
            static_assert(noexcept(Policy::fast_denied(owner,binding_name(),hr)),"fast_denied must not throw");
            Policy::fast_denied(owner,binding_name(),hr);
        }
        return failure_value(admitted(hr));
    }
    static R APIENTRY call(A... args) noexcept {
        if constexpr(fast_path()) {
            auto first=std::get<0>(std::tuple<A...>(args...));
            static_assert(noexcept(Policy::resolve(first)),"resolve must not throw");
            if(auto* owner=Policy::resolve(first)) {
                static_assert(!observed_slot<std::remove_pointer_t<decltype(owner)>>(),
                    "a slot with an observation hook must take the full path");
                static_assert(noexcept(Policy::fast_binding(*owner)),"fast binding must not throw");
                if(const auto* binding=Policy::fast_binding(*owner)) {
                    using FastScope=typename Policy::FastScope;
                    static_assert(std::is_nothrow_constructible_v<FastScope,decltype(*binding)>);
                    static_assert(std::is_nothrow_destructible_v<FastScope>);
                    FastScope scope(*binding);
                    if(scope.entered()) {
                        auto fn=Binding::original();
                        if(!fn) return fast_denied(owner,E_UNEXPECTED);
                        try {
                            return fn(args...);
                        } catch(const std::bad_alloc&) {
                            return fast_denied(owner,E_OUTOFMEMORY);
                        } catch(...) {
                            return fast_denied(owner,E_FAIL);
                        }
                    }
                }
            }
        }
        return full(args...);
    }
    static R full(A... args) noexcept {
        auto first=std::get<0>(std::tuple<A...>(args...));
        static_assert(noexcept(Policy::resolve(first)),"resolve must not throw");
        using Owner=std::remove_pointer_t<decltype(Policy::resolve(first))>;
        EntryTrace<Owner,Binding,Policy> trace;
        auto* owner=Policy::resolve(first);trace.owner=owner;
        const auto deny=[&](HRESULT hr) noexcept -> R {trace.outcome=hr;return denied(owner,hr);};
        if(!owner) return deny(E_INVALIDARG);
        using Scope=typename Policy::Scope;
        static_assert(std::is_nothrow_constructible_v<Scope,decltype(*owner)>);
        static_assert(std::is_nothrow_destructible_v<Scope>);
        Scope scope(*owner);
        static_assert(noexcept(scope.entered()),"scope authority query must not throw");
        if(!scope.entered()) return deny(E_UNEXPECTED);
        static_assert(noexcept(Binding::original()),"original lookup must not throw");
        auto fn=Binding::original();
        if(!fn) return deny(E_UNEXPECTED);
        try {
            if constexpr(std::is_void_v<R>) {
                fn(args...);trace.observed(args...);trace.outcome=S_OK;return;
            } else {
                R value=fn(args...);trace.observed(args...);
                if constexpr(std::is_same_v<R,HRESULT>)trace.outcome=value;
                else {
                    trace.outcome=S_OK;
                    if constexpr(std::is_integral_v<R>)trace.returned(static_cast<std::uint64_t>(value));
                }
                return value;
            }
        } catch(const std::bad_alloc&) {
            return deny(E_OUTOFMEMORY);
        } catch(...) {
            return deny(E_FAIL);
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

constexpr bool ddi_same_name(const char* a,const char* b) noexcept {
    while(*a && *a==*b){++a;++b;}
    return *a==*b;
}
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
#define N12_CORE_NAME(m) struct CoreName_##m {static constexpr const char* name() noexcept {return #m;}};
    NATIVE12_CORE_0088_MEMBERS(N12_CORE_NAME)
#undef N12_CORE_NAME
#define N12_LIST_NAME(m) struct ListName_##m {static constexpr const char* name() noexcept {return #m;}};
    NATIVE12_LIST_0092_MEMBERS(N12_LIST_NAME)
#undef N12_LIST_NAME
    template<auto Member,class Name> struct CoreBinding:Name {
        // The descriptor slots may take the policy's fast path (EntryThunk) as the recording slots do: a game fills
        // its tables between draws, thousands of calls a frame (Witcher 3, lab session 291: CopyDescriptors alone
        // 0.16 ms of the main thread's frame in the full entry). They write descriptor memory, report failures
        // through the device's error callback, which the fast scope binds as Scope does, and no hook observes them.
        static constexpr bool fast=ddi_same_name(Name::name(),"pfnCopyDescriptors") ||
            ddi_same_name(Name::name(),"pfnCopyDescriptorsSimple") ||
            ddi_same_name(Name::name(),"pfnCreateShaderResourceView") ||
            ddi_same_name(Name::name(),"pfnCreateConstantBufferView") ||
            ddi_same_name(Name::name(),"pfnCreateSampler") ||
            ddi_same_name(Name::name(),"pfnCreateUnorderedAccessView") ||
            ddi_same_name(Name::name(),"pfnCreateRenderTargetView") ||
            ddi_same_name(Name::name(),"pfnCreateDepthStencilView");
        // The AllowOutOfMemory category (ddi_admitted_create_failure): these two slots refuse a shared resource,
        // and the thunk's own refusals must not cost the application its device either (BD-075). The other
        // creation slots of this table belong in the same category and are not listed yet: each needs the audit
        // that docs/d3d12-shared-resources.md records as open, because a slot whose failures the runtime already
        // sees as E_OUTOFMEMORY gains nothing, while one that reports E_INVALIDARG for a bad argument would stop
        // saying so to the debug layer. Adding a name here is cheap; removing a diagnosis is not.
        static constexpr bool allow_out_of_memory=ddi_same_name(Name::name(),"pfnCreateHeapAndResource") ||
            ddi_same_name(Name::name(),"pfnOpenHeapAndResource");
        static auto original() noexcept {
            using Fn=std::remove_reference_t<decltype(core_.*Member)>;
            return core_ready_.load(std::memory_order_acquire)?core_.*Member:Fn{};
        }
    };
    template<unsigned Index,auto Member,class Name> struct ListBinding:Name {
        // The recording slots may take the policy's fast path (EntryThunk). Close and Reset bound a recording,
        // and Present is a queue operation (QueueDomainScope); those three always take the full entry.
        static constexpr bool fast=!ddi_same_name(Name::name(),"pfnCloseCommandList") &&
            !ddi_same_name(Name::name(),"pfnResetCommandList") && !ddi_same_name(Name::name(),"pfnPresent");
        static auto original() noexcept {
            using Fn=std::remove_reference_t<decltype(lists_[Index].*Member)>;
            return list_ready_[Index].load(std::memory_order_acquire)?lists_[Index].*Member:Fn{};
        }
    };
    template<unsigned Index> static HRESULT wrap_list_at(const List& source,List* out) noexcept {
        if(!out) return E_INVALIDARG;
        List wrapped{};
#define N12_WRAP_LIST(m) wrapped.m=&EntryThunk<decltype(source.m),ListBinding<Index,&List::m,ListName_##m>,Policy>::call;
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
#define N12_WRAP_CORE(m) wrapped.m=&EntryThunk<decltype(source.m),CoreBinding<&Core::m,CoreName_##m>,Policy>::call;
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
