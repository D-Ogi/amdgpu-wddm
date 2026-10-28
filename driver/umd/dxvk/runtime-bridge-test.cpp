// SPDX-License-Identifier: MIT
#include "runtime-bridge.h"
#include "ddi-error-policy.h"
#include <cstdlib>
#include <iostream>
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
void test_present_bridge();
void test_runtime_present();
int main() {
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
    // the way. Non-zero flags stay refused, so the ICD can never ask for the immediate destroy.
    D3DDDI_MAPGPUVIRTUALADDRESS mapping{};
    mapping.hAllocation=info.hAllocation; mapping.SizeInPages=1;
    require(host_dispatch(&bridge, BC250_HOST_MapGpuVirtualAddress, &mapping)==0x103);
    require(mapping.VirtualAddress==UINT64_C(0x600000000));
    D3DKMT_DESTROYALLOCATION2 release{};
    release.AllocationCount=1; release.phAllocationList=&info.hAllocation;
    release.Flags.AssumeNotInUse=1;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.Flags.Value=0; release.hResource=91;
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==static_cast<int32_t>(0xc00000bbu));
    release.hResource=0;
    require(deallocated==0);
    require(host_dispatch(&bridge, BC250_HOST_DestroyAllocation2, &release)==0);
    require(deallocated==1 && !freed_ranges && !evicted);
    command.BroadcastContext[0]=2;
    require(host_dispatch(&bridge, BC250_HOST_SubmitCommand, &command)<0 && submitted==3);
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
    std::cout << "PASS runtime bridge: allocation outputs, context translation, submission, sparse update, mapped destroy to Deallocate, worker denial, host-lost class, teardown\n";
}


