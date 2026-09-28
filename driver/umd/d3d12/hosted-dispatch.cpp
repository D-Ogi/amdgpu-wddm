// SPDX-License-Identifier: MIT
#include "hosted-dispatch.h"
#include <d3dkmthk.h>
#include <new>
#include <cstring>

namespace native12 {
struct HostedDispatch::Allocation {
    RuntimeAllocation owner;
    Allocation* next{};
    UINT64 va{},bytes{};
    bool busy{},locked{};
    Allocation(D3D12DDI_HRTDEVICE runtime,const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb)
        :owner(runtime,cb) {}
};
struct HostedDispatch::Sync {
    Sync* next{};
    D3DKMT_HANDLE handle{};
    D3DDDI_SYNCHRONIZATIONOBJECT_TYPE type{};
    const volatile UINT64* cpu{};
    bool busy{};
};
HostedDispatch::HostedDispatch(bc250::umd::RuntimeDomain& domain,D3D12DDI_HRTDEVICE runtime,
    const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& user,const D3DDDI_DEVICECALLBACKS& kernel,
    HostedDispatchHooks hooks) noexcept:domain_(domain),runtime_(runtime),user_(user),kernel_(kernel),hooks_(hooks) {}
HostedDispatch::~HostedDispatch(){discard_metadata();}
unsigned HostedDispatch::discard_metadata() noexcept {
    active_=false;unsigned unresolved=0;
    while(allocations_){auto record=allocations_;allocations_=record->next;
        if(record->owner.handle())++unresolved;
        record->owner.invalidate_runtime();delete record;}
    for(auto& ctx:contexts_){if(ctx.handle)++unresolved;ctx={};}
    while(syncs_){auto sync=syncs_;syncs_=sync->next;if(sync->handle)++unresolved;delete sync;}
    if(paging_.queue)++unresolved;paging_={};
    user_={};kernel_={};hooks_={};runtime_={};return unresolved;
}
HostedDispatch::Allocation* HostedDispatch::find(D3DKMT_HANDLE handle) noexcept {
    if(!handle)return nullptr;
    for(auto record=allocations_;record;record=record->next)
        if(record->owner.handle()==handle)return record;
    return nullptr;
}
HostedDispatch::Sync* HostedDispatch::find_sync(D3DKMT_HANDLE handle) noexcept {
    if(!handle)return nullptr;
    for(auto sync=syncs_;sync;sync=sync->next)if(sync->handle==handle)return sync;
    return nullptr;
}
HRESULT HostedDispatch::remove_device() noexcept {
    if(!lost_.exchange(true) && user_.pfnSetErrorCb)user_.pfnSetErrorCb(runtime_,D3DDDIERR_DEVICEREMOVED);
    return D3DDDIERR_DEVICEREMOVED;
}
bc250_host HostedDispatch::descriptor(uint64_t luid,void* identity) noexcept {
    bc250_host host{};host.sType=BC250_HOST_STYPE;host.version=BC250_HOST_VERSION;
    host.size=sizeof(host);host.adapter_luid=luid;host.identity=identity;
    host.userdata=this;host.dispatch=dispatch;return host;
}
HostedDispatch::Context* HostedDispatch::context(uint32_t token) noexcept {
    for(auto& ctx:contexts_)if(ctx.handle && ctx.token==token)return &ctx;
    return nullptr;
}
HRESULT HostedDispatch::internal_queue(uint32_t op,void* argument) noexcept {
    auto delegate=[&]() noexcept {return hooks_.queue?hooks_.queue(hooks_.userdata,op,argument):E_NOTIMPL;};
#define KT_QUEUE(name,args) (kernel_.pfn##name##Cb?kernel_.pfn##name##Cb(runtime_.handle,args):E_NOTIMPL)
    if(op==BC250_HOST_CREATE_QUEUE_CONTEXT || op==BC250_HOST_DESTROY_QUEUE_CONTEXT){
        auto& wrapper=*static_cast<bc250_host_queue_context*>(argument);
        if(wrapper.queue)return delegate();
        if(!wrapper.arguments)return E_INVALIDARG;
        if(op==BC250_HOST_CREATE_QUEUE_CONTEXT){
            auto& a=*static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(wrapper.arguments);
            if(a.hContext || next_context_==0x7fffffffu)return E_INVALIDARG;
            Context* ctx=nullptr;for(auto& entry:contexts_)if(!entry.handle && !entry.busy){ctx=&entry;break;}
            if(!ctx)return E_OUTOFMEMORY;
            D3DDDICB_CREATECONTEXTVIRTUAL b{};b.NodeOrdinal=a.NodeOrdinal;b.EngineAffinity=a.EngineAffinity;b.Flags=a.Flags;
            b.pPrivateDriverData=a.pPrivateDriverData;b.PrivateDriverDataSize=a.PrivateDriverDataSize;
            ctx->busy=true;HRESULT hr=KT_QUEUE(CreateContextVirtual,&b);ctx->busy=false;
            if(SUCCEEDED(hr)){
                ctx->handle=b.hContext;ctx->token=0x80000000u|++next_context_;
                if(!ctx->handle)return remove_device();a.hContext=ctx->token;
            }
            return hr;
        }
        auto& a=*static_cast<D3DKMT_DESTROYCONTEXT*>(wrapper.arguments);auto ctx=context(a.hContext);
        if(!ctx || ctx->busy)return E_INVALIDARG;
        D3DDDICB_DESTROYCONTEXT b{};b.hContext=ctx->handle;
        ctx->busy=true;HRESULT hr=KT_QUEUE(DestroyContext,&b);ctx->busy=false;
        if(hr==S_OK)*ctx={};return hr;
    }
    if(op==BC250_HOST_PUBLISH_PROGRESS){
        auto& a=*static_cast<bc250_host_progress*>(argument);auto ctx=context(a.context);
        if(!ctx)return delegate();
        if(!a.sync || !a.cpu_address || !a.value || a.value==UINT64_MAX ||
           (ctx->progress.sync && (ctx->progress.sync!=a.sync || ctx->progress.value>=a.value)))return E_INVALIDARG;
        ctx->progress=a;return S_OK;
    }
    if(op==BC250_HOST_WaitForSynchronizationObjectFromGpu){
        auto& a=*static_cast<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU*>(argument);auto ctx=context(a.hContext);
        if(!ctx)return delegate();if(ctx->busy || !a.ObjectCount || !a.ObjectHandleArray || !a.MonitoredFenceValueArray)return E_INVALIDARG;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU b{};b.hContext=ctx->handle;
        b.ObjectCount=a.ObjectCount;b.ObjectHandleArray=a.ObjectHandleArray;b.MonitoredFenceValueArray=a.MonitoredFenceValueArray;
        return KT_QUEUE(WaitForSynchronizationObjectFromGpu,&b);
    }
    if(op==BC250_HOST_SignalSynchronizationObjectFromGpu2){
        auto& a=*static_cast<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2*>(argument);
        if(!a.BroadcastContextCount || a.BroadcastContextCount>D3DDDI_MAX_BROADCAST_CONTEXT || a.Flags.Value ||
           !a.ObjectCount || !a.ObjectHandleArray || !a.MonitoredFenceValueArray)return E_INVALIDARG;
        HANDLE handles[D3DDDI_MAX_BROADCAST_CONTEXT]{};
        for(UINT i=0;i<a.BroadcastContextCount;++i){auto ctx=context(a.BroadcastContextArray[i]);if(!ctx)return delegate();if(ctx->busy)return E_INVALIDARG;handles[i]=ctx->handle;}
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 b{};b.ObjectCount=a.ObjectCount;b.ObjectHandleArray=a.ObjectHandleArray;
        b.BroadcastContextCount=a.BroadcastContextCount;b.BroadcastContextArray=handles;b.MonitoredFenceValueArray=a.MonitoredFenceValueArray;
        return KT_QUEUE(SignalSynchronizationObjectFromGpu2,&b);
    }
    if(op==BC250_HOST_SubmitCommand){
        auto& a=*static_cast<D3DKMT_SUBMITCOMMAND*>(argument);
        if(!a.BroadcastContextCount || a.BroadcastContextCount>D3DDDI_MAX_BROADCAST_CONTEXT || a.NumPrimaries>D3DDDI_MAX_WRITTEN_PRIMARIES ||
           a.Flags.NullRendering || a.Flags.PresentRedirected || a.Flags.NoKmdAccess || a.Flags.Reserved || a.PresentHistoryToken || a.NumHistoryBuffers)return E_NOTIMPL;
        D3DDDICB_SUBMITCOMMAND b{};b.Commands=a.Commands;b.CommandLength=a.CommandLength;
        b.pPrivateDriverData=a.pPrivateDriverData;b.PrivateDriverDataSize=a.PrivateDriverDataSize;b.BroadcastContextCount=a.BroadcastContextCount;
        for(UINT i=0;i<a.BroadcastContextCount;++i){auto ctx=context(a.BroadcastContext[i]);if(!ctx)return delegate();if(ctx->busy)return E_INVALIDARG;b.BroadcastContext[i]=ctx->handle;}
        b.NumPrimaries=a.NumPrimaries;for(UINT i=0;i<a.NumPrimaries;++i)b.WrittenPrimaries[i]=a.WrittenPrimaries[i];
        return KT_QUEUE(SubmitCommand,&b);
    }
    return delegate();
#undef KT_QUEUE
}
HRESULT HostedDispatch::operation(uint32_t op,void* argument) noexcept {
    if(op==BC250_HOST_CHECK_STATUS){
        if(paging_.cpu_address && *static_cast<const volatile UINT64*>(paging_.cpu_address)==UINT64_MAX)return remove_device();
        for(auto sync=syncs_;sync;sync=sync->next)if(!sync->busy && sync->cpu && *sync->cpu==UINT64_MAX)return remove_device();
        return lost()?D3DDDIERR_DEVICEREMOVED:S_OK;
    }
    if(op==BC250_HOST_REPORT_LOST)return remove_device();
    if(!argument)return E_INVALIDARG;
    if(lost())return D3DDDIERR_DEVICEREMOVED;
    // Validate both internal and application queue progress before delegation.
    // Queue hooks must not retain the borrowed CPU mapping after this call.
    if(op==BC250_HOST_PUBLISH_PROGRESS){
        const auto& a=*static_cast<const bc250_host_progress*>(argument);
        auto sync=find_sync(a.sync);
        if(!sync || sync->busy || sync->type!=D3DDDI_MONITORED_FENCE || !sync->cpu ||
           sync->cpu!=a.cpu_address || !a.value || a.value==UINT64_MAX)return E_INVALIDARG;
    }
#define KT_CALL(name,args) (kernel_.pfn##name##Cb?kernel_.pfn##name##Cb(runtime_.handle,args):E_NOTIMPL)
    switch(op){
    case BC250_HOST_CREATE_PAGING:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        if(paging_.queue)return E_UNEXPECTED;
        D3DDDICB_CREATEPAGINGQUEUE b{};b.Priority=D3DDDI_PAGINGQUEUE_PRIORITY_NORMAL;
        HRESULT hr=KT_CALL(CreatePagingQueue,&b);
        if(hr==S_OK){paging_={b.hPagingQueue,b.hSyncObject,b.FenceValueCPUVirtualAddress};
            *static_cast<bc250_host_paging*>(argument)=paging_;
            if(!paging_.queue || !paging_.sync || !paging_.cpu_address)return remove_device();}
        return hr;
    }
    case BC250_HOST_DESTROY_PAGING:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        if(!paging_.queue || static_cast<bc250_host_paging*>(argument)->queue!=paging_.queue || allocations_)return E_INVALIDARG;
        D3DDDI_DESTROYPAGINGQUEUE b{};b.hPagingQueue=paging_.queue;
        HRESULT hr=KT_CALL(DestroyPagingQueue,&b);if(hr==S_OK)paging_={};return hr;
    }
    case BC250_HOST_MakeResident:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        auto& a=*static_cast<D3DDDI_MAKERESIDENT*>(argument);
        if(!paging_.queue || a.hPagingQueue!=paging_.queue || !a.NumAllocations || !a.AllocationList || a.Flags.Reserved)return E_INVALIDARG;
        for(UINT i=0;i<a.NumAllocations;++i)if(!find(a.AllocationList[i]))return E_INVALIDARG;
        const UINT count=a.NumAllocations;
        HRESULT hr=KT_CALL(MakeResident,&a);
        if((hr==S_OK || hr==E_PENDING) && (a.NumAllocations!=count ||
           (hr==E_PENDING && (!a.PagingFenceValue || a.PagingFenceValue==UINT64_MAX))))return remove_device();
        // S_OK is immediately resident; only E_PENDING defines this output.
        // RADV combines it with the mapping fence for every success.
        if(hr==S_OK)a.PagingFenceValue=0;
        return hr;
    }
    case BC250_HOST_Evict:{
        if(hooks_.paging)return hooks_.paging(hooks_.userdata,op,argument);
        auto& a=*static_cast<D3DKMT_EVICT*>(argument);
        if(!a.NumAllocations || !a.AllocationList || a.Flags.Reserved)return E_INVALIDARG;
        for(UINT i=0;i<a.NumAllocations;++i)if(!find(a.AllocationList[i]))return E_INVALIDARG;
        D3DDDICB_EVICT b{};b.NumAllocations=a.NumAllocations;b.AllocationList=a.AllocationList;b.Flags=a.Flags;
        HRESULT hr=KT_CALL(Evict,&b);a.NumBytesToTrim=b.NumBytesToTrim;return hr;
    }
    case BC250_HOST_PUBLISH_PROGRESS:case BC250_HOST_CREATE_QUEUE_CONTEXT:case BC250_HOST_DESTROY_QUEUE_CONTEXT:
    case BC250_HOST_CreateContextVirtual:case BC250_HOST_DestroyContext:
    case BC250_HOST_WaitForSynchronizationObjectFromGpu:case BC250_HOST_SignalSynchronizationObjectFromGpu:
    case BC250_HOST_SignalSynchronizationObjectFromGpu2:case BC250_HOST_SubmitCommand:case BC250_HOST_UpdateGpuVirtualAddress:
    case BC250_HOST_CreateHwQueue:case BC250_HOST_DestroyHwQueue:case BC250_HOST_SubmitCommandToHwQueue:
    case BC250_HOST_SubmitWaitForSyncObjectsToHwQueue:case BC250_HOST_SubmitSignalSyncObjectsToHwQueue:
        return internal_queue(op,argument);
    case BC250_HOST_CreateAllocation2:{
        auto& a=*static_cast<D3DKMT_CREATEALLOCATION*>(argument);
        auto flags=a.Flags;flags.CreateResource=0;flags.NonSecure=0;
        D3DKMT_CREATEALLOCATIONFLAGS zero_flags{};
        if(a.hResource || a.hGlobalShare || a.NumAllocations!=1 || !a.pAllocationInfo2 ||
           std::memcmp(&flags,&zero_flags,sizeof(flags)) || (a.PrivateDriverDataSize&&!a.pPrivateDriverData))return E_INVALIDARG;
        const auto& input=*a.pAllocationInfo2;
        if(input.hAllocation || input.Flags.Value || (input.PrivateDriverDataSize&&!input.pPrivateDriverData))return E_INVALIDARG;
        auto record=new(std::nothrow) Allocation(runtime_,user_);if(!record)return E_OUTOFMEMORY;
        // Tracking storage exists before the runtime may allocate anything.
        record->busy=true;record->next=allocations_;allocations_=record;
        D3D12DDI_ALLOCATION_INFO_0022 info{};
        info.pSystemMem=input.pSystemMem;info.pPrivateDriverData=input.pPrivateDriverData;
        info.PrivateDriverDataSize=input.PrivateDriverDataSize;info.VidPnSourceId=input.VidPnSourceId;info.Priority=input.Priority;
        D3D12DDICB_ALLOCATE_0022 request{};request.pPrivateDriverData=a.pPrivateDriverData;
        request.PrivateDriverDataSize=a.PrivateDriverDataSize;request.NumAllocations=1;request.pAllocationInfo=&info;
        HRESULT hr=record->owner.open(request);record->busy=false;
        if(FAILED(hr)){
            auto link=&allocations_;while(*link!=record)link=&(*link)->next;*link=record->next;delete record;return hr;
        }
        a.pAllocationInfo2->hAllocation=record->owner.handle();a.pAllocationInfo2->GpuVirtualAddress=record->owner.address();
        // Native hKMResource is reserved. Internal allocations are released by handle list.
        a.hResource=0;return S_OK;
    }
    case BC250_HOST_DestroyAllocation2:{
        auto& a=*static_cast<D3DKMT_DESTROYALLOCATION2*>(argument);
        if(a.hResource || a.Flags.Value || a.AllocationCount!=1 || !a.phAllocationList)return E_INVALIDARG;
        auto record=find(a.phAllocationList[0]);if(!record || record->busy || record->locked || record->va)return E_INVALIDARG;
        record->busy=true;HRESULT hr=record->owner.close();record->busy=false;
        if(SUCCEEDED(hr)){auto link=&allocations_;while(*link!=record)link=&(*link)->next;*link=record->next;delete record;}
        return hr;
    }
    case BC250_HOST_MapGpuVirtualAddress:{
        auto& a=*static_cast<D3DDDI_MAPGPUVIRTUALADDRESS*>(argument);auto record=find(a.hAllocation);
        if(!record || record->busy || record->va || !a.SizeInPages || a.SizeInPages>UINT64_MAX/4096)return E_INVALIDARG;
        if(!hooks_.paging && (!paging_.queue || a.hPagingQueue!=paging_.queue))return E_INVALIDARG;
        record->busy=true;HRESULT hr=KT_CALL(MapGpuVirtualAddress,&a);record->busy=false;
        if(hr==S_OK || hr==E_PENDING){record->va=a.VirtualAddress;record->bytes=a.SizeInPages*4096;
            if(!record->va || (record->va&4095) || record->va>UINT64_MAX-record->bytes ||
               (hr==E_PENDING && (!a.PagingFenceValue || a.PagingFenceValue==UINT64_MAX)))return remove_device();}
        return hr;
    }
    case BC250_HOST_FreeGpuVirtualAddress:{
        auto& a=*static_cast<D3DKMT_FREEGPUVIRTUALADDRESS*>(argument);Allocation* record=nullptr;
        for(auto r=allocations_;r;r=r->next)if(r->va && r->va==a.BaseAddress && r->bytes==a.Size){record=r;break;}
        if(!record || record->busy || record->locked)return E_INVALIDARG;
        D3DDDICB_FREEGPUVIRTUALADDRESS b{};b.BaseAddress=a.BaseAddress;b.Size=a.Size;
        record->busy=true;HRESULT hr=KT_CALL(FreeGpuVirtualAddress,&b);record->busy=false;
        if(hr==S_OK){record->va=record->bytes=0;}return hr;
    }
    case BC250_HOST_Lock2:{
        auto& a=*static_cast<D3DKMT_LOCK2*>(argument);auto record=find(a.hAllocation);
        if(!record || record->busy || record->locked)return E_INVALIDARG;
        D3DDDICB_LOCK2 b{};b.hAllocation=a.hAllocation;b.Flags.Value=a.Flags.Value;
        record->busy=true;HRESULT hr=KT_CALL(Lock2,&b);record->busy=false;
        if(hr==S_OK){record->locked=true;a.pData=b.pData;if(!b.pData)return remove_device();}return hr;
    }
    case BC250_HOST_Unlock2:{
        auto& a=*static_cast<D3DKMT_UNLOCK2*>(argument);auto record=find(a.hAllocation);
        if(!record || record->busy || !record->locked)return E_INVALIDARG;
        D3DDDICB_UNLOCK2 b{};b.hAllocation=a.hAllocation;
        record->busy=true;HRESULT hr=KT_CALL(Unlock2,&b);record->busy=false;
        if(hr==S_OK)record->locked=false;return hr;
    }
    case BC250_HOST_CreateSynchronizationObject2:{
        auto& a=*static_cast<D3DKMT_CREATESYNCHRONIZATIONOBJECT2*>(argument);
        if(a.Info.Flags.Shared || a.Info.Flags.NtSecuritySharing)return E_NOTIMPL;
        if(a.hSyncObject)return E_INVALIDARG;
        auto sync=new(std::nothrow) Sync;if(!sync)return E_OUTOFMEMORY;
        sync->busy=true;sync->next=syncs_;syncs_=sync;
        D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 b{};b.Info=a.Info;
        HRESULT hr=KT_CALL(CreateSynchronizationObject2,&b);sync->busy=false;
        if(hr!=S_OK){auto link=&syncs_;while(*link!=sync)link=&(*link)->next;*link=sync->next;delete sync;return hr;}
        const bool duplicate=find_sync(b.hSyncObject)!=nullptr;
        sync->handle=b.hSyncObject;sync->type=b.Info.Type;
        if(sync->type==D3DDDI_MONITORED_FENCE)sync->cpu=static_cast<const volatile UINT64*>(b.Info.MonitoredFence.FenceValueCPUVirtualAddress);
        if(!sync->handle || duplicate || (sync->type==D3DDDI_MONITORED_FENCE && !sync->cpu))return remove_device();
        a.Info=b.Info;a.hSyncObject=b.hSyncObject;return S_OK;
    }
    case BC250_HOST_DestroySynchronizationObject:{
        D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT b{};
        b.hSyncObject=static_cast<D3DKMT_DESTROYSYNCHRONIZATIONOBJECT*>(argument)->hSyncObject;
        auto sync=find_sync(b.hSyncObject);if(!sync || sync->busy)return E_INVALIDARG;
        sync->busy=true;HRESULT hr=KT_CALL(DestroySynchronizationObject,&b);sync->busy=false;
        if(hr==S_OK){
            for(auto& ctx:contexts_)if(ctx.progress.sync==b.hSyncObject)ctx.progress={};
            auto link=&syncs_;while(*link!=sync)link=&(*link)->next;*link=sync->next;delete sync;
        }
        return hr;
    }
    case BC250_HOST_WaitForSynchronizationObjectFromCpu:{
        auto& a=*static_cast<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU*>(argument);
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU b{};
        b.ObjectCount=a.ObjectCount;b.ObjectHandleArray=a.ObjectHandleArray;b.FenceValueArray=a.FenceValueArray;
        b.hAsyncEvent=a.hAsyncEvent;b.Flags=a.Flags;return KT_CALL(WaitForSynchronizationObjectFromCpu,&b);
    }
    case BC250_HOST_SignalSynchronizationObjectFromCpu:{
        auto& a=*static_cast<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU*>(argument);
        if(a.Flags.Value)return E_NOTIMPL;
        D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU b{};b.ObjectCount=a.ObjectCount;
        b.ObjectHandleArray=a.ObjectHandleArray;b.FenceValueArray=a.FenceValueArray;return KT_CALL(SignalSynchronizationObjectFromCpu,&b);
    }
    default:return E_NOTIMPL;
    }
#undef KT_CALL
}
int32_t HostedDispatch::dispatch(void* userdata,uint32_t op,void* argument) noexcept {
    auto self=static_cast<HostedDispatch*>(userdata);
    if(!self || !self->active_ || !self->runtime_.handle || !self->domain_.entered())return static_cast<int32_t>(0xc000000du);
    HRESULT hr=self->operation(op,argument);
    if(hr==D3DDDIERR_DEVICEREMOVED || hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET || hr==DXGI_ERROR_DEVICE_HUNG){
        self->remove_device();return static_cast<int32_t>(0xc00002b6u);}
    if(hr==E_PENDING && (op==BC250_HOST_MapGpuVirtualAddress || op==BC250_HOST_MakeResident))return 0x103;
    if(hr==S_OK)return 0;
    return static_cast<int32_t>(hr==E_NOTIMPL?0xc00000bbu:hr==E_INVALIDARG?0xc000000du:hr==E_OUTOFMEMORY?0xc0000017u:0xc0000001u);
}
}
