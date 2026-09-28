// SPDX-License-Identifier: MIT
#include "hosted-queue.h"
#include <d3dkmthk.h>
#include <cstring>

namespace native12 {
struct HostedQueue::Call {HostedQueue* self;Alias* alias;uint32_t op;void* argument;};
HostedQueue::HostedQueue(bc250::umd::RuntimeDomain& domain,QueueEngineRegistry& registry,Device& device) noexcept
    :domain_(domain),registry_(registry),device_(device),runtime_device_(device.runtime.handle),
    submit_(device.kernel_callbacks.pfnSubmitCommandCb),wait_(device.kernel_callbacks.pfnWaitForSynchronizationObjectFromGpuCb),
    signal_(device.kernel_callbacks.pfnSignalSynchronizationObjectFromGpu2Cb) {}
HostedQueue::Alias* HostedQueue::find(uint32_t token) noexcept {
    if(!token || (token&0x80000000u))return nullptr;
    for(auto& alias:aliases_)if(alias.cookie && alias.token==token)return &alias;
    return nullptr;
}
unsigned HostedQueue::discard_metadata() noexcept {
    // The caller has already excluded all callers. No callback runs here.
    AcquireSRWLockExclusive(&lock_);active_=false;unsigned count=0;
    for(auto& alias:aliases_){if(alias.cookie)++count;alias={};}
    ReleaseSRWLockExclusive(&lock_);return count;
}
HRESULT HostedQueue::dispatch(void* user,uint32_t op,void* argument) noexcept {
    auto self=static_cast<HostedQueue*>(user);
    if(!self || !self->domain_.entered() || !argument || !self->runtime_device_)return E_INVALIDARG;
    if(self->device_.lost.load())return D3DDDIERR_DEVICEREMOVED;
    return self->invoke(op,argument);
}
HRESULT HostedQueue::invoke(uint32_t op,void* argument) noexcept {
    bool create=op==BC250_HOST_CREATE_QUEUE_CONTEXT;
    bool destroy=op==BC250_HOST_DESTROY_QUEUE_CONTEXT;
    void* cookie=nullptr;uint32_t token=0;void* payload=argument;
    if(create || destroy){
        auto& wrapper=*static_cast<bc250_host_queue_context*>(argument);
        if(!wrapper.queue || !wrapper.arguments)return E_INVALIDARG;
        cookie=wrapper.queue;payload=wrapper.arguments;
        if(create){
            const auto& a=*static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(payload);
            // Current registry contexts all use this exact GFX BC2C request.
            // Aliasing cannot silently change node/engine/context policy.
            ContextRequest expected;D3D12DDIARG_CREATECOMMANDQUEUE_0050 desc{};desc.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
            if(expected.prepare(desc)!=S_OK || a.hContext || a.NodeOrdinal!=expected.args.NodeOrdinal ||
               a.EngineAffinity!=expected.args.EngineAffinity || a.Flags.Value || !a.pPrivateDriverData ||
               a.PrivateDriverDataSize!=sizeof(expected.blob) || std::memcmp(a.pPrivateDriverData,&expected.blob,sizeof(expected.blob)))return E_NOTIMPL;
        }else token=static_cast<D3DKMT_DESTROYCONTEXT*>(payload)->hContext;
    }else if(op==BC250_HOST_PUBLISH_PROGRESS)token=static_cast<bc250_host_progress*>(payload)->context;
    else if(op==BC250_HOST_WaitForSynchronizationObjectFromGpu)token=static_cast<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU*>(payload)->hContext;
    else if(op==BC250_HOST_SignalSynchronizationObjectFromGpu2){
        const auto& a=*static_cast<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2*>(payload);
        if(a.BroadcastContextCount!=1 || !a.BroadcastContextArray)return E_NOTIMPL;
        token=a.BroadcastContextArray[0];
    }else if(op==BC250_HOST_SubmitCommand){
        const auto& a=*static_cast<D3DKMT_SUBMITCOMMAND*>(payload);
        if(a.BroadcastContextCount!=1)return E_NOTIMPL;token=a.BroadcastContext[0];
    }else return E_NOTIMPL;

    Alias* alias=nullptr;
    AcquireSRWLockExclusive(&lock_);
    if(!active_){ReleaseSRWLockExclusive(&lock_);return E_UNEXPECTED;}
    if(create){
        for(auto& entry:aliases_)if(entry.cookie==cookie){ReleaseSRWLockExclusive(&lock_);return E_UNEXPECTED;}
        if(next_token_==0x7fffffffu){ReleaseSRWLockExclusive(&lock_);return E_OUTOFMEMORY;}
        for(auto& entry:aliases_)if(!entry.cookie){alias=&entry;break;}
        if(!alias){ReleaseSRWLockExclusive(&lock_);return E_OUTOFMEMORY;}
        alias->cookie=cookie;alias->token=++next_token_;alias->busy=true;
    }else{
        alias=find(token);
        if(!alias || (destroy && alias->cookie!=cookie)){ReleaseSRWLockExclusive(&lock_);return E_INVALIDARG;}
        if(alias->busy){ReleaseSRWLockExclusive(&lock_);return E_PENDING;}
        alias->busy=true;
    }
    ReleaseSRWLockExclusive(&lock_);
    // Neither alias metadata lock nor registry lock is held over a callback.
    // busy excludes overlapping operations/retirement on this alias; different
    // aliases remain independent. Registry pin excludes physical queue teardown.
    Call call{this,alias,op,payload};
    HRESULT hr=registry_.with_binding(alias->cookie,device_,pinned,&call);
    AcquireSRWLockExclusive(&lock_);
    if(create && hr==S_OK)static_cast<D3DKMT_CREATECONTEXTVIRTUAL*>(payload)->hContext=alias->token;
    if((create && hr!=S_OK) || (destroy && hr==S_OK))*alias={};else alias->busy=false;
    ReleaseSRWLockExclusive(&lock_);return hr;
}
HRESULT APIENTRY HostedQueue::pinned(void* user,const QueueBindingView* view){
    auto& call=*static_cast<Call*>(user);auto& self=*call.self;auto& alias=*call.alias;
    if(!view || !view->context || !view->runtime_queue.handle)return E_INVALIDARG;
    if(call.op==BC250_HOST_CREATE_QUEUE_CONTEXT){
        // No new runtime/KMD context is created. Keep identity for subsequent
        // pinned validation; the view itself is never retained.
        alias.context=view->context;alias.runtime_queue=view->runtime_queue;return S_OK;
    }
    if(alias.context!=view->context || alias.runtime_queue.handle!=view->runtime_queue.handle)return E_INVALIDARG;
    if(call.op==BC250_HOST_DESTROY_QUEUE_CONTEXT)return S_OK; // Alias only.
    if(call.op==BC250_HOST_PUBLISH_PROGRESS){
        const auto& a=*static_cast<bc250_host_progress*>(call.argument);
        if(!a.sync || !a.cpu_address || !a.value || a.value==UINT64_MAX ||
           (alias.progress_sync && (alias.progress_sync!=a.sync || a.value<=alias.progress_value)))return E_INVALIDARG;
        if(*static_cast<const volatile UINT64*>(a.cpu_address)==UINT64_MAX)return D3DDDIERR_DEVICEREMOVED;
        alias.progress_sync=a.sync;alias.progress_value=a.value;
        // Never retain/dereference the borrowed CPU pointer after this call.
        return S_OK;
    }
    if(call.op==BC250_HOST_WaitForSynchronizationObjectFromGpu){
        const auto& a=*static_cast<D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU*>(call.argument);
        if(!a.ObjectCount || !a.ObjectHandleArray || !a.MonitoredFenceValueArray)return E_INVALIDARG;
        D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU b{};b.hContext=view->context;b.ObjectCount=a.ObjectCount;
        b.ObjectHandleArray=a.ObjectHandleArray;b.MonitoredFenceValueArray=a.MonitoredFenceValueArray;
        return self.wait_?self.wait_(self.runtime_device_,&b):E_NOTIMPL;
    }
    if(call.op==BC250_HOST_SignalSynchronizationObjectFromGpu2){
        const auto& a=*static_cast<D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2*>(call.argument);
        if(a.Flags.Value || !a.ObjectCount || !a.ObjectHandleArray || !a.MonitoredFenceValueArray)return E_INVALIDARG;
        HANDLE context=view->context;D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 b{};
        b.BroadcastContextCount=1;b.BroadcastContextArray=&context;b.ObjectCount=a.ObjectCount;
        b.ObjectHandleArray=a.ObjectHandleArray;b.MonitoredFenceValueArray=a.MonitoredFenceValueArray;
        return self.signal_?self.signal_(self.runtime_device_,&b):E_NOTIMPL;
    }
    if(call.op==BC250_HOST_SubmitCommand){
        const auto& a=*static_cast<D3DKMT_SUBMITCOMMAND*>(call.argument);
        if(a.NumPrimaries>D3DDDI_MAX_WRITTEN_PRIMARIES || a.Flags.NullRendering || a.Flags.PresentRedirected ||
           a.Flags.NoKmdAccess || a.Flags.Reserved || a.PresentHistoryToken || a.NumHistoryBuffers)return E_NOTIMPL;
        if((a.PrivateDriverDataSize&&!a.pPrivateDriverData) || !a.Commands || !a.CommandLength)return E_INVALIDARG;
        D3DDDICB_SUBMITCOMMAND b{};b.Commands=a.Commands;b.CommandLength=a.CommandLength;
        b.pPrivateDriverData=a.pPrivateDriverData;b.PrivateDriverDataSize=a.PrivateDriverDataSize;
        b.BroadcastContextCount=1;b.BroadcastContext[0]=view->context;b.NumPrimaries=a.NumPrimaries;
        for(UINT i=0;i<a.NumPrimaries;++i)b.WrittenPrimaries[i]=a.WrittenPrimaries[i];
        return self.submit_?self.submit_(self.runtime_device_,&b):E_NOTIMPL;
    }
    return E_NOTIMPL;
}
}
