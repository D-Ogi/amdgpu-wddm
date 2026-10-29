// SPDX-License-Identifier: MIT
#include "device-owner.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
using namespace bc250::umd;
namespace {
void check(bool ok) { if (!ok) std::abort(); }
int identity, context; unsigned creates, destroys; bool fail_create=false,fail_destroy=false,lost_destroy=false;
unsigned sync_destroys, paging_destroys; HRESULT sync_status=S_OK, paging_status=S_OK;
HRESULT APIENTRY create(HANDLE h,D3DDDICB_CREATECONTEXTVIRTUAL *c) {
    check(h==&identity && c->EngineAffinity==1); ++creates;
    if (fail_create) return E_OUTOFMEMORY;
    c->hContext=&context; return S_OK;
}
HRESULT APIENTRY destroy(HANDLE h,const D3DDDICB_DESTROYCONTEXT *c) {
    check(h==&identity && c->hContext==&context); ++destroys;
    return fail_destroy ? E_FAIL : lost_destroy ? D3DDDIERR_DEVICEREMOVED : S_OK;
}
HRESULT APIENTRY destroy_sync(HANDLE h,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *d) {
    check(h==&identity && d->hSyncObject==9); ++sync_destroys; return sync_status;
}
HRESULT APIENTRY destroy_paging(HANDLE h,const D3DDDI_DESTROYPAGINGQUEUE *d) {
    check(h==&identity && d->hPagingQueue==11); ++paging_destroys; return paging_status;
}
void APIENTRY set_error(D3D10DDI_HRTCORELAYER,HRESULT) { std::abort(); }
}
int main() {
    D3DDDI_DEVICECALLBACKS cb{}; cb.pfnCreateContextVirtualCb=create;
    cb.pfnDestroyContextCb=destroy; cb.pfnDestroySynchronizationObjectCb=destroy_sync;
    cb.pfnDestroyPagingQueueCb=destroy_paging;
    D3D10DDI_CORELAYER_DEVICECALLBACKS um{}; um.pfnSetErrorCb=set_error;
    DXGI_DDI_BASE_CALLBACKS dxgi{};
    D3D10DDIARG_CREATEDEVICE args{}; args.pKTCallbacks=&cb; args.pUMCallbacks=&um;
    args.DXGIBaseDDI.pDXGIBaseCallbacks=&dxgi;
    args.hRTDevice.handle=reinterpret_cast<decltype(args.hRTDevice.handle)>(&identity);
    BC250_DXVK_ENGINE_FUNCS funcs{};
    BC250_DXVK_SHELL_SERVICES services{};
    DeviceOwner owner;
    RuntimeSurface *surface=nullptr;
    RuntimeSurface foreignSurface;
    RuntimeSurfaceRequest surfaceRequest{};
    D3D11_TEXTURE2D_DESC1 surfaceDesc{};
    check(owner.begin_surface(surfaceRequest,surfaceDesc,surface)==E_UNEXPECTED && !surface);
    check(owner.finish_surface(foreignSurface)==E_INVALIDARG);
    check(owner.close_surface(foreignSurface)==E_INVALIDARG);
    check(owner.surface_count()==0 && !owner.has_live_objects());
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    RuntimeDomain::Scope entry(owner.runtime().domain);
    check(owner.begin_surface(surfaceRequest,surfaceDesc,surface)==E_UNEXPECTED && !surface);
    check(owner.finish_surface(foreignSurface)==E_INVALIDARG && owner.close_surface(foreignSurface)==E_INVALIDARG);
    fail_create=true;
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_OUTOFMEMORY);
    check(creates==1 && destroys==0 && !owner.runtime().present_context);
    // BD-253: the step that failed, for the CreateDevice line, survives the rollback.
    check(!std::strcmp(owner.step(),"present-context"));
    fail_create=false;
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_INVALIDARG);
    check(creates==2 && destroys==1 && !owner.runtime().present_context);
    check(!std::strcmp(owner.step(),"modules"));
    fail_destroy=true;
    check(FAILED(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)));
    check(creates==3 && destroys==2 && owner.runtime().present_context==&context && owner.has_live_objects());
    check(owner.runtime().DXGICallbacks==&dxgi);
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    check(creates==3);
    fail_destroy=false;
    check(owner.close()==S_OK && destroys==3 && !owner.runtime().present_context && !owner.runtime().DXGICallbacks);
    check(owner.close()==S_OK && destroys==3 && !owner.has_live_objects());
    check(owner.retain_code_modules(reinterpret_cast<const void *>(&create),reinterpret_cast<const void *>(&destroy))==S_OK);
    check(owner.retained_module_count()==3 && owner.has_live_objects());
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    owner.runtime().present_context=&context; fail_destroy=true;
    check(owner.close()==E_FAIL && owner.retained_module_count()==3);
    // BD-253: the step that kept the device, for the DestroyDevice line.
    check(!std::strcmp(owner.step(),"close-present-context"));
    fail_destroy=false;
    check(owner.close()==S_OK && owner.retained_module_count()==0 && !owner.has_live_objects());
    check(!std::strcmp(owner.step(),"none"));
    // BD-253: the drain proof is the kernel's alone. A fence at the value of a host record is no proof: the ICD
    // publishes a record only after its submission and its signal succeeded, so an accepted submission k can still
    // run while the record and the fence are at k-1. Each call reads the fences again: no answer is kept, and only
    // the first call with a timeout waits.
    {
        HostBridge b{}; b.device=&owner.runtime();
        UINT64 present=3, progress=4;
        check(!settle_device_work(b,0));
        b.present_cpu=&present; b.present_sync=9; b.present_value=3;
        check(!settle_device_work(b,0));
        b.progress[2]={3,7,4,&progress};
        check(!settle_device_work(b,0) && !b.drain_tried);
        progress=UINT64_MAX; check(settle_device_work(b,0) && kernel_ended_device_work(b) && !b.drain_tried);
        // A TRUE is not kept: a fence that does not read the lost value any more gives FALSE again.
        progress=4; check(!settle_device_work(b,0));
        // A FALSE is not kept: after the one wait ended without the proof, a later kernel error is seen.
        check(!settle_device_work(b,5) && b.drain_tried);
        const ULONGLONG start=GetTickCount64();
        check(!settle_device_work(b,2000) && GetTickCount64()-start<500);
        progress=UINT64_MAX; check(settle_device_work(b,0) && settle_device_work(b,2000));
        present=UINT64_MAX; b.progress[2]={}; check(settle_device_work(b,0));
    }
    // BD-253: close on a lost device. While the kernel has not ended the work of the device, a lost-device status of a
    // runtime callback still keeps the device. When the kernel ends it during the one wait of close, close completes
    // and the device is freed. The proof stays valid for the steps after close-session, which forgets the fences.
    {
        UINT64 fence=4;
        owner.runtime().present_context=&context; lost_destroy=true;
        owner.bridge().device_lost=true; owner.bridge().drain_tried=true; owner.bridge().progress[0]={1,7,5,&fence};
        const unsigned before=destroys;
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && destroys==before+1 && owner.runtime().present_context==&context);
        check(!std::strcmp(owner.step(),"close-present-context") && owner.has_live_objects());
        // A fence at the record's value is no proof either.
        owner.bridge().device_lost=true; owner.bridge().progress[0]={1,7,4,&fence};
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && destroys==before+2 && owner.has_live_objects());
        owner.bridge().drain_tried=false; owner.bridge().device_lost=true; owner.bridge().progress[0]={1,7,5,&fence};
        std::thread gpu([&]() { Sleep(50); *(volatile UINT64 *)&fence=UINT64_MAX; });
        const ULONGLONG start=GetTickCount64();
        const HRESULT closed=owner.close();
        const ULONGLONG waited=GetTickCount64()-start;
        gpu.join();
        check(closed==S_OK && waited>=30 && waited<2000 && destroys==before+3);
        check(!owner.runtime().present_context && !owner.has_live_objects() && !std::strcmp(owner.step(),"none"));
        // close-present-sync: the same rule for the destroy of the present fence. That fence's own lost value is a
        // proof too.
        UINT64 presentFence=2;
        lost_destroy=false; sync_status=D3DDDIERR_DEVICEREMOVED;
        owner.bridge().device_lost=true; owner.bridge().drain_tried=true;
        owner.bridge().present_sync=9; owner.bridge().present_cpu=&presentFence; owner.bridge().present_value=2;
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && sync_destroys==1 && owner.bridge().present_sync==9);
        check(!std::strcmp(owner.step(),"close-present-sync"));
        presentFence=UINT64_MAX;
        check(owner.close()==S_OK && sync_destroys==2 && !owner.has_live_objects());
        // close-paging-queue: the same rule for the paging queue of the surfaces. A failure that is not a loss keeps
        // the queue whatever the fences say.
        UINT64 pagingFence=0, progressFence=4;
        sync_status=S_OK; paging_status=D3DDDIERR_DEVICEREMOVED;
        owner.surface_queue()={11,12,&pagingFence};
        owner.bridge().device_lost=true; owner.bridge().drain_tried=true; owner.bridge().progress[0]={1,7,5,&progressFence};
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && paging_destroys==1 && owner.has_live_objects());
        check(!std::strcmp(owner.step(),"close-paging-queue"));
        progressFence=UINT64_MAX; paging_status=E_FAIL;
        check(owner.close()==E_FAIL && paging_destroys==2 && owner.has_live_objects());
        paging_status=D3DDDIERR_DEVICEREMOVED;
        check(owner.close()==S_OK && paging_destroys==3 && !owner.has_live_objects());
        paging_status=S_OK;
        // A failure that is not a loss still keeps the device, whatever the fences say.
        owner.runtime().present_context=&context; lost_destroy=false; fail_destroy=true;
        owner.bridge().device_lost=true;
        check(owner.close()==E_FAIL && owner.has_live_objects());
        fail_destroy=false;
        check(owner.close()==S_OK && !owner.has_live_objects());
    }
    std::cout << "PASS DDI device owner: runtime arguments, initialization rollback, failed cleanup retention/retry, "
        "close on a lost device\n";
}
