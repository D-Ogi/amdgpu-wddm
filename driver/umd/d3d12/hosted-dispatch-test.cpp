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
static unsigned allocations=0,deallocations=0,context_creates=0,context_destroys=0,native_queue_calls=0,submitted=0,errors=0,
    signalled=0,rewinds=0;
static unsigned frees=0,evictions=0;
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
static HRESULT APIENTRY free_va(HANDLE device,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){assert(device==device_handle && a->BaseAddress==UINT64_C(0x400000000) && a->Size==4096);++frees;return fail_free?E_FAIL:S_OK;}
// Counted only to show that the R1 destroy path makes neither call (BD-201): the ICD asks for an Evict.
static HRESULT APIENTRY evict(HANDLE device,D3DDDICB_EVICT* a){assert(device==device_handle && a->NumAllocations);++evictions;return S_OK;}
static HRESULT APIENTRY resident(HANDLE device,D3DDDI_MAKERESIDENT* a){assert(device==device_handle && a->hPagingQueue==9 && a->AllocationList[0]==77);a->PagingFenceValue=immediate_resident?UINT64_MAX:6;return immediate_resident?S_OK:E_PENDING;}
static HRESULT APIENTRY lock_memory(HANDLE device,D3DDDICB_LOCK2* a){assert(device==device_handle && a->hAllocation==77);a->pData=mapped;return S_OK;}
static HRESULT APIENTRY unlock_memory(HANDLE device,const D3DDDICB_UNLOCK2* a){assert(device==device_handle && a->hAllocation==77);return S_OK;}
static HRESULT APIENTRY create_context(HANDLE device,D3DDDICB_CREATECONTEXTVIRTUAL* a){assert(device==device_handle);++context_creates;a->hContext=context_handle;return S_OK;}
static HRESULT APIENTRY destroy_context(HANDLE device,const D3DDDICB_DESTROYCONTEXT* a){assert(device==device_handle && a->hContext==context_handle);++context_destroys;return fail_destroy?E_FAIL:S_OK;}
static HRESULT APIENTRY wrong_native_queue(D3D12DDI_HRTCOMMANDQUEUE,D3DDDICB_CREATECONTEXTVIRTUAL*){++native_queue_calls;return E_FAIL;}
static HRESULT APIENTRY wait_gpu(HANDLE device,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU* a){assert(device==device_handle && a->hContext==context_handle && a->ObjectCount==1 && a->MonitoredFenceValueArray[0]==6);return S_OK;}
static HRESULT APIENTRY submit(HANDLE device,const D3DDDICB_SUBMITCOMMAND* a){assert(device==device_handle && a->BroadcastContextCount==1 && a->BroadcastContext[0]==context_handle);++submitted;return S_OK;}
static HRESULT APIENTRY signal_gpu(HANDLE device,const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2* a){
    assert(device==device_handle && a->BroadcastContextCount==1 && a->BroadcastContextArray[0]==context_handle);
    assert(a->ObjectCount==1 && a->MonitoredFenceValueArray[0]==6 && !(a->Flags.Value&~4u));
    ++signalled;rewinds+=a->Flags.AllowFenceRewind;return S_OK;}
static VOID APIENTRY removed(D3D12DDI_HRTDEVICE device,HRESULT result){assert(device.handle==device_handle && result==D3DDDIERR_DEVICEREMOVED);++errors;}
// Address reuse across threads (trial 172). The runtime releases an address inside its free callback and
// may hand it out again at once to another thread, before the freeing thread takes the bridge's lock again.
// during_free runs on a thread of its own inside that window, as a second recording thread of the game does.
static constexpr UINT64 reused_va=UINT64_C(0x500000000);
static D3DKMT_HANDLE next_reused_handle=200;
static unsigned reuse_errors=0,reuse_frees=0,reuse_reserves=0,reuse_deallocations=0;
static std::function<void()> during_free,during_deallocate;
static bool fail_reuse_deallocate=false,fail_reuse_free=false;
static HRESULT APIENTRY reuse_allocate(D3D12DDI_HRTDEVICE device,D3D12DDICB_ALLOCATE_0022* a){
 assert(device.handle==device_handle && a->NumAllocations==1);a->pAllocationInfo->hAllocation=next_reused_handle++;return S_OK;
}
// BD-201: the API-allocated range of an ordinary allocation is freed by the destruction, so the runtime
// releases it somewhere inside this callback. during_deallocate runs in that window on a thread of its own.
static HRESULT APIENTRY reuse_deallocate(D3D12DDI_HRTDEVICE device,const D3D12DDICB_DEALLOCATE_0022* a){
 assert(device.handle==device_handle && !a->hResource && a->NumAllocations==1 && !a->Flags);
 ++reuse_deallocations;if(fail_reuse_deallocate)return E_FAIL;
 if(auto other=std::exchange(during_deallocate,nullptr)){std::thread worker(other);worker.join();}
 return S_OK;
}
static HRESULT APIENTRY reuse_map(HANDLE device,D3DDDI_MAPGPUVIRTUALADDRESS* a){
 assert(device==device_handle);a->VirtualAddress=a->BaseAddress?a->BaseAddress:reused_va;a->PagingFenceValue=4;return E_PENDING;
}
static HRESULT APIENTRY reuse_reserve(HANDLE device,D3DDDI_RESERVEGPUVIRTUALADDRESS* a){
 assert(device==device_handle);++reuse_reserves;a->VirtualAddress=a->BaseAddress?a->BaseAddress:reused_va;a->PagingFenceValue=0;return S_OK;
}
static HRESULT APIENTRY reuse_free(HANDLE device,const D3DDDICB_FREEGPUVIRTUALADDRESS* a){
 assert(device==device_handle && a->BaseAddress==reused_va);++reuse_frees;
 if(fail_reuse_free)return E_FAIL;
 // From here on the address is free: the other thread's call receives it before this callback returns.
 if(auto other=std::exchange(during_free,nullptr)){std::thread worker(other);worker.join();}
 return S_OK;
}
static VOID APIENTRY reuse_removed(D3D12DDI_HRTDEVICE device,HRESULT result){assert(device.handle==device_handle && result==D3DDDIERR_DEVICEREMOVED);++reuse_errors;}
int main(){
 static_assert(BC250_HOST_VERSION==5 && sizeof(bc250_host)==(sizeof(void*)==8?56:40));
 D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 um{};um.pfnAllocateCb=allocate;um.pfnDeallocateCb=deallocate;um.pfnCreateContextVirtualCb=wrong_native_queue;um.pfnSetErrorCb=removed;
 D3DDDI_DEVICECALLBACKS kt{};kt.pfnCreatePagingQueueCb=paging_create;kt.pfnDestroyPagingQueueCb=paging_destroy;kt.pfnMapGpuVirtualAddressCb=map_va;kt.pfnFreeGpuVirtualAddressCb=free_va;
 kt.pfnMakeResidentCb=resident;kt.pfnEvictCb=evict;kt.pfnLock2Cb=lock_memory;kt.pfnUnlock2Cb=unlock_memory;kt.pfnCreateContextVirtualCb=create_context;kt.pfnDestroyContextCb=destroy_context;
 kt.pfnCreateSynchronizationObject2Cb=create_sync;kt.pfnDestroySynchronizationObjectCb=destroy_sync;
 kt.pfnWaitForSynchronizationObjectFromGpuCb=wait_gpu;kt.pfnSubmitCommandCb=submit;kt.pfnSignalSynchronizationObjectFromGpu2Cb=signal_gpu;
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
 // BD-201 R4: the device-wide retirement proof gates a release (heap-import.cpp:253 releases a quarantined
 // import only once it holds), so a fence value it cannot trust must prove nothing. A reading of UINT64_MAX
 // keeps its mark and fails the proof, instead of reporting everything retired.
 sync.hSyncObject=0;assert(call(BC250_HOST_CreateSynchronizationObject2,&sync)==0 && sync.hSyncObject==45);
 progress.sync=45;progress.value=5;progress.cpu_address=&sync_fence;
 sync_fence=1;assert(call(BC250_HOST_PUBLISH_PROGRESS,&progress)==0);
 native12::ProgressSnapshot running{};bridge.progress_snapshot(&running);
 assert(running.complete && running.count==1 && running.marks[0].value==5 && !bridge.progress_retired(&running));
 sync_fence=UINT64_MAX;
 native12::ProgressSnapshot removed{};bridge.progress_snapshot(&removed);
 assert(removed.complete && removed.count==1 && !bridge.progress_retired(&removed));
 // And it ends the proof of a snapshot taken while the fence was still running.
 assert(!bridge.progress_retired(&running));
 // A fence that truly caught up needs no mark and is retired at once.
 sync_fence=5;native12::ProgressSnapshot caught{};bridge.progress_snapshot(&caught);
 assert(caught.complete && !caught.count && bridge.progress_retired(&caught));
 sync_release.hSyncObject=45;
 assert(call(BC250_HOST_DestroySynchronizationObject,&sync_release)==0 && sync_destroys==3);
 sync_fence=0;
 D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait{};wait.hContext=context.hContext;wait.ObjectCount=1;wait.ObjectHandleArray=&paging.sync;wait.MonitoredFenceValueArray=&make.PagingFenceValue;
 assert(call(BC250_HOST_WaitForSynchronizationObjectFromGpu,&wait)==0);
 // The hosted ICD signals with AllowFenceRewind (wait-before-signal); the flag reaches the runtime, any other
 // flag is refused before it.
 D3DKMT_HANDLE signal_context=context.hContext;
 D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal{};signal.ObjectCount=1;signal.ObjectHandleArray=&paging.sync;
 signal.BroadcastContextCount=1;signal.BroadcastContextArray=&signal_context;signal.MonitoredFenceValueArray=&make.PagingFenceValue;
 assert(call(BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==0 && signalled==1 && !rewinds);
 signal.Flags.AllowFenceRewind=1;
 assert(call(BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==0 && signalled==2 && rewinds==1);
 signal.Flags.EnqueueCpuEvent=1;
 assert(call(BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)<0 && signalled==2);
 D3DKMT_SUBMITCOMMAND command{};command.BroadcastContextCount=1;command.BroadcastContext[0]=context.hContext;
 assert(call(BC250_HOST_SubmitCommand,&command)==0 && submitted==1);
 D3DKMT_DESTROYCONTEXT destroy{};destroy.hContext=context.hContext;wrapper.arguments=&destroy;
 fail_destroy=true;assert(call(BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)<0);assert(call(BC250_HOST_SubmitCommand,&command)==0 && submitted==2);
 fail_destroy=false;assert(call(BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)==0);assert(call(BC250_HOST_SubmitCommand,&command)<0 && submitted==2);
 context={};wrapper.arguments=&context;assert(call(BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper)==0 && context.hContext!=old_token);
 destroy.hContext=context.hContext;wrapper.arguments=&destroy;assert(call(BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)==0);
 D3DKMT_DESTROYALLOCATION2 release{};release.AllocationCount=1;release.phAllocationList=&info.hAllocation;
 D3DKMT_FREEGPUVIRTUALADDRESS free{};free.BaseAddress=mapping.VirtualAddress;free.Size=4096;
 // The flags, the resource and the count stay refused whatever the mapping is (BD-201 keeps them).
 release.Flags.AssumeNotInUse=1;assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && !deallocations);
 release.Flags.Value=0;release.AllocationCount=2;assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && !deallocations);
 release.AllocationCount=1;release.hResource=5;assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && !deallocations);
 release.hResource=0;
 // BD-045: a locked record refuses its destroy and stays whole, its mapping included.
 assert(call(BC250_HOST_Lock2,&lock)==0 && lock.pData==mapped);
 assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && !deallocations);
 assert(call(BC250_HOST_Unlock2,&unlock)==0);
 // BD-201: a refused deallocate keeps the record and its handle, so the retry succeeds, but its range is
 // lost and counted: the bridge never frees it or asks for it again. Nothing else releases the range on
 // the way: no Evict, no FreeGpuVirtualAddress.
 const unsigned freed=frees;
 fail_destroy=true;assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && deallocations==1);
 assert(bridge.lost_va_ranges()==1 && bridge.lost_va_bytes()==4096);
 assert(call(BC250_HOST_Lock2,&lock)==0 && call(BC250_HOST_Unlock2,&unlock)==0);
 // A range lost by a destroy is not the bridge's to free at all: the destroy takes it. (What the bridge
 // asks the runtime for at that address is proved in the reuse block below, where the reservation callback
 // counts its calls, and the free its own range keeps is case D there.)
 assert(call(BC250_HOST_FreeGpuVirtualAddress,&free)<0 && frees==freed);
 fail_destroy=false;assert(call(BC250_HOST_DestroyAllocation2,&release)==0 && deallocations==2);
 // Once the destroy is accepted the range goes with it; the count of the loss stays.
 assert(frees==freed && !evictions && bridge.lost_va_ranges()==1);
 assert(call(BC250_HOST_DestroyAllocation2,&release)<0 && deallocations==2);
 // The range went with the deferred destruction: it is no longer the bridge's to free.
 assert(call(BC250_HOST_FreeGpuVirtualAddress,&free)<0 && frees==freed);
 // The order of the explicit free then the destroy stays admitted: an R2 or replayable BO uses it.
 info.hAllocation=0;assert(call(BC250_HOST_CreateAllocation2,&create)==0 && info.hAllocation==77);
 assert(call(BC250_HOST_MapGpuVirtualAddress,&mapping)==0x103 && mapping.VirtualAddress==UINT64_C(0x400000000));
 // A refused free loses the range as well, and counts it, but its own retry of that exact extent is the one
 // request a lost range still answers (case D of the reuse block proves the rest of that rule).
 fail_free=true;assert(call(BC250_HOST_FreeGpuVirtualAddress,&free)<0 && frees==freed+1);
 assert(bridge.lost_va_ranges()==2 && bridge.lost_va_bytes()==8192);
 fail_free=false;assert(call(BC250_HOST_FreeGpuVirtualAddress,&free)==0 && frees==freed+2);
 assert(call(BC250_HOST_FreeGpuVirtualAddress,&free)<0 && frees==freed+2);
 assert(bridge.lost_va_ranges()==2 && bridge.lost_va_bytes()==8192);
 assert(call(BC250_HOST_DestroyAllocation2,&release)==0 && deallocations==3 && !evictions);
 // BD-201 R4: no hosted shell answers BC250_HOST_GetDeviceState yet, although worker_admits lists the
 // opcode. This pins the rule of the class comment, that an unsupported operation gets a refusal and never
 // a fabricated success, and it marks the gap: a device-state answer is what a loss test that does not read
 // UINT64_MAX out of a fence value would need (D3DKMT_DEVICEEXECUTION, d3dkmthk.md:39800-39815; the only
 // documented UINT64_MAX is a MonitoredValue with no waiters, native-gpu-fence-objects.md:46).
 // (An argument is needed to pass the common null check and reach the switch itself.)
 UINT64 state=0;assert(call(BC250_HOST_GetDeviceState,&state)==static_cast<int32_t>(0xc00000bbu) && !state);
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
 {
  // BD-201: the same window on the destroy path. A mapped, unlocked internal allocation is destroyed with
  // its mapping live; the runtime releases the API-allocated range inside the deallocate callback and hands
  // it to another thread's map (A) or reservation (B) before that callback returns. A refused deallocate
  // keeps the record and its handle, but loses its range for good (C).
  auto rum=um;rum.pfnAllocateCb=reuse_allocate;rum.pfnDeallocateCb=reuse_deallocate;rum.pfnSetErrorCb=reuse_removed;
  auto rkt=kt;rkt.pfnMapGpuVirtualAddressCb=reuse_map;rkt.pfnReserveGpuVirtualAddressCb=reuse_reserve;rkt.pfnFreeGpuVirtualAddressCb=reuse_free;
  HostedDispatch destroying(domain,{device_handle},rum,rkt);
  auto on=[&](uint32_t op,void* a){return HostedDispatch::dispatch(&destroying,op,a);};
  const unsigned noted=reuse_errors;   // the block above removed one device on purpose
  bc250_host_paging pages{};assert(on(BC250_HOST_CREATE_PAGING,&pages)==0);
  constexpr UINT64 bytes=65536;
  // One internal allocation with its own ordinary mapping at the shared address.
  // page_count 0 is the whole extent; a smaller map is used to overlap a lost range with one page.
  auto mapped_one=[&](D3DDDI_ALLOCATIONINFO2& out,D3DDDI_MAPGPUVIRTUALADDRESS& view,UINT64 page_count=0){
   out={};D3DKMT_CREATEALLOCATION request{};request.NumAllocations=1;request.pAllocationInfo2=&out;
   const bool made=on(BC250_HOST_CreateAllocation2,&request)==0;
   view={};view.hPagingQueue=pages.queue;view.hAllocation=out.hAllocation;
   view.SizeInPages=page_count?page_count:bytes/4096;
   return made && on(BC250_HOST_MapGpuVirtualAddress,&view)==0x103 && view.VirtualAddress==reused_va;};
  D3DDDI_ALLOCATIONINFO2 a{},b{},c{};D3DDDI_MAPGPUVIRTUALADDRESS a_map{},b_map{},c_map{};
  D3DKMT_DESTROYALLOCATION2 gone{};gone.AllocationCount=1;
  D3DKMT_FREEGPUVIRTUALADDRESS range{};range.BaseAddress=reused_va;range.Size=bytes;
  // A. The map of another allocation inside the callback: no false overlap, no removal.
  assert(mapped_one(a,a_map));
  gone.phAllocationList=&a.hAllocation;
  int32_t taken=-1;
  during_deallocate=[&]{bc250::umd::RuntimeDomain::Scope entered(domain);taken=mapped_one(b,b_map)?0:-1;};
  unsigned released=reuse_deallocations;
  assert(on(BC250_HOST_DestroyAllocation2,&gone)==0 && reuse_deallocations==released+1);
  assert(!taken && !destroying.lost() && reuse_errors==noted);
  // The record went with its mapping: neither a second destroy nor a free of its range finds it.
  assert(on(BC250_HOST_DestroyAllocation2,&gone)<0);
  // B. A reservation inside the callback takes the address of the allocation being destroyed.
  gone.phAllocationList=&b.hAllocation;
  D3DDDI_RESERVEGPUVIRTUALADDRESS held{};held.BaseAddress=reused_va;held.Size=bytes;int32_t reserved=-1;
  during_deallocate=[&]{bc250::umd::RuntimeDomain::Scope entered(domain);reserved=on(BC250_HOST_ReserveGpuVirtualAddress,&held);};
  released=reuse_deallocations;
  assert(on(BC250_HOST_DestroyAllocation2,&gone)==0 && reuse_deallocations==released+1);
  assert(!reserved && held.VirtualAddress==reused_va && !destroying.lost() && reuse_errors==noted);
  unsigned va_frees=reuse_frees;
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&range)==0 && reuse_frees==va_frees+1);
  // C. A refused deallocate: the handle stays findable and destroyable, but the range is lost, not owned
  //    again. The runtime may have released it inside the callback that then failed, so the bridge asks for
  //    nothing at that address itself (the fixed reservation is refused before the runtime sees it), while
  //    owned() ignores the range, so another thread's map of it is admitted with no false overlap. The
  //    retried destroy succeeds, and the refusal goes with the record, not with the address.
  assert(mapped_one(c,c_map));
  gone.phAllocationList=&c.hAllocation;
  fail_reuse_deallocate=true;released=reuse_deallocations;
  const unsigned asked=reuse_reserves;
  assert(on(BC250_HOST_DestroyAllocation2,&gone)<0 && reuse_deallocations==released+1);
  assert(destroying.lost_va_ranges()==1 && destroying.lost_va_bytes()==bytes);
  assert(on(BC250_HOST_ReserveGpuVirtualAddress,&held)<0 && reuse_reserves==asked && !destroying.lost());
  // The lost range is free for the runtime to hand out: a map of it from another thread is admitted.
  D3DDDI_ALLOCATIONINFO2 d{};D3DDDI_MAPGPUVIRTUALADDRESS d_map{};int32_t again=-1;
  std::thread elsewhere([&]{bc250::umd::RuntimeDomain::Scope entered(domain);again=mapped_one(d,d_map)?0:-1;});
  elsewhere.join();
  assert(!again && d_map.VirtualAddress==reused_va && !destroying.lost() && reuse_errors==noted);
  fail_reuse_deallocate=false;
  assert(on(BC250_HOST_DestroyAllocation2,&gone)==0 && reuse_deallocations==released+2);
  assert(destroying.lost_va_ranges()==1 && destroying.lost_va_bytes()==bytes);
  // With both records gone the address is the runtime's to give again, which is what proves the refusal
  // above belonged to the lost record.
  gone.phAllocationList=&d.hAllocation;
  assert(on(BC250_HOST_DestroyAllocation2,&gone)==0 && reuse_deallocations==released+3);
  assert(on(BC250_HOST_ReserveGpuVirtualAddress,&held)==0 && held.VirtualAddress==reused_va && reuse_reserves==asked+1);
  va_frees=reuse_frees;
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&range)==0 && reuse_frees==va_frees+1);
  // D. The explicit free follows the same rule: a refused free loses the extent as a refused destroy does,
  //    and a lost record keeps exactly one way out, the retry of its own extent. Nothing else frees it.
  D3DDDI_ALLOCATIONINFO2 e{};D3DDDI_MAPGPUVIRTUALADDRESS e_map{};
  assert(mapped_one(e,e_map));
  va_frees=reuse_frees;fail_reuse_free=true;
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&range)<0 && reuse_frees==va_frees+1);
  assert(destroying.lost_va_ranges()==2 && destroying.lost_va_bytes()==2*bytes);
  // owned() ignores the lost extent, so an overlapping map from another thread is no false overlap and
  // removes nothing. One page inside the lost range, so that the retry below can name only the lost record.
  D3DDDI_ALLOCATIONINFO2 f{};D3DDDI_MAPGPUVIRTUALADDRESS f_map{};int32_t overlapping=-1;
  std::thread beside([&]{bc250::umd::RuntimeDomain::Scope entered(domain);overlapping=mapped_one(f,f_map,1)?0:-1;});
  beside.join();
  assert(!overlapping && f_map.VirtualAddress==reused_va && !destroying.lost() && reuse_errors==noted);
  // A different extent is refused without reaching the runtime, the lost record's own is accepted, and the
  // retry clears the loss with the range (the count of the event stays).
  fail_reuse_free=false;va_frees=reuse_frees;
  D3DKMT_FREEGPUVIRTUALADDRESS other{};other.BaseAddress=reused_va;other.Size=2*bytes;
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&other)<0 && reuse_frees==va_frees);
  assert(on(BC250_HOST_FreeGpuVirtualAddress,&range)==0 && reuse_frees==va_frees+1);
  assert(destroying.lost_va_ranges()==2 && destroying.lost_va_bytes()==2*bytes);
  gone.phAllocationList=&e.hAllocation;assert(on(BC250_HOST_DestroyAllocation2,&gone)==0);
  gone.phAllocationList=&f.hAllocation;assert(on(BC250_HOST_DestroyAllocation2,&gone)==0);
  assert(on(BC250_HOST_DESTROY_PAGING,&pages)==0);
  assert(!destroying.lost() && reuse_errors==noted && destroying.discard_metadata()==0);
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
 // An imported heap routes its callback HRESULT through the same boundary as internal allocations.
 // Device loss must not turn into STATUS_UNSUCCESSFUL, nor may a replay worker make the runtime callback.
 for(const HRESULT hr:{D3DDDIERR_DEVICEREMOVED,DXGI_ERROR_DEVICE_REMOVED,DXGI_ERROR_DEVICE_HUNG,DXGI_ERROR_DEVICE_RESET}){
  HostedDispatch imported(domain,{device_handle},um,kt);
  const unsigned reported=errors;
  {
   HostedDispatch::WorkerScope loss_worker(imported);
   assert(imported.complete_operation(BC250_HOST_Unlock2,hr)==static_cast<int32_t>(0xc00002b6u));
   assert(imported.lost() && errors==reported);
  }
  imported.report_deferred_removal();assert(errors==reported+1);
  assert(imported.complete_operation(BC250_HOST_Unlock2,hr)==static_cast<int32_t>(0xc00002b6u));
  imported.report_deferred_removal();assert(errors==reported+1);
 }
 {
  HostedDispatch healthy(domain,{device_handle},um,kt);
  const unsigned reported=errors;
  assert(healthy.complete_operation(BC250_HOST_Lock2,S_OK)==0);
  assert(healthy.complete_operation(BC250_HOST_Lock2,E_OUTOFMEMORY)==static_cast<int32_t>(0xc0000017u));
  assert(healthy.complete_operation(BC250_HOST_Unlock2,E_FAIL)==static_cast<int32_t>(0xc0000001u));
  assert(!healthy.lost() && errors==reported);
 }
 std::puts("PASS hosted dispatch: native allocation/paging/VA/lock, pending residency, internal full-width context token, "
           "cleanup retries, destroy of a mapped allocation with its retry and the lost range of a refused one, scope, "
           "loss, a fence at UINT64_MAX that proves no retirement, address reuse inside a free and inside a destroy, "
           "replay worker scope, imported-heap error propagation");
}
