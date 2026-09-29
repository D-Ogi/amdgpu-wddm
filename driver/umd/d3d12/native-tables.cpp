// SPDX-License-Identifier: MIT
#include "native-tables.h"
#include "device-state.h"
#include "device-engine.h"
#include "device-table.h"
#include "ddi-entry.h"
#include "ddi-trace.h"
#include "fence-ddi.h"
#include "shell-core-ddi.h"
#include "native-queue-ddi.h"
#include "native-residency-ddi.h"
#include <cstring>

namespace native12 {
namespace {
// Diagnostics must not turn a normally refused malformed input into a fault.
// Copy only fixed-size SDK inputs; never follow handles or unbounded arrays.
template<class T> bool trace_input(const T* source,T& destination) noexcept {
    if(!source)return false;
    __try {destination=*source;return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR
             ? EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {destination={};return false;}
}
struct EntryPolicy {
    using Scope=DeviceEngineScope;
    static uint64_t entry(Device*,const char* name) noexcept {return ddi_trace_begin(name);}
    static void leave(Device*,const char* name,uint64_t id,HRESULT outcome) noexcept {ddi_trace_end(name,id,outcome);}
    // Sizes only: other scalar returns can be addresses, which a trace must not carry.
    static void returned(Device*,const char* name,uint64_t id,uint64_t value) noexcept {
        if(!id || (std::strncmp(name,"pfnCalcPrivate",14) && std::strcmp(name,"pfnGetDescriptorSizeInBytes")))return;
        std::fprintf(stderr,"{\"event\":\"ddi-return\",\"sequence\":%llu,\"name\":\"%s\",\"value\":%llu}\n",
            static_cast<unsigned long long>(id),name,static_cast<unsigned long long>(value));
        std::fflush(stderr);
    }
    static void observed(Device*,const char* name,D3D12DDI_HDEVICE,DXGI_FORMAT format,UINT* output) noexcept {
        if(!ddi_trace_enabled())return;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        std::fprintf(stderr,"{\"event\":\"ddi-format\",\"name\":\"%s\",\"format\":%u,\"output_present\":%u,\"support\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(format),unsigned(output!=nullptr),output?*output:0,now.QuadPart,GetCurrentThreadId());
        std::fflush(stderr);
    }
    static void observed(Device*,const char* name,D3D12DDI_HDEVICE,DXGI_FORMAT format,UINT samples,
        D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags,UINT* output) noexcept {
        if(!ddi_trace_enabled())return;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        std::fprintf(stderr,"{\"event\":\"ddi-msaa\",\"name\":\"%s\",\"format\":%u,\"samples\":%u,\"flags\":%u,\"output_present\":%u,\"levels\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(format),samples,unsigned(flags),unsigned(output!=nullptr),output?*output:0,now.QuadPart,GetCurrentThreadId());
        std::fflush(stderr);
    }
    static void observed(Device* device,const char* name,D3D12DDI_HDEVICE,UINT count,UINT* map) noexcept {
        if(!ddi_trace_enabled() || !map || !count)return;
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        std::fprintf(stderr,"{\"event\":\"ddi-node-map\",\"name\":\"%s\",\"count\":%u,\"first\":%u,\"lost\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,count,map[0],unsigned(device && device->lost.load()),now.QuadPart,GetCurrentThreadId());
        std::fflush(stderr);
    }
    static void observed(Device*,const char* name,D3D12DDI_HDEVICE,
        const D3D12DDIARG_CREATEHEAP_0001* heap,D3D12DDI_HHEAP,D3D12DDI_HRTRESOURCE,
        const D3D12DDIARG_CREATERESOURCE_0088* resource,const D3D12DDI_CLEAR_VALUES*,
        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session,D3D12DDI_HRESOURCE) noexcept {
        if(!ddi_trace_enabled())return;
        D3D12DDIARG_CREATEHEAP_0001 h{};D3D12DDIARG_CREATERESOURCE_0088 r{};
        D3D12DDIARG_ROW_MAJOR_RESOURCE_LAYOUT row{};
        const bool heap_readable=trace_input(heap,h),resource_readable=trace_input(resource,r);
        const bool row_present=resource_readable && r.pRowMajorLayout;
        // WDK26100 d3d12umddi.h:10884: pitches are meaningful only for ROW_MAJOR.
        const bool row_readable=row_present && r.Layout==D3D12DDI_TL_ROW_MAJOR && trace_input(r.pRowMajorLayout,row);
        LARGE_INTEGER now{};QueryPerformanceCounter(&now);
        std::fprintf(stderr,"{\"event\":\"ddi-create-heap-resource\",\"name\":\"%s\","
            "\"heap_present\":%u,\"heap_readable\":%u,\"heap_flags\":%u,\"cpu_page\":%u,\"memory_pool\":%u,"
            "\"bytes\":%llu,\"alignment\":%llu,\"creation_node_mask\":%u,\"visible_node_mask\":%u,"
            "\"resource_present\":%u,\"resource_readable\":%u,\"resource_type\":%u,\"layout\":%u,"
            "\"width\":%llu,\"height\":%u,\"depth\":%u,\"mips\":%u,\"resource_flags\":%u,\"num_castable_formats\":%u,"
            "\"row_major_present\":%u,\"row_major_readable\":%u,\"row_pitch\":%u,\"slice_pitch\":%u,"
            "\"protected_session_present\":%u,\"qpc\":%lld,\"thread\":%lu}\n",
            name,unsigned(heap!=nullptr),unsigned(heap_readable),unsigned(h.Flags),unsigned(h.CPUPageProperty),unsigned(h.MemoryPool),
            static_cast<unsigned long long>(h.ByteSize),static_cast<unsigned long long>(h.Alignment),h.CreationNodeMask,h.VisibleNodeMask,
            unsigned(resource!=nullptr),unsigned(resource_readable),unsigned(r.ResourceType),unsigned(r.Layout),
            static_cast<unsigned long long>(r.Width),r.Height,unsigned(r.DepthOrArraySize),unsigned(r.MipLevels),unsigned(r.Flags),r.NumCastableFormats,
            unsigned(row_present),unsigned(row_readable),row.RowPitch,row.SlicePitch,unsigned(session.pDrvPrivate!=nullptr),
            now.QuadPart,GetCurrentThreadId());
        std::fflush(stderr);
    }
    static Device* resolve(D3D12DDI_HDEVICE handle) noexcept {return static_cast<Device*>(handle.pDrvPrivate);}
    static Device* resolve(D3D12DDI_HCOMMANDLIST handle) noexcept {
        return static_cast<Device*>(engine_ddi::command_list_shell(handle));
    }
    static Device* resolve(D3D12DDI_HCOMMANDQUEUE handle) noexcept {return resolve_queue_device(handle);}
    // Unsupported object-specific slots (e.g. metacommands) never borrow another
    // device's scope. Their creation already refuses in the engine boundary.
    template<class T> static Device* resolve(T) noexcept {return nullptr;}
    static void failure(Device* device,HRESULT hr) noexcept {
        if(!device)return;
        if(device_engine_entered(*device))report_device_error(*device,hr);
        else device->lost.store(true);
    }
};
engine_ddi::DeviceContext* APIENTRY resolve_engine(D3D12DDI_HDEVICE handle) {
    auto device=static_cast<Device*>(handle.pDrvPrivate);
    return device && device_engine_entered(*device)?engine_context(*device):nullptr;
}
UINT APIENTRY present_private_size(D3D12DDI_HDEVICE,const D3D12DDIARG_PRESENT_0001*) {return 0;}
void APIENTRY present(D3D12DDI_HCOMMANDLIST list,D3D12DDI_HCOMMANDQUEUE,
    const D3D12DDIARG_PRESENT_0001*,D3D12DDI_PRESENT_0051* result,
    D3D12DDI_PRESENT_CONTEXTS_0051* contexts,D3D12DDI_PRESENT_HWQUEUES_0051* queues) {
    if(result)*result={};if(contexts)*contexts={};if(queues)*queues={};
    if(auto device=EntryPolicy::resolve(list))report_device_error(*device,E_NOTIMPL);
}
using Queue=D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001;
using Extended=D3D12DDI_EXTENDED_FEATURES_FUNCS_0021;
const Queue& queue_original() noexcept {
    static const Queue table=[]() noexcept {Queue value{};fill_native_queue_table(&value);return value;}();return table;
}
const Extended& extended_original() noexcept {
    static const Extended table=[]() noexcept {Extended value{};fill_native_extended_table(&value);return value;}();return table;
}
struct QueueName_pfnExecuteCommandLists {static const char* name() noexcept {return "pfnExecuteCommandLists";}};
struct QueueName_pfnUpdateTileMappings {static const char* name() noexcept {return "pfnUpdateTileMappings";}};
struct QueueName_pfnCopyTileMappings {static const char* name() noexcept {return "pfnCopyTileMappings";}};
struct QueueName_pfnSignalFence {static const char* name() noexcept {return "pfnSignalFence";}};
struct QueueName_pfnWaitForFence {static const char* name() noexcept {return "pfnWaitForFence";}};
template<auto Member,class Name> struct QueueBinding {static const char* name() noexcept {return Name::name();} static auto original() noexcept {return queue_original().*Member;}};
struct ExtendedName_pfnGetSupportedExtendedFeatures {static const char* name() noexcept {return "pfnGetSupportedExtendedFeatures";}};
struct ExtendedName_pfnGetSupportedExtendedFeatureVersions {static const char* name() noexcept {return "pfnGetSupportedExtendedFeatureVersions";}};
struct ExtendedName_pfnEnableExtendedFeature {static const char* name() noexcept {return "pfnEnableExtendedFeature";}};
struct ExtendedName_pfnSetExtendedFeatureCallbacks {static const char* name() noexcept {return "pfnSetExtendedFeatureCallbacks";}};
template<auto Member,class Name> struct ExtendedBinding {static const char* name() noexcept {return Name::name();} static auto original() noexcept {return extended_original().*Member;}};
}
HRESULT fill_native_tables(Adapter& adapter,D3D12DDI_TABLE_TYPE type,void* output,SIZE_T size,
    UINT number,D3D12DDI_HRTTABLE runtime) noexcept {
    if(!output)return E_INVALIDARG;
    const engine_ddi::FillInfo fill{sizeof(fill),resolve_engine};
    switch(type){
    case D3D12DDI_TABLE_TYPE_DEVICE_CORE:{
        using Core=D3D12DDI_DEVICE_FUNCS_CORE_0088;
        if(number || size!=sizeof(Core))return E_INVALIDARG;
        Core shell{},original{},wrapped{};
        install_native_queue_entries(shell);install_fence_entries(shell);
        install_shell_core_entries(shell);install_native_residency_entries(shell);
        shell.pfnGetPresentPrivateDriverDataSize=present_private_size;
        HRESULT hr=compose_core_0092(&original,sizeof(original),shell,fill);if(hr!=S_OK)return hr;
        hr=DdiEntryTables<EntryPolicy>::wrap_core(original,&wrapped);if(hr!=S_OK)return hr;
        *static_cast<Core*>(output)=wrapped;return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_LIST_3D:{
        using List=D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
        if(number>1 || size!=sizeof(List) || !runtime.handle)return E_INVALIDARG;
        List original{},wrapped{};
        HRESULT hr=compose_list_0092(&original,sizeof(original),number,present,fill);if(hr!=S_OK)return hr;
        hr=DdiEntryTables<EntryPolicy>::wrap_list(number,original,&wrapped);if(hr!=S_OK)return hr;
        AcquireSRWLockExclusive(&adapter.tables_lock);
        auto& prior=adapter.list_tables[number];
        if(prior.handle && prior.handle!=runtime.handle)hr=E_UNEXPECTED;
        else{prior=runtime;*static_cast<List*>(output)=wrapped;}
        ReleaseSRWLockExclusive(&adapter.tables_lock);return hr;
    }
    case D3D12DDI_TABLE_TYPE_COMMAND_QUEUE_3D:{
        if(number || size!=sizeof(Queue))return E_INVALIDARG;
        Queue wrapped{};
#define WRAP_QUEUE(member) wrapped.member=&EntryThunk<decltype(wrapped.member),QueueBinding<&Queue::member,QueueName_##member>,EntryPolicy>::call;
        WRAP_QUEUE(pfnExecuteCommandLists)
        WRAP_QUEUE(pfnUpdateTileMappings)
        WRAP_QUEUE(pfnCopyTileMappings)
        WRAP_QUEUE(pfnSignalFence)
        WRAP_QUEUE(pfnWaitForFence)
#undef WRAP_QUEUE
        *static_cast<Queue*>(output)=wrapped;return S_OK;
    }
    case D3D12DDI_TABLE_TYPE_0020_EXTENDED_FEATURES:{
        if(number || size!=sizeof(Extended))return E_INVALIDARG;
        Extended wrapped{};
#define WRAP_EXTENDED(member) wrapped.member=&EntryThunk<decltype(wrapped.member),ExtendedBinding<&Extended::member,ExtendedName_##member>,EntryPolicy>::call;
        WRAP_EXTENDED(pfnGetSupportedExtendedFeatures)
        WRAP_EXTENDED(pfnGetSupportedExtendedFeatureVersions)
        WRAP_EXTENDED(pfnEnableExtendedFeature)
        WRAP_EXTENDED(pfnSetExtendedFeatureCallbacks)
#undef WRAP_EXTENDED
        *static_cast<Extended*>(output)=wrapped;return S_OK;
    }
    default:return E_NOTIMPL;
    }
}
}
