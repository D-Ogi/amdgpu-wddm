// SPDX-License-Identifier: MIT
#include "hosted-queue.h"
#include <d3dkmthk.h>
#include <cassert>
#include <cstdio>
#include <memory>
#include <thread>
namespace {
struct Fixture;
struct RuntimeQueue {Fixture* owner;HANDLE context;bool live{};};
struct EngineQueue {Fixture* owner;void* cookie;UINT token;};
HRESULT make(engine_ddi::DeviceContext*,const BC250_VKD3D_COMMAND_QUEUE_DESC*,void*,engine_ddi::EngineQueue**) noexcept;
HRESULT execute(engine_ddi::EngineQueue*,UINT,const D3D12DDI_HCOMMANDLIST*) noexcept;
HRESULT close(engine_ddi::EngineQueue**) noexcept;
HRESULT healthy(void*) noexcept{return S_OK;}
struct Fixture {
 native12::Device device;
 bc250::umd::RuntimeDomain domain;
 native12::QueueEngineRegistry registry;
 std::unique_ptr<native12::HostedQueue> bridge;
 RuntimeQueue runtime[2];
 native12::QueueEngineSlot slots[2]{};
 UINT tokens[2]{};
 unsigned creates{},destroys{},submits{},waits{},signals{},updates{};
 bool fail_submit{},reentrant{};
 Fixture():registry(device,reinterpret_cast<engine_ddi::DeviceContext*>(this),{make,execute,close,healthy,this}){
  runtime[0]={this,reinterpret_cast<HANDLE>(UINT64_C(0x100000001))};runtime[1]={this,reinterpret_cast<HANDLE>(UINT64_C(0x200000002))};
  device.runtime.handle=this;
  device.callbacks.pfnCreateContextVirtualCb=[](D3D12DDI_HRTCOMMANDQUEUE rt,D3DDDICB_CREATECONTEXTVIRTUAL* a)->HRESULT{
   auto r=static_cast<RuntimeQueue*>(rt.handle);assert(!r->live);r->live=true;++r->owner->creates;a->hContext=r->context;return S_OK;};
  device.callbacks.pfnDestroyContextCb=[](D3D12DDI_HRTCOMMANDQUEUE rt,const D3DDDICB_DESTROYCONTEXT* a)->HRESULT{
   auto r=static_cast<RuntimeQueue*>(rt.handle);assert(r->live && a->hContext==r->context);r->live=false;++r->owner->destroys;return S_OK;};
  device.kernel_callbacks.pfnSubmitCommandCb=[](HANDLE h,const D3DDDICB_SUBMITCOMMAND* a)->HRESULT{
   auto f=static_cast<Fixture*>(h);assert(a->BroadcastContextCount==1);unsigned index=a->BroadcastContext[0]==f->runtime[0].context?0:1;
   assert(a->BroadcastContext[0]==f->runtime[index].context && f->runtime[index].live && a->Commands==0x12340000 && a->CommandLength==16);
   ++f->submits;
   if(f->reentrant){
    D3DKMT_DESTROYCONTEXT d{};d.hContext=f->tokens[index];bc250_host_queue_context w{f->slots[index].cookie,&d};
    assert(native12::HostedQueue::dispatch(f->bridge.get(),BC250_HOST_DESTROY_QUEUE_CONTEXT,&w)==E_PENDING);
   D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op{};D3DKMT_UPDATEGPUVIRTUALADDRESS u{};u.hContext=f->tokens[index];
   u.hFenceObject=71;u.NumOperations=1;u.Operations=&op;u.FenceValue=9;
   assert(native12::HostedQueue::dispatch(f->bridge.get(),BC250_HOST_UpdateGpuVirtualAddress,&u)==E_PENDING && f->updates==index);
    assert(f->registry.destroy(f->slots[index])==E_PENDING && f->slots[index].cookie);
   }
   return f->fail_submit?E_FAIL:S_OK;};
  device.kernel_callbacks.pfnWaitForSynchronizationObjectFromGpuCb=[](HANDLE h,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a)->HRESULT{
   auto f=static_cast<Fixture*>(h);assert((a->hContext==f->runtime[0].context || a->hContext==f->runtime[1].context) && a->ObjectCount==1 && a->ObjectHandleArray[0]==71 && a->MonitoredFenceValueArray[0]==9);++f->waits;return S_OK;};
  device.kernel_callbacks.pfnSignalSynchronizationObjectFromGpu2Cb=[](HANDLE h,const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2* a)->HRESULT{
   auto f=static_cast<Fixture*>(h);assert(a->BroadcastContextCount==1 && (a->BroadcastContextArray[0]==f->runtime[0].context || a->BroadcastContextArray[0]==f->runtime[1].context));
   assert(a->ObjectCount==1 && a->ObjectHandleArray[0]==71 && a->MonitoredFenceValueArray[0]==9);++f->signals;return S_OK;};
  device.kernel_callbacks.pfnUpdateGpuVirtualAddressCb=[](HANDLE h,const D3DDDICB_UPDATEGPUVIRTUALADDRESS* a)->HRESULT{
   auto f=static_cast<Fixture*>(h);assert(a->hContext==f->runtime[0].context || a->hContext==f->runtime[1].context);
   assert(a->hFenceObject==71 && a->NumOperations==2 && a->Operations && a->FenceValue==9 && !a->Reserved0 && !a->Reserved1 && !a->Flags.Value);
   ++f->updates;return S_OK;};
  bridge=std::make_unique<native12::HostedQueue>(domain,registry,device);
 }
};
HRESULT make(engine_ddi::DeviceContext* d,const BC250_VKD3D_COMMAND_QUEUE_DESC*,void* cookie,engine_ddi::EngineQueue** out) noexcept{
 auto f=reinterpret_cast<Fixture*>(d);native12::ContextRequest request;D3D12DDIARG_CREATECOMMANDQUEUE_0050 desc{};desc.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;assert(request.prepare(desc)==S_OK);
 D3DKMT_CREATECONTEXTVIRTUAL create{};create.NodeOrdinal=request.args.NodeOrdinal;create.EngineAffinity=request.args.EngineAffinity;create.pPrivateDriverData=&request.blob;create.PrivateDriverDataSize=sizeof(request.blob);
 bc250_host_queue_context wrapper{cookie,&create};HRESULT hr=native12::HostedQueue::dispatch(f->bridge.get(),BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper);
 if(hr!=S_OK)return hr;assert(create.hContext && !(create.hContext&0x80000000u));
 auto q=new EngineQueue{f,cookie,create.hContext};f->tokens[f->creates-1]=q->token;*out=reinterpret_cast<engine_ddi::EngineQueue*>(q);return S_OK;
}
HRESULT execute(engine_ddi::EngineQueue*,UINT,const D3D12DDI_HCOMMANDLIST*) noexcept{return S_OK;}
HRESULT close(engine_ddi::EngineQueue** engine) noexcept{
 auto q=reinterpret_cast<EngineQueue*>(*engine);D3DKMT_DESTROYCONTEXT destroy{};destroy.hContext=q->token;bc250_host_queue_context wrapper{q->cookie,&destroy};
 HRESULT hr=native12::HostedQueue::dispatch(q->owner->bridge.get(),BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper);if(hr!=S_OK)return hr;
 delete q;*engine=nullptr;return S_OK;
}
}
int main(){
 Fixture f;int dummy=0;
 assert(native12::HostedQueue::dispatch(f.bridge.get(),99,&dummy)==E_INVALIDARG);
 bc250::umd::RuntimeDomain::Scope scope(f.domain);
 D3D12DDIARG_CREATECOMMANDQUEUE_0050 desc{};desc.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
 assert(f.registry.create(desc,{&f.runtime[0]},f.slots[0])==S_OK);
 assert(f.registry.create(desc,{&f.runtime[1]},f.slots[1])==S_OK);
 assert(f.creates==2 && f.tokens[0]!=f.tokens[1]);
 D3DKMT_HANDLE object=71;UINT64 value=9,completed=0;
 for(unsigned i=0;i<2;++i){
  D3DKMT_SUBMITCOMMAND submit{};submit.BroadcastContextCount=1;submit.BroadcastContext[0]=f.tokens[i];submit.Commands=0x12340000;submit.CommandLength=16;
  f.reentrant=true;assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_SubmitCommand,&submit)==S_OK);f.reentrant=false;
  D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};wait.hContext=f.tokens[i];wait.ObjectCount=1;wait.ObjectHandleArray=&object;wait.MonitoredFenceValueArray=&value;
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_WaitForSynchronizationObjectFromGpu,&wait)==S_OK);
  D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal{};signal.BroadcastContextCount=1;signal.BroadcastContextArray=&f.tokens[i];signal.ObjectCount=1;signal.ObjectHandleArray=&object;signal.MonitoredFenceValueArray=&value;
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==S_OK);
  // The update names the queue by its token; the runtime gets the queue's full context.
  D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION ops[2]{};D3DKMT_UPDATEGPUVIRTUALADDRESS update{};update.hContext=f.tokens[i];
  update.hFenceObject=object;update.NumOperations=2;update.Operations=ops;update.FenceValue=value;
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_UpdateGpuVirtualAddress,&update)==S_OK && f.updates==i+1);
  update.Flags.DoNotWait=1;assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_UpdateGpuVirtualAddress,&update)==E_NOTIMPL);
  update.Flags.Value=0;update.NumOperations=0;
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_UpdateGpuVirtualAddress,&update)==E_INVALIDARG && f.updates==i+1);
  assert(completed==0); // Accepted callback requests are not fabricated GPU completion.
  bc250_host_progress progress{f.tokens[i],object,value,&completed};
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_PUBLISH_PROGRESS,&progress)==S_OK);
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_PUBLISH_PROGRESS,&progress)==E_INVALIDARG);
 }
 assert(f.submits==2 && f.waits==2 && f.signals==2 && !f.destroys);
 std::thread worker([&]{assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_CHECK_STATUS,&dummy)==E_INVALIDARG);});worker.join();
 native12::Device foreign;foreign.runtime.handle=&foreign;native12::HostedQueue wrong(f.domain,f.registry,foreign);
 native12::ContextRequest request;assert(request.prepare(desc)==S_OK);D3DKMT_CREATECONTEXTVIRTUAL create{};create.EngineAffinity=1;create.pPrivateDriverData=&request.blob;create.PrivateDriverDataSize=sizeof(request.blob);
 bc250_host_queue_context wrapper{f.slots[0].cookie,&create};assert(native12::HostedQueue::dispatch(&wrong,BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper)==E_INVALIDARG && !create.hContext);
 D3DKMT_SUBMITCOMMAND submit{};submit.BroadcastContextCount=1;submit.BroadcastContext[0]=f.tokens[0];submit.Commands=0x12340000;submit.CommandLength=16;
 f.fail_submit=true;assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_SubmitCommand,&submit)==E_FAIL);f.fail_submit=false;
 assert(f.registry.destroy(f.slots[0])==S_OK && f.destroys==1);
 unsigned prior=f.submits;assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_SubmitCommand,&submit)==E_INVALIDARG && f.submits==prior);
 {D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op{};D3DKMT_UPDATEGPUVIRTUALADDRESS stale{};stale.hContext=f.tokens[0];stale.hFenceObject=object;
  stale.NumOperations=1;stale.Operations=&op;stale.FenceValue=value;
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_UpdateGpuVirtualAddress,&stale)==E_INVALIDARG && f.updates==2);}
 submit.BroadcastContext[0]=0x80000001u;assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_SubmitCommand,&submit)==E_INVALIDARG);
 assert(f.registry.destroy(f.slots[1])==S_OK && f.destroys==2);
 unsigned unresolved=99;assert(f.registry.discard_retired_metadata(unresolved)==S_OK && !unresolved);
 assert(f.bridge->discard_metadata()==0 && wrong.discard_metadata()==0);
 std::puts("PASS hosted application queues: two real registry owners, full-width KT context transport, alias-only close, reentrant pin safety, failed callback, wrong device/thread, stale token, no fabricated fence completion, address update by token");
}
