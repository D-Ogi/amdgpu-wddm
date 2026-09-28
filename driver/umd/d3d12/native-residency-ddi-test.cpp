// SPDX-License-Identifier: MIT
#include "native-residency-ddi.h"
#include "device-state.h"
#include "engine-ddi/engine-ddi.h"
#include <cassert>
#include <cstdio>
namespace {
struct Fixture {native12::Device device;unsigned resolved{},makes{},evicts{},errors{};unsigned expected_resolved{};HRESULT status{S_OK};UINT64 fence{42};};
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
namespace native12 {engine_ddi::DeviceContext* engine_context(Device& d) noexcept{return reinterpret_cast<engine_ddi::DeviceContext*>(d.runtime.handle);}}
namespace engine_ddi {HRESULT object_allocation(DeviceContext* c,D3D12DDI_HANDLE_AND_TYPE o,D3DKMT_HANDLE* a) noexcept{return resolve(c,o,a);}}
int main(){
 Fixture f;initialize(f);
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
 const auto makes=f.makes,evicts=f.evicts;
 args.NumObjects=0;args.pObjects=nullptr;fence=88;args.WaitMask=7;
 assert(native12::native_residency_make(f.device,&args,nullptr,nullptr)==S_OK && !fence && !args.WaitMask && f.makes==makes);
 ev.NumObjects=0;ev.pObjects=nullptr;assert(native12::native_residency_evict(f.device,&ev,nullptr,nullptr)==S_OK && f.evicts==evicts);
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
 puts("PASS native residency DDI: pre-resolve/deduplicate single callback, real paging token, pending fence/wait mask, empty/internal skips, unknown refusal, typed installer, malformed acceptance removal");
}
