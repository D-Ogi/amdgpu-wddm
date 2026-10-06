// SPDX-License-Identifier: MIT
#include "runtime-bridge.h"
#include <cstdlib>
#include <string>
#include <thread>
namespace {
using namespace bc250::umd;
void check(bool ok) { if (!ok) std::abort(); }
int id, present, render1, render2;
UINT64 cpu_fence=0, progress1=0, progress2=0;
std::string order;
unsigned cpu_waits=0, signals=0;
bool fail_signal=false;
HRESULT APIENTRY create(HANDLE h, D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *a) {
    check(h==&id && a->Info.Type==D3DDDI_MONITORED_FENCE);
    a->hSyncObject=80; a->Info.MonitoredFence.FenceValueCPUVirtualAddress=&cpu_fence;
    order+='C'; return S_OK;
}
HRESULT APIENTRY wait(HANDLE h, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *a) {
    check(h==&id && a->hContext==&present && a->ObjectCount==2);
    check(a->ObjectHandleArray[0]==11 && a->ObjectHandleArray[1]==12);
    check(a->MonitoredFenceValueArray[0]==7 && a->MonitoredFenceValueArray[1]==9);
    order+='W'; return S_OK;
}
HRESULT APIENTRY signal(HANDLE h, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *a) {
    check(h==&id && a->ObjectCount==1 && *a->ObjectHandleArray==80);
    check(a->BroadcastContextCount==1 && *a->BroadcastContextArray==&present);
    check(*a->MonitoredFenceValueArray==signals+1);
    ++signals; order+='S'; return fail_signal ? E_FAIL : S_OK;
}
HRESULT APIENTRY idle(HANDLE h, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *a) {
    check(h==&id && a->ObjectCount==1 && *a->ObjectHandleArray==80 && *a->FenceValueArray==1);
    ++cpu_waits; check(SetEvent(a->hAsyncEvent)!=FALSE); return S_OK;
}
}
void test_present_bridge() {
    RuntimeDevice device;
    device.hDevice=&id; device.present_context=&present;
    device.KTCallbacks.pfnCreateSynchronizationObject2Cb=create;
    device.KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb=wait;
    device.KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb=signal;
    device.KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb=idle;
    HostBridge b{}; b.device=&device;
    b.contexts[0]=&render1; b.contexts[1]=&render2;
    b.progress[0]={1,11,7,&progress1}; b.progress[1]={2,12,9,&progress2};
    check(queue_present_wait(b)==E_INVALIDARG);
    {
        RuntimeDomain::Scope entry(device.domain);
        check(queue_present_wait(b)==S_OK && order=="CW" && cpu_waits==0);
        check(signal_present(b)==S_OK && b.present_value==1 && order=="CWS" && cpu_waits==0);
        check(queue_present_wait(b)==S_OK && order=="CWSW"); // Fence reused, no second create.
        check(wait_present_idle(b)==S_OK && cpu_waits==1);
        // BD-065: one value. Zero needs no wait; a value never signalled is refused; a value the CPU-visible
        // fence already reached returns without a runtime wait.
        check(wait_present_value(b,0)==S_OK && wait_present_value(b,2)==E_INVALIDARG && cpu_waits==1);
        cpu_fence=1; check(wait_present_value(b,1)==S_OK && cpu_waits==1); cpu_fence=0;
        check(wait_present_value(b,1)==S_OK && cpu_waits==2);
        std::thread worker([&] {
            check(signal_present(b)==E_INVALIDARG);
            check(wait_present_idle(b)==E_INVALIDARG);
            check(wait_present_value(b,1)==E_INVALIDARG);
        }); worker.join();
        b.present_value=UINT64_MAX-1;
        check(FAILED(signal_present(b)) && signals==1); // Never publish loss sentinel.
        b.present_value=1; fail_signal=true;
        check(FAILED(signal_present(b)) && b.present_value==1 && b.submission_failed);
        check(FAILED(queue_present_wait(b)));
    }
    check(signals==2 && cpu_waits==2);
}
