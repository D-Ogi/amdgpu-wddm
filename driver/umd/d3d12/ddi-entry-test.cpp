// SPDX-License-Identifier: MIT
#include "ddi-entry.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <stdexcept>

namespace {
struct Owner {
    bool allowed{true};
    bool fast{};                        // FastPolicy admits this owner's recording slots to the fast path
    std::atomic<unsigned> entered{},left{},calls{},failures{};
    std::atomic<unsigned> fast_entered{},fast_left{};
    std::atomic<HRESULT> last_error{};
    std::atomic<bool> failure_in_scope{};
};
thread_local Owner* current{};
struct Policy {
    template<class Handle> static Owner* resolve(Handle h) noexcept {
        return static_cast<Owner*>(h.pDrvPrivate);
    }
    class Scope {
        Owner* owner_;Owner* previous_;bool active_;
    public:
        explicit Scope(Owner& owner) noexcept:owner_(&owner),previous_(current),active_(owner.allowed) {
            if(active_){current=&owner;++owner.entered;}
        }
        ~Scope(){if(active_){++owner_->left;current=previous_;}}
        bool entered() const noexcept{return active_;}
    };
    static void failure(Owner* owner,HRESULT hr) noexcept {
        if(owner){++owner->failures;owner->last_error.store(hr);owner->failure_in_scope.store(current==owner);}
    }
};
struct SecondPolicy:Policy {};
// The recording fast path (EntryThunk, ListBinding::fast): an owner with `fast` set admits a thread that is in no
// scope; FastScope binds `current` as Scope does, and counts apart, so a test sees which path a call took.
struct FastPolicy:Policy {
    static const Owner* fast_binding(Owner& owner) noexcept {return owner.fast && !current?&owner:nullptr;}
    class FastScope {
        Owner* owner_;Owner* previous_;bool active_;
    public:
        explicit FastScope(const Owner& owner) noexcept
          :owner_(const_cast<Owner*>(&owner)),previous_(current),active_(owner.allowed) {
            if(active_){current=owner_;++owner_->fast_entered;}
        }
        ~FastScope(){if(active_){++owner_->fast_left;current=previous_;}}
        bool entered() const noexcept{return active_;}
    };
};
struct TracePolicy:Policy {
    struct Event {Owner* owner;const char* name;std::uint64_t id;HRESULT outcome;bool begin;};
    inline static Event events[32]{};
    inline static unsigned count{};
    inline static std::uint64_t sequence{};
    template<class Handle> static Owner* resolve(Handle h) noexcept {
        // A begin record must exist even if resolving the first handle fails.
        assert(count && events[count-1].begin);
        return Policy::resolve(h);
    }
    static std::uint64_t entry(Owner* owner,const char* name) noexcept {
        assert(!owner && !current && count<32);
        const auto id=++sequence;events[count++]={owner,name,id,S_OK,true};return id;
    }
    static void leave(Owner* owner,const char* name,std::uint64_t id,HRESULT outcome) noexcept {
        assert(!current && count<32); // scope restoration precedes leave
        events[count++]={owner,name,id,outcome,false};
    }
    static void paired(const char* name,Owner* owner,HRESULT outcome) {
        assert(count>=2 && count%2==0);
        const auto& begin=events[count-2];const auto& end=events[count-1];
        assert(begin.begin && !end.begin && begin.id==end.id && begin.id==count/2);
        assert(!begin.owner && end.owner==owner && end.outcome==outcome);
        assert(!std::strcmp(begin.name,name) && !std::strcmp(end.name,name));
    }
};
// Observation is independent of paired tracing and accepts only typed slots.
struct ObservedPolicy:Policy {
    inline static unsigned observations{};
    inline static DXGI_FORMAT format_value{DXGI_FORMAT_UNKNOWN};
    inline static UINT output_value{},samples_value{};
    inline static D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags_value{};
    inline static unsigned heap_observations{};
    inline static const D3D12DDIARG_CREATEHEAP_0001* heap_input{};
    inline static const D3D12DDIARG_CREATERESOURCE_0088* resource_input{};
    inline static UINT64 heap_bytes{};
    inline static UINT row_pitch{};
    inline static bool protected_session{};
    static void observed(Owner* owner,const char* name,D3D12DDI_HDEVICE device,
            const D3D12DDIARG_CREATEHEAP_0001* heap,D3D12DDI_HHEAP,D3D12DDI_HRTRESOURCE,
            const D3D12DDIARG_CREATERESOURCE_0088* resource,const D3D12DDI_CLEAR_VALUES*,
            D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session,D3D12DDI_HRESOURCE) noexcept {
        assert(owner && current==owner && device.pDrvPrivate==owner);
        assert(!std::strcmp(name,"pfnCreateHeapAndResource"));
        ++heap_observations;heap_input=heap;resource_input=resource;
        heap_bytes=heap?heap->ByteSize:0;protected_session=session.pDrvPrivate!=nullptr;
        row_pitch=resource && resource->Layout==D3D12DDI_TL_ROW_MAJOR && resource->pRowMajorLayout
            ? resource->pRowMajorLayout->RowPitch:0;
    }
    static void observed(Owner* owner,const char* name,D3D12DDI_HDEVICE device,
            DXGI_FORMAT format,UINT* output) noexcept {
        assert(owner && current==owner && device.pDrvPrivate==owner && output);
        assert(!std::strcmp(name,"pfnCheckFormatSupport"));
        ++observations;format_value=format;output_value=*output;
    }
    static void observed(Owner* owner,const char* name,D3D12DDI_HDEVICE device,
            DXGI_FORMAT format,UINT samples,D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags,UINT* output) noexcept {
        assert(owner && current==owner && device.pDrvPrivate==owner && output);
        assert(!std::strcmp(name,"pfnCheckMultisampleQualityLevels"));
        ++observations;format_value=format;output_value=*output;samples_value=samples;flags_value=flags;
    }
};
using Core=D3D12DDI_DEVICE_FUNCS_CORE_0088;
using List=D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
using Tables=native12::DdiEntryTables<Policy>;
Core wrapped_core{};
Owner* nested_owner{};
bool nested{};
unsigned throwing{};
unsigned matching_throwing{};
unsigned heap_throwing{};
void APIENTRY format(D3D12DDI_HDEVICE h,DXGI_FORMAT,UINT* out) {
    auto* owner=static_cast<Owner*>(h.pDrvPrivate);assert(current==owner);++owner->calls;
    if(nested && owner!=nested_owner) {
        UINT inner{};wrapped_core.pfnCheckFormatSupport({nested_owner},DXGI_FORMAT_R8G8B8A8_UNORM,&inner);
        assert(current==owner && inner==17);
    }
    if(throwing==1)throw std::bad_alloc{};
    if(throwing==2)throw std::runtime_error("test");
    *out=17;
}
void APIENTRY multisample(D3D12DDI_HDEVICE h,DXGI_FORMAT,UINT,
        D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS,UINT* out) {
    assert(current==static_cast<Owner*>(h.pDrvPrivate));++current->calls;
    *out=9;
    if(throwing)throw std::runtime_error("partially written output");
}
D3D12DDI_DRIVER_MATCHING_IDENTIFIER_STATUS APIENTRY matching_identifier(D3D12DDI_HDEVICE h,
    D3D12DDI_SERIALIZED_DATA_TYPE type, const D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054* identifier) {
    assert(current==static_cast<Owner*>(h.pDrvPrivate) && identifier);
    assert(type==D3D12DDI_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE);
    ++current->calls;
    if(matching_throwing==1)throw std::bad_alloc{};
    if(matching_throwing==2)throw std::runtime_error("matching identifier test");
    return D3D12DDI_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE;
}
HRESULT APIENTRY heap_resource(D3D12DDI_HDEVICE device,const D3D12DDIARG_CREATEHEAP_0001*,
        D3D12DDI_HHEAP,D3D12DDI_HRTRESOURCE,const D3D12DDIARG_CREATERESOURCE_0088*,
        const D3D12DDI_CLEAR_VALUES*,D3D12DDI_HPROTECTEDRESOURCESESSION_0030,D3D12DDI_HRESOURCE) {
    assert(current==static_cast<Owner*>(device.pDrvPrivate));++current->calls;
    if(heap_throwing==1)throw std::bad_alloc{};
    if(heap_throwing==2)throw std::runtime_error("heap input refused by exception");
    return E_NOTIMPL;
}
static_assert(std::is_same_v<decltype(&heap_resource),PFND3D12DDI_CREATEHEAPANDRESOURCE_0088>);
void APIENTRY other_format(D3D12DDI_HDEVICE,DXGI_FORMAT,UINT*){}
SIZE_T APIENTRY fence_size(D3D12DDI_HDEVICE h,const D3D12DDIARG_CREATE_FENCE*) {
    assert(current==static_cast<Owner*>(h.pDrvPrivate));++current->calls;return 4097;
}
void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST h,UINT x,UINT y,UINT z) {
    assert(current==static_cast<Owner*>(h.pDrvPrivate) && x==2 && y==3 && z==4);++current->calls;
}
// Fast-path fixtures: a recording slot that enters another recording slot of the same list (the inner call must
// take the full path, being nested), and one that throws.
D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 fast_list{};
unsigned topology_throwing{};
void APIENTRY nested_draw(D3D12DDI_HCOMMANDLIST h,UINT,UINT,UINT,UINT) {
    assert(current==static_cast<Owner*>(h.pDrvPrivate));++current->calls;
    fast_list.pfnDispatch(h,2,3,4);
    assert(current==static_cast<Owner*>(h.pDrvPrivate));
}
static_assert(std::is_same_v<decltype(&nested_draw),PFND3D12DDI_DRAWINSTANCED>);
void APIENTRY throwing_topology(D3D12DDI_HCOMMANDLIST h,D3D12DDI_PRIMITIVE_TOPOLOGY) {
    assert(current==static_cast<Owner*>(h.pDrvPrivate));++current->calls;
    if(topology_throwing==1)throw std::bad_alloc{};
    if(topology_throwing==2)throw std::runtime_error("recording refused by exception");
}
static_assert(std::is_same_v<decltype(&throwing_topology),PFND3D12DDI_IA_SETTOPOLOGY_0003>);
struct NullFastBinding {static constexpr bool fast=true;static decltype(&dispatch) original() noexcept{return nullptr;}};
struct NamedNullFastBinding:NullFastBinding {static constexpr const char* name() noexcept {return "pfnNamed";}};
struct ThrowingFastBinding {
    static constexpr bool fast=true;static constexpr const char* name() noexcept {return "pfnIaSetTopology";}
    static decltype(&throwing_topology) original() noexcept {return throwing_topology;}
};
// A policy's fast_denied hook: the leave hook's failure record for a refusal on the fast path. It runs after
// failure, still inside FastScope, with the binding's name ("unnamed" without one).
struct FastNotePolicy:FastPolicy {
    inline static const char* noted_name{};
    inline static HRESULT noted{};
    inline static unsigned notes{},failures_at_note{};
    static void fast_denied(Owner* owner,const char* name,HRESULT hr) noexcept {
        assert(owner && current==owner);
        noted_name=name;noted=hr;++notes;failures_at_note=owner->failures.load();
    }
};
template<class Fn>struct Dummy;
template<class R,class... A>struct Dummy<R(APIENTRY*)(A...)> {
    static R APIENTRY call(A...) {
        assert(current);++current->calls;
        if constexpr(std::is_same_v<R,HRESULT>)return S_OK;
        else if constexpr(!std::is_void_v<R>)return R{};
    }
};
HRESULT APIENTRY result(D3D12DDI_HDEVICE h,int value) {
    assert(current==static_cast<Owner*>(h.pDrvPrivate));++current->calls;
    if(value==1)throw std::bad_alloc{};
    if(value==2)throw std::runtime_error("test");
    if(value==3)return E_ACCESSDENIED;
    if(value==4)return S_OK;
    return S_FALSE;
}
struct ResultBinding {static auto original() noexcept{return result;}};
using ResultThunk=native12::EntryThunk<decltype(&result),ResultBinding,Policy>;
struct NullBinding {static decltype(&result) original() noexcept{return nullptr;}};
struct TracedResultBinding:ResultBinding {static constexpr const char* name() noexcept{return "result";}};
using TracedResult=native12::EntryThunk<decltype(&result),TracedResultBinding,TracePolicy>;
Core source_core() {
    Core t{};
    t.pfnCheckFormatSupport=Dummy<decltype(t.pfnCheckFormatSupport)>::call;
    t.pfnCheckMultisampleQualityLevels=Dummy<decltype(t.pfnCheckMultisampleQualityLevels)>::call;
    t.pfnGetMipPacking=Dummy<decltype(t.pfnGetMipPacking)>::call;
    t.pfnCalcPrivateElementLayoutSize=Dummy<decltype(t.pfnCalcPrivateElementLayoutSize)>::call;
    t.pfnCreateElementLayout=Dummy<decltype(t.pfnCreateElementLayout)>::call;
    t.pfnDestroyElementLayout=Dummy<decltype(t.pfnDestroyElementLayout)>::call;
    t.pfnCalcPrivateBlendStateSize=Dummy<decltype(t.pfnCalcPrivateBlendStateSize)>::call;
    t.pfnCreateBlendState=Dummy<decltype(t.pfnCreateBlendState)>::call;
    t.pfnDestroyBlendState=Dummy<decltype(t.pfnDestroyBlendState)>::call;
    t.pfnCalcPrivateDepthStencilStateSize=Dummy<decltype(t.pfnCalcPrivateDepthStencilStateSize)>::call;
    t.pfnCreateDepthStencilState=Dummy<decltype(t.pfnCreateDepthStencilState)>::call;
    t.pfnDestroyDepthStencilState=Dummy<decltype(t.pfnDestroyDepthStencilState)>::call;
    t.pfnCalcPrivateRasterizerStateSize=Dummy<decltype(t.pfnCalcPrivateRasterizerStateSize)>::call;
    t.pfnCreateRasterizerState=Dummy<decltype(t.pfnCreateRasterizerState)>::call;
    t.pfnDestroyRasterizerState=Dummy<decltype(t.pfnDestroyRasterizerState)>::call;
    t.pfnCalcPrivateShaderSize=Dummy<decltype(t.pfnCalcPrivateShaderSize)>::call;
    t.pfnCreateVertexShader=Dummy<decltype(t.pfnCreateVertexShader)>::call;
    t.pfnCreatePixelShader=Dummy<decltype(t.pfnCreatePixelShader)>::call;
    t.pfnCreateGeometryShader=Dummy<decltype(t.pfnCreateGeometryShader)>::call;
    t.pfnCreateComputeShader=Dummy<decltype(t.pfnCreateComputeShader)>::call;
    t.pfnCalcPrivateGeometryShaderWithStreamOutput=Dummy<decltype(t.pfnCalcPrivateGeometryShaderWithStreamOutput)>::call;
    t.pfnCreateGeometryShaderWithStreamOutput=Dummy<decltype(t.pfnCreateGeometryShaderWithStreamOutput)>::call;
    t.pfnCalcPrivateTessellationShaderSize=Dummy<decltype(t.pfnCalcPrivateTessellationShaderSize)>::call;
    t.pfnCreateHullShader=Dummy<decltype(t.pfnCreateHullShader)>::call;
    t.pfnCreateDomainShader=Dummy<decltype(t.pfnCreateDomainShader)>::call;
    t.pfnDestroyShader=Dummy<decltype(t.pfnDestroyShader)>::call;
    t.pfnCalcPrivateCommandQueueSize=Dummy<decltype(t.pfnCalcPrivateCommandQueueSize)>::call;
    t.pfnCreateCommandQueue=Dummy<decltype(t.pfnCreateCommandQueue)>::call;
    t.pfnDestroyCommandQueue=Dummy<decltype(t.pfnDestroyCommandQueue)>::call;
    t.pfnCalcPrivateCommandPoolSize=Dummy<decltype(t.pfnCalcPrivateCommandPoolSize)>::call;
    t.pfnCreateCommandPool=Dummy<decltype(t.pfnCreateCommandPool)>::call;
    t.pfnDestroyCommandPool=Dummy<decltype(t.pfnDestroyCommandPool)>::call;
    t.pfnResetCommandPool=Dummy<decltype(t.pfnResetCommandPool)>::call;
    t.pfnCalcPrivatePipelineStateSize=Dummy<decltype(t.pfnCalcPrivatePipelineStateSize)>::call;
    t.pfnCreatePipelineState=Dummy<decltype(t.pfnCreatePipelineState)>::call;
    t.pfnDestroyPipelineState=Dummy<decltype(t.pfnDestroyPipelineState)>::call;
    t.pfnCalcPrivateCommandListSize=Dummy<decltype(t.pfnCalcPrivateCommandListSize)>::call;
    t.pfnCreateCommandList=Dummy<decltype(t.pfnCreateCommandList)>::call;
    t.pfnDestroyCommandList=Dummy<decltype(t.pfnDestroyCommandList)>::call;
    t.pfnCalcPrivateFenceSize=Dummy<decltype(t.pfnCalcPrivateFenceSize)>::call;
    t.pfnCreateFence=Dummy<decltype(t.pfnCreateFence)>::call;
    t.pfnDestroyFence=Dummy<decltype(t.pfnDestroyFence)>::call;
    t.pfnCalcPrivateDescriptorHeapSize=Dummy<decltype(t.pfnCalcPrivateDescriptorHeapSize)>::call;
    t.pfnCreateDescriptorHeap=Dummy<decltype(t.pfnCreateDescriptorHeap)>::call;
    t.pfnDestroyDescriptorHeap=Dummy<decltype(t.pfnDestroyDescriptorHeap)>::call;
    t.pfnGetDescriptorSizeInBytes=Dummy<decltype(t.pfnGetDescriptorSizeInBytes)>::call;
    t.pfnGetCPUDescriptorHandleForHeapStart=Dummy<decltype(t.pfnGetCPUDescriptorHandleForHeapStart)>::call;
    t.pfnGetGPUDescriptorHandleForHeapStart=Dummy<decltype(t.pfnGetGPUDescriptorHandleForHeapStart)>::call;
    t.pfnCreateShaderResourceView=Dummy<decltype(t.pfnCreateShaderResourceView)>::call;
    t.pfnCreateConstantBufferView=Dummy<decltype(t.pfnCreateConstantBufferView)>::call;
    t.pfnCreateSampler=Dummy<decltype(t.pfnCreateSampler)>::call;
    t.pfnCreateUnorderedAccessView=Dummy<decltype(t.pfnCreateUnorderedAccessView)>::call;
    t.pfnCreateRenderTargetView=Dummy<decltype(t.pfnCreateRenderTargetView)>::call;
    t.pfnCreateDepthStencilView=Dummy<decltype(t.pfnCreateDepthStencilView)>::call;
    t.pfnCalcPrivateRootSignatureSize=Dummy<decltype(t.pfnCalcPrivateRootSignatureSize)>::call;
    t.pfnCreateRootSignature=Dummy<decltype(t.pfnCreateRootSignature)>::call;
    t.pfnDestroyRootSignature=Dummy<decltype(t.pfnDestroyRootSignature)>::call;
    t.pfnMapHeap=Dummy<decltype(t.pfnMapHeap)>::call;
    t.pfnUnmapHeap=Dummy<decltype(t.pfnUnmapHeap)>::call;
    t.pfnCalcPrivateHeapAndResourceSizes=Dummy<decltype(t.pfnCalcPrivateHeapAndResourceSizes)>::call;
    t.pfnCreateHeapAndResource=Dummy<decltype(t.pfnCreateHeapAndResource)>::call;
    t.pfnDestroyHeapAndResource=Dummy<decltype(t.pfnDestroyHeapAndResource)>::call;
    t.pfnMakeResident=Dummy<decltype(t.pfnMakeResident)>::call;
    t.pfnEvict=Dummy<decltype(t.pfnEvict)>::call;
    t.pfnCalcPrivateOpenedHeapAndResourceSizes=Dummy<decltype(t.pfnCalcPrivateOpenedHeapAndResourceSizes)>::call;
    t.pfnOpenHeapAndResource=Dummy<decltype(t.pfnOpenHeapAndResource)>::call;
    t.pfnCopyDescriptors=Dummy<decltype(t.pfnCopyDescriptors)>::call;
    t.pfnCopyDescriptorsSimple=Dummy<decltype(t.pfnCopyDescriptorsSimple)>::call;
    t.pfnCalcPrivateQueryHeapSize=Dummy<decltype(t.pfnCalcPrivateQueryHeapSize)>::call;
    t.pfnCreateQueryHeap=Dummy<decltype(t.pfnCreateQueryHeap)>::call;
    t.pfnDestroyQueryHeap=Dummy<decltype(t.pfnDestroyQueryHeap)>::call;
    t.pfnCalcPrivateCommandSignatureSize=Dummy<decltype(t.pfnCalcPrivateCommandSignatureSize)>::call;
    t.pfnCreateCommandSignature=Dummy<decltype(t.pfnCreateCommandSignature)>::call;
    t.pfnDestroyCommandSignature=Dummy<decltype(t.pfnDestroyCommandSignature)>::call;
    t.pfnCheckResourceVirtualAddress=Dummy<decltype(t.pfnCheckResourceVirtualAddress)>::call;
    t.pfnCheckResourceAllocationInfo=Dummy<decltype(t.pfnCheckResourceAllocationInfo)>::call;
    t.pfnCheckSubresourceInfo=Dummy<decltype(t.pfnCheckSubresourceInfo)>::call;
    t.pfnCheckExistingResourceAllocationInfo=Dummy<decltype(t.pfnCheckExistingResourceAllocationInfo)>::call;
    t.pfnOfferResources=Dummy<decltype(t.pfnOfferResources)>::call;
    t.pfnReclaimResources=Dummy<decltype(t.pfnReclaimResources)>::call;
    t.pfnGetImplicitPhysicalAdapterMask=Dummy<decltype(t.pfnGetImplicitPhysicalAdapterMask)>::call;
    t.pfnGetPresentPrivateDriverDataSize=Dummy<decltype(t.pfnGetPresentPrivateDriverDataSize)>::call;
    t.pfnQueryNodeMap=Dummy<decltype(t.pfnQueryNodeMap)>::call;
    t.pfnRetrieveShaderComment=Dummy<decltype(t.pfnRetrieveShaderComment)>::call;
    t.pfnCheckResourceAllocationHandle=Dummy<decltype(t.pfnCheckResourceAllocationHandle)>::call;
    t.pfnCalcPrivatePipelineLibrarySize=Dummy<decltype(t.pfnCalcPrivatePipelineLibrarySize)>::call;
    t.pfnCreatePipelineLibrary=Dummy<decltype(t.pfnCreatePipelineLibrary)>::call;
    t.pfnDestroyPipelineLibrary=Dummy<decltype(t.pfnDestroyPipelineLibrary)>::call;
    t.pfnAddPipelineStateToLibrary=Dummy<decltype(t.pfnAddPipelineStateToLibrary)>::call;
    t.pfnCalcSerializedLibrarySize=Dummy<decltype(t.pfnCalcSerializedLibrarySize)>::call;
    t.pfnSerializeLibrary=Dummy<decltype(t.pfnSerializeLibrary)>::call;
    t.pfnGetDebugAllocationInfo=Dummy<decltype(t.pfnGetDebugAllocationInfo)>::call;
    t.pfnCalcPrivateCommandRecorderSize=Dummy<decltype(t.pfnCalcPrivateCommandRecorderSize)>::call;
    t.pfnCreateCommandRecorder=Dummy<decltype(t.pfnCreateCommandRecorder)>::call;
    t.pfnDestroyCommandRecorder=Dummy<decltype(t.pfnDestroyCommandRecorder)>::call;
    t.pfnCommandRecorderSetCommandPoolAsTarget=Dummy<decltype(t.pfnCommandRecorderSetCommandPoolAsTarget)>::call;
    t.pfnCalcPrivateSchedulingGroupSize=Dummy<decltype(t.pfnCalcPrivateSchedulingGroupSize)>::call;
    t.pfnCreateSchedulingGroup=Dummy<decltype(t.pfnCreateSchedulingGroup)>::call;
    t.pfnDestroySchedulingGroup=Dummy<decltype(t.pfnDestroySchedulingGroup)>::call;
    t.pfnEnumerateMetaCommands=Dummy<decltype(t.pfnEnumerateMetaCommands)>::call;
    t.pfnEnumerateMetaCommandParameters=Dummy<decltype(t.pfnEnumerateMetaCommandParameters)>::call;
    t.pfnCalcPrivateMetaCommandSize=Dummy<decltype(t.pfnCalcPrivateMetaCommandSize)>::call;
    t.pfnCreateMetaCommand=Dummy<decltype(t.pfnCreateMetaCommand)>::call;
    t.pfnDestroyMetaCommand=Dummy<decltype(t.pfnDestroyMetaCommand)>::call;
    t.pfnGetMetaCommandRequiredParameterInfo=Dummy<decltype(t.pfnGetMetaCommandRequiredParameterInfo)>::call;
    t.pfnCalcPrivateStateObjectSize=Dummy<decltype(t.pfnCalcPrivateStateObjectSize)>::call;
    t.pfnCreateStateObject=Dummy<decltype(t.pfnCreateStateObject)>::call;
    t.pfnDestroyStateObject=Dummy<decltype(t.pfnDestroyStateObject)>::call;
    t.pfnGetRaytracingAccelerationStructurePrebuildInfo=Dummy<decltype(t.pfnGetRaytracingAccelerationStructurePrebuildInfo)>::call;
    t.pfnCheckDriverMatchingIdentifier=Dummy<decltype(t.pfnCheckDriverMatchingIdentifier)>::call;
    t.pfnGetShaderIdentifier=Dummy<decltype(t.pfnGetShaderIdentifier)>::call;
    t.pfnGetShaderStackSize=Dummy<decltype(t.pfnGetShaderStackSize)>::call;
    t.pfnGetPipelineStackSize=Dummy<decltype(t.pfnGetPipelineStackSize)>::call;
    t.pfnSetPipelineStackSize=Dummy<decltype(t.pfnSetPipelineStackSize)>::call;
    t.pfnSetBackgroundProcessingMode=Dummy<decltype(t.pfnSetBackgroundProcessingMode)>::call;
    t.pfnCalcPrivateAddToStateObjectSize=Dummy<decltype(t.pfnCalcPrivateAddToStateObjectSize)>::call;
    t.pfnAddToStateObject=Dummy<decltype(t.pfnAddToStateObject)>::call;
    t.pfnCreateSamplerFeedbackUnorderedAccessView=Dummy<decltype(t.pfnCreateSamplerFeedbackUnorderedAccessView)>::call;
    t.pfnCreateAmplificationShader=Dummy<decltype(t.pfnCreateAmplificationShader)>::call;
    t.pfnCreateMeshShader=Dummy<decltype(t.pfnCreateMeshShader)>::call;
    t.pfnCalcPrivateMeshShaderSize=Dummy<decltype(t.pfnCalcPrivateMeshShaderSize)>::call;
    t.pfnImplicitShaderCacheControl=Dummy<decltype(t.pfnImplicitShaderCacheControl)>::call;
    t.pfnCheckFormatSupport=format;t.pfnCalcPrivateFenceSize=fence_size;
    t.pfnCheckMultisampleQualityLevels=multisample;
    t.pfnCreateHeapAndResource=heap_resource;
    t.pfnCheckDriverMatchingIdentifier=matching_identifier;
    return t;
}
List source_list() {
    List t{};
    t.pfnCloseCommandList=Dummy<decltype(t.pfnCloseCommandList)>::call;
    t.pfnResetCommandList=Dummy<decltype(t.pfnResetCommandList)>::call;
    t.pfnDrawInstanced=Dummy<decltype(t.pfnDrawInstanced)>::call;
    t.pfnDrawIndexedInstanced=Dummy<decltype(t.pfnDrawIndexedInstanced)>::call;
    t.pfnDispatch=Dummy<decltype(t.pfnDispatch)>::call;
    t.pfnClearUnorderedAccessViewUint=Dummy<decltype(t.pfnClearUnorderedAccessViewUint)>::call;
    t.pfnClearUnorderedAccessViewFloat=Dummy<decltype(t.pfnClearUnorderedAccessViewFloat)>::call;
    t.pfnClearRenderTargetView=Dummy<decltype(t.pfnClearRenderTargetView)>::call;
    t.pfnClearDepthStencilView=Dummy<decltype(t.pfnClearDepthStencilView)>::call;
    t.pfnDiscardResource=Dummy<decltype(t.pfnDiscardResource)>::call;
    t.pfnCopyTextureRegion=Dummy<decltype(t.pfnCopyTextureRegion)>::call;
    t.pfnResourceCopy=Dummy<decltype(t.pfnResourceCopy)>::call;
    t.pfnCopyTiles=Dummy<decltype(t.pfnCopyTiles)>::call;
    t.pfnCopyBufferRegion=Dummy<decltype(t.pfnCopyBufferRegion)>::call;
    t.pfnResourceResolveSubresource=Dummy<decltype(t.pfnResourceResolveSubresource)>::call;
    t.pfnExecuteBundle=Dummy<decltype(t.pfnExecuteBundle)>::call;
    t.pfnExecuteIndirect=Dummy<decltype(t.pfnExecuteIndirect)>::call;
    t.pfnResourceBarrier=Dummy<decltype(t.pfnResourceBarrier)>::call;
    t.pfnBlt=Dummy<decltype(t.pfnBlt)>::call;
    t.pfnPresent=Dummy<decltype(t.pfnPresent)>::call;
    t.pfnBeginQuery=Dummy<decltype(t.pfnBeginQuery)>::call;
    t.pfnEndQuery=Dummy<decltype(t.pfnEndQuery)>::call;
    t.pfnResolveQueryData=Dummy<decltype(t.pfnResolveQueryData)>::call;
    t.pfnSetPredication=Dummy<decltype(t.pfnSetPredication)>::call;
    t.pfnIaSetTopology=Dummy<decltype(t.pfnIaSetTopology)>::call;
    t.pfnRsSetViewports=Dummy<decltype(t.pfnRsSetViewports)>::call;
    t.pfnRsSetScissorRects=Dummy<decltype(t.pfnRsSetScissorRects)>::call;
    t.pfnOmSetBlendFactor=Dummy<decltype(t.pfnOmSetBlendFactor)>::call;
    t.pfnOmSetStencilRef=Dummy<decltype(t.pfnOmSetStencilRef)>::call;
    t.pfnSetPipelineState=Dummy<decltype(t.pfnSetPipelineState)>::call;
    t.pfnSetDescriptorHeaps=Dummy<decltype(t.pfnSetDescriptorHeaps)>::call;
    t.pfnSetComputeRootSignature=Dummy<decltype(t.pfnSetComputeRootSignature)>::call;
    t.pfnSetGraphicsRootSignature=Dummy<decltype(t.pfnSetGraphicsRootSignature)>::call;
    t.pfnSetComputeRootDescriptorTable=Dummy<decltype(t.pfnSetComputeRootDescriptorTable)>::call;
    t.pfnSetGraphicsRootDescriptorTable=Dummy<decltype(t.pfnSetGraphicsRootDescriptorTable)>::call;
    t.pfnSetComputeRoot32BitConstant=Dummy<decltype(t.pfnSetComputeRoot32BitConstant)>::call;
    t.pfnSetGraphicsRoot32BitConstant=Dummy<decltype(t.pfnSetGraphicsRoot32BitConstant)>::call;
    t.pfnSetComputeRoot32BitConstants=Dummy<decltype(t.pfnSetComputeRoot32BitConstants)>::call;
    t.pfnSetGraphicsRoot32BitConstants=Dummy<decltype(t.pfnSetGraphicsRoot32BitConstants)>::call;
    t.pfnSetComputeRootConstantBufferView=Dummy<decltype(t.pfnSetComputeRootConstantBufferView)>::call;
    t.pfnSetGraphicsRootConstantBufferView=Dummy<decltype(t.pfnSetGraphicsRootConstantBufferView)>::call;
    t.pfnSetComputeRootShaderResourceView=Dummy<decltype(t.pfnSetComputeRootShaderResourceView)>::call;
    t.pfnSetGraphicsRootShaderResourceView=Dummy<decltype(t.pfnSetGraphicsRootShaderResourceView)>::call;
    t.pfnSetComputeRootUnorderedAccessView=Dummy<decltype(t.pfnSetComputeRootUnorderedAccessView)>::call;
    t.pfnSetGraphicsRootUnorderedAccessView=Dummy<decltype(t.pfnSetGraphicsRootUnorderedAccessView)>::call;
    t.pfnIASetIndexBuffer=Dummy<decltype(t.pfnIASetIndexBuffer)>::call;
    t.pfnIASetVertexBuffers=Dummy<decltype(t.pfnIASetVertexBuffers)>::call;
    t.pfnSOSetTargets=Dummy<decltype(t.pfnSOSetTargets)>::call;
    t.pfnOMSetRenderTargets=Dummy<decltype(t.pfnOMSetRenderTargets)>::call;
    t.pfnSetMarker=Dummy<decltype(t.pfnSetMarker)>::call;
    t.pfnClearRootArguments=Dummy<decltype(t.pfnClearRootArguments)>::call;
    t.pfnAtomicCopyBufferRegion=Dummy<decltype(t.pfnAtomicCopyBufferRegion)>::call;
    t.pfnOMSetDepthBounds=Dummy<decltype(t.pfnOMSetDepthBounds)>::call;
    t.pfnSetSamplePositions=Dummy<decltype(t.pfnSetSamplePositions)>::call;
    t.pfnResourceResolveSubresourceRegion=Dummy<decltype(t.pfnResourceResolveSubresourceRegion)>::call;
    t.pfnSetProtectedResourceSession=Dummy<decltype(t.pfnSetProtectedResourceSession)>::call;
    t.pfnWriteBufferImmediate=Dummy<decltype(t.pfnWriteBufferImmediate)>::call;
    t.pfnSetViewInstanceMask=Dummy<decltype(t.pfnSetViewInstanceMask)>::call;
    t.pfnInitializeMetaCommand=Dummy<decltype(t.pfnInitializeMetaCommand)>::call;
    t.pfnExecuteMetaCommand=Dummy<decltype(t.pfnExecuteMetaCommand)>::call;
    t.pfnBuildRaytracingAccelerationStructure=Dummy<decltype(t.pfnBuildRaytracingAccelerationStructure)>::call;
    t.pfnEmitRaytracingAccelerationStructurePostbuildInfo=Dummy<decltype(t.pfnEmitRaytracingAccelerationStructurePostbuildInfo)>::call;
    t.pfnCopyRaytracingAccelerationStructure=Dummy<decltype(t.pfnCopyRaytracingAccelerationStructure)>::call;
    t.pfnSetPipelineState1=Dummy<decltype(t.pfnSetPipelineState1)>::call;
    t.pfnDispatchRays=Dummy<decltype(t.pfnDispatchRays)>::call;
    t.pfnRSSetShadingRate=Dummy<decltype(t.pfnRSSetShadingRate)>::call;
    t.pfnRSSetShadingRateImage=Dummy<decltype(t.pfnRSSetShadingRateImage)>::call;
    t.pfnDispatchMesh=Dummy<decltype(t.pfnDispatchMesh)>::call;
    t.pfnBarrier=Dummy<decltype(t.pfnBarrier)>::call;
    t.pfnOmSetAlphaBlendFactor=Dummy<decltype(t.pfnOmSetAlphaBlendFactor)>::call;
    t.pfnDispatch=dispatch;return t;
}
}
int main() {
    static_assert(Tables::core_slots==122 && Tables::list_slots==70);
    static_assert(std::is_convertible_v<decltype(&ResultThunk::call),decltype(&result)>);
    Core source=source_core(),untouched=source,output=source;
    List list_source=source_list(),wrapped_compute{},wrapped_graphics{};
    Core incomplete=source;incomplete.pfnImplicitShaderCacheControl=nullptr;
    assert(Tables::wrap_core(incomplete,&output)==E_INVALIDARG && !std::memcmp(&output,&untouched,sizeof(output)));
    assert(Tables::wrap_core(source,&wrapped_core)==S_OK);
    assert(Tables::wrap_list(0,list_source,&wrapped_compute)==S_OK);
    assert(Tables::wrap_list(1,list_source,&wrapped_graphics)==S_OK);
    assert(wrapped_compute.pfnDispatch!=wrapped_graphics.pfnDispatch);
    assert(Tables::wrap_list(2,list_source,&wrapped_compute)==E_INVALIDARG);
    assert(Tables::wrap_core(wrapped_core,&output)==E_INVALIDARG); // no double wrapping
    Core changed=source;changed.pfnCheckFormatSupport=other_format;
    assert(Tables::wrap_core(changed,&output)==E_UNEXPECTED && !std::memcmp(&output,&untouched,sizeof(output)));
    assert(Tables::wrap_core(source,&output)==S_OK && output.pfnCheckFormatSupport==wrapped_core.pfnCheckFormatSupport);
    // Different Policy types have independent immutable original tables.
    assert(native12::DdiEntryTables<SecondPolicy>::wrap_core(changed,&output)==S_OK);

    Owner a,b;D3D12DDI_HDEVICE ha{&a},hb{&b};
    UINT value=0;nested_owner=&b;nested=true;
    wrapped_core.pfnCheckFormatSupport(ha,DXGI_FORMAT_R8G8B8A8_UNORM,&value);
    assert(value==17 && a.calls==1 && b.calls==1 && !current && a.entered==a.left && b.entered==b.left);
    nested=false;
    assert(wrapped_core.pfnCalcPrivateFenceSize(ha,nullptr)==4097 && !current);
    wrapped_compute.pfnDispatch({&a},2,3,4);wrapped_graphics.pfnDispatch({&b},2,3,4);
    assert(ResultThunk::call(ha,0)==S_FALSE && !current);
    assert(ResultThunk::call(ha,1)==E_OUTOFMEMORY && a.last_error==E_OUTOFMEMORY && a.failure_in_scope);
    assert(ResultThunk::call(ha,2)==E_FAIL && a.last_error==E_FAIL && a.failure_in_scope);
    assert(!current && a.entered==a.left);
    assert((native12::EntryThunk<decltype(&result),NullBinding,Policy>::call(ha,0)==E_UNEXPECTED));
    throwing=1;wrapped_core.pfnCheckFormatSupport(ha,DXGI_FORMAT_R8G8B8A8_UNORM,&value);
    assert(a.last_error==E_OUTOFMEMORY && !current && a.entered==a.left);throwing=0;
    assert(ResultThunk::call({},0)==E_INVALIDARG && !current);
    b.allowed=false;
    const unsigned calls_before=b.calls.load(),entered_before=b.entered.load();
    assert(ResultThunk::call(hb,0)==E_UNEXPECTED && b.last_error==E_UNEXPECTED && !b.failure_in_scope);
    assert(wrapped_core.pfnCalcPrivateFenceSize(hb,nullptr)==0);
    value=123;wrapped_core.pfnCheckFormatSupport(hb,DXGI_FORMAT_R8G8B8A8_UNORM,&value);
    assert(value==123 && b.calls==calls_before && b.entered==entered_before && !current);b.allowed=true;

    // Actual enum-returning core slot: zero is success, so denial and exceptions
    // must use UNRECOGNIZED while a successful original result passes unchanged.
    D3D12DDI_SERIALIZED_DATA_DRIVER_MATCHING_IDENTIFIER_0054 identifier{};
    constexpr auto serialized_type=D3D12DDI_SERIALIZED_DATA_RAYTRACING_ACCELERATION_STRUCTURE;
    constexpr auto unrecognized=D3D12DDI_DRIVER_MATCHING_IDENTIFIER_UNRECOGNIZED;
    assert(wrapped_core.pfnCheckDriverMatchingIdentifier(ha,serialized_type,&identifier)==
        D3D12DDI_DRIVER_MATCHING_IDENTIFIER_COMPATIBLE_WITH_DEVICE);
    assert(wrapped_core.pfnCheckDriverMatchingIdentifier({},serialized_type,&identifier)==unrecognized && !current);
    b.allowed=false;
    const unsigned matching_calls_before=b.calls.load();
    assert(wrapped_core.pfnCheckDriverMatchingIdentifier(hb,serialized_type,&identifier)==unrecognized);
    assert(b.calls==matching_calls_before && b.last_error==E_UNEXPECTED && !b.failure_in_scope);
    b.allowed=true;
    matching_throwing=1;
    assert(wrapped_core.pfnCheckDriverMatchingIdentifier(ha,serialized_type,&identifier)==unrecognized);
    assert(a.last_error==E_OUTOFMEMORY && a.failure_in_scope && !current && a.entered==a.left);
    matching_throwing=2;
    assert(wrapped_core.pfnCheckDriverMatchingIdentifier(ha,serialized_type,&identifier)==unrecognized);
    assert(a.last_error==E_FAIL && a.failure_in_scope && !current && a.entered==a.left);
    matching_throwing=0;

    // Optional diagnostics preserve the underlying return and pair every path.
    Core traced_core{};List traced_list{};
    assert(native12::DdiEntryTables<TracePolicy>::wrap_core(source,&traced_core)==S_OK);
    assert(native12::DdiEntryTables<TracePolicy>::wrap_list(1,list_source,&traced_list)==S_OK);
    traced_core.pfnCheckFormatSupport(ha,DXGI_FORMAT_UNKNOWN,&value);
    TracePolicy::paired("pfnCheckFormatSupport",&a,S_OK);
    assert(traced_core.pfnCalcPrivateFenceSize(ha,nullptr)==4097);
    TracePolicy::paired("pfnCalcPrivateFenceSize",&a,S_OK);
    traced_list.pfnDispatch({&a},2,3,4);
    TracePolicy::paired("pfnDispatch",&a,S_OK);
    assert(TracedResult::call(ha,4)==S_OK);TracePolicy::paired("result",&a,S_OK);
    assert(TracedResult::call(ha,0)==S_FALSE);TracePolicy::paired("result",&a,S_FALSE);
    assert(TracedResult::call(ha,3)==E_ACCESSDENIED);TracePolicy::paired("result",&a,E_ACCESSDENIED);
    assert(TracedResult::call(ha,1)==E_OUTOFMEMORY);TracePolicy::paired("result",&a,E_OUTOFMEMORY);
    assert(TracedResult::call(ha,2)==E_FAIL);TracePolicy::paired("result",&a,E_FAIL);
    assert(TracedResult::call({},0)==E_INVALIDARG);TracePolicy::paired("result",nullptr,E_INVALIDARG);
    b.allowed=false;
    assert(TracedResult::call(hb,0)==E_UNEXPECTED);TracePolicy::paired("result",&b,E_UNEXPECTED);
    b.allowed=true;
    assert((native12::EntryThunk<decltype(&result),NullBinding,TracePolicy>::call(ha,0)==E_UNEXPECTED));
    TracePolicy::paired("unnamed",&a,E_UNEXPECTED);
    throwing=2;traced_core.pfnCheckFormatSupport(ha,DXGI_FORMAT_UNKNOWN,&value);throwing=0;
    TracePolicy::paired("pfnCheckFormatSupport",&a,E_FAIL);
    assert(!current && a.entered==a.left && b.entered==b.left);

    // Observe the original typed arguments and mutated outputs before unwind.
    Core observed_core{};
    assert(native12::DdiEntryTables<ObservedPolicy>::wrap_core(source,&observed_core)==S_OK);
    value=123;
    observed_core.pfnCheckFormatSupport(ha,DXGI_FORMAT_R8G8B8A8_UNORM,&value);
    assert(ObservedPolicy::observations==1 && ObservedPolicy::output_value==17 && value==17);
    assert(ObservedPolicy::format_value==DXGI_FORMAT_R8G8B8A8_UNORM && !current);
    constexpr auto msaa_flags=static_cast<D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS>(1);
    observed_core.pfnCheckMultisampleQualityLevels(ha,DXGI_FORMAT_R16G16_FLOAT,4,msaa_flags,&value);
    assert(ObservedPolicy::observations==2 && ObservedPolicy::output_value==9 && value==9);
    assert(ObservedPolicy::format_value==DXGI_FORMAT_R16G16_FLOAT && ObservedPolicy::samples_value==4);
    assert(ObservedPolicy::flags_value==msaa_flags && !current);
    // Calls without a matching typed overload continue unchanged.
    assert(observed_core.pfnCalcPrivateFenceSize(ha,nullptr)==4097 && ObservedPolicy::observations==2);
    b.allowed=false;value=123;
    observed_core.pfnCheckFormatSupport(hb,DXGI_FORMAT_UNKNOWN,&value);
    observed_core.pfnCheckMultisampleQualityLevels({},DXGI_FORMAT_UNKNOWN,2,msaa_flags,&value);
    assert(ObservedPolicy::observations==2 && value==123);b.allowed=true;
    throwing=1;observed_core.pfnCheckFormatSupport(ha,DXGI_FORMAT_UNKNOWN,&value);throwing=0;
    assert(ObservedPolicy::observations==2 && value==123);
    throwing=2;
    observed_core.pfnCheckMultisampleQualityLevels(ha,DXGI_FORMAT_UNKNOWN,2,msaa_flags,&value);
    throwing=0;
    assert(ObservedPolicy::observations==2 && value==9 && !current && a.entered==a.left);

    // Actual 0088 heap slot: a normally returned E_NOTIMPL still observes the
    // original public inputs, with no changed HRESULT, lost width, or copied handles.
    D3D12DDIARG_CREATEHEAP_0001 heap{};heap.ByteSize=UINT64{1}<<34;heap.Alignment=65536;
    D3D12DDIARG_ROW_MAJOR_RESOURCE_LAYOUT row{4096,8192};
    D3D12DDIARG_CREATERESOURCE_0088 resource{};resource.ResourceType=D3D12DDI_RT_BUFFER;
    resource.Layout=D3D12DDI_TL_ROW_MAJOR;resource.pRowMajorLayout=&row;resource.Width=4096;
    assert(observed_core.pfnCreateHeapAndResource(ha,&heap,{},{},&resource,nullptr,{&a},{})==E_NOTIMPL);
    assert(ObservedPolicy::heap_observations==1 && ObservedPolicy::heap_input==&heap && ObservedPolicy::resource_input==&resource);
    assert(ObservedPolicy::heap_bytes==(UINT64{1}<<34) && ObservedPolicy::row_pitch==4096 && ObservedPolicy::protected_session && !current);
    assert(observed_core.pfnCreateHeapAndResource(ha,nullptr,{},{},nullptr,nullptr,{},{})==E_NOTIMPL);
    assert(ObservedPolicy::heap_observations==2 && !ObservedPolicy::heap_input && !ObservedPolicy::resource_input);
    assert(!ObservedPolicy::heap_bytes && !ObservedPolicy::row_pitch && !ObservedPolicy::protected_session);
    resource.Layout=D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE;
    assert(observed_core.pfnCreateHeapAndResource(ha,&heap,{},{},&resource,nullptr,{},{})==E_NOTIMPL);
    assert(ObservedPolicy::heap_observations==3 && !ObservedPolicy::row_pitch);
    b.allowed=false;
    assert(observed_core.pfnCreateHeapAndResource(hb,&heap,{},{},&resource,nullptr,{},{})==E_UNEXPECTED);b.allowed=true;
    assert(observed_core.pfnCreateHeapAndResource({},&heap,{},{},&resource,nullptr,{},{})==E_INVALIDARG);
    heap_throwing=1;
    assert(observed_core.pfnCreateHeapAndResource(ha,&heap,{},{},&resource,nullptr,{},{})==E_OUTOFMEMORY);
    heap_throwing=2;
    assert(observed_core.pfnCreateHeapAndResource(ha,&heap,{},{},&resource,nullptr,{},{})==E_FAIL);heap_throwing=0;
    assert(ObservedPolicy::heap_observations==3 && !current && a.entered==a.left && b.entered==b.left);

    // Recording fast path: under a policy with FastScope, the recording slots of an admitted owner run in
    // FastScope (no Scope, no trace hooks); Close, Reset and Present, an owner the policy does not admit, a
    // nested entry, and a policy without FastScope take the full path. Failures are reported inside FastScope.
    using FastTables=native12::DdiEntryTables<FastPolicy>;
    List fast_source=source_list();
    fast_source.pfnDrawInstanced=nested_draw;fast_source.pfnIaSetTopology=throwing_topology;
    assert(FastTables::wrap_list(1,fast_source,&fast_list)==S_OK);
    Owner f;f.fast=true;D3D12DDI_HCOMMANDLIST hf{&f};
    fast_list.pfnDispatch(hf,2,3,4);
    assert(f.calls==1 && f.fast_entered==1 && f.fast_left==1 && f.entered==0 && !current);
    fast_list.pfnCloseCommandList(hf);
    fast_list.pfnResetCommandList(hf,nullptr);
    fast_list.pfnPresent(hf,{},nullptr,nullptr,nullptr,nullptr);
    assert(f.calls==4 && f.fast_entered==1 && f.entered==3 && f.entered==f.left && !current);
    // Nested: the outer draw is fast, the dispatch it makes sees a bound thread and takes the full path.
    fast_list.pfnDrawInstanced(hf,3,1,0,0);
    assert(f.calls==6 && f.fast_entered==2 && f.fast_left==2 && f.entered==4 && f.entered==f.left && !current);
    // An owner the policy does not admit: full path, and the full path still refuses a denied owner.
    Owner g;D3D12DDI_HCOMMANDLIST hg{&g};
    fast_list.pfnDispatch(hg,2,3,4);
    assert(g.calls==1 && g.fast_entered==0 && g.entered==1 && !current);
    g.allowed=false;
    fast_list.pfnDispatch(hg,2,3,4);
    assert(g.calls==1 && g.failures==1 && g.last_error==E_UNEXPECTED && !g.failure_in_scope && !current);
    // Admitted but the fast scope does not enter: the full path decides, and refuses here.
    g.fast=true;
    fast_list.pfnDispatch(hg,2,3,4);
    assert(g.calls==1 && g.fast_entered==0 && g.failures==2 && !g.failure_in_scope && !current);
    g.allowed=true;g.fast=false;
    // Exceptions and a missing original are failures reported inside FastScope.
    topology_throwing=1;fast_list.pfnIaSetTopology(hf,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    assert(f.failures==1 && f.last_error==E_OUTOFMEMORY && f.failure_in_scope && !current);
    topology_throwing=2;fast_list.pfnIaSetTopology(hf,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    assert(f.failures==2 && f.last_error==E_FAIL && f.failure_in_scope && !current);
    topology_throwing=0;fast_list.pfnIaSetTopology(hf,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    assert(f.failures==2 && f.fast_entered==5 && f.fast_entered==f.fast_left && f.entered==f.left);
    native12::EntryThunk<decltype(&dispatch),NullFastBinding,FastPolicy>::call(hf,2,3,4);
    assert(f.failures==3 && f.last_error==E_UNEXPECTED && f.failure_in_scope && f.fast_entered==6 && !current);
    assert((!native12::EntryThunk<decltype(&dispatch),NullFastBinding,Policy>::fast_path()));
    // fast_denied sees each refusal after failure, in FastScope, with the slot's name; a success notes nothing.
    native12::EntryThunk<decltype(&dispatch),NamedNullFastBinding,FastNotePolicy>::call(hf,2,3,4);
    assert(FastNotePolicy::notes==1 && FastNotePolicy::noted==E_UNEXPECTED && FastNotePolicy::failures_at_note==4);
    assert(!std::strcmp(FastNotePolicy::noted_name,"pfnNamed") && f.failures==4 && !current);
    native12::EntryThunk<decltype(&dispatch),NullFastBinding,FastNotePolicy>::call(hf,2,3,4);
    assert(FastNotePolicy::notes==2 && !std::strcmp(FastNotePolicy::noted_name,"unnamed") && f.failures==5);
    using NotedTopology=native12::EntryThunk<decltype(&throwing_topology),ThrowingFastBinding,FastNotePolicy>;
    topology_throwing=2;NotedTopology::call(hf,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    assert(FastNotePolicy::notes==3 && FastNotePolicy::noted==E_FAIL && FastNotePolicy::failures_at_note==6);
    assert(!std::strcmp(FastNotePolicy::noted_name,"pfnIaSetTopology") && f.last_error==E_FAIL && !current);
    topology_throwing=0;NotedTopology::call(hf,D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    assert(FastNotePolicy::notes==3 && f.failures==6 && f.fast_entered==f.fast_left && !current);
    // A null handle resolves to no owner: refused on the full path, as before.
    fast_list.pfnDispatch({},2,3,4);
    assert(!current);
    // The same slots under a policy without FastScope never take it (wrapped_compute is Policy's).
    const unsigned fast_before=f.fast_entered.load(),entered_before_full=f.entered.load();
    wrapped_compute.pfnDispatch(hf,2,3,4);
    assert(f.fast_entered==fast_before && f.entered==entered_before_full+1 && !current);
    // Several threads at once, fast on f and full on g.
    std::thread fast_a([&]{for(unsigned i=0;i<1000;++i){fast_list.pfnDispatch(hf,2,3,4);assert(!current);}});
    std::thread fast_b([&]{for(unsigned i=0;i<1000;++i){fast_list.pfnDispatch(hg,2,3,4);assert(!current);}});
    fast_a.join();fast_b.join();
    assert(f.fast_entered==fast_before+1000 && f.fast_entered==f.fast_left && g.entered==g.left && g.fast_entered==0);

    std::thread call_a([&]{for(unsigned i=0;i<1000;++i){UINT v{};wrapped_core.pfnCheckFormatSupport(ha,DXGI_FORMAT_UNKNOWN,&v);assert(v==17 && !current);}});
    std::thread call_b([&]{for(unsigned i=0;i<1000;++i){wrapped_compute.pfnDispatch({&b},2,3,4);assert(!current);}});
    std::thread refill([&]{for(unsigned i=0;i<1000;++i){Core c{};List l{};assert(Tables::wrap_core(source,&c)==S_OK);assert(Tables::wrap_list(i%2,list_source,&l)==S_OK);assert(Tables::wrap_core(changed,&c)==E_UNEXPECTED);}});
    call_a.join();call_b.join();refill.join();
    assert(!current && a.entered==a.left && b.entered==b.left);
    std::puts("DDI entry: 122 core + 70x2 list signatures; scope, denial, exceptions, immutable refill, paired tracing, typed output observation, recording fast path (admission, Close/Reset/Present and nested fallback, failures inside the fast scope with the fast_denied note) and concurrency pass");
}
