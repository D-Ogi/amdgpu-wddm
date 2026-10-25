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
 unsigned creates{},destroys{},submits{},waits{},signals{},updates{},rewinds{};
 bool fail_submit{},reentrant{};
 // The engine substitute's bind (make): the request a hosted ICD sends for the VkQueue it bound.
 enum Bind {BindGfx,BindCompute,BindNone,BindBadIp} bind{BindGfx};
 UINT last_node{UINT_MAX},last_ip{UINT_MAX},last_blob_node{UINT_MAX};
 Fixture():registry(device,reinterpret_cast<engine_ddi::DeviceContext*>(this),{make,execute,close,healthy,this}){
  runtime[0]={this,reinterpret_cast<HANDLE>(UINT_PTR{0x10000001})};runtime[1]={this,reinterpret_cast<HANDLE>(UINT_PTR{0x20000002})};
  device.runtime.handle=this;
  device.callbacks.pfnCreateContextVirtualCb=[](D3D12DDI_HRTCOMMANDQUEUE rt,D3DDDICB_CREATECONTEXTVIRTUAL* a)->HRESULT{
   auto r=static_cast<RuntimeQueue*>(rt.handle);assert(!r->live);r->live=true;++r->owner->creates;a->hContext=r->context;
   assert(a->PrivateDriverDataSize==sizeof(bc250_umd_context_private));
   const auto b=static_cast<const bc250_umd_context_private*>(a->pPrivateDriverData);
   r->owner->last_node=a->NodeOrdinal;r->owner->last_ip=b->ip_type;r->owner->last_blob_node=b->node_ordinal;return S_OK;};
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
   assert(a->ObjectCount==1 && a->ObjectHandleArray[0]==71 && a->MonitoredFenceValueArray[0]==9);
   assert(!(a->Flags.Value&~4u));f->rewinds+=a->Flags.AllowFenceRewind;++f->signals;return S_OK;};
  device.kernel_callbacks.pfnUpdateGpuVirtualAddressCb=[](HANDLE h,const D3DDDICB_UPDATEGPUVIRTUALADDRESS* a)->HRESULT{
   auto f=static_cast<Fixture*>(h);assert(a->hContext==f->runtime[0].context || a->hContext==f->runtime[1].context);
   assert(a->hFenceObject==71 && a->NumOperations==2 && a->Operations && a->FenceValue==9 && !a->Reserved0 && !a->Reserved1 && !a->Flags.Value);
   ++f->updates;return S_OK;};
  bridge=std::make_unique<native12::HostedQueue>(domain,registry,device);
 }
};
HRESULT make(engine_ddi::DeviceContext* d,const BC250_VKD3D_COMMAND_QUEUE_DESC*,void* cookie,engine_ddi::EngineQueue** out) noexcept{
 auto f=reinterpret_cast<Fixture*>(d);
 if(f->bind==Fixture::BindNone){*out=reinterpret_cast<engine_ddi::EngineQueue*>(new EngineQueue{f,cookie,0});return S_OK;}
 // The ICD's two requests: GFX on node 0 for a GENERAL VkQueue, COMPUTE on node 2 for a COMPUTE one.
 native12::ContextRequest request;D3D12DDIARG_CREATECOMMANDQUEUE_0050 desc{};
 const bool compute=f->bind==Fixture::BindCompute || f->bind==Fixture::BindBadIp;
 desc.QueueFlags=compute?D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE:D3D12DDI_COMMAND_QUEUE_FLAG_3D;assert(request.prepare(desc,compute)==S_OK);
 if(f->bind==Fixture::BindBadIp)request.blob.ip_type=AMDGPU_HW_IP_GFX;   // node 2 with the GFX IP: no queue of either kind
 D3DKMT_CREATECONTEXTVIRTUAL create{};create.NodeOrdinal=request.args.NodeOrdinal;create.EngineAffinity=request.args.EngineAffinity;create.pPrivateDriverData=&request.blob;create.PrivateDriverDataSize=sizeof(request.blob);
 create.ClientHint=D3DKMT_CLIENTHINT_VULKAN;
 bc250_host_queue_context wrapper{cookie,&create};HRESULT hr=native12::HostedQueue::dispatch(f->bridge.get(),BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper);
 if(hr!=S_OK)return hr;assert(create.hContext && !(create.hContext&0x80000000u));
 auto q=new EngineQueue{f,cookie,create.hContext};f->tokens[(f->creates-1)&1]=q->token;*out=reinterpret_cast<engine_ddi::EngineQueue*>(q);return S_OK;
}
HRESULT execute(engine_ddi::EngineQueue*,UINT,const D3D12DDI_HCOMMANDLIST*) noexcept{return S_OK;}
HRESULT close(engine_ddi::EngineQueue** engine) noexcept{
 auto q=reinterpret_cast<EngineQueue*>(*engine);
 if(!q->token){delete q;*engine=nullptr;return S_OK;}   // an engine queue that never bound
 D3DKMT_DESTROYCONTEXT destroy{};destroy.hContext=q->token;bc250_host_queue_context wrapper{q->cookie,&destroy};
 HRESULT hr=native12::HostedQueue::dispatch(q->owner->bridge.get(),BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper);if(hr!=S_OK)return hr;
 delete q;*engine=nullptr;return S_OK;
}
}
// A D3D12 COMPUTE or COPY queue has no context before the engine binds its VkQueue; the ICD's request opens it there,
// on node 2 for a VkQueue of the compute family (the MEC) and on node 0 for a GENERAL one (no compute family: node 2
// closed, or RADV_BC250_ACE_QUEUES=0 in a lab-flavour ICD). A DIRECT queue keeps its node 0 context and refuses the
// node 2 request.
void compute_queues(){
 Fixture g;bc250::umd::RuntimeDomain::Scope scope(g.domain);
 D3D12DDIARG_CREATECOMMANDQUEUE_0050 direct{};direct.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
 D3D12DDIARG_CREATECOMMANDQUEUE_0050 comp{};comp.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
 D3D12DDIARG_CREATECOMMANDQUEUE_0050 copy{};copy.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
 // The request of a DIRECT queue cannot be COMPUTE.
 {native12::ContextRequest r;assert(r.prepare(direct,true)==E_NOTIMPL && r.prepare(comp,true)==S_OK && r.args.NodeOrdinal==2 &&
  r.blob.ip_type==AMDGPU_HW_IP_COMPUTE && r.blob.node_ordinal==2 && r.prepare(copy,true)==S_OK);}
 g.bind=Fixture::BindGfx;assert(g.registry.create(direct,{&g.runtime[0]},g.slots[0])==S_OK);
 assert(g.creates==1 && g.last_node==0 && g.last_ip==AMDGPU_HW_IP_GFX && g.last_blob_node==0);
 g.bind=Fixture::BindCompute;assert(g.registry.create(comp,{&g.runtime[1]},g.slots[1])==S_OK);
 assert(g.creates==2 && g.last_node==2 && g.last_ip==AMDGPU_HW_IP_COMPUTE && g.last_blob_node==2 && g.runtime[1].live);
 HANDLE context=nullptr;assert(g.registry.present_context(g.slots[1],&context)==S_OK && context==g.runtime[1].context);
 // One fence across the two nodes: the node 2 queue waits for a value and signals, then the node 0 queue waits and
 // signals (wait-before-signal flag included). Each call names its own queue's context.
 D3DKMT_HANDLE object=71;UINT64 value=9;
 for(unsigned i:{1u,0u}){
  D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};wait.hContext=g.tokens[i];wait.ObjectCount=1;wait.ObjectHandleArray=&object;wait.MonitoredFenceValueArray=&value;
  assert(native12::HostedQueue::dispatch(g.bridge.get(),BC250_HOST_WaitForSynchronizationObjectFromGpu,&wait)==S_OK);
  D3DKMT_SUBMITCOMMAND submit{};submit.BroadcastContextCount=1;submit.BroadcastContext[0]=g.tokens[i];submit.Commands=0x12340000;submit.CommandLength=16;
  assert(native12::HostedQueue::dispatch(g.bridge.get(),BC250_HOST_SubmitCommand,&submit)==S_OK);
  D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal{};signal.BroadcastContextCount=1;signal.BroadcastContextArray=&g.tokens[i];signal.ObjectCount=1;
  signal.ObjectHandleArray=&object;signal.MonitoredFenceValueArray=&value;signal.Flags.AllowFenceRewind=1;
  assert(native12::HostedQueue::dispatch(g.bridge.get(),BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==S_OK);
 }
 assert(g.waits==2 && g.submits==2 && g.signals==2 && g.rewinds==2);
 // A second bind of the bound queue's cookie with another request is refused; the node 2 context stays.
 {native12::ContextRequest other;assert(other.prepare(comp)==S_OK);D3DKMT_CREATECONTEXTVIRTUAL c{};c.NodeOrdinal=0;c.EngineAffinity=1;
  c.pPrivateDriverData=&other.blob;c.PrivateDriverDataSize=sizeof(other.blob);
  assert(g.registry.bind_context(g.slots[1].cookie,g.device,c)==E_NOTIMPL && g.runtime[1].live);}
 assert(g.registry.destroy(g.slots[1])==S_OK && g.destroys==1 && !g.runtime[1].live);
 // A COPY queue on a device without a compute family: the ICD binds a GENERAL VkQueue, the context is opened on node 0.
 g.bind=Fixture::BindGfx;assert(g.registry.create(copy,{&g.runtime[1]},g.slots[1])==S_OK);
 assert(g.creates==3 && g.last_node==0 && g.last_ip==AMDGPU_HW_IP_GFX);
 assert(g.registry.destroy(g.slots[1])==S_OK && g.destroys==2);
 // A COPY queue on a device with one: node 2.
 g.bind=Fixture::BindCompute;assert(g.registry.create(copy,{&g.runtime[1]},g.slots[1])==S_OK);
 assert(g.creates==4 && g.last_node==2 && g.last_ip==AMDGPU_HW_IP_COMPUTE);
 assert(g.registry.destroy(g.slots[1])==S_OK && g.destroys==3);
 // A DIRECT queue whose VkQueue binds with the node 2 request: refused, its node 0 context closed, the slot empty.
 g.bind=Fixture::BindCompute;assert(g.registry.create(direct,{&g.runtime[1]},g.slots[1])==E_NOTIMPL);
 assert(g.creates==5 && g.destroys==4 && !g.runtime[1].live && !g.slots[1].cookie);
 // Node 2 with the GFX IP: neither request, no context is opened.
 g.bind=Fixture::BindBadIp;assert(g.registry.create(comp,{&g.runtime[1]},g.slots[1])==E_NOTIMPL);
 assert(g.creates==5 && !g.runtime[1].live && !g.slots[1].cookie);
 // An engine queue that never binds: no context, the queue is refused, the engine queue closed.
 g.bind=Fixture::BindNone;assert(g.registry.create(comp,{&g.runtime[1]},g.slots[1])==E_UNEXPECTED);
 assert(g.creates==5 && !g.runtime[1].live && !g.slots[1].cookie && !g.device.lost.load());
 // Outside a create, nothing opens a context: a cookie the registry does not know is refused.
 {native12::ContextRequest r;assert(r.prepare(comp,true)==S_OK);D3DKMT_CREATECONTEXTVIRTUAL c{};c.NodeOrdinal=2;c.EngineAffinity=1;
  c.pPrivateDriverData=&r.blob;c.PrivateDriverDataSize=sizeof(r.blob);int stray=0;
  assert(g.registry.bind_context(&stray,g.device,c)==E_INVALIDARG && g.creates==5);}
 assert(g.registry.destroy(g.slots[0])==S_OK && g.destroys==5);
 unsigned unresolved=99;assert(g.registry.discard_retired_metadata(unresolved)==S_OK && !unresolved);
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
  // The wait-before-signal flag of the hosted ICD reaches the runtime; any other flag is refused before it.
  signal.Flags.AllowFenceRewind=1;
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==S_OK);
  assert(f.rewinds==i+1);
  signal.Flags.SignalAtSubmission=1;
  assert(native12::HostedQueue::dispatch(f.bridge.get(),BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==E_INVALIDARG);
  assert(f.signals==2*(i+1) && f.rewinds==i+1);
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
 assert(f.submits==2 && f.waits==2 && f.signals==4 && f.rewinds==2 && !f.destroys);
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
 compute_queues();
 std::puts("PASS hosted application queues: two real registry owners, full-width KT context transport, alias-only close, reentrant pin safety, failed callback, wrong device/thread, stale token, no fabricated fence completion, address update by token, "
           "compute and copy queues opened at their bind on node 2 or node 0, a DIRECT queue refused on node 2, an engine queue that never binds, "
           "cross-node GPU wait and signal on one fence");
}
