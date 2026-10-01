// SPDX-License-Identifier: MIT
// DDI entries of one device on several threads at once, without a GPU. DeviceEngineScope binds each
// thread and excludes none of them; the hosted callbacks of those threads run through one bridge at the
// same time, with runtime callbacks in flight on all of them; the heap imports admit owner scopes and
// imports on several threads at once. Each "at once" is a meeting inside a fake callback or a scope that
// only succeeds when every thread is inside together, bounded by a deadline: a lock held across the
// scope or the callback turns it into a failure, not a hang. Built with AMDGPU_WDDM_D3D12_HOST_TEST,
// which gives a device an owner without the engine device (device-engine.h).
#include "device-engine.h"
#include "device-state.h"
#include "heap-import.h"
#include <bc250_host_bootstrap.h>
#include <d3dkmthk.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
using namespace native12;
namespace {
constexpr unsigned kThreads=8,kRounds=200,kImportThreads=4,kImportRounds=50;
constexpr ULONGLONG kDeadlineMs=5000;
template<class T> T handle(uintptr_t n){return reinterpret_cast<T>(n);}
std::atomic<unsigned> next_allocation{100},next_sync{1},next_context{1},next_memory{1};
std::atomic<unsigned> errors{},allocations{},deallocations{},paging_creates{},paging_destroys{},failures{};
std::atomic<UINT64> next_va{UINT64_C(0x100000000)};
UINT64 paging_fence=0;                          // never UINT64_MAX: nothing is lost
UINT64 fences[4096]{};                          // the CPU values of the monitored fences
char mapped[4096];
bc250_host hosts[2]{};                          // the fake ICD's view of each instance, in creation order
std::atomic<unsigned> instances{},live_instances{};
// Callers wait, bounded, until `expected` of them are inside at the same time. Once met, it stays met;
// once a caller's deadline passes unmet, it stays missed and later callers pass at once, so a lock that
// excludes the callers costs one deadline, not one per call.
struct Meeting {
    std::atomic<unsigned> expected{},inside{};
    std::atomic<bool> met{},missed{};
    void arm(unsigned n) noexcept {inside=0;met=false;missed=false;expected=n;}
    void disarm() noexcept {expected=0;}
    void pass() noexcept {
        const unsigned n=expected.load();if(!n || missed.load())return;
        if(inside.fetch_add(1)+1>=n)met=true;
        const ULONGLONG until=GetTickCount64()+kDeadlineMs;
        while(!met.load() && !missed.load() && GetTickCount64()<until)SwitchToThread();
        if(!met.load())missed=true;
        inside.fetch_sub(1);
    }
};
Meeting scope_meeting,lock_meeting,allocate_meeting,submit_meeting;

// The runtime: thread-safe fakes, unique handles and addresses.
HRESULT APIENTRY allocate_cb(D3D12DDI_HRTDEVICE,D3D12DDICB_ALLOCATE_0022* a){
    allocate_meeting.pass();
    ++allocations;a->pAllocationInfo->hAllocation=next_allocation.fetch_add(1);return S_OK;
}
HRESULT APIENTRY deallocate_cb(D3D12DDI_HRTDEVICE,const D3D12DDICB_DEALLOCATE_0022* a){
    if(a->NumAllocations!=1 || !a->HandleList || !a->HandleList[0])++failures;
    ++deallocations;return S_OK;
}
VOID APIENTRY set_error(D3D12DDI_HRTDEVICE,HRESULT){++errors;}
HRESULT APIENTRY paging_create(HANDLE,D3DDDICB_CREATEPAGINGQUEUE* a){
    ++paging_creates;a->hPagingQueue=9;a->hSyncObject=10;a->FenceValueCPUVirtualAddress=&paging_fence;return S_OK;
}
HRESULT APIENTRY paging_destroy(HANDLE,const D3DDDI_DESTROYPAGINGQUEUE*){++paging_destroys;return S_OK;}
HRESULT APIENTRY map_va(HANDLE,D3DDDI_MAPGPUVIRTUALADDRESS* a){
    a->VirtualAddress=a->BaseAddress?a->BaseAddress:next_va.fetch_add(0x10000);a->PagingFenceValue=0;return S_OK;
}
HRESULT APIENTRY reserve_va(HANDLE,D3DDDI_RESERVEGPUVIRTUALADDRESS* a){
    a->VirtualAddress=next_va.fetch_add(0x10000);a->PagingFenceValue=0;return S_OK;
}
HRESULT APIENTRY free_va(HANDLE,const D3DDDICB_FREEGPUVIRTUALADDRESS*){return S_OK;}
HRESULT APIENTRY make_resident(HANDLE,D3DDDI_MAKERESIDENT* a){a->PagingFenceValue=0;return S_OK;}
HRESULT APIENTRY evict(HANDLE,D3DDDICB_EVICT* a){a->NumBytesToTrim=0;return S_OK;}
HRESULT APIENTRY lock2(HANDLE,D3DDDICB_LOCK2* a){lock_meeting.pass();a->pData=mapped;return S_OK;}
HRESULT APIENTRY unlock2(HANDLE,const D3DDDICB_UNLOCK2*){return S_OK;}
HRESULT APIENTRY create_sync(HANDLE,D3DDDICB_CREATESYNCHRONIZATIONOBJECT2* a){
    const unsigned n=next_sync.fetch_add(1);if(n>=4096){++failures;return E_OUTOFMEMORY;}
    a->hSyncObject=1000+n;a->Info.MonitoredFence.FenceValueCPUVirtualAddress=&fences[n];return S_OK;
}
HRESULT APIENTRY destroy_sync(HANDLE,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT*){return S_OK;}
HRESULT APIENTRY create_context(HANDLE,D3DDDICB_CREATECONTEXTVIRTUAL* a){
    a->hContext=handle<HANDLE>(0x7000+next_context.fetch_add(1));return S_OK;
}
HRESULT APIENTRY destroy_context(HANDLE,const D3DDDICB_DESTROYCONTEXT*){return S_OK;}
HRESULT APIENTRY submit(HANDLE,const D3DDDICB_SUBMITCOMMAND* a){
    if(a->BroadcastContextCount!=1 || !a->BroadcastContext[0])++failures;
    submit_meeting.pass();return S_OK;
}
HRESULT APIENTRY wait_cpu(HANDLE,const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU*){return S_OK;}

// The ICD: its instance takes the host descriptor and fills the queue functions, as hosted RADV does.
struct Chain {uint32_t sType;const void* pNext;};
int32_t bind_queue(void*,void*){return 0;}
int32_t unbind_queue(void*){return 0;}
VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo* info,const VkAllocationCallbacks*,VkInstance* out){
    const unsigned index=instances.load();
    if(!info || !out || index>=2)return VK_ERROR_INITIALIZATION_FAILED;
    bool host=false,binding=false;
    for(auto next=info->pNext;next;next=static_cast<const Chain*>(next)->pNext){
        const auto type=static_cast<const Chain*>(next)->sType;
        if(type==BC250_HOST_STYPE){hosts[index]=*static_cast<const bc250_host*>(next);host=true;}
        else if(type==BC250_HOST_QUEUE_BINDING_STYPE){
            auto funcs=static_cast<const bc250_host_queue_binding*>(next)->funcs;
            if(funcs){funcs->bind=bind_queue;funcs->unbind=unbind_queue;binding=true;}
        }
    }
    if(!host || !binding || !hosts[index].dispatch || hosts[index].version!=BC250_HOST_VERSION)
        return VK_ERROR_INITIALIZATION_FAILED;
    ++instances;++live_instances;*out=handle<VkInstance>(0x5000+index);return VK_SUCCESS;
}
void VKAPI_CALL destroy_instance(VkInstance,const VkAllocationCallbacks*){--live_instances;}
void VKAPI_CALL memory_properties(VkPhysicalDevice,VkPhysicalDeviceMemoryProperties* out){
    *out={};out->memoryTypeCount=2;
    out->memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    out->memoryTypes[1].propertyFlags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VkResult VKAPI_CALL buffer_create(VkDevice,const VkBufferCreateInfo*,const VkAllocationCallbacks*,VkBuffer* out){
    *out=handle<VkBuffer>(7);return VK_SUCCESS;
}
void VKAPI_CALL buffer_destroy(VkDevice,VkBuffer,const VkAllocationCallbacks*){}
void VKAPI_CALL buffer_requirements(VkDevice,VkBuffer,VkMemoryRequirements* out){*out={65536,65536,3};}
VkResult VKAPI_CALL memory_allocate(VkDevice,const VkMemoryAllocateInfo*,const VkAllocationCallbacks*,VkDeviceMemory* out){
    *out=handle<VkDeviceMemory>(0x100000+next_memory.fetch_add(1));return VK_SUCCESS;
}
void VKAPI_CALL memory_free(VkDevice,VkDeviceMemory,const VkAllocationCallbacks*){}
PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice,const char* name){
#define FN(n,f) if(!std::strcmp(name,n))return reinterpret_cast<PFN_vkVoidFunction>(f)
    FN("vkAllocateMemory",memory_allocate);FN("vkFreeMemory",memory_free);FN("vkCreateBuffer",buffer_create);
    FN("vkDestroyBuffer",buffer_destroy);FN("vkGetBufferMemoryRequirements",buffer_requirements);return nullptr;
}
PFN_vkVoidFunction VKAPI_CALL icd_gipa(VkInstance,const char* name){
    FN("vkCreateInstance",create_instance);FN("vkDestroyInstance",destroy_instance);
    FN("vkGetPhysicalDeviceMemoryProperties",memory_properties);FN("vkGetDeviceProcAddr",gdpa);return nullptr;
#undef FN
}
int32_t call(unsigned device,uint32_t op,void* argument){return hosts[device].dispatch(hosts[device].userdata,op,argument);}

// 1. Scopes: every thread inside its own scope of the device at the same time.
void scopes(Device& device,Device& other){
    scope_meeting.arm(kThreads);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<kThreads;++t)threads.emplace_back([&]{
        if(device_engine_entered(device)){++failures;return;}
        DeviceEngineScope scope(device);
        if(!scope.entered()){++failures;return;}
        // Nested on the same device: admitted. Another device from inside this one: refused.
        {DeviceEngineScope nested(device);if(!nested.entered())++failures;}
        {DeviceEngineScope foreign(other);if(foreign.entered())++failures;}
        if(!device_engine_entered(device) || device_engine_entered(other))++failures;
        scope_meeting.pass();
    });
    for(auto& thread:threads)thread.join();
    scope_meeting.disarm();
    assert(!failures.load() && scope_meeting.met.load());
}
// One round of the callbacks hosted RADV makes during recording and resource work: an allocation
// mapped, resident, locked and released; a reservation with a zero view; a fence and an internal
// context with a publication and a submission; a status check.
bool round_trip(const bc250_host_paging& paging){
    unsigned private_data=1;D3DDDI_ALLOCATIONINFO2 info{};info.pPrivateDriverData=&private_data;
    info.PrivateDriverDataSize=sizeof(private_data);
    D3DKMT_CREATEALLOCATION create{};create.NumAllocations=1;create.pAllocationInfo2=&info;
    if(call(0,BC250_HOST_CreateAllocation2,&create)!=0 || !info.hAllocation)return false;
    D3DKMT_HANDLE allocation=info.hAllocation;
    D3DDDI_MAPGPUVIRTUALADDRESS map{};map.hPagingQueue=paging.queue;map.hAllocation=allocation;map.SizeInPages=1;
    map.Protection.Write=1;
    if(call(0,BC250_HOST_MapGpuVirtualAddress,&map)!=0 || !map.VirtualAddress)return false;
    D3DDDI_MAKERESIDENT resident{};resident.hPagingQueue=paging.queue;resident.NumAllocations=1;
    resident.AllocationList=&allocation;
    if(call(0,BC250_HOST_MakeResident,&resident)!=0)return false;
    D3DKMT_LOCK2 lock{};lock.hAllocation=allocation;
    if(call(0,BC250_HOST_Lock2,&lock)!=0 || lock.pData!=mapped)return false;
    D3DKMT_UNLOCK2 unlock{};unlock.hAllocation=allocation;
    if(call(0,BC250_HOST_Unlock2,&unlock)!=0)return false;
    D3DDDI_RESERVEGPUVIRTUALADDRESS reserve{};reserve.Size=65536;
    if(call(0,BC250_HOST_ReserveGpuVirtualAddress,&reserve)!=0 || !reserve.VirtualAddress)return false;
    D3DDDI_MAPGPUVIRTUALADDRESS view{};view.hPagingQueue=paging.queue;view.BaseAddress=reserve.VirtualAddress;
    view.SizeInPages=16;view.Protection.Zero=1;
    if(call(0,BC250_HOST_MapGpuVirtualAddress,&view)!=0)return false;
    D3DKMT_FREEGPUVIRTUALADDRESS range{};range.BaseAddress=reserve.VirtualAddress;range.Size=65536;
    if(call(0,BC250_HOST_FreeGpuVirtualAddress,&range)!=0)return false;
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 sync{};sync.Info.Type=D3DDDI_MONITORED_FENCE;
    if(call(0,BC250_HOST_CreateSynchronizationObject2,&sync)!=0 || !sync.hSyncObject)return false;
    D3DKMT_CREATECONTEXTVIRTUAL context{};bc250_host_queue_context wrapper{nullptr,&context};
    if(call(0,BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper)!=0 || !context.hContext)return false;
    bc250_host_progress progress{context.hContext,sync.hSyncObject,1,
        static_cast<const uint64_t*>(sync.Info.MonitoredFence.FenceValueCPUVirtualAddress)};
    if(call(0,BC250_HOST_PUBLISH_PROGRESS,&progress)!=0)return false;
    D3DKMT_SUBMITCOMMAND command{};command.BroadcastContextCount=1;command.BroadcastContext[0]=context.hContext;
    if(call(0,BC250_HOST_SubmitCommand,&command)!=0)return false;
    if(call(0,BC250_HOST_CHECK_STATUS,nullptr)!=0)return false;
    D3DKMT_DESTROYCONTEXT context_release{};context_release.hContext=context.hContext;wrapper.arguments=&context_release;
    if(call(0,BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)!=0)return false;
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT sync_release{};sync_release.hSyncObject=sync.hSyncObject;
    if(call(0,BC250_HOST_DestroySynchronizationObject,&sync_release)!=0)return false;
    D3DKMT_EVICT trim{};trim.NumAllocations=1;trim.AllocationList=&allocation;
    if(call(0,BC250_HOST_Evict,&trim)!=0)return false;
    D3DKMT_FREEGPUVIRTUALADDRESS va{};va.BaseAddress=map.VirtualAddress;va.Size=4096;
    if(call(0,BC250_HOST_FreeGpuVirtualAddress,&va)!=0)return false;
    D3DKMT_DESTROYALLOCATION2 release{};release.AllocationCount=1;release.phAllocationList=&allocation;
    return call(0,BC250_HOST_DestroyAllocation2,&release)==0;
}
// 2. Hosted callbacks: every thread inside the runtime's Lock2 at the same time, then rounds on all.
void callbacks(Device& device){
    bc250_host_paging paging{};
    {DeviceEngineScope scope(device);assert(scope.entered() && call(0,BC250_HOST_CREATE_PAGING,&paging)==0 && paging.queue==9);}
    lock_meeting.arm(kThreads);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<kThreads;++t)threads.emplace_back([&]{
        DeviceEngineScope scope(device);
        if(!scope.entered()){++failures;return;}
        for(unsigned round=0;round<kRounds;++round)if(!round_trip(paging)){++failures;return;}
    });
    for(auto& thread:threads)thread.join();
    lock_meeting.disarm();
    assert(!failures.load() && lock_meeting.met.load());
    {DeviceEngineScope scope(device);assert(scope.entered() && call(0,BC250_HOST_DESTROY_PAGING,&paging)==0);}
}
// 3. Outside any scope of the device, a callback is refused; so is one from inside another device's scope.
void refusals(Device& other){
    std::thread outside([]{if(call(0,BC250_HOST_CHECK_STATUS,nullptr)>=0)++failures;});outside.join();
    std::thread foreign([&]{
        DeviceEngineScope scope(other);
        if(!scope.entered() || call(0,BC250_HOST_CHECK_STATUS,nullptr)>=0 || call(1,BC250_HOST_CHECK_STATUS,nullptr)!=0)++failures;
    });
    foreign.join();
    assert(!failures.load());
}
// 4. Heap imports: owner scopes open on every thread, all inside the runtime's allocate at the same
// time, each with its own reports; a second scope on the same thread is still refused.
void imports(Device& device){
    auto owner=engine_imports(device);assert(owner);
    allocate_meeting.arm(kImportThreads);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<kImportThreads;++t)threads.emplace_back([&]{
        DeviceEngineScope scope(device);
        if(!scope.entered()){++failures;return;}
        D3D12DDIARG_CREATEHEAP_0001 heap{};heap.Flags=D3D12DDI_HEAP_FLAG_BUFFERS;heap.CreationNodeMask=heap.VisibleNodeMask=1;
        heap.CPUPageProperty=D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
        D3D12DDIARG_CREATERESOURCE_0088 resource{};resource.ResourceType=D3D12DDI_RT_BUFFER;
        engine_ddi::MemoryRequest request{};request.size=sizeof(request);request.rt_owner={handle<void*>(2)};
        request.heap=&heap;request.resource=&resource;request.byte_size=4096;request.alignment=65536;
        request.flags=engine_ddi::kMemoryDedicated;
        for(unsigned round=0;round<kImportRounds;++round){
            OwnerScope span(owner,0);
            if(span.refused()){++failures;return;}
            {OwnerScope nested(owner,0);if(!nested.refused())++failures;}
            engine_ddi::ImportedMemory memory{};
            if(owner->allocate(&request,&memory)!=S_OK || owner->last_report().stage!=ImportStage::Done ||
               !memory.memory || memory.memory_type_index!=0){++failures;return;}
            if(!owner->owns_allocation(memory.allocation) || !owner->borrow_backing(memory.allocation)){++failures;return;}
            // Borrowed backing: the release waits for its return.
            if(owner->free(&memory)!=E_PENDING)++failures;
            owner->return_backing(memory.allocation);
            if(owner->free(&memory)!=S_OK || owner->last_free_report().stage!=FreeStage::Done ||
               owner->owns_allocation(memory.allocation))++failures;
        }
    });
    for(auto& thread:threads)thread.join();
    allocate_meeting.disarm();
    assert(!failures.load() && allocate_meeting.met.load());
}
// 5. One internal context named by two threads at once, which RADV's queue lock never lets happen
// (lend() in hosted-dispatch.cpp): both submissions are made, not refused, and the context closes after.
void shared_context(Device& device){
    D3DKMT_CREATECONTEXTVIRTUAL context{};bc250_host_queue_context wrapper{nullptr,&context};
    {DeviceEngineScope scope(device);
     assert(scope.entered() && call(0,BC250_HOST_CREATE_QUEUE_CONTEXT,&wrapper)==0 && context.hContext);}
    submit_meeting.arm(2);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<2;++t)threads.emplace_back([&]{
        DeviceEngineScope scope(device);
        D3DKMT_SUBMITCOMMAND command{};command.BroadcastContextCount=1;command.BroadcastContext[0]=context.hContext;
        if(!scope.entered() || call(0,BC250_HOST_SubmitCommand,&command)!=0)++failures;
    });
    for(auto& thread:threads)thread.join();
    submit_meeting.disarm();
    assert(!failures.load() && submit_meeting.met.load());
    D3DKMT_DESTROYCONTEXT release{};release.hContext=context.hContext;wrapper.arguments=&release;
    {DeviceEngineScope scope(device);assert(scope.entered() && call(0,BC250_HOST_DESTROY_QUEUE_CONTEXT,&wrapper)==0);}
}
// 6. The recording binding (lever L2): published only when asked and only for an untraced device. A RecordingScope
// grants what DeviceEngineScope grants (callbacks accepted, device_engine_entered) on every thread at once, and
// takes it back; inside it, admit() refuses (a nested entry goes the full way), the full scope admits the same
// device and refuses another; inside another device's scope admit() refuses; cleared, the device has no binding.
constexpr unsigned kRecordingRounds=50;
void recording(Device& device,Device& other){
    assert(!device.recording && !other.recording);
    device.trace_mode=2;
    assert(!host_test_bind_recording(device,true) && !device.recording);
    device.trace_mode=0;
    assert(host_test_bind_recording(device,true) && device.recording && RecordingScope::admit(device.recording));
    bc250_host_paging paging{};
    {RecordingScope scope(*RecordingScope::admit(device.recording));
     assert(scope.entered() && call(0,BC250_HOST_CREATE_PAGING,&paging)==0 && paging.queue==9);}
    assert(!device_engine_entered(device));
    scope_meeting.arm(kThreads);
    std::vector<std::thread> threads;
    for(unsigned t=0;t<kThreads;++t)threads.emplace_back([&]{
        if(device_engine_entered(device)){++failures;return;}
        const RecordingBinding* binding=RecordingScope::admit(device.recording);
        if(!binding){++failures;return;}
        RecordingScope scope(*binding);
        if(!scope.entered() || !device_engine_entered(device) || device_engine_entered(other)){++failures;return;}
        if(RecordingScope::admit(device.recording))++failures;
        {DeviceEngineScope nested(device);if(!nested.entered())++failures;}
        {DeviceEngineScope foreign(other);if(foreign.entered())++failures;}
        if(!device_engine_entered(device))++failures;
        for(unsigned round=0;round<kRecordingRounds;++round)if(!round_trip(paging)){++failures;return;}
        scope_meeting.pass();
    });
    for(auto& thread:threads)thread.join();
    scope_meeting.disarm();
    assert(!failures.load() && scope_meeting.met.load() && !device_engine_entered(device));
    std::thread foreign([&]{
        DeviceEngineScope scope(other);
        if(!scope.entered() || RecordingScope::admit(device.recording))++failures;
    });
    foreign.join();
    std::thread after([]{if(call(0,BC250_HOST_CHECK_STATUS,nullptr)>=0)++failures;});
    after.join();
    {RecordingScope scope(*RecordingScope::admit(device.recording));
     assert(scope.entered() && call(0,BC250_HOST_DESTROY_PAGING,&paging)==0);}
    assert(!failures.load() && !host_test_bind_recording(device,false) && !device.recording);
}
}
int main(){
    Adapter adapter{};adapter.contract.luid=UINT64_C(0x0000000100000042);
    Device device{},other{};
    for(Device* d:{&device,&other}){
        d->adapter=&adapter;d->runtime={handle<void*>(1)};
        d->callbacks.pfnAllocateCb=allocate_cb;d->callbacks.pfnDeallocateCb=deallocate_cb;d->callbacks.pfnSetErrorCb=set_error;
        auto& k=d->kernel_callbacks;
        k.pfnCreatePagingQueueCb=paging_create;k.pfnDestroyPagingQueueCb=paging_destroy;k.pfnMapGpuVirtualAddressCb=map_va;
        k.pfnReserveGpuVirtualAddressCb=reserve_va;k.pfnFreeGpuVirtualAddressCb=free_va;k.pfnMakeResidentCb=make_resident;
        k.pfnEvictCb=evict;k.pfnLock2Cb=lock2;k.pfnUnlock2Cb=unlock2;
        k.pfnCreateSynchronizationObject2Cb=create_sync;k.pfnDestroySynchronizationObjectCb=destroy_sync;
        k.pfnCreateContextVirtualCb=create_context;k.pfnDestroyContextCb=destroy_context;k.pfnSubmitCommandCb=submit;
        k.pfnWaitForSynchronizationObjectFromCpuCb=wait_cpu;
    }
    assert(host_test_open_device_engine(device,icd_gipa,handle<VkPhysicalDevice>(3),handle<VkDevice>(4))==S_OK);
    assert(host_test_open_device_engine(other,icd_gipa,handle<VkPhysicalDevice>(3),handle<VkDevice>(4))==S_OK);
    assert(instances.load()==2 && live_instances.load()==2 && hosts[0].userdata!=hosts[1].userdata);
    scopes(device,other);
    callbacks(device);
    refusals(other);
    imports(device);
    shared_context(device);
    recording(device,other);
    assert(!errors.load() && !device.lost.load() && !other.lost.load());
    // Everything created was released: the owners close without an unresolved object.
    assert(host_test_close_device_engine(other) && host_test_close_device_engine(device));
    assert(!live_instances.load() && !device.engine && !other.engine && !device.recording);
    assert(allocations.load()==kThreads*kRounds+kImportThreads*kImportRounds+kThreads*kRecordingRounds &&
           deallocations.load()==allocations.load());
    assert(paging_creates.load()==3 && paging_destroys.load()==3);
    std::printf("PASS entry concurrency: %u threads in scopes of one device at once, %u hosted callback rounds "
        "with all threads inside one runtime callback together, outside and foreign scopes refused, %u heap "
        "imports under owner scopes on %u threads at once, one internal context submitted on by two threads at "
        "once and closed, %u callback rounds under recording scopes on %u threads at once (L2 binding: "
        "untraced only, nested and foreign refused, cleared at close)\n",kThreads,kThreads*kRounds,
        kImportThreads*kImportRounds,kImportThreads,kThreads*kRecordingRounds,kThreads);
}
