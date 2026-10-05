// SPDX-License-Identifier: MIT
#include "hosted-dispatch.h"
#include <d3dkmthk.h>
#include <cassert>
#include <functional>
#include <thread>
#include <utility>
#include <cstdio>
using native12::HostedDispatch;
static HANDLE const device_handle=reinterpret_cast<HANDLE>(UINT64_C(0x1234567887654321));
static HANDLE const context_handle=reinterpret_cast<HANDLE>(UINT64_C(0x34567890abcdef12));
static UINT64 fence=10;
static char mapped[4096];
static unsigned allocations=0,deallocations=0,context_creates=0,context_destroys=0,native_queue_calls=0,submitted=0,errors=0;
static bool fail_destroy=false,fail_free=false,bad_fence=false;
static bool immediate_resident=false,fail_sync_create=false,fail_sync_destroy=false;
static unsigned sync_destroys=0;
static UINT64 sync_fence=0;
static HRESULT APIENTRY create_sync(HANDLE device,D3DDDICB_CREATESYNCHRONIZATIONOBJECT2* a){
 assert(device==device_handle);if(fail_sync_create)return E_FAIL;
 a->hSyncObject=45;a->Info.MonitoredFence.FenceValueCPUVirtualAddress=&sync_fence;return S_OK;
}
static HRESULT APIENTRY destroy_sync(HANDLE device,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT* a){
 assert(device==device_handle && a->hSyncObject==45);++sync_destroys;return fail_sync_destroy?E_FAIL:S_OK;
}
static HRESULT APIENTRY allocate(D3D12DDI_HRTDEVICE device,D3D12DDICB_ALLOCATE_0022* a){
 assert(device.handle==device_handle && !a->hResource && !a->hKMResource && a->NumAllocations==1);
 assert(a->pAllocationInfo->PrivateDriverDataSize==4 && a->pAllocationInfo->pPrivateDriverData);
 ++allocations;a->pAllocationInfo->hAllocation=77;return S_OK;
}
static HRESULT APIENTRY deallocate(D3D12DDI_HRTDEVICE device,const D3D12DDICB_DEALLOCATE_0022* a){
 assert(device.handle==device_handle && !a->hResource && a->NumAllocations==1 && a->HandleList[0]==77 && !a->Flags);
 ++deallocations;return fail_destroy?E_FAIL:S_OK;
}
static HRESULT APIENTRY paging_create(HANDLE device,D3DDDICB_CREATEPAGINGQUEUE* a){assert(device==device_handle);a->hPagingQueue=9;a->hSyncObject=10;a->FenceValueCPUVirtualAddress=&fence;return S_OK;}
static HRESULT APIENTRY paging_destroy(HANDLE device,const D3DDDI_DESTROYPAGINGQUEUE* a){assert(device==device_handle && a->hPagingQueue==9);return S_OK;}
static HRESULT APIENTRY map_va(HANDLE device,D3DDDI_MAPGPUVIRTUALADDRESS* a){assert(device==device_handle && a->hPagingQueue==9 && a->hAllocation==77);a->VirtualAddress=UINT64_C(0x400000000);a->PagingFenceValue=bad_fence?0:4;return E_PENDING;}
static HRESULT APIENTRY free_va(HANDLE device,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){assert(device==device_handle && a->BaseAddress==UINT64_C(0x400000000) && a->Size==4096);return fail_free?E_FAIL:S_OK;}
static HRESULT APIENTRY resident(HANDLE device,D3DDDI_MAKERESIDENT* a){assert(device==device_handle && a->hPagingQueue==9 && a->AllocationList[0]==77);a->PagingFenceValue=immediate_resident?UINT64_MAX:6;return immediate_resident?S_OK:E_PENDING;}
static HRESULT APIENTRY lock_memory(HANDLE device,D3DDDICB_LOCK2* a){assert(device==device_handle && a->hAllocation==77);a->pData=mapped;return S_OK;}
static HRESULT APIENTRY unlock_memory(HANDLE device,const D3DDDICB_UNLOCK2* a){assert(device==device_handle && a->hAllocation==77);return S_OK;}
static HRESULT APIENTRY create_context(HANDLE device,D3DDDICB_CREATECONTEXTVIRTUAL* a){assert(device==device_handle);++context_creates;a->hContext=context_handle;return S_OK;}
static HRESULT APIENTRY destroy_context(HANDLE device,const D3DDDICB_DESTROYCONTEXT* a){assert(device==device_handle && a->hContext==context_handle);++context_destroys;return fail_destroy?E_FAIL:S_OK;}
static HRESULT APIENTRY wrong_native_queue(D3D12DDI_HRTCOMMANDQUEUE,D3DDDICB_CREATECONTEXTVIRTUAL*){++native_queue_calls;return E_FAIL;}
static HRESULT APIENTRY wait_gpu(HANDLE device,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a){assert(device==device_handle && a->hContext==context_handle && a->ObjectCount==1 && a->MonitoredFenceValueArray[0]==6);return S_OK;}
static HRESULT APIENTRY submit(HANDLE device,const D3DDDICB_SUBMITCOMMAND* a){assert(device==device_handle && a->BroadcastContextCount==1 && a->BroadcastContext[0]==context_handle);++submitted;return S_OK;}
static VOID APIENTRY removed(D3D12DDI_HRTDEVICE device,HRESULT result){assert(device.handle==device_handle && result==D3DDDIERR_DEVICEREMOVED);++errors;}
// Address reuse across threads (trial 172). The runtime releases an address inside its free callback and
// may hand it out again at once to another thread, before the freeing thread takes the bridge's lock again.
// during_free runs on a thread of its own inside that window, as a second recording thread of the game does.
static constexpr UINT64 reused_va=UINT64_C(0x500000000);
static D3DKMT_HANDLE next_reused_handle=200;
static unsigned reuse_errors=0,reuse_frees=0;
static std::function<void()> during_free;
static HRESULT APIENTRY reuse_allocate(D3D12DDI_HRTDEVICE device,D3D12DDICB_ALLOCATE_0022* a){
 assert(device.handle==device_handle && a->NumAllocations==1);a->pAllocationInfo->hAllocation=next_reused_handle++;return S_OK;
}
static HRESULT APIENTRY reuse_deallocate(D3D12DDI_HRTDEVICE device,const D3D12DDICB_DEALLOCATE_0022*){assert(device.handle==device_handle);return S_OK;}
static HRESULT APIENTRY reuse_map(HANDLE device,D3DDDI_MAPGPUVIRTUALADDRESS* a){
 assert(device==device_handle);a->VirtualAddress=a->BaseAddress?a->BaseAddress:reused_va;a->PagingFenceValue=4;return E_PENDING;
}
static HRESULT APIENTRY reuse_reserve(HANDLE device,D3DDDI_RESERVEGPUVIRTUALADDRESS* a){
 assert(device==device_handle);a->VirtualAddress=a->BaseAddress?a->BaseAddress:reused_va;a->PagingFenceValue=0;return S_OK;
}
static HRESULT APIENTRY reuse_free(HANDLE device,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){
 assert(device==device_handle && a->BaseAddress==reused_va);++reuse_frees;
 // From here on the address is free: the other thread's call receives it before this callback returns.
 if(auto other=std::exchange(during_free,nullptr)){std::thread worker(other);worker.join();}
 return S_OK;
}
static VOID APIENTRY reuse_removed(D3D12DDI_HRTDEVICE device,HRESULT result){assert(device.handle==device_handle && result==D3DDDIERR_DEVICEREMOVED);++reuse_errors;}
int main(){
 static_assert(BC250_HOST_VERSION==5 && sizeof(bc250_host)==56);
 D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 um{};um.pfnAllocateCb=allocate;um.pfnDeallocateCb=deallocate;um.pfnCreateContextVirtualCb=wrong_native_queue;um.pfnSetErrorCb=removed;
 D3DDDI_DEVICECALLBACKS kt{};kt.pfnCreatePagingQueueCb=paging_create;kt.pfnDestroyPagingQueueCb=paging_destroy;kt.pfnMapGpuVirtualAddressCb=map_va;kt.pfnFreeGpuVirtualAddressCb=free_va;
 kt.pfnMakeResidentCb=resident;kt.pfnLock2Cb=lock_memory;kt.pfnUnlock2Cb=unlock_memory;kt.pfnCreateContextVirtualCb=create_context;kt.pfnDestroyContextCb=destroy_context;
 kt.pfnCreateSynchronizationObject2Cb=create_sync;kt.pfnDestroySynchronizationObjectCb=destroy_sync;
 kt.pfnWaitForSynchronizationObjectFromGpuCb=wait_gpu;kt.pfnSubmitCommandCb=submit;
 bc250::umd::RuntimeDomain domain;HostedDispatch bridge(domain,{device_handle},um,kt);
 const auto host=bridge.descriptor(42,&bridge);
 auto call=[&](uint32_t op,void* a=nullptr){return host.dispatch(host.userdata,op,a);};
 assert(call(BC250_HOST_CHECK_STATUS)<0);
 bc250::umd::RuntimeDomain::Scope scope(domain);
 assert(call(BC250_HOST_CHECK_STATUS)==0);
 std::thread worker([&]{assert(call(BC250_HOST_CHECK_STATUS)<0);});worker.join();
 int dummy=0;assert(call(63,&dummy)<0 && call(BC250_HOST_CreateAllocation2)<0);
 bc250_host_paging paging{};assert(call(BC250_HOST_CREATE_PAGING,&paging)==0 && paging.queue==9);
 D3DDDI_ALLOCATIONINFO2 info{};unsigned private_data=123;info.pPrivateDriverData=&private_data;info.PrivateDriverDataSize=sizeof(private_data);
 D3DKMT_CREATEALLOCATION create{};create.NumAllocations=1;create.pAllocationInfo2=&info;create.Flags.CreateResource=1;create.Flags.NonSecure=1;
 create.Flags.CreateShared=1;assert(call(BC250_HOST_CreateAllocation2,&create)<0 && !allocations);create.Flags.CreateShared=0;
 assert(call(BC250_HOST_CreateAllocation2,&create)==0 && info.hAllocation==77 && !create.hResource && allocations==1);
 D3DDDI_MAPGPUVIRTUALADDRESS mapping{};mapping.hPagingQueue=9;mapping.hAllocation=77;mapping.SizeInPages=1;
 assert(call(BC250_HOST_MapGpuVirtualAddress,&mapping)==0x103 && mapping.PagingFenceValue==4);
 D3DDDI_MAKERESIDENT make{};make.hPagingQueue=9;make.NumAllocations=1;make.AllocationList=&info.hAllocation;
 assert(call(BC250_HOST_MakeResident,&make)==0x103 && make.PagingFenceValue==6);
 immediate_resident=true;assert(call(BC250_HOST_MakeResident,&make)==0 && make.PagingFenceValue==0);
 immediate_resident=false;make.PagingFenceValue=6;
 D3DKMT_LOCK2 lock{};lock.hAllocation=77;assert(call(BC250_HOST_Lock2,&lock)==0 && lock.pData==mapped);
 D3DKMT_UNLOCK2 unlock{};unlock.hAllocation=77;assert(call(BC250_HOST_Unlock2,&unlock)==0);
 D3DKMT_CREATECONTEXTVIRTUAL context{};bc250_host_queue_context wrapper{nullptr,&context};
 assert(call(BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper)==0 && context.hContext && !native_queue_calls && context_creates==1);
 const auto old_token=context.hContext;
 D3DKMT_CREATESYNCHRONIZATIONOBJECT2 sync{};sync.Info.Type=D3DDDI_MONITORED_FENCE;
 fail_sync_create=true;assert(call(BC250_HOST_CreateSynchronizationObject2,&sync)<0 && !sync.hSyncObject);fail_sync_create=false;
 assert(call(BC250_HOST_CreateSynchronizationObject2,&sync)==0 && sync.hSyncObject==45);
 D3DKMT_DESTROYSYNCHRONIZATIONOBJECT sync_release{};sync_release.hSyncObject=46;
 assert(call(BC250_HOST_DestroySynchronizationObject,&sync_release)<0 && sync_destroys==0);
 bc250_host_progress progress{context.hContext,45,1,&fence};
 assert(call(BC250_HOST_PUBLISH_PROGRESS,&progress)<0);
 progress.cpu_address=&sync_fence;assert(call(BC250_HOST_PUBLISH_PROGRESS,&progress)==0);
 sync_release.hSyncObject=45;fail_sync_destroy=true;
 assert(call(BC250_HOST_DestroySynchronizationObject,&sync_release)<0 && sync_destroys==1);
 ++progress.value;assert(call(BC250_HOST_PUBLISH_PROGRESS,&progress)==0);
 fail_sync_destroy=false;assert(call(BC250_HOST_DestroySynchronizationObject,&sync_release)==0 && sync_destroys==2);
 assert(call(BC250_HOST_DestroySynchronizationObject,&sync_release)<0 && sync_destroys==2);
 assert(call(BC250_HOST_PUBLISH_PROGRESS,&progress)<0);
 // Removed mappings must never participate in later status scans.
 sync_fence=UINT64_MAX;assert(call(BC250_HOST_CHECK_STATUS)==0);sync_fence=0;
 D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};wait.hContext=context.hContext;wait.ObjectCount=1;wait.ObjectHandleArray=&paging.sync;wait.MonitoredFenceValueArray=&make.PagingFenceValue;
 assert(call(BC250_HOST_WaitForSynchronizationObjectFromGpu,&wait)==0);
 D3DKMT_SUBMITCOMMAND command{};command.BroadcastContextCount=1;command.BroadcastContext[0]=context.hContext;
 assert(call(BC250_HOST_SubmitCommand,&command)==0 && submitted==1);
 D3DKMT_DESTROYCONTEXT destroy{};destroy.hContext=context.hContext;wrapper.arguments=&destroy;
 fail_destroy=true;assert(call(BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)<0);assert(call(BC250_HOST_SubmitCommand,&command)==0 && submitted==2);
 fail_destroy=false;assert(call(BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)==0);assert(call(BC250_HOST_SubmitCommand,&command)<0 && submitted==2);
 context={};wrapper.arguments=&context;assert(call(BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper)==0 && context.hContext!=old_token);
 destroy.hContext=context.hContext;wrapper.arguments=&destroy;assert(call(BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)==0);
 D3DKMT_DESTROYALLOCATION2 release{};release.AllocationCount=1;release.phAllocationList=&info.hAllocation;
 assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && !deallocations);
 D3DKMT_FREEGPUVIRTUALADDRESS free{};free.BaseAddress=mapping.VirtualAddress;free.Size=4096;
 fail_free=true;assert(call(BC250_HOST_FreeGpuVirtualAddress,&free)<0 && call(BC250_HOST_DestroyAllocation2,&release)<0);
 fail_free=false;assert(call(BC250_HOST_FreeGpuVirtualAddress,&free)==0);
 fail_destroy=true;assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && deallocations==1);
 fail_destroy=false;assert(call(BC250_HOST_DestroyAllocation2,&release)==0 && deallocations==2);
 assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && deallocations==2);
 assert(call(BC250_HOST_DESTROY_PAGING,&paging)==0);
 assert(bridge.discard_metadata()==0 && call(BC250_HOST_CHECK_STATUS)<0);
 HostedDispatch lost(domain,{device_handle},um,kt);assert(HostedDispatch::dispatch(&lost,BC250_HOST_REPORT_LOST,nullptr)<0);
 assert(HostedDispatch::dispatch(&lost,BC250_HOST_CHECK_STATUS,nullptr)<0 && errors==1);
 assert(HostedDispatch::dispatch(&lost,BC250_HOST_REPORT_LOST,nullptr)<0 && errors==1);
 HostedDispatch malformed(domain,{device_handle},um,kt);auto malformed_call=[&](uint32_t op,void* a){return HostedDispatch::dispatch(&malformed,op,a);};
 paging={};assert(malformed_call(BC250_HOST_CREATE_PAGING,&paging)==0);
 info.hAllocation=0;assert(malformed_call(BC250_HOST_CreateAllocation2,&create)==0);
 mapping.VirtualAddress=0;mapping.PagingFenceValue=0;bad_fence=true;
 assert(malformed_call(BC250_HOST_MapGpuVirtualAddress,&mapping)<0 && malformed.lost());
 assert(malformed.discard_metadata()==2);
 HostedDispatch retained(domain,{device_handle},um,kt);
 sync.hSyncObject=0;assert(HostedDispatch::dispatch(&retained,BC250_HOST_CreateSynchronizationObject2,&sync)==0);
 fail_sync_destroy=true;assert(HostedDispatch::dispatch(&retained,BC250_HOST_DestroySynchronizationObject,&sync_release)<0);
 assert(retained.discard_metadata()==1);fail_sync_destroy=false;
 HostedDispatch sentinel(domain,{device_handle},um,kt);
 sync.hSyncObject=0;assert(HostedDispatch::dispatch(&sentinel,BC250_HOST_CreateSynchronizationObject2,&sync)==0);
 sync_fence=UINT64_MAX;assert(HostedDispatch::dispatch(&sentinel,BC250_HOST_CHECK_STATUS,nullptr)<0 && sentinel.lost());
 assert(sentinel.discard_metadata()==1);sync_fence=0;
 {
  auto rum=um;rum.pfnAllocateCb=reuse_allocate;rum.pfnDeallocateCb=reuse_deallocate;rum.pfnSetErrorCb=reuse_removed;
  auto rkt=kt;rkt.pfnMapGpuVirtualAddressCb=reuse_map;rkt.pfnReserveGpuVirtualAddressCb=reuse_reserve;rkt.pfnFreeGpuVirtualAddressCb=reuse_free;
  HostedDispatch reuse(domain,{device_handle},rum,rkt);
  auto on=[&](uint32_t op,void* a){return HostedDispatch::dispatch(&reuse,op,a);};
  bc250_host_paging reuse_paging{};assert(on(BC250_HOST_CREATE_PAGING,&reuse_paging)==0);
  constexpr UINT64 bytes=65536;
  auto allocate_one=[&](D3DDDI_ALLOCATIONINFO2& out){
   D3DKMT_CREATEALLOCATION request{};request.NumAllocations=1;request.pAllocationInfo2=&out;
   return on(BC250_HOST_CreateAllocation2,&request);};
  auto map_one=[&](D3DKMT_HANDLE handle,D3DDDI_MAPGPUVIRTUALADDRESS& out){
   out={};out.hPagingQueue=reuse_paging.queue;out.hAllocation=handle;out.SizeInPages=bytes/4096;
   return on(BC250_HOST_MapGpuVirtualAddress,&out);};
  D3DKMT_FREEGPUVIRTUALADDRESS release_va{};release_va.BaseAddress=reused_va;release_va.Size=bytes;
  D3DDDI_ALLOCATIONINFO2 x{},y{};assert(allocate_one(x)==0 && allocate_one(y)==0 && x.hAllocation!=y.hAllocation);
  D3DDDI_MAPGPUVIRTUALADDRESS x_map{},y_map{};
  assert(map_one(x.hAllocation,x_map)==0x103 && x_map.VirtualAddress==reused_va);
  // 1. The trial 172 removal (MapGpuVirtualAddress, malformed answer): Y is mapped at X's address while
  //    the free of X's address is inside the runtime.
  int32_t y_status=-1;
  during_free=[&]{bc250::umd::RuntimeDomain::Scope entered(domain);y_status=map_one(y.hAllocation,y_map);};
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&release_va)==0 && reuse_frees==1);
  assert(y_status==0x103 && y_map.VirtualAddress==reused_va && !reuse.lost() && !reuse_errors);
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&release_va)==0 && reuse_frees==2);
  // 2. A reservation receives the address of a reservation whose free is inside the runtime.
  D3DDDI_RESERVEGPUVIRTUALADDRESS first{},second{};first.Size=second.Size=bytes;
  assert(on(BC250_HOST_ReserveGpuVirtualAddress,&first)==0 && first.VirtualAddress==reused_va);
  int32_t second_status=-1;
  during_free=[&]{bc250::umd::RuntimeDomain::Scope entered(domain);second_status=on(BC250_HOST_ReserveGpuVirtualAddress,&second);};
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&release_va)==0 && reuse_frees==3);
  assert(second_status==0 && second.VirtualAddress==reused_va && !reuse.lost() && !reuse_errors);
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&release_va)==0 && reuse_frees==4);
  // 3. A fixed reservation over an address whose free is in flight is the runtime's to judge.
  assert(map_one(x.hAllocation,x_map)==0x103 && x_map.VirtualAddress==reused_va);
  D3DDDI_RESERVEGPUVIRTUALADDRESS fixed{};fixed.BaseAddress=reused_va;fixed.Size=bytes;int32_t fixed_status=-1;
  during_free=[&]{bc250::umd::RuntimeDomain::Scope entered(domain);fixed_status=on(BC250_HOST_ReserveGpuVirtualAddress,&fixed);};
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&release_va)==0 && reuse_frees==5);
  assert(fixed_status==0 && fixed.VirtualAddress==reused_va && !reuse.lost() && !reuse_errors);
  // The check itself stays: an answer inside a live extent (the fixed reservation) still removes the device.
  assert(map_one(y.hAllocation,y_map)<0 && reuse.lost() && reuse_errors==1);
  assert(reuse.discard_metadata()==4);
 }
 // 4. A deferred-replay worker of engine-ddi (WorkerScope inside the device's domain) reaches the memory and CPU-side
 //    sync operations only; a removal it finds marks the device lost at once and reaches the runtime's error callback
 //    from report_deferred_removal on a DDI thread, once. The scope names one device and ends with its thread's scope.
 {
  constexpr int32_t refused=static_cast<int32_t>(0xc000000du);
  HostedDispatch work(domain,{device_handle},um,kt),other(domain,{device_handle},um,kt);
  auto on=[](HostedDispatch& d,uint32_t op,void* a){return HostedDispatch::dispatch(&d,op,a);};
  assert(HostedDispatch::worker_admits(BC250_HOST_Lock2) && HostedDispatch::worker_admits(BC250_HOST_REPORT_LOST));
  assert(!HostedDispatch::worker_admits(BC250_HOST_SubmitCommand) && !HostedDispatch::worker_admits(BC250_HOST_CREATE_QUEUE_CONTEXT));
  const unsigned creates=context_creates,submits=submitted,reported=errors;
  std::thread([&]{
   bc250::umd::RuntimeDomain::Scope entered(domain);
   D3DKMT_CREATECONTEXTVIRTUAL created{};bc250_host_queue_context queue{nullptr,&created};
   bc250_host_paging pages{};
   {
    HostedDispatch::WorkerScope scope(work);
    assert(on(work,BC250_HOST_CHECK_STATUS,nullptr)==0);
    assert(on(work,BC250_HOST_CREATE_PAGING,&pages)==0 && pages.queue==9 && on(work,BC250_HOST_DESTROY_PAGING,&pages)==0);
    assert(on(work,BC250_HOST_CREATE_QUEUE_CONTEXT,&queue)==refused && !created.hContext && context_creates==creates);
    D3DKMT_SUBMITCOMMAND command{};assert(on(work,BC250_HOST_SubmitCommand,&command)==refused && submitted==submits);
    D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU gpu_wait{};
    assert(on(work,BC250_HOST_WaitForSynchronizationObjectFromGpu,&gpu_wait)==refused);
    bc250_host_progress published{};assert(on(work,BC250_HOST_PUBLISH_PROGRESS,&published)==refused);
    // Another device on the same thread is not restricted.
    assert(on(other,BC250_HOST_CREATE_QUEUE_CONTEXT,&queue)==0 && created.hContext && context_creates==creates+1);
    D3DKMT_DESTROYCONTEXT gone{};gone.hContext=created.hContext;bc250_host_queue_context end{nullptr,&gone};
    assert(on(other,BC250_HOST_DESTROY_QUEUE_CONTEXT,&end)==0);
    // A removal on the worker: lost at once, no callback from this thread, also not from a report made here.
    assert(on(work,BC250_HOST_REPORT_LOST,nullptr)<0 && work.lost() && errors==reported);
    work.report_deferred_removal();assert(errors==reported && on(work,BC250_HOST_CHECK_STATUS,nullptr)<0);
   }
   // The scope has ended: this thread is a DDI thread of the device again.
   created={};assert(on(other,BC250_HOST_CREATE_QUEUE_CONTEXT,&queue)==0 && context_creates==creates+2);
   D3DKMT_DESTROYCONTEXT gone{};gone.hContext=created.hContext;bc250_host_queue_context end{nullptr,&gone};
   assert(on(other,BC250_HOST_DESTROY_QUEUE_CONTEXT,&end)==0);
  }).join();
  assert(errors==reported);
  work.report_deferred_removal();assert(errors==reported+1);
  work.report_deferred_removal();assert(errors==reported+1);
  assert(work.discard_metadata()==0 && other.discard_metadata()==0);
 }
 std::puts("PASS hosted dispatch: native allocation/paging/VA/lock, pending residency, internal full-width context token, cleanup retries, scope, loss, address reuse inside a free, replay worker scope");
}
