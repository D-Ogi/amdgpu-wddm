// SPDX-License-Identifier: MIT
#include "runtime-bridge.h"
#include "present-contract.h"
#include <cstdlib>
#include <string>
using namespace bc250::umd;
namespace {
void check(bool value) { if (!value) std::abort(); }
int identity, context, render, dxgi;
UINT64 cpu=0;
std::string trace;
HRESULT flush_result, wait_result, present_result, signal_result;
BOOL seen_override_valid=FALSE; DXGI_DDI_FLIP_INTERVAL_TYPE seen_override=DXGI_DDI_FLIP_INTERVAL_IMMEDIATE;
HRESULT flush(void *p) {
    trace+='F';
    auto *b=static_cast<HostBridge *>(p);
    b->progress[0]={1,61,19,&cpu};
    return flush_result;
}
HRESULT APIENTRY create(HANDLE h, D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *p) {
    check(h==&identity); trace+='C'; p->hSyncObject=62;
    p->Info.MonitoredFence.FenceValueCPUVirtualAddress=&cpu; return S_OK;
}
HRESULT APIENTRY wait(HANDLE h, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *p) {
    check(h==&identity && p->hContext==&context && p->ObjectCount==1);
    check(*p->ObjectHandleArray==61 && *p->MonitoredFenceValueArray==19);
    trace+='W'; return wait_result;
}
HRESULT APIENTRY present(HANDLE h, DXGIDDICB_PRESENT *p) {
    check(h==&identity && p->hContext==&context && p->hSrcAllocation==71 && p->hDstAllocation==72);
    check(p->pDXGIContext==&dxgi && p->BroadcastContextCount==0);
    seen_override_valid=p->SyncIntervalOverrideValid; seen_override=p->SyncIntervalOverride;
    trace+='P'; return present_result;
}
HRESULT APIENTRY alternate_present(HANDLE h,DXGIDDICB_PRESENT *p) {
    trace+='A'; return present(h,p);
}
HRESULT APIENTRY signal(HANDLE h, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *p) {
    check(h==&identity && p->ObjectCount==1 && *p->ObjectHandleArray==62);
    check(*p->MonitoredFenceValueArray==1); trace+='S'; return signal_result;
}
// BD-170: what the Present entry returns. S_OK or device removal only: a refused callback is S_OK (the runtime
// maps the kernel status), a success code is S_OK, and a failure of the shell's own steps is device removal.
void test_present_ddi_status() {
    check(present_ddi_status(S_OK,S_OK)==S_OK);
    check(present_ddi_status(S_FALSE,S_FALSE)==S_OK);
    check(present_ddi_status(DXGI_STATUS_OCCLUDED,DXGI_STATUS_OCCLUDED)==S_OK);
    check(present_ddi_status(E_INVALIDARG,E_INVALIDARG)==S_OK && present_callback_refused(E_INVALIDARG,E_INVALIDARG));
    check(present_ddi_status(E_FAIL,E_FAIL)==S_OK);
    check(present_ddi_status(E_FAIL,S_OK)==D3DDDIERR_DEVICEREMOVED && !present_callback_refused(E_FAIL,S_OK));
    check(present_ddi_status(E_INVALIDARG,S_OK)==D3DDDIERR_DEVICEREMOVED);
    check(present_ddi_status(E_OUTOFMEMORY,S_OK)==D3DDDIERR_DEVICEREMOVED);
    check(present_ddi_status(E_FAIL,E_INVALIDARG)==D3DDDIERR_DEVICEREMOVED);
    check(present_ddi_status(D3DDDIERR_DEVICEREMOVED,DXGI_ERROR_DEVICE_REMOVED)==D3DDDIERR_DEVICEREMOVED);
    check(present_ddi_status(DXGI_ERROR_DEVICE_HUNG,S_OK)==D3DDDIERR_DEVICEREMOVED);
    check(!present_callback_refused(D3DDDIERR_DEVICEREMOVED,DXGI_ERROR_DEVICE_REMOVED));
    check(!present_callback_refused(S_FALSE,S_FALSE));
}
}
void test_runtime_present() {
    test_present_ddi_status();
    // entry: what the Present entry returns for the case (present_ddi_status of the result and the callback status).
    struct Case { HRESULT f,w,p,s,result; const char *order; bool lost; HRESULT entry; };
    const Case cases[] = {
        {S_OK,S_OK,S_OK,S_OK,S_OK,"FCWPS",false,S_OK},
        {S_OK,S_OK,S_FALSE,S_OK,S_FALSE,"FCWPS",false,S_OK},
        {E_FAIL,S_OK,S_OK,S_OK,E_FAIL,"F",false,D3DDDIERR_DEVICEREMOVED},
        {S_OK,E_FAIL,S_OK,S_OK,E_FAIL,"FCW",false,D3DDDIERR_DEVICEREMOVED},
        {S_OK,S_OK,E_FAIL,S_OK,E_FAIL,"FCWP",false,S_OK},
        {S_OK,S_OK,E_INVALIDARG,S_OK,E_INVALIDARG,"FCWP",false,S_OK},
        {S_OK,S_OK,S_OK,E_FAIL,E_FAIL,"FCWPS",false,D3DDDIERR_DEVICEREMOVED},
        {S_OK,S_OK,DXGI_ERROR_DEVICE_REMOVED,S_OK,D3DDDIERR_DEVICEREMOVED,"FCWP",true,D3DDDIERR_DEVICEREMOVED},
    };
    for (const auto &c:cases) {
        RuntimeDevice d; d.hDevice=&identity; d.present_context=&context;
        DXGI_DDI_BASE_CALLBACKS callbacks{}; callbacks.pfnPresentCb=present; d.DXGICallbacks=&callbacks;
        d.KTCallbacks.pfnCreateSynchronizationObject2Cb=create;
        d.KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb=wait;
        d.KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb=signal;
        HostBridge b{}; b.device=&d; b.contexts[0]=&render;
        trace.clear(); flush_result=c.f; wait_result=c.w; present_result=c.p; signal_result=c.s;
        check(present_runtime(b,71,72,&dxgi,flush,&b)==E_INVALIDARG && trace.empty());
        RuntimeDomain::Scope scope(d.domain);
        HRESULT callback=E_PENDING; char step='?';
        const HRESULT result=present_runtime(b,71,72,&dxgi,flush,&b,PresentSyncOverride{},&callback,&step);
        check(result==c.result && trace==c.order);
        // The step that ended the present: the last letter of the trace where a step failed, none where all passed.
        const char last=c.order[std::string(c.order).size()-1];
        check(step==(FAILED(c.result) ? (last=='C' ? 'W' : last) : 0));
        // The callback status: the callback's own where the present reached it, S_OK where it did not.
        check(callback==(std::string(c.order).find('P')!=std::string::npos ? c.p : S_OK));
        check(present_ddi_status(result,callback)==c.entry);
        check(b.device_lost==c.lost);
        check(b.present_value==((SUCCEEDED(c.f)&&SUCCEEDED(c.w)&&SUCCEEDED(c.p)&&SUCCEEDED(c.s)) ? 1u : 0u));
        trace.clear();
        callbacks.pfnPresentCb=nullptr;
        check(present_runtime(b,71,72,&dxgi,flush,&b)==E_NOTIMPL && trace.empty());
    }
    RuntimeDevice d; d.hDevice=&identity; d.present_context=&context;
    DXGI_DDI_BASE_CALLBACKS callbacks{}; callbacks.pfnPresentCb=present; d.DXGICallbacks=&callbacks;
    d.KTCallbacks.pfnCreateSynchronizationObject2Cb=create;
    d.KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb=wait;
    d.KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb=signal;
    flush_result=wait_result=present_result=signal_result=S_OK;
    for (int phase=0;phase<3;++phase) {
        // Model runtime mutation while no thread is inside the UMD.
        callbacks.pfnPresentCb=phase==0 ? present : phase==1 ? alternate_present : nullptr;
        HostBridge b{}; b.device=&d; b.contexts[0]=&render; trace.clear();
        RuntimeDomain::Scope scope(d.domain);
        HRESULT hr=present_runtime(b,71,72,&dxgi,flush,&b);
        check(hr==(phase==2 ? E_NOTIMPL : S_OK));
        check(trace==(phase==0 ? "FCWPS" : phase==1 ? "FCWAPS" : ""));
    }
    d.DXGICallbacks=nullptr;
    HostBridge absent{}; absent.device=&d;
    RuntimeDomain::Scope scope(d.domain);
    check(present_runtime(absent,71,72,&dxgi,flush,&absent)==E_NOTIMPL);
    // The per-application VSync setting: no override unless asked for, then the asked interval.
    d.DXGICallbacks=&callbacks; callbacks.pfnPresentCb=present;
    const PresentSyncOverride overrides[]={{},{true,DXGI_DDI_FLIP_INTERVAL_IMMEDIATE},{true,DXGI_DDI_FLIP_INTERVAL_ONE}};
    for (const auto &o:overrides) {
        HostBridge b{}; b.device=&d; b.contexts[0]=&render; trace.clear();
        seen_override_valid=2; seen_override=DXGI_DDI_FLIP_INTERVAL_FOUR;
        check(present_runtime(b,71,72,&dxgi,flush,&b,o)==S_OK && trace=="FCWPS");
        check(seen_override_valid==(o.valid ? TRUE : FALSE));
        check(seen_override==(o.valid ? o.interval : DXGI_DDI_FLIP_INTERVAL_IMMEDIATE));
    }

}
