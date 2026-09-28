// SPDX-License-Identifier: MIT
#include "queue-registry.h"
#include <cassert>
#include <cstdio>
static bool failCreate,failDestroy;
static unsigned creates,destroys;
static HRESULT APIENTRY create(D3D12DDI_HRTCOMMANDQUEUE,D3DDDICB_CREATECONTEXTVIRTUAL* a){
    ++creates;if(failCreate)return E_OUTOFMEMORY;
    a->hContext=reinterpret_cast<HANDLE>(UINT_PTR(creates));return S_OK;
}
static HRESULT APIENTRY destroy(D3D12DDI_HRTCOMMANDQUEUE,const D3DDDICB_DESTROYCONTEXT*){
    ++destroys;return failDestroy?E_FAIL:S_OK;
}
int main(){
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 cb{};cb.pfnCreateContextVirtualCb=create;cb.pfnDestroyContextCb=destroy;
    D3D12DDIARG_CREATECOMMANDQUEUE_0050 desc{};desc.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    native12::QueueRegistry registry;native12::QueueSlot a{},b{};
    failCreate=true;assert(registry.create(desc,{},cb,a)==E_OUTOFMEMORY && !a.owner && registry.empty());
    failCreate=false;assert(registry.create(desc,{},cb,a)==S_OK);
    assert(registry.create(desc,{},cb,b)==S_OK && a.owner->context.handle()!=b.owner->context.handle());
    assert(registry.drain_retired()==E_UNEXPECTED && !registry.empty());
    unsigned count=creates;assert(registry.create(desc,{},cb,a)==E_UNEXPECTED && creates==count);
    failDestroy=true;assert(registry.destroy(a)==E_FAIL && !a.owner && !registry.empty());
    // The caller may now discard/reuse the private slot without losing the retained context.
    assert(registry.drain_retired()==E_UNEXPECTED);
    failDestroy=false;assert(registry.destroy(b)==S_OK && !b.owner);
    failDestroy=true;assert(registry.drain_retired()==E_FAIL && !registry.empty());
    failDestroy=false;assert(registry.drain_retired()==S_OK && registry.empty());
    count=destroys;assert(registry.destroy(a)==S_OK && registry.drain_retired()==S_OK && destroys==count);
    puts("queue registry failure ownership tests passed");
}
