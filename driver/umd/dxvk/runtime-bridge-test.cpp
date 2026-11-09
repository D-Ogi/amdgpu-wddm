// SPDX-License-Identifier: MIT
#include "runtime-bridge.h"
#include "ddi-error-policy.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
using namespace bc250::umd;
static void require(bool ok) { if (!ok) std::abort(); }
static int identity, context;
static unsigned hostReports; static HRESULT hostReported;
static void APIENTRY count_host_error(D3D10DDI_HRTCORELAYER, HRESULT hr) { ++hostReports; hostReported=hr; }
static unsigned allocated, created, destroyed, submitted, waited, updated;
static unsigned gpu_signals, gpu_rewinds, cpu_signals;
static HRESULT APIENTRY signal_gpu(HANDLE device, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *a) {
    require(device==&identity && a->BroadcastContextCount==1 && a->BroadcastContextArray[0]==&context);
    require(a->ObjectCount==2 && a->ObjectHandleArray[0]==45 && a->ObjectHandleArray[1]==46);
    require(a->MonitoredFenceValueArray[0]==9 && a->MonitoredFenceValueArray[1]==10);
    ++gpu_signals; gpu_rewinds+=a->Flags.AllowFenceRewind; return S_OK;
}
static HRESULT APIENTRY signal_cpu(HANDLE device, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *a) {
    require(device==&identity && a->ObjectCount==2 && a->ObjectHandleArray[0]==45 && a->ObjectHandleArray[1]==46);
    require(a->FenceValueArray[0]==9 && a->FenceValueArray[1]==10);
    ++cpu_signals; return S_OK;
}
static HRESULT APIENTRY allocate(HANDLE device, D3DDDICB_ALLOCATE *a) {
    require(device==&identity && a->NumAllocations==1 && a->PrivateDriverDataSize==4);
    ++allocated; a->hKMResource=91; a->pAllocationInfo2[0].hAllocation=37; return S_OK;
}
static HRESULT APIENTRY create_context(HANDLE device, D3DDDICB_CREATECONTEXTVIRTUAL *a) {
    require(device==&identity && a->NodeOrdinal==2 && a->EngineAffinity==1);
    ++created; a->hContext=&context; return S_OK;
}
static HRESULT APIENTRY destroy_context(HANDLE device, const D3DDDICB_DESTROYCONTEXT *a) {
    require(device==&identity && a->hContext==&context); ++destroyed; return S_OK;
}
static HRESULT APIENTRY submit(HANDLE device, const D3DDDICB_SUBMITCOMMAND *a) {
    require(device==&identity && a->BroadcastContextCount==1 && a->BroadcastContext[0]==&context);
    require(a->Commands==4096 && a->CommandLength==64); ++submitted; return S_OK;
}
static HRESULT APIENTRY wait_gpu(HANDLE device, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *a) {
    require(device==&identity && a->hContext==&context && a->ObjectCount==1);
    require(*a->ObjectHandleArray==45 && *a->MonitoredFenceValueArray==9);
    ++waited; return S_OK;
}
static HRESULT APIENTRY update_va(HANDLE device, const D3DDDICB_UPDATEGPUVIRTUALADDRESS *a) {
    require(device==&identity && a->hContext==&context && a->hFenceObject==52 && a->NumOperations==1);
    require(a->Operations && a->FenceValue==7 && !a->Reserved0 && !a->Reserved1 && !a->Flags.Value);
    ++updated; return S_OK;
}
static HRESULT APIENTRY resident(HANDLE device, D3DDDI_MAKERESIDENT *) {
    require(device==&identity); return E_PENDING;
}
// BD-201: the ICD destroys an ordinary allocation with its mapping live and lets the API-allocated range go
// with the deferred destruction. This bridge keeps no address bookkeeping of its own, so the only question is
// whether the forwarding still reaches Deallocate and releases nothing else on the way.
static unsigned deallocated, freed_ranges, evicted;
static HRESULT APIENTRY deallocate(HANDLE device, CONST D3DDDICB_DEALLOCATE *a) {
    require(device==&identity && !a->hResource && a->NumAllocations==1 && a->HandleList && a->HandleList[0]==37);
    ++deallocated; return S_OK;
}
// BD-200: the AssumeNotInUse destroy, with that flag alone.
static unsigned deallocated2;
static HRESULT APIENTRY deallocate2(HANDLE device, CONST D3DDDICB_DEALLOCATE2 *a) {
    require(device==&identity && !a->hResource && a->NumAllocations==1 && a->HandleList && a->HandleList[0]==37);
    require(a->Flags.Value==1 && a->Flags.AssumeNotInUse);
    ++deallocated2; return S_OK;
}
static HRESULT APIENTRY map_range(HANDLE device, D3DDDI_MAPGPUVIRTUALADDRESS *a) {
    require(device==&identity && a->hAllocation==37);
    a->VirtualAddress=UINT64_C(0x600000000); a->PagingFenceValue=3; return E_PENDING;
}
static HRESULT APIENTRY free_range(HANDLE device, CONST D3DDDICB_FREEGPUVIRTUALADDRESS *) {
    require(device==&identity); ++freed_ranges; return S_OK;
}
static HRESULT APIENTRY evict_range(HANDLE device, D3DDDICB_EVICT *) {
    require(device==&identity); ++evicted; return S_OK;
}
// BD-198: monitored fences of the ICD. The second fence of each pair reads the lost value already at its creation,
// as a fence whose storage another writer changed would.
static volatile UINT64 fence_storage[4];
static unsigned sync_created, sync_destroyed;
static HRESULT APIENTRY create_sync(HANDLE device, D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *a) {
    require(device==&identity && a->Info.Type==D3DDDI_MONITORED_FENCE && sync_created<4);
    const unsigned i=sync_created++;
    fence_storage[i]=(i&1) ? UINT64_MAX : a->Info.MonitoredFence.InitialFenceValue;
    a->hSyncObject=70+i;
    a->Info.MonitoredFence.FenceValueCPUVirtualAddress=const_cast<UINT64 *>(&fence_storage[i]);
    a->Info.MonitoredFence.FenceValueGPUVirtualAddress=0x10000+8*i;
    return S_OK;
}
static HRESULT APIENTRY destroy_sync(HANDLE device, CONST D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *a) {
    require(device==&identity && a->hSyncObject>=70 && a->hSyncObject<74); ++sync_destroyed; return S_OK;
}
void test_present_bridge();
void test_runtime_present();
// BD-179: the adapter check of the host status. A scripted adapter list: the LUIDs it has, whether the enumeration
// fails, and the handles it opened and the test closed.
static UINT64 adapter_list[2]; static unsigned adapter_count; static bool adapter_enum_fails;
static unsigned adapter_enums, adapter_opened, adapter_closed;
// The entry takes a pointer to const and writes NumAdapters all the same (d3dkmthk.h, 10.0.26100).
static NTSTATUS APIENTRY enum_adapters(const D3DKMT_ENUMADAPTERS2 *in) {
    auto *e=const_cast<D3DKMT_ENUMADAPTERS2 *>(in);
    ++adapter_enums;
    if (adapter_enum_fails) return static_cast<NTSTATUS>(0xc0000001u);
    require(e->pAdapters && e->NumAdapters>=adapter_count);
    for (unsigned i=0;i<adapter_count;++i) {
        e->pAdapters[i]={}; e->pAdapters[i].hAdapter=0x100+i; ++adapter_opened;
        std::memcpy(&e->pAdapters[i].AdapterLuid,&adapter_list[i],sizeof(UINT64));
    }
    e->NumAdapters=adapter_count; return 0;
}
static NTSTATUS APIENTRY close_adapter(const D3DKMT_CLOSEADAPTER *c) {
    require(c->hAdapter>=0x100 && c->hAdapter<0x100+adapter_count); ++adapter_closed; return 0;
}
static void test_adapter_presence() {
    const AdapterPresenceKmt kmt{enum_adapters,close_adapter};
    const UINT64 luid=0x8582, restarted=0x3a8fa1;
    const auto status=[](HostBridge &b) { return host_dispatch(&b,BC250_HOST_CHECK_STATUS,nullptr); };
    const auto due=[](HostBridge &b) { b.adapter_next_check_ms=0; };
    {
        // The case of the BD-179 dump: the check sees the adapter, a restart replaces it by one with a new LUID,
        // and the next check latches the loss. The loss is the proof that the device's work ended.
        RuntimeDevice d; d.hDevice=&identity; HostBridge b{}; b.device=&d; b.adapter_kmt=&kmt;
        (void)host_descriptor(b,luid);
        require(b.adapter_luid==luid);
        RuntimeDomain::Scope scope(d.domain);
        ddi_entry_class=DdiErrorClass::nothing;
        adapter_list[0]=luid; adapter_count=1; adapter_enum_fails=false; adapter_enums=adapter_opened=adapter_closed=0;
        require(status(b)==0 && b.adapter_state==kAdapterSeen && adapter_enums==1);
        // Within kAdapterCheckMs the list is not read again.
        adapter_list[0]=restarted;
        require(status(b)==0 && adapter_enums==1 && !b.device_lost);
        // A failed enumeration is no evidence.
        due(b); adapter_enum_fails=true;
        require(status(b)==0 && adapter_enums==2 && b.adapter_state==kAdapterSeen && !b.device_lost);
        due(b); adapter_enum_fails=false;
        require(!kernel_ended_device_work(b));
        require(status(b)==static_cast<int32_t>(0xc00002b6u) && adapter_enums==3 && b.device_lost && adapter_left(b));
        require(kernel_ended_device_work(b) && settle_device_work(b,0));
        // The state stays: the list is not read again, every status reports the loss.
        due(b);
        require(status(b)<0 && adapter_enums==3);
        require(adapter_opened==adapter_closed && adapter_opened==2); // the failed enumeration opened none
        const DeviceLossRecord loss=last_device_loss();
        require(!std::strcmp(loss.reason,"the adapter of this device left the system"));
    }
    {
        // A LUID that the list never had (a wrong LUID, a list that this process cannot read) never arms the check.
        RuntimeDevice d; d.hDevice=&identity; HostBridge b{}; b.device=&d; b.adapter_kmt=&kmt;
        (void)host_descriptor(b,luid);
        RuntimeDomain::Scope scope(d.domain);
        adapter_list[0]=restarted; adapter_count=1; adapter_enums=0;
        for (int i=0;i<3;++i) { due(b); require(status(b)==0); }
        require(adapter_enums==3 && b.adapter_state==kAdapterUnchecked && !b.device_lost && !kernel_ended_device_work(b));
    }
    {
        // An adapter that stays: no loss, ever; two adapters in the list, both handles closed.
        RuntimeDevice d; d.hDevice=&identity; HostBridge b{}; b.device=&d; b.adapter_kmt=&kmt;
        (void)host_descriptor(b,luid);
        RuntimeDomain::Scope scope(d.domain);
        adapter_list[0]=restarted; adapter_list[1]=luid; adapter_count=2; adapter_enums=adapter_opened=adapter_closed=0;
        for (int i=0;i<3;++i) { due(b); require(status(b)==0); }
        require(adapter_enums==3 && b.adapter_state==kAdapterSeen && !b.device_lost && adapter_opened==adapter_closed);
    }
    {
        // A bridge without a LUID (no hosted instance yet) makes no kernel call.
        RuntimeDevice d; d.hDevice=&identity; HostBridge b{}; b.device=&d; b.adapter_kmt=&kmt;
        RuntimeDomain::Scope scope(d.domain);
        adapter_enums=0;
        require(status(b)==0 && adapter_enums==0);
    }
    // The probe alone: a complete list with the LUID, without it, a failed one, and no entries.
    adapter_list[0]=luid; adapter_count=1; adapter_enum_fails=false;
    require(probe_adapter_presence(luid,kmt)==AdapterPresence::present);
    require(probe_adapter_presence(restarted,kmt)==AdapterPresence::gone);
    adapter_enum_fails=true;
    require(probe_adapter_presence(luid,kmt)==AdapterPresence::unknown);
    adapter_enum_fails=false;
    require(probe_adapter_presence(luid,AdapterPresenceKmt{})==AdapterPresence::unknown);
    require(probe_adapter_presence(0,kmt)==AdapterPresence::unknown);
}
// BD-327: the host calls of an engine pipeline worker (EngineWorkerGate). The worker is outside the RuntimeDomain;
// its device admits only driver-internal memory calls on it while the gate is open, and none after close revoked it.
static unsigned worker_allocated, worker_deallocated, worker_waits, worker_errors;
static HANDLE worker_inside, worker_release;
static HRESULT APIENTRY worker_allocate(HANDLE device, D3DDDICB_ALLOCATE *a) {
    require(device==&identity && a->NumAllocations==1 && !a->hResource);
    if (worker_inside) { SetEvent(worker_inside); WaitForSingleObject(worker_release,INFINITE); }
    ++worker_allocated; a->hKMResource=92; a->pAllocationInfo2[0].hAllocation=38; return S_OK;
}
static HRESULT APIENTRY worker_deallocate(HANDLE device, CONST D3DDDICB_DEALLOCATE *a) {
    require(device==&identity && a->NumAllocations==1 && a->HandleList[0]==38); ++worker_deallocated; return S_OK;
}
// BD-200: a destroy with AssumeNotInUse reaches pfnDeallocate2Cb with that flag alone.
static unsigned worker_deallocated2;
static HRESULT APIENTRY worker_deallocate2(HANDLE device, CONST D3DDDICB_DEALLOCATE2 *a) {
    require(device==&identity && !a->hResource && a->NumAllocations==1 && a->HandleList[0]==38 && a->Flags.Value==1);
    ++worker_deallocated2; return S_OK;
}
static HRESULT APIENTRY worker_map(HANDLE device, D3DDDI_MAPGPUVIRTUALADDRESS *a) {
    require(device==&identity); a->VirtualAddress=UINT64_C(0x80000000); a->PagingFenceValue=4; return E_PENDING;
}
static HRESULT APIENTRY worker_wait(HANDLE device, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *a) {
    require(device==&identity && a->ObjectCount==1); ++worker_waits; return S_OK;
}
static void APIENTRY worker_error(D3D10DDI_HRTCORELAYER, HRESULT) { ++worker_errors; }
static void test_worker_scope() {
    RuntimeDevice device; device.hDevice=&identity;
    device.KTCallbacks.pfnAllocateCb=worker_allocate;
    device.KTCallbacks.pfnDeallocateCb=worker_deallocate;
    device.KTCallbacks.pfnDeallocate2Cb=worker_deallocate2;
    device.KTCallbacks.pfnMapGpuVirtualAddressCb=worker_map;
    device.KTCallbacks.pfnMakeResidentCb=resident;
    device.KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb=worker_wait;
    device.KTCallbacks.pfnSubmitCommandCb=submit;
    device.UMCallbacks.pfnSetErrorCb=worker_error;
    HostBridge bridge{}; bridge.device=&device;
    RuntimeDevice other; HostBridge otherBridge{}; otherBridge.device=&other;
    D3DDDI_ALLOCATIONINFO2 info{};
    D3DKMT_CREATEALLOCATION internal{};
    auto reset=[&] {
        info={}; internal={}; internal.NumAllocations=1; internal.pAllocationInfo2=&info;
        internal.PrivateDriverDataSize=4; internal.Flags.CreateResource=1; internal.Flags.NonSecure=1;
    };
    auto on_thread=[](const EngineWorkerGate *gate, auto &&body) {
        std::thread t([&] { EngineWorkerGate::bind(gate); body(); EngineWorkerGate::bind(nullptr); }); t.join();
    };
    // A thread in no scope: refused, as before BD-327.
    reset();
    on_thread(nullptr,[&] { require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain); });
    require(worker_allocated==0 && bridge.worker_calls==0 && bridge.worker_refused==0);
    // A worker of this device: the internal allocation of hosted RADV (CreateResource and NonSecure set, as
    // radv_wddm2_bo.c sets them), its mapping, residency, paging wait and destruction reach the runtime.
    on_thread(&device.workers,[&] {
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==0);
        require(internal.hResource==92 && info.hAllocation==38);
        D3DDDI_MAPGPUVIRTUALADDRESS map{}; map.hAllocation=38;
        require(host_dispatch(&bridge,BC250_HOST_MapGpuVirtualAddress,&map)==0x103 && map.VirtualAddress==0x80000000);
        D3DKMT_HANDLE handle=38;
        D3DDDI_MAKERESIDENT residency{}; residency.NumAllocations=1; residency.AllocationList=&handle;
        require(host_dispatch(&bridge,BC250_HOST_MakeResident,&residency)==0x103);
        D3DKMT_HANDLE fence=60; UINT64 value=4;
        D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait{}; wait.ObjectCount=1; wait.ObjectHandleArray=&fence;
        wait.FenceValueArray=&value;
        require(host_dispatch(&bridge,BC250_HOST_WaitForSynchronizationObjectFromCpu,&wait)==0 && worker_waits==1);
        // An event wait or one of two objects is not the paging wait of an allocation.
        HANDLE event=reinterpret_cast<HANDLE>(1); wait.hAsyncEvent=event;
        require(host_dispatch(&bridge,BC250_HOST_WaitForSynchronizationObjectFromCpu,&wait)==RuntimeDomain::wrong_domain);
        wait.hAsyncEvent=nullptr; wait.ObjectCount=2;
        require(host_dispatch(&bridge,BC250_HOST_WaitForSynchronizationObjectFromCpu,&wait)==RuntimeDomain::wrong_domain);
        D3DKMT_DESTROYALLOCATION2 destroy{}; destroy.phAllocationList=&handle; destroy.AllocationCount=1;
        require(host_dispatch(&bridge,BC250_HOST_DestroyAllocation2,&destroy)==0 && worker_deallocated==1);
        // BD-200: AssumeNotInUse alone is admitted on a worker too and reaches pfnDeallocate2Cb; with another bit
        // next to it the call is refused before the runtime.
        destroy.Flags.AssumeNotInUse=1;
        require(host_dispatch(&bridge,BC250_HOST_DestroyAllocation2,&destroy)==0 && worker_deallocated2==1);
        destroy.Flags.SynchronousDestroy=1;
        require(host_dispatch(&bridge,BC250_HOST_DestroyAllocation2,&destroy)==RuntimeDomain::wrong_domain);
        require(worker_deallocated==1 && worker_deallocated2==1);
    });
    require(worker_allocated==1 && worker_waits==1 && bridge.worker_calls==6 && bridge.worker_refused==3);
    // Shared, runtime-resource, existing-memory and cross-adapter allocations stay refused on a worker.
    on_thread(&device.workers,[&] {
        reset(); internal.Flags.CreateShared=1;
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain);
        reset(); internal.Flags.NtSecuritySharing=1;
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain);
        reset(); internal.Flags.CrossAdapter=1;
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain);
        reset(); internal.Flags.ExistingSection=1;
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain);
        reset(); internal.hResource=91;
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain);
        static const char memory[4096]={};
        reset(); info.pSystemMem=memory;
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain);
        reset(); internal.NumAllocations=2;
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain);
        D3DKMT_HANDLE handle=38;
        D3DKMT_DESTROYALLOCATION2 destroy{}; destroy.phAllocationList=&handle; destroy.AllocationCount=1; destroy.hResource=92;
        require(host_dispatch(&bridge,BC250_HOST_DestroyAllocation2,&destroy)==RuntimeDomain::wrong_domain);
        // No submission, context or loss report from a worker.
        D3DKMT_SUBMITCOMMAND command{}; command.Commands=4096; command.CommandLength=64;
        require(host_dispatch(&bridge,BC250_HOST_SubmitCommand,&command)==RuntimeDomain::wrong_domain);
        D3DKMT_CREATECONTEXTVIRTUAL create{}; create.NodeOrdinal=2; create.EngineAffinity=1;
        require(host_dispatch(&bridge,BC250_HOST_CreateContextVirtual,&create)==RuntimeDomain::wrong_domain);
        require(host_dispatch(&bridge,BC250_HOST_REPORT_LOST,nullptr)==RuntimeDomain::wrong_domain);
        // The status check reads only: a latched loss is STATUS_DEVICE_REMOVED with no runtime error callback.
        require(host_dispatch(&bridge,BC250_HOST_CHECK_STATUS,nullptr)==0);
        bridge.device_lost=true;
        require(host_dispatch(&bridge,BC250_HOST_CHECK_STATUS,nullptr)==(int32_t)0xc00002b6);
        bridge.device_lost=false;
    });
    require(worker_allocated==1 && worker_deallocated==1 && submitted==0 && worker_errors==0 && !bridge.lost_reported);
    require(bridge.worker_calls==6 && bridge.worker_refused==14);
    // A worker of another device that reaches this bridge (a winsys kept for another identity, BD-253): refused.
    reset();
    on_thread(&other.workers,[&] { require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain); });
    require(worker_allocated==1 && bridge.worker_calls==6);
    (void)otherBridge;
    // revoke waits for the admitted call in flight, and after it the worker's calls are refused.
    worker_inside=CreateEventW(nullptr,TRUE,FALSE,nullptr); worker_release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    require(worker_inside && worker_release);
    reset();
    std::atomic_bool revoked{false};
    std::thread inFlight([&] {
        EngineWorkerGate::bind(&device.workers);
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==0);
        EngineWorkerGate::bind(nullptr);
    });
    require(WaitForSingleObject(worker_inside,10000)==WAIT_OBJECT_0);
    std::thread closer([&] { device.workers.revoke(); revoked=true; });
    Sleep(100);
    require(!revoked.load());
    SetEvent(worker_release);
    inFlight.join(); closer.join();
    require(revoked.load() && device.workers.revoked() && worker_allocated==2);
    CloseHandle(worker_inside); CloseHandle(worker_release); worker_inside=worker_release=nullptr;
    reset();
    on_thread(&device.workers,[&] { require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==RuntimeDomain::wrong_domain); });
    require(worker_allocated==2 && bridge.worker_calls==7 && bridge.worker_refused==15);
    // A new initialize of the owner (after a close that joined the workers) opens the gate again.
    device.workers.reopen();
    reset();
    on_thread(&device.workers,[&] { require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==0); });
    require(!device.workers.revoked() && worker_allocated==3 && bridge.worker_calls==8 && bridge.worker_refused==15);
    device.workers.revoke();
    // The DDI thread of the device is not a worker: its path is the RuntimeDomain's, unchanged by the gate.
    {
        RuntimeDomain::Scope scope(device.domain);
        reset();
        require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==0 && worker_allocated==4);
    }
    report_host_scope_calls(bridge);
    std::cout << "worker scope: PASS\n";
}
// BD-327: the lab flavour writes one line for each of the first 16 admitted worker calls of the process; the release
// flavour writes none. test_worker_scope admitted 8 calls before this test, so 20 more pass the budget of 16.
static std::string g_log_path;
static HRESULT APIENTRY note_allocate(HANDLE runtime, D3DDDICB_ALLOCATE *a) {
    require(runtime==&context && a->NumAllocations==1); a->hKMResource=93; a->pAllocationInfo2[0].hAllocation=39;
    return S_OK;
}
static void test_worker_admit_note() {
    RuntimeDevice device; device.hDevice=&context;
    device.KTCallbacks.pfnAllocateCb=note_allocate;
    HostBridge bridge{}; bridge.device=&device;
    std::thread worker([&] {
        EngineWorkerGate::bind(&device.workers);
        for (int i=0;i<20;++i) {
            D3DDDI_ALLOCATIONINFO2 info{};
            D3DKMT_CREATEALLOCATION internal{}; internal.NumAllocations=1; internal.pAllocationInfo2=&info;
            internal.PrivateDriverDataSize=4; internal.Flags.CreateResource=1; internal.Flags.NonSecure=1;
            require(host_dispatch(&bridge,BC250_HOST_CreateAllocation2,&internal)==0);
        }
        EngineWorkerGate::bind(nullptr);
    });
    worker.join();
    require(bridge.worker_calls==20);
    device.workers.revoke();
    char runtime[32]; std::snprintf(runtime,sizeof(runtime),"runtime=%p ",static_cast<void *>(&context));
    unsigned lines=0, ours=0;
    std::ifstream log(g_log_path);
    for (std::string line; std::getline(log,line);) {
        // The map and residency calls of test_worker_scope are admitted with E_PENDING (hr=8000000A).
        if (line.rfind("BC250 BD-327: worker host op ",0)!=0 || line.find(" admitted: hr=")==std::string::npos)
            continue;
        ++lines;
        if (line.find(runtime)!=std::string::npos) {
            ++ours;
            require(line.find("BC250 BD-327: worker host op 16 admitted: hr=00000000 ")==0);
        }
        require(line.find(" identity=")!=std::string::npos && line.find(" thread=")!=std::string::npos);
    }
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    // 8 lines of test_worker_scope's device, 8 of this one, then the budget is spent.
    require(lines==16 && ours==8);
#else
    require(lines==0 && ours==0);
#endif
    std::cout << "worker admit note: PASS (" << lines << " lines)\n";
}
int main() {
    // The log sink reads AMDGPU_WDDM_LOG once, at the first line of the process: set it before any test prints.
    char temp[MAX_PATH]; require(GetTempPathA(MAX_PATH,temp)!=0);
    g_log_path=std::string(temp)+"runtime-bridge-test-"+std::to_string(GetCurrentProcessId())+".log";
    DeleteFileA(g_log_path.c_str());
    require(SetEnvironmentVariableA("AMDGPU_WDDM_LOG",("file:"+g_log_path).c_str()));
    test_worker_scope();
    test_worker_admit_note();
    test_present_bridge();
    test_runtime_present();
    RuntimeDevice device;
    device.hDevice=&identity;
    device.KTCallbacks.pfnAllocateCb=allocate;
    device.KTCallbacks.pfnCreateContextVirtualCb=create_context;
    device.KTCallbacks.pfnDestroyContextCb=destroy_context;
    device.KTCallbacks.pfnSubmitCommandCb=submit;
    device.KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb=wait_gpu;
    device.KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb=signal_gpu;
    device.KTCallbacks.pfnSignalSynchronizationObjectFromCpuCb=signal_cpu;
    device.KTCallbacks.pfnMakeResidentCb=resident;
    device.KTCallbacks.pfnUpdateGpuVirtualAddressCb=update_va;
    device.KTCallbacks.pfnDeallocateCb=deallocate;
    device.KTCallbacks.pfnMapGpuVirtualAddressCb=map_range;
    device.KTCallbacks.pfnFreeGpuVirtualAddressCb=free_range;
    device.KTCallbacks.pfnEvictCb=evict_range;
    device.KTCallbacks.pfnCreateSynchronizationObject2Cb=create_sync;
    device.KTCallbacks.pfnDestroySynchronizationObjectCb=destroy_sync;
    HostBridge bridge{}; bridge.device=&device;
    require(host_dispatch(&bridge, BC250_HOST_CHECK_STATUS, nullptr)<0);
    const auto host=host_descriptor(bridge, 123);
    // BD-253: the ICD knows the device by the address of its RuntimeDevice, never by the runtime handle, which the
    // runtime can give to a new device while the old one's winsys is still alive. The switch gives the handle again.
    require(host.version==5 && host.size==sizeof(bc250_host) && host.identity==&device && host.identity!=&identity &&
        host.adapter_luid==123);
    device.legacy_host_identity=true;
    require(host_descriptor(bridge, 123).identity==&identity);
    device.legacy_host_identity=false;
    {
        // Two devices that the runtime gives the same handle (the second after the first one's destruction) have
        // two identities as long as both owners live.
        RuntimeDevice second; second.hDevice=&identity;
        HostBridge secondBridge{}; secondBridge.device=&second;
        require(host_descriptor(secondBridge, 123).identity!=host.identity);
    }
    RuntimeDomain::Scope scope(device.domain);
    require(host.dispatch(host.userdata, BC250_HOST_CHECK_STATUS, nullptr)==0);
    require(host_dispatch(&bridge, BC250_HOST_CHECK_STATUS, nullptr)==0);
    D3DDDI_ALLOCATIONINFO2 info{};
    D3DKMT_CREATEALLOCATION allocation{};
    allocation.NumAllocations=1; allocation.pAllocationInfo2=&info; allocation.PrivateDriverDataSize=4;
    require(host_dispatch(&bridge, BC250_HOST_CreateAllocation2, &allocation)==0);
    require(allocated==1 && allocation.hResource==91 && info.hAllocation==37);
    std::thread worker([&] { require(host_dispatch(&bridge, BC250_HOST_CreateAllocation2, &allocation)<0); });
    worker.join(); require(allocated==1);
    D3DKMT_CREATECONTEXTVIRTUAL create{}; create.NodeOrdinal=2; create.EngineAffinity=1;
    require(host_dispatch(&bridge, BC250_HOST_CreateContextVirtual, &create)==0);
    require(create.hContext==1 && created==1);
    D3DKMT_HANDLE signal_objects[]={45,46}; UINT64 signal_values[]={9,10};
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal{};
    signal.ObjectCount=2; signal.ObjectHandleArray=signal_objects;
    signal.BroadcastContextCount=1; signal.BroadcastContextArray=&create.hContext;
    signal.MonitoredFenceValueArray=signal_values;
    require(host_dispatch(&bridge,BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==0);
    signal.Flags.AllowFenceRewind=1;
    require(host_dispatch(&bridge,BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)==0);
    require(gpu_signals==2 && gpu_rewinds==1);
    signal.Flags.SignalAtSubmission=1;
    require(host_dispatch(&bridge,BC250_HOST_SignalSynchronizationObjectFromGpu2,&signal)<0 && gpu_signals==2);
    require(bridge.submission_failed); bridge.submission_failed=false;
    D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU cpu_signal{};
    cpu_signal.ObjectCount=2; cpu_signal.ObjectHandleArray=signal_objects; cpu_signal.FenceValueArray=signal_values;
    require(host_dispatch(&bridge,BC250_HOST_SignalSynchronizationObjectFromCpu,&cpu_signal)==0);
    cpu_signal.Flags.AllowFenceRewind=1;
    require(host_dispatch(&bridge,BC250_HOST_SignalSynchronizationObjectFromCpu,&cpu_signal)==0 && cpu_signals==2);
    cpu_signal.Flags.SignalAtSubmission=1;
    require(host_dispatch(&bridge,BC250_HOST_SignalSynchronizationObjectFromCpu,&cpu_signal)<0 && cpu_signals==2);
    D3DKMT_SUBMITCOMMAND command{}; command.Commands=4096; command.CommandLength=64;
    command.BroadcastContextCount=1; command.BroadcastContext[0]=create.hContext;
    require(host_dispatch(&bridge, BC250_HOST_SubmitCommand, &command)==0 && submitted==1);
    bridge.present_sync=45; bridge.present_value=9;
    require(host_dispatch(&bridge, BC250_HOST_SubmitCommand, &command)==0 && submitted==2 && waited==1);
    require(host_dispatch(&bridge, BC250_HOST_SubmitCommand, &command)==0 && submitted==3 && waited==1);
    D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION op{};
    op.OperationType=D3DDDI_UPDATEGPUVIRTUALADDRESS_UNMAP;
    D3DKMT_UPDATEGPUVIRTUALADDRESS sparse{}; sparse.hContext=create.hContext; sparse.hFenceObject=52;
    sparse.NumOperations=1; sparse.Operations=&op; sparse.FenceValue=7;
    require(host_dispatch(&bridge, BC250_HOST_UpdateGpuVirtualAddress, &sparse)==0 && updated==1);
    auto refused=sparse; refused.hContext=2;
    require(host_dispatch(&bridge, BC250_HOST_UpdateGpuVirtualAddress, &refused)<0);
    refused=sparse; refused.FenceValue=0;
    require(host_dispatch(&bridge, BC250_HOST_UpdateGpuVirtualAddress, &refused)<0);
    refused=sparse; refused.NumOperations=0;
    require(host_dispatch(&bridge, BC250_HOST_UpdateGpuVirtualAddress, &refused)<0);
    refused=sparse; refused.Flags.DoNotWait=1;
    require(host_dispatch(&bridge, BC250_HOST_UpdateGpuVirtualAddress, &refused)==static_cast<int32_t>(0xc00000bbu));
    require(updated==1 && bridge.submission_failed); bridge.submission_failed=false;
    D3DDDI_MAKERESIDENT residency{};
    require(host_dispatch(&bridge, BC250_HOST_MakeResident, &residency)==0x103);
    // BD-201: DestroyAllocation2 with a mapped range reaches the plain Deallocate callback, whose default
    // behaviour is VidMm's deferral (allocation-usage-tracking.md), and nothing else releases the range on
    // the way. BD-200: AssumeNotInUse alone reaches pfnDeallocate2Cb (below); every other flag stays refused.
    D3DDDI_MAPGPUVIRTUALADDRESS mapping{};
    mapping.hAllocation=info.hAllocation; mapping.SizeInPages=1;
    require(host_dispatch(&bridge, BC250_HOST_MapGpuVirtualAddress, &mapping)==0x103);
    require(mapping.VirtualAddress==UINT64_C(0x600000000));
    D3DKMT_DESTROYALLOCATION2 release{};
    release.AllocationCount=1; release.phAllocationList=&info.hAllocation;
    // A runtime without pfnDeallocate2Cb: the AssumeNotInUse destroy is not supported, and the ICD's retry with
    // flags 0 is the one that reaches the runtime (radv_wddm2_bo_destroy_now).
    release.Flags.AssumeNotInUse=1;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    device.KTCallbacks.pfnDeallocate2Cb=deallocate2;
    // Every other bit, alone or next to AssumeNotInUse, is refused before the runtime.
    release.Flags.SynchronousDestroy=1;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.Flags.AssumeNotInUse=0;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.Flags.Value=0; release.Flags.SystemUseOnly=1;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.Flags.Value=0; release.Flags.Reserved=1;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.Flags.Value=0; release.hResource=91;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.Flags.AssumeNotInUse=1;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.Flags.Value=0; release.hResource=0;
    require(deallocated==0 && deallocated2==0);
    // BD-200: AssumeNotInUse alone reaches pfnDeallocate2Cb with that flag alone, and nothing else on the way.
    release.Flags.AssumeNotInUse=1;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==0);
    require(deallocated==0 && deallocated2==1 && !freed_ranges && !evicted);
    // Flags 0 keep the plain Deallocate callback.
    release.Flags.Value=0;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==0);
    require(deallocated==1 && deallocated2==1 && !freed_ranges && !evicted);
    command.BroadcastContext[0]=2;
    require(host_dispatch(&bridge, BC250_HOST_SubmitCommand, &command)<0 && submitted==3);
    // BD-198: the GPU work count: the three submissions and the one sparse update that reached the runtime. The fence
    // signals and the calls that the bridge refused itself do not count.
    require(bridge.gpu_work_calls==4 && !device_queued_no_work(bridge));
    // BD-198: each monitored fence of the ICD gets a record with its value right after the creation; a destroy removes
    // the record. A fence that reads the lost value at its creation is recorded as such.
    {
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 fence{};
        fence.Info.Type=D3DDDI_MONITORED_FENCE; fence.Info.MonitoredFence.EngineAffinity=1;
        require(host_dispatch(&bridge, BC250_HOST_CreateSynchronizationObject2, &fence)==0 && fence.hSyncObject==70);
        auto stale=fence; stale.hSyncObject=0;
        stale.Info.MonitoredFence.FenceValueCPUVirtualAddress=nullptr; stale.Info.MonitoredFence.FenceValueGPUVirtualAddress=0;
        require(host_dispatch(&bridge, BC250_HOST_CreateSynchronizationObject2, &stale)==0 && stale.hSyncObject==71);
        unsigned records=0; const HostFence *first=nullptr, *second=nullptr;
        for (const auto &f:bridge.fences) {
            if (!f.handle) continue;
            ++records; if (f.handle==70) first=&f; if (f.handle==71) second=&f;
        }
        require(records==2 && first && second && first->at_create==0 && first->cpu==&fence_storage[0]);
        require(second->at_create==UINT64_MAX && second->initial==0 && !bridge.fences_unrecorded);
        D3DKMT_DESTROYSYNCHRONIZATIONOBJECT gone{}; gone.hSyncObject=71;
        require(host_dispatch(&bridge, BC250_HOST_DestroySynchronizationObject, &gone)==0 && sync_destroyed==1);
        records=0; for (const auto &f:bridge.fences) records+=f.handle!=0;
        require(records==1);
        gone.hSyncObject=70;
        require(host_dispatch(&bridge, BC250_HOST_DestroySynchronizationObject, &gone)==0 && sync_destroyed==2);
        records=0; for (const auto &f:bridge.fences) records+=f.handle!=0;
        require(records==0);
    }
    D3DKMT_DESTROYCONTEXT destroy{}; destroy.hContext=create.hContext;
    require(host_dispatch(&bridge, BC250_HOST_DestroyContext, &destroy)==0 && destroyed==1);
    require(host_dispatch(&bridge, BC250_HOST_DestroyContext, &destroy)<0 && destroyed==1);
    require(host_dispatch(&bridge, 63, &destroy)<0);
    require(host_dispatch(nullptr, BC250_HOST_CHECK_STATUS, nullptr)<0);
    volatile UINT64 lost=UINT64_MAX;
    device.pagingFence=&lost;
    require(host_dispatch(&bridge, BC250_HOST_CHECK_STATUS, nullptr)==static_cast<int32_t>(0xc00002b6u));
    lost=0;
    require(host_dispatch(&bridge, BC250_HOST_CHECK_STATUS, nullptr)==static_cast<int32_t>(0xc00002b6u));
    // The host reports a lost device from inside the engine, under whichever DDI entry is running, so
    // it reports through that entry's error class. A capability-check entry accepts no status at all,
    // and the report then waits for an entry whose page allows device removal: the loss is not lost
    // (BD-071 review).
    {
        RuntimeDevice losing; losing.hDevice=&identity;
        losing.UMCallbacks.pfnSetErrorCb=count_host_error;
        HostBridge reporting{}; reporting.device=&losing;
        volatile UINT64 gone=UINT64_MAX; losing.pagingFence=&gone;
        RuntimeDomain::Scope reportingScope(losing.domain);
        hostReports=0; hostReported=S_OK;
        ddi_entry_class=DdiErrorClass::nothing;
        require(host_dispatch(&reporting, BC250_HOST_CHECK_STATUS, nullptr)<0);
        require(hostReports==0 && reporting.device_lost && !reporting.lost_reported);
        ddi_entry_class=DdiErrorClass::check_invalid_arg;
        require(host_dispatch(&reporting, BC250_HOST_CHECK_STATUS, nullptr)<0);
        require(hostReports==1 && hostReported==E_INVALIDARG && reporting.lost_reported);
        ddi_entry_class=DdiErrorClass::removed_only;
        require(host_dispatch(&reporting, BC250_HOST_CHECK_STATUS, nullptr)<0);
        require(hostReports==1); // One report per loss, whatever follows.
    }
    // BD-258: the loss report of the ICD may name the read that saw the lost value. A report, a malformed one and none
    // all latch the loss; a value that is not the lost value reports nothing.
    for (int form=0;form<3;++form) {
        RuntimeDevice losing; losing.hDevice=&identity;
        HostBridge reporting{}; reporting.device=&losing;
        RuntimeDomain::Scope reportingScope(losing.domain);
        ddi_entry_class=DdiErrorClass::nothing;
        bc250_host_lost_report report{BC250_HOST_LOST_REPORT_STYPE,sizeof(report),"progress fence before submit",7,0,
            UINT64_MAX};
        if (form==1) report.sType=0;
        require(host_dispatch(&reporting, BC250_HOST_REPORT_LOST, form==2 ? nullptr : &report)<0);
        require(reporting.device_lost && reporting.submission_failed);
        // BD-258: the process keeps the last loss for the line of a CreateDevice refusal, with copies of its texts.
        const DeviceLossRecord loss=last_device_loss();
        require(loss.losses>=2 && loss.op==BC250_HOST_REPORT_LOST && loss.hr==S_OK && loss.at_ms);
        require(!std::strcmp(loss.reason,"the ICD reports a loss"));
        require(!std::strcmp(loss.site,form==0 ? "progress fence before submit" : "-"));
    }
    // BD-198: a loss at a recorded fence. The loss line reads the record of the site fence and every recorded fence of
    // the device; the record stays until the ICD destroys the fence.
    {
        RuntimeDevice losing; losing.hDevice=&identity;
        losing.KTCallbacks.pfnCreateSynchronizationObject2Cb=create_sync;
        HostBridge reporting{}; reporting.device=&losing;
        RuntimeDomain::Scope reportingScope(losing.domain);
        ddi_entry_class=DdiErrorClass::nothing;
        D3DKMT_CREATESYNCHRONIZATIONOBJECT2 fence{};
        fence.Info.Type=D3DDDI_MONITORED_FENCE; fence.Info.MonitoredFence.EngineAffinity=1;
        require(host_dispatch(&reporting, BC250_HOST_CreateSynchronizationObject2, &fence)==0 && fence.hSyncObject==72);
        fence_storage[2]=UINT64_MAX;
        bc250_host_lost_report report{BC250_HOST_LOST_REPORT_STYPE,sizeof(report),"progress fence before submit",72,0,
            UINT64_MAX};
        require(host_dispatch(&reporting, BC250_HOST_REPORT_LOST, &report)<0 && reporting.device_lost);
        require(reporting.fences[0].handle==72 && reporting.fences[0].at_create==0 && device_queued_no_work(reporting));
    }
    {
        RuntimeDevice losing; losing.hDevice=&identity;
        HostBridge reporting{}; reporting.device=&losing;
        RuntimeDomain::Scope reportingScope(losing.domain);
        const bc250_host host=host_descriptor(reporting,1);
        require(bc250_host_fence_valid(&host,5,"sync value",3) && !reporting.device_lost);
        require(!bc250_host_fence_valid(&host,UINT64_MAX,"sync value",3) && reporting.device_lost);
    }
    test_adapter_presence();
    std::cout << "PASS runtime bridge: allocation outputs, context translation, submission, sparse update, mapped destroy to Deallocate, AssumeNotInUse to Deallocate2, worker denial, host-lost class, adapter check, teardown\n";
}


