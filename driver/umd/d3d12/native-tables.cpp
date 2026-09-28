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

namespace native12 {
namespace {
struct EntryPolicy {
    using Scope=DeviceEngineScope;
    static uint64_t entry(Device*,const char* name) noexcept {return ddi_trace_begin(name);}
    static void leave(Device*,const char* name,uint64_t id,HRESULT outcome) noexcept {ddi_trace_end(name,id,outcome);}
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
