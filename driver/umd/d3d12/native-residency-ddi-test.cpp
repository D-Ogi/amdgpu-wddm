// SPDX-License-Identifier: MIT
#include "native-residency-ddi.h"
#include "device-state.h"
#include "engine-ddi/engine-ddi.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace {
struct Fixture {native12::Device device;unsigned resolved{},makes{},evicts{},errors{};unsigned expected_resolved{};HRESULT status{S_OK};UINT64 fence{42};
 // BD-052: what the heap imports were told, and the allocation list of the last notice.
 unsigned noted_resident{},noted_evicted{},noted_count{};D3DKMT_HANDLE noted[4]{};};
Fixture* fixture=nullptr;
HRESULT resolve(void* user,D3D12DDI_HANDLE_AND_TYPE object,D3DKMT_HANDLE* allocation) noexcept{
 auto f=static_cast<Fixture*>(user);++f->resolved;*allocation=0;
 switch(reinterpret_cast<UINT_PTR>(object.Handle)){
 case 1:case 2:*allocation=7;return S_OK;
 case 3:*allocation=9;return S_OK;
 case 4:return S_FALSE;
 case 6:return S_OK;
 default:return E_INVALIDARG;
 }
}
HRESULT APIENTRY make(D3D12DDI_HRTDEVICE rt,D3D12DDI_HRTPAGINGQUEUE paging,D3DDDI_MAKERESIDENT* args){
 auto f=static_cast<Fixture*>(rt.handle);assert(f->resolved==f->expected_resolved);
 assert(paging.handle==reinterpret_cast<HANDLE>(UINT64_C(0x100009876)) && args->hPagingQueue==0);
 assert(args->NumAllocations==2 && args->AllocationList[0]==7 && args->AllocationList[1]==9 && args->Flags.CantTrimFurther);
 ++f->makes;args->PagingFenceValue=f->fence;return f->status;
}
HRESULT APIENTRY evict(D3D12DDI_HRTDEVICE rt,const D3DDDICB_EVICT* args){
 auto f=static_cast<Fixture*>(rt.handle);assert(f->resolved==f->expected_resolved);
 assert(args->NumAllocations==2 && args->AllocationList[0]==7 && args->AllocationList[1]==9 && args->Flags.NotWrittenTo);
 ++f->evicts;return f->status;
}
void initialize(Fixture& f){
 f.device.runtime.handle=&f;f.device.callbacks.pfnMakeResidentCb=make;f.device.callbacks.pfnEvictCb=evict;
 f.device.callbacks.pfnSetErrorCb=[](D3D12DDI_HRTDEVICE rt,HRESULT hr){auto p=static_cast<Fixture*>(rt.handle);assert(hr==D3DDDIERR_DEVICEREMOVED);++p->errors;};
}
D3D12DDI_HANDLE_AND_TYPE object(UINT_PTR id){return {reinterpret_cast<void*>(id),D3D12DDI_HT_HEAP};}
}
// Link-only substitutes: exercise the production typed wrapper with the same
// controlled resolver; no engine objects or GPU driver are loaded by this test.
namespace native12 {engine_ddi::DeviceContext* engine_context(Device& d) noexcept{return reinterpret_cast<engine_ddi::DeviceContext*>(d.runtime.handle);}
// BD-052: the seam the residency DDIs use to tell this device's heap imports what the runtime has just
// done. Here it only records the notice and the handles it carried.
void note(const D3DKMT_HANDLE* allocations,unsigned count) noexcept {
 fixture->noted_count=count;
 for(unsigned i=0;i<count && i<4;++i)fixture->noted[i]=allocations[i];
}
void engine_note_runtime_residency(Device&,const D3DKMT_HANDLE* a,unsigned c) noexcept{++fixture->noted_resident;note(a,c);}
void engine_note_runtime_eviction(Device&,const D3DKMT_HANDLE* a,unsigned c) noexcept{++fixture->noted_evicted;note(a,c);}}
// BD-312: OfferResources and ReclaimResources against a model of the kernel's offer list. The model refuses the offer
// of allocation 11 (a heap locked for a CPU mapping) and of any list that holds it, discards what discard names, and
// fails every reclaim of an allocation it does not hold offered: a reclaim the driver must not make.
namespace bd312 {
struct Kernel {
 native12::Device device;
 std::vector<D3DKMT_HANDLE> held;                         // offered in the kernel
 std::vector<D3DKMT_HANDLE> discard;                      // discarded on reclaim
 std::vector<D3DKMT_HANDLE> last;                         // the list of the last callback
 unsigned offers{},reclaims{},errors{},bad{};
 D3DDDI_OFFER_PRIORITY priority{};
 HRESULT offer_status{S_OK},reclaim_status{S_OK};UINT64 reclaim_fence{0};
 bool contains(D3DKMT_HANDLE a) const {return std::find(held.begin(),held.end(),a)!=held.end();}
};
Kernel* kernel=nullptr;
constexpr D3DKMT_HANDLE kLocked=11;
const D3D12DDI_HRTPAGINGQUEUE kQueue{reinterpret_cast<HANDLE>(UINT64_C(0x100004321))};
HRESULT resolve(void*,D3D12DDI_HANDLE_AND_TYPE object,D3DKMT_HANDLE* allocation) noexcept{
 *allocation=0;
 switch(reinterpret_cast<UINT_PTR>(object.Handle)){
 case 1:case 2:*allocation=7;return S_OK;                // a placed resource and its heap share one allocation
 case 3:*allocation=9;return S_OK;
 case 4:return S_FALSE;                                   // descriptor heap, query heap, reserved resource
 case 10:*allocation=kLocked;return S_OK;
 case 12:*allocation=13;return S_OK;
 default:return E_INVALIDARG;
 }
}
HRESULT APIENTRY offer(D3D12DDI_HRTDEVICE rt,const D3D12DDICB_OFFERALLOCATIONS* args){
 auto k=static_cast<Kernel*>(rt.handle);++k->offers;k->priority=args->Priority;
 k->last.assign(args->HandleList,args->HandleList+args->NumAllocations);
 if(k->offer_status!=S_OK)return k->offer_status;
 for(UINT i=0;i<args->NumAllocations;++i){
  if(args->HandleList[i]==kLocked)return E_INVALIDARG;
  if(!args->HandleList[i] || k->contains(args->HandleList[i]))++k->bad;  // zero or offered twice
 }
 k->held.insert(k->held.end(),args->HandleList,args->HandleList+args->NumAllocations);
 return S_OK;
}
HRESULT APIENTRY reclaim(D3D12DDI_HRTDEVICE rt,D3D12DDI_HRTPAGINGQUEUE queue,D3D12DDICB_RECLAIMALLOCATIONS2* args){
 auto k=static_cast<Kernel*>(rt.handle);++k->reclaims;
 k->last.assign(args->HandleList,args->HandleList+args->NumAllocations);
 if(!args->pDiscarded){++k->bad;return E_INVALIDARG;}
 if(queue.handle!=kQueue.handle)++k->bad;
 for(UINT i=0;i<args->NumAllocations;++i)if(!k->contains(args->HandleList[i]))++k->bad;
 args->PagingFenceValue=k->reclaim_fence;
 if(k->reclaim_status!=S_OK && k->reclaim_status!=E_PENDING)return k->reclaim_status;
 for(UINT i=0;i<args->NumAllocations;++i){
  const auto a=args->HandleList[i];
  args->pDiscarded[i]=std::find(k->discard.begin(),k->discard.end(),a)!=k->discard.end();
  k->held.erase(std::remove(k->held.begin(),k->held.end(),a),k->held.end());
 }
 return k->reclaim_status;
}
void initialize(Kernel& k){
 k.device.runtime.handle=&k;
 k.device.callbacks.pfnOfferAllocationsCb=offer;k.device.callbacks.pfnReclaimAllocations2Cb=reclaim;
 k.device.callbacks.pfnSetErrorCb=[](D3D12DDI_HRTDEVICE rt,HRESULT hr){
  auto p=static_cast<Kernel*>(rt.handle);assert(p->device.lost.load() && hr==D3DDDIERR_DEVICEREMOVED);++p->errors;};
}
struct Reclaim {
 D3D12DDI_HRTPAGINGQUEUE queue=kQueue;UINT64 fence=777;std::vector<BOOL> discarded;D3D12DDIARG_RECLAIMRESOURCES_0001 args{};
 explicit Reclaim(std::vector<D3D12DDI_HANDLE_AND_TYPE>& objects):discarded(objects.size(),TRUE){
  args.NumAdapters=1;args.pRTPagingQueue=&queue;args.NumObjects=static_cast<UINT>(objects.size());
  args.pObjects=objects.data();args.pDiscarded=discarded.data();args.pPagingFenceValue=&fence;args.WaitMask=5;
 }
};
HRESULT offer_objects(Kernel& k,std::vector<D3D12DDI_HANDLE_AND_TYPE> objects,D3DDDI_OFFER_PRIORITY priority=D3DDDI_OFFER_PRIORITY_NORMAL){
 D3D12DDIARG_OFFERRESOURCES args{};args.NumObjects=static_cast<UINT>(objects.size());args.pObjects=objects.data();args.Priority=priority;
 return native12::native_residency_offer(k.device,&args,resolve,nullptr);
}
std::vector<D3DKMT_HANDLE> offered(Kernel& k){return k.device.offered;}
void run(){
 using V=std::vector<D3DKMT_HANDLE>;
 {
  // Offer: one callback for the deduplicated, sorted allocations; S_FALSE and refused objects are left out.
  Kernel k;initialize(k);
  assert(offer_objects(k,{object(3),object(1),object(4),object(2),object(3),object(5)},D3DDDI_OFFER_PRIORITY_HIGH)==S_OK);
  assert(k.offers==1 && k.last==V({7,9}) && k.priority==D3DDDI_OFFER_PRIORITY_HIGH && offered(k)==V({7,9}));
  // Offered again before a reclaim: the kernel takes each offer once, so no callback.
  assert(offer_objects(k,{object(1),object(3)})==S_OK && k.offers==1);
  // Reclaim: one callback on the runtime's paging queue, discarded mapped back to every object of that allocation.
  k.discard={9};
  std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(3),object(1),object(4),object(2),object(5)};Reclaim r(objects);
  assert(native12::native_residency_reclaim(k.device,&r.args,resolve,nullptr)==S_OK);
  assert(k.reclaims==1 && k.last==V({7,9}) && offered(k).empty() && k.held.empty());
  assert(r.discarded==std::vector<BOOL>({TRUE,FALSE,FALSE,FALSE,FALSE}) && !r.fence && !r.args.WaitMask);
  // Reclaimed already: nothing to give back, nothing discarded, no callback.
  Reclaim again(objects);
  assert(native12::native_residency_reclaim(k.device,&again.args,resolve,nullptr)==S_OK && k.reclaims==1);
  assert(again.discarded==std::vector<BOOL>(5,FALSE) && !again.fence && !again.args.WaitMask);
  assert(!k.device.lost && !k.errors && !k.bad);
 }
 {
  // A locked allocation: the batch is refused, each allocation is offered alone, the locked one stays the
  // application's. Its object reclaims as not discarded, and the kernel is never asked for it.
  Kernel k;initialize(k);
  assert(offer_objects(k,{object(10),object(1),object(12)})==S_OK);
  assert(k.offers==4 && offered(k)==V({7,13}) && k.held==V({7,13}));
  k.discard={7,kLocked};
  std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(10),object(2),object(12)};Reclaim r(objects);
  assert(native12::native_residency_reclaim(k.device,&r.args,resolve,nullptr)==S_OK);
  assert(k.last==V({7,13}) && r.discarded==std::vector<BOOL>({FALSE,TRUE,FALSE}) && offered(k).empty());
  assert(!k.device.lost && !k.errors && !k.bad);
 }
 {
  // A pending page-in: E_PENDING with the paging fence of adapter 0 and its bit in WaitMask.
  Kernel k;initialize(k);
  assert(offer_objects(k,{object(1)})==S_OK);
  k.reclaim_status=E_PENDING;k.reclaim_fence=55;
  std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(1)};Reclaim r(objects);
  assert(native12::native_residency_reclaim(k.device,&r.args,resolve,nullptr)==E_PENDING);
  assert(r.fence==55 && r.args.WaitMask==1 && r.discarded[0]==FALSE && offered(k).empty());
  // S_OK with a fence value carries no wait: WaitMask is valid only with E_PENDING.
  assert(offer_objects(k,{object(1)})==S_OK);
  k.reclaim_status=S_OK;k.reclaim_fence=66;Reclaim s(objects);
  assert(native12::native_residency_reclaim(k.device,&s.args,resolve,nullptr)==S_OK && !s.fence && !s.args.WaitMask);
  assert(!k.device.lost && !k.errors && !k.bad);
 }
 {
  // E_OUTOFMEMORY reclaims nothing: the allocation stays offered, the device stays, and a later reclaim takes it.
  Kernel k;initialize(k);
  assert(offer_objects(k,{object(1)})==S_OK);
  k.reclaim_status=E_OUTOFMEMORY;
  std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(1)};Reclaim r(objects);
  assert(native12::native_residency_reclaim(k.device,&r.args,resolve,nullptr)==E_OUTOFMEMORY);
  assert(offered(k)==V({7}) && !k.device.lost && !k.errors && !r.args.WaitMask);
  k.reclaim_status=S_OK;Reclaim s(objects);
  assert(native12::native_residency_reclaim(k.device,&s.args,resolve,nullptr)==S_OK && offered(k).empty() && k.reclaims==2);
  assert(!k.bad);
 }
 {
  // Negative control: any other reclaim failure, and E_PENDING without a fence, leave allocations the application
  // now uses in an unknown state. The device is removed and the DDI answers D3DDDIERR_DEVICEREMOVED.
  for(const auto mode:{0,1}){
   Kernel k;initialize(k);
   assert(offer_objects(k,{object(1)})==S_OK);
   k.reclaim_status=mode?E_PENDING:E_FAIL;k.reclaim_fence=0;
   std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(1)};Reclaim r(objects);
   assert(native12::native_residency_reclaim(k.device,&r.args,resolve,nullptr)==D3DDDIERR_DEVICEREMOVED);
   assert(k.device.lost && k.errors==1 && !r.args.WaitMask && !r.fence && r.discarded[0]==FALSE);
   // Removed: both slots answer D3DDDIERR_DEVICEREMOVED and call nothing.
   const auto offers=k.offers,reclaims=k.reclaims;
   assert(offer_objects(k,{object(3)})==D3DDDIERR_DEVICEREMOVED && k.offers==offers);
   Reclaim s(objects);
   assert(native12::native_residency_reclaim(k.device,&s.args,resolve,nullptr)==D3DDDIERR_DEVICEREMOVED);
   assert(k.reclaims==reclaims && s.discarded[0]==FALSE && !s.args.WaitMask && k.errors==1);
  }
 }
 {
  // Negative control: an offer the kernel answers with a removed device returns D3DDDIERR_DEVICEREMOVED; an offer
  // refused for every allocation is still S_OK, with nothing offered.
  Kernel k;initialize(k);
  k.offer_status=D3DDDIERR_DEVICEREMOVED;
  assert(offer_objects(k,{object(1),object(3)})==D3DDDIERR_DEVICEREMOVED && k.offers==1 && offered(k).empty());
  Kernel j;initialize(j);
  j.offer_status=E_INVALIDARG;
  assert(offer_objects(j,{object(1),object(3)})==S_OK && j.offers==3 && offered(j).empty() && !j.device.lost && !j.errors);
 }
 {
  // Negative control: without either kernel callback nothing is offered, and the slot still succeeds.
  for(const auto which:{0,1}){
   Kernel k;initialize(k);
   if(which)k.device.callbacks.pfnReclaimAllocations2Cb=nullptr;else k.device.callbacks.pfnOfferAllocationsCb=nullptr;
   assert(offer_objects(k,{object(1),object(3)})==S_OK && !k.offers && offered(k).empty() && !k.device.lost);
  }
 }
 {
  // Negative control: a malformed reclaim touches no array it was not given and calls nothing.
  Kernel k;initialize(k);
  assert(offer_objects(k,{object(1)})==S_OK);
  std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(1)};
  Reclaim two(objects);two.args.NumAdapters=2;
  assert(native12::native_residency_reclaim(k.device,&two.args,resolve,nullptr)==E_INVALIDARG && !two.args.WaitMask);
  assert(two.fence==777 && two.discarded[0]==TRUE);
  Reclaim no_queue(objects);no_queue.args.pRTPagingQueue=nullptr;
  assert(native12::native_residency_reclaim(k.device,&no_queue.args,resolve,nullptr)==E_INVALIDARG);
  Reclaim no_out(objects);no_out.args.pDiscarded=nullptr;
  assert(native12::native_residency_reclaim(k.device,&no_out.args,resolve,nullptr)==E_INVALIDARG);
  assert(native12::native_residency_reclaim(k.device,nullptr,resolve,nullptr)==E_INVALIDARG);
  assert(native12::native_residency_offer(k.device,nullptr,resolve,nullptr)==E_INVALIDARG);
  assert(!k.reclaims && offered(k)==V({7}) && !k.device.lost && !k.errors);
 }
 {
  // A released allocation leaves the offered list (heap-import.cpp, release_owned): a later allocation with the same
  // handle value is not reclaimed as offered.
  Kernel k;initialize(k);
  assert(offer_objects(k,{object(1),object(3)})==S_OK && offered(k)==V({7,9}));
  native12::forget_offered(k.device,7);native12::forget_offered(k.device,0);native12::forget_offered(k.device,1234);
  assert(offered(k)==V({9}));
  k.held={9};
  std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(1),object(3)};Reclaim r(objects);
  assert(native12::native_residency_reclaim(k.device,&r.args,resolve,nullptr)==S_OK && k.last==V({9}) && !k.bad);
 }
 {
  // The installed table slots, through the production wrappers.
  D3D12DDI_DEVICE_FUNCS_CORE_0088 table{};native12::install_native_residency_entries(table);
  if(table.pfnOfferResources!=native12::native_offer_resources || table.pfnReclaimResources!=native12::native_reclaim_resources ||
     !table.pfnOfferResources || !table.pfnReclaimResources){std::puts("FAIL: offer/reclaim slots not installed");std::exit(1);}
  Kernel k;initialize(k);kernel=&k;
  std::vector<D3D12DDI_HANDLE_AND_TYPE> objects{object(3)};
  D3D12DDIARG_OFFERRESOURCES args{};args.NumObjects=1;args.pObjects=objects.data();args.Priority=D3DDDI_OFFER_PRIORITY_LOW;
  assert(table.pfnOfferResources({&k.device},&args)==S_OK && offered(k)==V({9}) && k.priority==D3DDDI_OFFER_PRIORITY_LOW);
  Reclaim r(objects);
  assert(table.pfnReclaimResources({&k.device},&r.args)==S_OK && offered(k).empty() && r.discarded[0]==FALSE);
  assert(table.pfnOfferResources({},&args)==E_INVALIDARG);
  Reclaim null_device(objects);
  assert(table.pfnReclaimResources({},&null_device.args)==E_INVALIDARG && !null_device.args.WaitMask && !null_device.fence);
  assert(!k.device.lost && !k.errors && !k.bad);
  kernel=nullptr;
 }
}
}
namespace engine_ddi {HRESULT object_allocation(DeviceContext* c,D3D12DDI_HANDLE_AND_TYPE o,D3DKMT_HANDLE* a) noexcept{
 if(bd312::kernel && c==reinterpret_cast<DeviceContext*>(bd312::kernel))return bd312::resolve(c,o,a);
 return resolve(c,o,a);}}
int main(){
 // A failed assert ends the test with its message and exit code 3, without a crash report (and its window) per failure.
 _set_abort_behavior(0,_CALL_REPORTFAULT);
 bd312::run();
 Fixture f;initialize(f);fixture=&f;
 D3D12DDI_HANDLE_AND_TYPE objects[]={object(3),object(1),object(4),object(2),object(3)};
 D3D12DDI_HRTPAGINGQUEUE queue{reinterpret_cast<HANDLE>(UINT64_C(0x100009876))};UINT64 fence=99;
 D3D12DDIARG_MAKERESIDENT_0001 args{};args.NumAdapters=1;args.pRTPagingQueue=&queue;args.NumObjects=5;args.pObjects=objects;args.pPagingFenceValue=&fence;args.WaitMask=99;args.Flags.CantTrimFurther=1;
 f.expected_resolved=5;
 assert(native12::native_residency_make(f.device,&args,resolve,&f)==S_OK && f.makes==1 && !fence && !args.WaitMask);
 f.expected_resolved+=5;f.status=E_PENDING;
 assert(native12::native_residency_make(f.device,&args,resolve,&f)==E_PENDING && f.makes==2 && fence==42 && args.WaitMask==1);
 f.expected_resolved+=5;f.status=E_OUTOFMEMORY;
 assert(native12::native_residency_make(f.device,&args,resolve,&f)==E_OUTOFMEMORY && f.makes==3 && !fence && !args.WaitMask && !f.evicts);
 f.status=S_OK;f.expected_resolved+=5;
 D3D12DDIARG_EVICT ev{};ev.NumObjects=5;ev.pObjects=objects;ev.Flags.NotWrittenTo=1;
 assert(native12::native_residency_evict(f.device,&ev,resolve,&f)==S_OK && f.evicts==1);
 // BD-052: every accepted residency operation of the runtime reaches the heap imports, with the resolved
 // and deduplicated allocation list of that one callback. A pending MakeResident counts as accepted, an
 // E_OUTOFMEMORY refusal does not, and the Evict is told only when the kernel took it.
 assert(f.noted_resident==2 && f.noted_evicted==1 && f.noted_count==2 && f.noted[0]==7 && f.noted[1]==9);
 const auto makes=f.makes,evicts=f.evicts;
 const auto noted_resident=f.noted_resident,noted_evicted=f.noted_evicted;
 args.NumObjects=0;args.pObjects=nullptr;fence=88;args.WaitMask=7;
 assert(native12::native_residency_make(f.device,&args,nullptr,nullptr)==S_OK && !fence && !args.WaitMask && f.makes==makes);
 assert(f.noted_resident==noted_resident);              // an empty list is no residency operation at all
 ev.NumObjects=0;ev.pObjects=nullptr;assert(native12::native_residency_evict(f.device,&ev,nullptr,nullptr)==S_OK && f.evicts==evicts);
 assert(f.noted_evicted==noted_evicted);
 auto internal=object(4);args.NumObjects=1;args.pObjects=&internal;
 assert(native12::native_residency_make(f.device,&args,resolve,&f)==S_OK && !fence && !args.WaitMask && f.makes==makes);
 D3D12DDI_HANDLE_AND_TYPE unknown[]={object(1),object(5),object(3)};args.NumObjects=3;args.pObjects=unknown;
 assert(native12::native_residency_make(f.device,&args,resolve,&f)==E_INVALIDARG && f.makes==makes);
 auto malformed=object(6);args.NumObjects=1;args.pObjects=&malformed;
 assert(native12::native_residency_make(f.device,&args,resolve,&f)==E_UNEXPECTED && f.makes==makes);
 args.NumAdapters=2;assert(native12::native_residency_make(f.device,&args,resolve,&f)==E_NOTIMPL && !args.WaitMask && f.makes==makes);args.NumAdapters=1;
 args.NumObjects=5;args.pObjects=objects;f.expected_resolved=f.resolved+5;
 D3D12DDI_DEVICE_FUNCS_CORE_0088 table{};native12::install_native_residency_entries(table);
 assert(table.pfnMakeResident==native12::native_make_resident && table.pfnEvict==native12::native_evict);
 assert(table.pfnMakeResident({&f.device},&args)==S_OK && f.makes==makes+1 && !fence && !args.WaitMask);
 f.expected_resolved=f.resolved+5;f.status=E_PENDING;f.fence=0;
 assert(table.pfnMakeResident({&f.device},&args)==E_UNEXPECTED && f.device.lost && f.errors==1 && !fence && !args.WaitMask);
 auto calls=f.makes;assert(table.pfnMakeResident({&f.device},&args)==D3DDDIERR_DEVICEREMOVED && f.makes==calls);
 puts("PASS native residency DDI: pre-resolve/deduplicate single callback, real paging token, pending fence/wait mask, empty/internal skips, unknown refusal, typed installer, malformed acceptance removal, BD-052 residency and eviction notices to the heap imports, BD-312 offer/reclaim (batch, per-allocation retry of a locked heap, discarded map, pending fence, OOM keeps the offer, removal on any other failure, missing callbacks, malformed arguments, released allocations)");
}
