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
    device.KTCallbacks.pfnMakeResidentCb=resident;
    device.KTCallbacks.pfnUpdateGpuVirtualAddressCb=update_va;
    HostBridge bridge{}; bridge.device=&device;
    require(host_dispatch(&bridge, BC250_HOST_CHECK_STATUS, nullptr)<0);
    const auto host=host_descriptor(bridge, 123);
    require(host.version==5 && host.size==sizeof(bc250_host) && host.identity==&identity && host.adapter_luid==123);
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
    std::cout << "PASS runtime bridge: allocation outputs, context translation, submission, sparse update, worker denial, host-lost class, teardown\n";
}


