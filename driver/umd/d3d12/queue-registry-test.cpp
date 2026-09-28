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
    unsigned unresolved=0;native12::QueueRegistry registry;native12::QueueSlot a{},b{};
    failCreate=true;assert(registry.create(desc,{},cb,a)==E_OUTOFMEMORY && !a.owner && registry.empty());
    failCreate=false;assert(registry.create(desc,{},cb,a)==S_OK);
    assert(registry.create(desc,{},cb,b)==S_OK && a.owner->context.handle()!=b.owner->context.handle());
    assert(registry.discard_retired_metadata(unresolved)==E_UNEXPECTED && !registry.empty());
    unsigned count=creates;assert(registry.create(desc,{},cb,a)==E_UNEXPECTED && creates==count);
    failDestroy=true;assert(registry.destroy(a)==E_FAIL && !a.owner && !registry.empty());
    // The caller may now discard/reuse the private slot without losing the retained context.
    assert(registry.discard_retired_metadata(unresolved)==E_UNEXPECTED);
    failDestroy=false;assert(registry.destroy(b)==S_OK && !b.owner);
    count=destroys;
    assert(registry.discard_retired_metadata(unresolved)==S_FALSE && unresolved==1 && registry.empty());
    assert(destroys==count); // No callback on a runtime queue whose lifetime has ended.
    assert(registry.destroy(a)==S_OK && registry.discard_retired_metadata(unresolved)==S_OK && !unresolved && destroys==count);
    assert(registry.create(desc,{},cb,a)==S_OK);
    assert(registry.create(desc,{},cb,b)==S_OK);
    failDestroy=true;assert(registry.destroy(a)==E_FAIL);
    count=destroys;
    unsigned retired=0,active=0;
    registry.discard_device_metadata(retired,active);
    assert(retired==1 && active==1 && registry.empty() && destroys==count);
    b.owner=nullptr; // Runtime storage is no longer usable after device teardown.
    registry.discard_device_metadata(retired,active);
    assert(!retired && !active && destroys==count);
    puts("queue registry failure ownership tests passed");
}
