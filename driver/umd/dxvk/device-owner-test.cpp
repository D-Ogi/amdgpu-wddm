// SPDX-License-Identifier: MIT
#include "device-owner.h"
#include <cstdlib>
#include <iostream>
using namespace bc250::umd;
namespace {
void check(bool ok) { if (!ok) std::abort(); }
int identity, context; unsigned creates, destroys; bool fail_create=false,fail_destroy=false;
HRESULT APIENTRY create(HANDLE h,D3DDDICB_CREATECONTEXTVIRTUAL *c) {
    check(h==&identity && c->EngineAffinity==1); ++creates;
    if (fail_create) return E_OUTOFMEMORY;
    c->hContext=&context; return S_OK;
}
HRESULT APIENTRY destroy(HANDLE h,const D3DDDICB_DESTROYCONTEXT *c) {
    check(h==&identity && c->hContext==&context); ++destroys;
    return fail_destroy ? E_FAIL : S_OK;
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
    fail_create=false;
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_INVALIDARG);
    check(creates==2 && destroys==1 && !owner.runtime().present_context);
    fail_destroy=true;
    check(FAILED(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)));
    check(creates==3 && destroys==2 && owner.runtime().present_context==&context && owner.has_live_objects());
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    check(creates==3);
    fail_destroy=false;
    check(owner.close()==S_OK && destroys==3 && !owner.runtime().present_context);
    check(owner.close()==S_OK && destroys==3 && !owner.has_live_objects());
    check(owner.retain_code_modules(reinterpret_cast<const void *>(&create),reinterpret_cast<const void *>(&destroy))==S_OK);
    check(owner.retained_module_count()==3 && owner.has_live_objects());
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    owner.runtime().present_context=&context; fail_destroy=true;
    check(owner.close()==E_FAIL && owner.retained_module_count()==3);
    fail_destroy=false;
    check(owner.close()==S_OK && owner.retained_module_count()==0 && !owner.has_live_objects());
    std::cout << "PASS DDI device owner: runtime arguments, initialization rollback, failed cleanup retention/retry\n";
}
