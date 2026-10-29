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
HRESULT APIENTRY create(HANDLE h,D3DDDICB_CREATECONTEXTVIRTUAL *c) {
    check(h==&identity && c->EngineAffinity==1); ++creates;
    if (fail_create) return E_OUTOFMEMORY;
    c->hContext=&context; return S_OK;
}
HRESULT APIENTRY destroy(HANDLE h,const D3DDDICB_DESTROYCONTEXT *c) {
    check(h==&identity && c->hContext==&context); ++destroys;
    return fail_destroy ? E_FAIL : lost_destroy ? D3DDDIERR_DEVICEREMOVED : S_OK;
}
HRESULT APIENTRY destroy_sync(HANDLE,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *) { std::abort(); }
void APIENTRY set_error(D3D10DDI_HRTCORELAYER,HRESULT) { std::abort(); }
}
int main() {
    D3DDDI_DEVICECALLBACKS cb{}; cb.pfnCreateContextVirtualCb=create;
    cb.pfnDestroyContextCb=destroy; cb.pfnDestroySynchronizationObjectCb=destroy_sync;
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
    // BD-253: the drain proof. Each fence must reach the last value of this device; the lost-device value counts.
    {
        HostBridge b{}; b.device=&owner.runtime();
        UINT64 present=2, progress=4;
        check(device_work_drained(b));
        b.present_cpu=&present; b.present_sync=9; b.present_value=3;
        check(!device_work_drained(b));
        present=3; check(device_work_drained(b));
        b.progress[2]={3,7,5,&progress};
        check(!device_work_drained(b));
        progress=UINT64_MAX; check(device_work_drained(b) && kernel_ended_device_work(b));
        progress=5; check(device_work_drained(b));
        b.progress[2].cpu_address=nullptr; check(!device_work_drained(b));
    }
    // BD-253: close on a lost device. While the work of the device has not ended, a lost-device status of a runtime
    // callback still keeps the device, and the bounded wait is not repeated. When the work ends during the wait,
    // close completes and the device is freed.
    {
        UINT64 fence=4;
        owner.runtime().present_context=&context; lost_destroy=true;
        owner.bridge().device_lost=true; owner.bridge().progress[0]={1,7,5,&fence};
        check(!settle_device_work(owner.bridge(),5) && owner.bridge().drain_tried && !owner.bridge().work_ended);
        const unsigned before=destroys;
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && destroys==before+1 && owner.runtime().present_context==&context);
        check(!std::strcmp(owner.step(),"close-present-context") && owner.has_live_objects());
        owner.bridge().drain_tried=false; owner.bridge().device_lost=true; owner.bridge().progress[0]={1,7,5,&fence};
        std::thread gpu([&]() { Sleep(50); *(volatile UINT64 *)&fence=5; });
        const ULONGLONG start=GetTickCount64();
        const HRESULT closed=owner.close();
        const ULONGLONG waited=GetTickCount64()-start;
        gpu.join();
        check(closed==S_OK && waited>=30 && waited<2000 && destroys==before+2);
        check(!owner.runtime().present_context && !owner.has_live_objects() && !std::strcmp(owner.step(),"none"));
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
