// SPDX-License-Identifier: MIT
// Reserved address ranges through the hosted bridge: reserve, views inside a reservation, free by
// exact extent, and the queued update with everything it names held for the length of its callback.
#include "hosted-dispatch.h"
#include <d3dkmthk.h>
#include <cassert>
#include <cstdio>
#include <initializer_list>
using native12::HostedDispatch;
namespace {
HANDLE const device_handle=reinterpret_cast<HANDLE>(UINT64_C(0x1234567887654321));
HANDLE const context_handle=reinterpret_cast<HANDLE>(UINT64_C(0x34567890abcdef12));
constexpr UINT64 kLow=UINT64_C(0x200000000),kHigh=UINT64_C(0x200000000000);
constexpr D3DKMT_HANDLE kImported=500,kRetired=501;
UINT64 paging_fence=10,sync_fence=0;
D3DKMT_HANDLE next_allocation=100;
unsigned reserves=0,maps=0,frees=0,updates=0,queue_updates=0,deallocations=0,errors=0,lent=0,refused_inside=0;
UINT64 reserve_answer=0,map_answer=0;
HRESULT reserve_result=S_OK,free_result=S_OK,update_result=S_OK;
D3DDDI_MAPGPUVIRTUALADDRESS last_map{};
D3DDDICB_UPDATEGPUVIRTUALADDRESS last_update{};
void (*inside)()=nullptr;

HRESULT APIENTRY allocate(D3D12DDI_HRTDEVICE device,D3D12DDICB_ALLOCATE_0022* a){
 assert(device.handle==device_handle && a->NumAllocations==1);a->pAllocationInfo->hAllocation=next_allocation++;return S_OK;
}
HRESULT APIENTRY deallocate(D3D12DDI_HRTDEVICE,const D3D12DDICB_DEALLOCATE_0022* a){
 assert(a->NumAllocations==1 && !a->Flags);++deallocations;return S_OK;
}
VOID APIENTRY removed(D3D12DDI_HRTDEVICE,HRESULT result){assert(result==D3DDDIERR_DEVICEREMOVED);++errors;}
HRESULT APIENTRY paging_create(HANDLE,D3DDDICB_CREATEPAGINGQUEUE* a){a->hPagingQueue=9;a->hSyncObject=10;a->FenceValueCPUVirtualAddress=&paging_fence;return S_OK;}
HRESULT APIENTRY paging_destroy(HANDLE,const D3DDDI_DESTROYPAGINGQUEUE*){return S_OK;}
HRESULT APIENTRY reserve(HANDLE device,D3DDDI_RESERVEGPUVIRTUALADDRESS* a){
 assert(device==device_handle && !a->hAdapter && !a->Reserved0 && !a->Reserved1 && a->Size && !(a->Size&65535));
 ++reserves;if(reserve_result!=S_OK)return reserve_result;
 a->VirtualAddress=reserve_answer?reserve_answer:a->BaseAddress;return S_OK;
}
HRESULT APIENTRY map_va(HANDLE device,D3DDDI_MAPGPUVIRTUALADDRESS* a){
 assert(device==device_handle && a->hPagingQueue==9);++maps;last_map=*a;
 a->VirtualAddress=map_answer?map_answer:a->BaseAddress;a->PagingFenceValue=4;return E_PENDING;
}
HRESULT APIENTRY free_va(HANDLE device,const D3DDDICB_FREEGPUVIRTUALADDRESS*){
 assert(device==device_handle);++frees;return free_result;
}
HRESULT APIENTRY create_context(HANDLE,D3DDDICB_CREATECONTEXTVIRTUAL* a){a->hContext=context_handle;return S_OK;}
HRESULT APIENTRY destroy_context(HANDLE,const D3DDDICB_DESTROYCONTEXT* a){assert(a->hContext==context_handle);return S_OK;}
HRESULT APIENTRY create_sync(HANDLE,D3DDDICB_CREATESYNCHRONIZATIONOBJECT2* a){
 a->hSyncObject=45;a->Info.MonitoredFence.FenceValueCPUVirtualAddress=&sync_fence;return S_OK;
}
HRESULT APIENTRY destroy_sync(HANDLE,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT*){return S_OK;}
HRESULT APIENTRY update(HANDLE device,const D3DDDICB_UPDATEGPUVIRTUALADDRESS* a){
 // The callback's own request: the full context handle, never the bridge's 32-bit token.
 assert(device==device_handle && a->hContext==context_handle && a->hFenceObject==45 && a->NumOperations && a->Operations);
 assert(!a->Reserved0 && !a->Reserved1 && !a->Flags.Value);
 ++updates;last_update=*a;if(inside)inside();return update_result;
}
bool borrow(void*,D3DKMT_HANDLE handle) noexcept {if(handle!=kImported)return false;++lent;return true;}
void give_back(void*,D3DKMT_HANDLE handle) noexcept {assert(handle==kImported && lent);--lent;}
// An application queue's context: the bridge hands the whole request to the queue's owner.
HRESULT queue(void*,uint32_t op,void* argument) noexcept {
 assert(op==BC250_HOST_UpdateGpuVirtualAddress);
 assert(lent==1);
 // A token that is no queue of the owner's, as HostedQueue answers it.
 if(static_cast<D3DKMT_UPDATEGPUVIRTUALADDRESS*>(argument)->hContext!=7u)return E_INVALIDARG;
 ++queue_updates;return S_OK;
}
D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION map_op(UINT64 base,UINT64 bytes,D3DKMT_HANDLE handle,UINT64 part=0){
 D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op{};op.OperationType=D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP_PROTECT;
 op.MapProtect.BaseAddress=base;op.MapProtect.SizeInBytes=bytes;op.MapProtect.hAllocation=handle;
 op.MapProtect.AllocationSizeInBytes=part;op.MapProtect.Protection.Write=1;op.MapProtect.DriverProtection=1;return op;
}
D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION unmap_op(UINT64 base,UINT64 bytes){
 D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op{};op.OperationType=D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP;
 op.Unmap.BaseAddress=base;op.Unmap.SizeInBytes=bytes;op.Unmap.Protection.Zero=1;return op;
}
HostedDispatch* reentered=nullptr;
D3DKMT_HANDLE reentered_backing=0,reentered_token=0;
UINT64 reentered_base=0,reentered_bytes=0;
void reenter(){
 auto call=[&](uint32_t op,void* a){return HostedDispatch::dispatch(reentered,op,a);};
 D3DKMT_FREEGPUVIRTUALADDRESS release{};release.BaseAddress=reentered_base;release.Size=reentered_bytes;
 const unsigned before=frees;
 if(call(BC250_HOST_FreeGpuVirtualAddress,&release)<0 && frees==before)++refused_inside;
 D3DKMT_DESTROYSYNCHRONIZATIONOBJECT sync{};sync.hSyncObject=45;
 if(call(BC250_HOST_DestroySynchronizationObject,&sync)<0)++refused_inside;
 D3DKMT_DESTROYCONTEXT context{};context.hContext=reentered_token;bc250_host_queue_context wrapper{nullptr,&context};
 if(call(BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)<0)++refused_inside;
 D3DKMT_DESTROYALLOCATION2 destroy{};destroy.AllocationCount=1;destroy.phAllocationList=&reentered_backing;
 const unsigned released=deallocations;
 if(call(BC250_HOST_DestroyAllocation2,&destroy)<0 && deallocations==released)++refused_inside;
 assert(lent==1); // the imported backing of this batch is held as well
}
}
int main(){
 D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 um{};um.pfnAllocateCb=allocate;um.pfnDeallocateCb=deallocate;um.pfnSetErrorCb=removed;
 D3DDDI_DEVICECALLBACKS kt{};kt.pfnCreatePagingQueueCb=paging_create;kt.pfnDestroyPagingQueueCb=paging_destroy;
 kt.pfnReserveGpuVirtualAddressCb=reserve;kt.pfnMapGpuVirtualAddressCb=map_va;kt.pfnFreeGpuVirtualAddressCb=free_va;
 kt.pfnCreateContextVirtualCb=create_context;kt.pfnDestroyContextCb=destroy_context;
 kt.pfnCreateSynchronizationObject2Cb=create_sync;kt.pfnDestroySynchronizationObjectCb=destroy_sync;
 kt.pfnUpdateGpuVirtualAddressCb=update;
 bc250::umd::RuntimeDomain domain;bc250::umd::RuntimeDomain::Scope scope(domain);
 native12::HostedDispatchHooks hooks{nullptr,nullptr,queue,borrow,give_back};
 HostedDispatch bridge(domain,{device_handle},um,kt,hooks);
 auto call=[&](uint32_t op,void* a){return HostedDispatch::dispatch(&bridge,op,a);};
 bc250_host_paging paging{};assert(call(BC250_HOST_CREATE_PAGING,&paging)==0);

 // Reserve: what is refused never reaches the runtime.
 D3DDDI_RESERVEGPUVIRTUALADDRESS r{};r.hAdapter=77;r.MinimumAddress=kLow;r.MaximumAddress=kHigh;
 assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0);
 r.Size=65536+4096;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0);
 r.Size=0x100000;r.BaseAddress=kLow+4096;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0);
 r.BaseAddress=0;r.Reserved0=1;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0);
 r.Reserved0=0;r.Reserved1=1;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0);
 r.Reserved1=0;r.Reserved2=1;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0);
 r.Reserved2=0;r.MaximumAddress=kLow+65536;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0 && !reserves);
 r.MaximumAddress=kHigh;
 // A failed callback leaves no record: the extent it might have named cannot be freed.
 reserve_result=E_FAIL;reserve_answer=kLow;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)<0 && reserves==1);
 D3DKMT_FREEGPUVIRTUALADDRESS release{};release.BaseAddress=kLow;release.Size=r.Size;
 assert(call(BC250_HOST_FreeGpuVirtualAddress,&release)<0 && !frees);
 reserve_result=S_OK;assert(call(BC250_HOST_ReserveGpuVirtualAddress,&r)==0 && r.VirtualAddress==kLow && reserves==2);
 // A fixed base: the second range of the ICD's residency emulation.
 D3DDDI_RESERVEGPUVIRTUALADDRESS high{};high.BaseAddress=kLow+0x10000000;high.Size=0x100000;reserve_answer=0;
 assert(call(BC250_HOST_ReserveGpuVirtualAddress,&high)==0 && high.VirtualAddress==high.BaseAddress && reserves==3);
 D3DDDI_RESERVEGPUVIRTUALADDRESS again=high;again.VirtualAddress=0;
 assert(call(BC250_HOST_ReserveGpuVirtualAddress,&again)<0 && reserves==3);

 // Zero pages inside a reservation, and nowhere else.
 D3DDDI_MAPGPUVIRTUALADDRESS zero{};zero.hPagingQueue=9;zero.BaseAddress=kLow+65536;zero.SizeInPages=16;zero.Protection.Zero=1;
 assert(call(BC250_HOST_MapGpuVirtualAddress,&zero)==0x103 && maps==1 && zero.PagingFenceValue==4 && !last_map.hAllocation);
 zero.Protection.Write=1;assert(call(BC250_HOST_MapGpuVirtualAddress,&zero)<0);zero.Protection.Write=0;
 zero.BaseAddress=kLow+0x100000-4096;assert(call(BC250_HOST_MapGpuVirtualAddress,&zero)<0);
 zero.BaseAddress=kLow-4096;assert(call(BC250_HOST_MapGpuVirtualAddress,&zero)<0);
 zero.BaseAddress=kLow+0x40000000;assert(call(BC250_HOST_MapGpuVirtualAddress,&zero)<0 && maps==1);

 // The shared backing as the ICD makes it: an ordinary allocation with its own mapping, then
 // viewed twice inside reservations. Its own mapping survives and is still owed a Free.
 D3DDDI_ALLOCATIONINFO2 info{};unsigned payload=1;info.pPrivateDriverData=&payload;info.PrivateDriverDataSize=sizeof(payload);
 D3DKMT_CREATEALLOCATION create{};create.NumAllocations=1;create.pAllocationInfo2=&info;create.Flags.CreateResource=1;create.Flags.NonSecure=1;
 assert(call(BC250_HOST_CreateAllocation2,&create)==0);const D3DKMT_HANDLE shared=info.hAllocation;
 D3DDDI_MAPGPUVIRTUALADDRESS own{};own.hPagingQueue=9;own.hAllocation=shared;own.SizeInPages=8;
 const UINT64 own_va=UINT64_C(0x400000000);
 map_answer=own_va;assert(call(BC250_HOST_MapGpuVirtualAddress,&own)==0x103 && own.VirtualAddress==own_va && maps==2);
 map_answer=0;
 D3DDDI_MAPGPUVIRTUALADDRESS view{};view.hPagingQueue=9;view.hAllocation=shared;view.SizeInPages=8;view.BaseAddress=kLow;
 assert(call(BC250_HOST_MapGpuVirtualAddress,&view)==0x103 && view.VirtualAddress==kLow);
 view.BaseAddress=high.BaseAddress;view.VirtualAddress=0;
 assert(call(BC250_HOST_MapGpuVirtualAddress,&view)==0x103 && maps==4 && last_map.hAllocation==shared);
 view.Protection.Zero=1;assert(call(BC250_HOST_MapGpuVirtualAddress,&view)<0 && maps==4);view.Protection.Zero=0;
 // An ordinary mapping may not be placed over a reservation's edge or over another mapping.
 D3DDDI_MAPGPUVIRTUALADDRESS second{};second.hPagingQueue=9;second.SizeInPages=8;
 info.hAllocation=0;assert(call(BC250_HOST_CreateAllocation2,&create)==0);const D3DKMT_HANDLE plain=info.hAllocation;
 second.hAllocation=plain;second.BaseAddress=kLow+0x100000-4096;assert(call(BC250_HOST_MapGpuVirtualAddress,&second)<0);
 second.BaseAddress=own_va+4096;assert(call(BC250_HOST_MapGpuVirtualAddress,&second)<0 && maps==4);
 // A backing without a mapping of its own is viewed and then destroyed without any Free.
 second.BaseAddress=kLow+0x80000;assert(call(BC250_HOST_MapGpuVirtualAddress,&second)==0x103 && maps==5);
 D3DKMT_DESTROYALLOCATION2 destroy{};destroy.AllocationCount=1;destroy.phAllocationList=&info.hAllocation;
 assert(call(BC250_HOST_DestroyAllocation2,&destroy)==0 && deallocations==1);
 info.hAllocation=0;assert(call(BC250_HOST_CreateAllocation2,&create)==0);const D3DKMT_HANDLE loose=info.hAllocation;

 // Update on a context of the bridge's own.
 D3DKMT_CREATECONTEXTVIRTUAL context{};bc250_host_queue_context wrapper{nullptr,&context};
 assert(call(BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper)==0 && (context.hContext&0x80000000u));
 D3DKMT_CREATESYNCHRONIZATIONOBJECT2 sync{};sync.Info.Type=D3DDDI_MONITORED_FENCE;
 assert(call(BC250_HOST_CreateSynchronizationObject2,&sync)==0 && sync.hSyncObject==45);
 D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION ops[3]{map_op(kLow,0x10000,shared,0x10000),unmap_op(kLow+0x10000,0x10000),
     map_op(kLow+0x20000,0x20000,kImported,0x10000)};
 D3DKMT_UPDATEGPUVIRTUALADDRESS u{};u.hDevice=1;u.hContext=context.hContext;u.hFenceObject=45;u.NumOperations=3;
 u.Operations=ops;u.FenceValue=5;
 assert(call(BC250_HOST_UpdateGpuVirtualAddress,&u)==0 && updates==1 && !lent);
 assert(last_update.NumOperations==3 && last_update.Operations==ops && last_update.FenceValue==5);
 assert(ops[0].MapProtect.DriverProtection==1); // the ICD's value reaches the runtime as written
 // Every refusal is made before the callback and leaves nothing held.
 auto refused=[&]{const int32_t status=call(BC250_HOST_UpdateGpuVirtualAddress,&u);return status<0 && updates==1 && !lent;};
 u.NumOperations=0;assert(refused());u.NumOperations=3;
 u.Operations=nullptr;assert(refused());u.Operations=ops;
 u.Reserved0=1;assert(refused());u.Reserved0=0;
 u.Reserved1=1;assert(refused());u.Reserved1=0;
 u.FenceValue=0;assert(refused());u.FenceValue=UINT64_MAX-1;assert(refused());u.FenceValue=6;
 u.Flags.DoNotWait=1;assert(refused());u.Flags.Value=0;
 u.hFenceObject=46;assert(refused());u.hFenceObject=45;
 u.hContext=0x80000077u;assert(refused());u.hContext=context.hContext;
 auto with=[&](const D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION& op){const auto saved=ops[1];ops[1]=op;const bool r=refused();ops[1]=saved;return r;};
 D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION copy{};copy.OperationType=D3DDDI_UPDATEGPUVIRTUALADDRESS_COPY;assert(with(copy));
 assert(with(unmap_op(kLow+0x10001,0x10000)) && with(unmap_op(kLow+0x10000,0)) && with(unmap_op(kLow+0xff000,0x2000)));
 assert(with(unmap_op(high.BaseAddress,0x10000)));               // another reservation in the same call
 assert(with(unmap_op(own_va,0x1000)));                          // an ordinary mapping is not a reservation
 assert(with(map_op(kLow+0x10000,0x10000,0)) && with(map_op(kLow+0x10000,0x10000,999)) && with(map_op(kLow+0x10000,0x10000,kRetired)));
 assert(with(map_op(kLow+0x10000,0x3000,shared,0x2000)) && with(map_op(kLow+0x10000,0x2000,shared,0x3000)));
 auto bad=map_op(kLow+0x10000,0x10000,shared);bad.MapProtect.Protection.Zero=1;assert(with(bad));
 bad=map_op(kLow+0x10000,0x10000,shared);bad.MapProtect.AllocationOffsetInBytes=1;assert(with(bad));
 bad=unmap_op(kLow+0x10000,0x10000);bad.Unmap.Protection.Write=1;assert(with(bad));
 bad=unmap_op(kLow+0x10000,0x10000);bad.Unmap.Protection.Zero=0;bad.Unmap.Protection.NoAccess=1;
 ops[1]=bad;assert(call(BC250_HOST_UpdateGpuVirtualAddress,&u)==0 && updates==2 && !lent);
 // A part of 0 is the whole extent; a plain MAP carries no protection.
 ops[0]=map_op(kLow,0x10000,loose);ops[0].OperationType=D3DDDI_UPDATEGPUVIRTUALADDRESS_MAP;
 ops[0].Map={kLow,0x10000,loose,0,0};
 assert(call(BC250_HOST_UpdateGpuVirtualAddress,&u)==0 && updates==3);

 // A call that re-enters the bridge from inside the callback cannot take away what it names.
 reentered=&bridge;reentered_backing=loose;reentered_token=context.hContext;reentered_base=kLow;reentered_bytes=r.Size;
 inside=reenter;assert(call(BC250_HOST_UpdateGpuVirtualAddress,&u)==0 && updates==4 && refused_inside==4 && !lent);
 // A failed callback returns everything it held and erases nothing.
 refused_inside=0;update_result=E_FAIL;
 assert(call(BC250_HOST_UpdateGpuVirtualAddress,&u)<0 && updates==5 && refused_inside==4 && !lent && !bridge.lost());
 inside=nullptr;update_result=S_OK;assert(call(BC250_HOST_UpdateGpuVirtualAddress,&u)==0 && updates==6);

 // An application queue's context goes to the queue's owner with the same holds.
 D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION one=map_op(kLow,0x10000,kImported);
 u.hContext=7;u.NumOperations=1;u.Operations=&one;
 assert(call(BC250_HOST_UpdateGpuVirtualAddress,&u)==0 && queue_updates==1 && updates==6 && !lent);

 // Free: by the exact extent of its owner. A failed callback keeps the record for the retry.
 assert(call(BC250_HOST_DESTROY_PAGING,&paging)<0);
 release.BaseAddress=kLow;release.Size=r.Size-65536;assert(call(BC250_HOST_FreeGpuVirtualAddress,&release)<0 && !frees);
 release.Size=r.Size;free_result=E_FAIL;assert(call(BC250_HOST_FreeGpuVirtualAddress,&release)<0 && frees==1);
 free_result=S_OK;assert(call(BC250_HOST_FreeGpuVirtualAddress,&release)==0 && frees==2);
 assert(call(BC250_HOST_FreeGpuVirtualAddress,&release)<0 && frees==2);
 zero.BaseAddress=kLow+65536;assert(call(BC250_HOST_MapGpuVirtualAddress,&zero)<0); // the range is gone
 // The shared backing still has its own mapping: destroy waits for that Free.
 destroy.phAllocationList=&shared;assert(call(BC250_HOST_DestroyAllocation2,&destroy)<0 && deallocations==1);
 release.BaseAddress=own_va;release.Size=8*4096;assert(call(BC250_HOST_FreeGpuVirtualAddress,&release)==0 && frees==3);
 assert(call(BC250_HOST_DestroyAllocation2,&destroy)==0 && deallocations==2);
 destroy.phAllocationList=&loose;assert(call(BC250_HOST_DestroyAllocation2,&destroy)==0 && deallocations==3);
 // One reservation is left: it is counted at the end.
 assert(call(BC250_HOST_DESTROY_PAGING,&paging)<0 && !bridge.lost() && !errors);
 assert(bridge.discard_metadata()==4); // reservation, context, fence, paging queue

 // An ordinary mapping placed by the runtime outside the limits that were asked for: past the maximum,
 // below the minimum, and with its end past the maximum.
 auto removed_by_map=[&](UINT64 answer){
  HostedDispatch other(domain,{device_handle},um,kt,hooks);const unsigned before=errors;
  bc250_host_paging p{};assert(HostedDispatch::dispatch(&other,BC250_HOST_CREATE_PAGING,&p)==0);
  D3DDDI_ALLOCATIONINFO2 i{};unsigned data=1;i.pPrivateDriverData=&data;i.PrivateDriverDataSize=sizeof(data);
  D3DKMT_CREATEALLOCATION c{};c.NumAllocations=1;c.pAllocationInfo2=&i;
  assert(HostedDispatch::dispatch(&other,BC250_HOST_CreateAllocation2,&c)==0);
  D3DDDI_MAPGPUVIRTUALADDRESS a{};a.hPagingQueue=9;a.hAllocation=i.hAllocation;a.SizeInPages=2;
  a.MinimumAddress=UINT64_C(0x400000000);a.MaximumAddress=UINT64_C(0x400010000);map_answer=answer;
  const bool refused=HostedDispatch::dispatch(&other,BC250_HOST_MapGpuVirtualAddress,&a)<0;map_answer=0;
  const bool lost=refused && other.lost() && errors==before+1;other.discard_metadata();return lost;
 };
 {   // the same request answered inside its limits is an ordinary mapping
  HostedDispatch other(domain,{device_handle},um,kt,hooks);
  bc250_host_paging p{};assert(HostedDispatch::dispatch(&other,BC250_HOST_CREATE_PAGING,&p)==0);
  D3DDDI_ALLOCATIONINFO2 i{};unsigned data=1;i.pPrivateDriverData=&data;i.PrivateDriverDataSize=sizeof(data);
  D3DKMT_CREATEALLOCATION c{};c.NumAllocations=1;c.pAllocationInfo2=&i;
  assert(HostedDispatch::dispatch(&other,BC250_HOST_CreateAllocation2,&c)==0);
  D3DDDI_MAPGPUVIRTUALADDRESS a{};a.hPagingQueue=9;a.hAllocation=i.hAllocation;a.SizeInPages=2;
  a.MinimumAddress=UINT64_C(0x400000000);a.MaximumAddress=UINT64_C(0x400010000);map_answer=UINT64_C(0x40000e000);
  assert(HostedDispatch::dispatch(&other,BC250_HOST_MapGpuVirtualAddress,&a)==0x103 && !other.lost());map_answer=0;
  other.discard_metadata();
 }
 // Answers of the runtime that would give one extent two owners remove the device.
 auto removed_by=[&](void (*arrange)(HostedDispatch&)){
  HostedDispatch other(domain,{device_handle},um,kt,hooks);const unsigned before=errors;
  bc250_host_paging p{};assert(HostedDispatch::dispatch(&other,BC250_HOST_CREATE_PAGING,&p)==0);
  D3DDDI_RESERVEGPUVIRTUALADDRESS first{};first.Size=0x100000;first.MinimumAddress=kLow;first.MaximumAddress=kHigh;
  reserve_answer=kLow;assert(HostedDispatch::dispatch(&other,BC250_HOST_ReserveGpuVirtualAddress,&first)==0);
  arrange(other);reserve_answer=0;map_answer=0;
  const bool lost=other.lost() && errors==before+1;other.discard_metadata();return lost;
 };
 assert(removed_by([](HostedDispatch& b){
  D3DDDI_RESERVEGPUVIRTUALADDRESS a{};a.Size=0x100000;reserve_answer=kLow+0x80000; // not on the granule, inside the first
  assert(HostedDispatch::dispatch(&b,BC250_HOST_ReserveGpuVirtualAddress,&a)<0);}));
 assert(removed_by([](HostedDispatch& b){
  D3DDDI_RESERVEGPUVIRTUALADDRESS a{};a.Size=0x100000;reserve_answer=kLow+0x10000; // overlaps the first
  assert(HostedDispatch::dispatch(&b,BC250_HOST_ReserveGpuVirtualAddress,&a)<0);}));
 assert(removed_by([](HostedDispatch& b){
  D3DDDI_RESERVEGPUVIRTUALADDRESS a{};a.Size=0x100000;a.MinimumAddress=kLow;a.MaximumAddress=kLow+0x200000;
  reserve_answer=kLow+0x200000; // ends past the maximum
  assert(HostedDispatch::dispatch(&b,BC250_HOST_ReserveGpuVirtualAddress,&a)<0);}));
 assert(removed_by([](HostedDispatch& b){
  D3DDDI_RESERVEGPUVIRTUALADDRESS a{};a.Size=0x100000;a.BaseAddress=kLow+0x1000000;reserve_answer=kLow+0x2000000;
  assert(HostedDispatch::dispatch(&b,BC250_HOST_ReserveGpuVirtualAddress,&a)<0);})); // a fixed base moved
 assert(removed_by([](HostedDispatch& b){
  D3DDDI_MAPGPUVIRTUALADDRESS a{};a.hPagingQueue=9;a.BaseAddress=kLow;a.SizeInPages=1;a.Protection.Zero=1;map_answer=kLow+4096;
  assert(HostedDispatch::dispatch(&b,BC250_HOST_MapGpuVirtualAddress,&a)<0);})); // a view moved
 assert(removed_by([](HostedDispatch& b){
  D3DDDI_ALLOCATIONINFO2 i{};unsigned data=1;i.pPrivateDriverData=&data;i.PrivateDriverDataSize=sizeof(data);
  D3DKMT_CREATEALLOCATION c{};c.NumAllocations=1;c.pAllocationInfo2=&i;
  assert(HostedDispatch::dispatch(&b,BC250_HOST_CreateAllocation2,&c)==0);
  D3DDDI_MAPGPUVIRTUALADDRESS a{};a.hPagingQueue=9;a.hAllocation=i.hAllocation;a.SizeInPages=1;map_answer=kLow+0x1000;
  assert(HostedDispatch::dispatch(&b,BC250_HOST_MapGpuVirtualAddress,&a)<0);})); // placed inside a reservation
 for(const UINT64 answer:{UINT64_C(0x500000000),UINT64_C(0x3ffff0000),UINT64_C(0x40000f000)})assert(removed_by_map(answer));
 std::puts("PASS hosted sparse: reserve and its answers, zero and backed views, shared backing with its own mapping, "
           "free by exact extent with retry, update admission, holds across reentry and failure, queue route, closure count");
}
