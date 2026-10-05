// SPDX-License-Identifier: MIT
#include "queue-context.h"
#include <cassert>
#include <cstdio>
static unsigned creates,destroys;
static bool failCreate,failDestroy;
static D3D12DDI_HRTCOMMANDQUEUE seen{};
static HANDLE destroyed{};
static HRESULT APIENTRY create(D3D12DDI_HRTCOMMANDQUEUE q,D3DDDICB_CREATECONTEXTVIRTUAL* a) {
    ++creates;seen=q;assert(a->NodeOrdinal==0 && a->EngineAffinity==1 && !a->hContext);
    if(failCreate)return E_OUTOFMEMORY;
    a->hContext=reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(100+creates));return S_OK;
}
static HRESULT APIENTRY destroy(D3D12DDI_HRTCOMMANDQUEUE q,const D3DDDICB_DESTROYCONTEXT* a) {
    ++destroys;seen=q;destroyed=a->hContext;return failDestroy?E_FAIL:S_OK;
}
int main() {
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 cb{};cb.pfnCreateContextVirtualCb=create;cb.pfnDestroyContextCb=destroy;
    D3D12DDI_HRTCOMMANDQUEUE ra{},rb{};
    static_assert(sizeof(ra)==sizeof(UINT_PTR));UINT_PTR av=1,bv=2;
    memcpy(&ra,&av,sizeof(ra));memcpy(&rb,&bv,sizeof(rb));
    native12::QueueContext a(ra,cb),b(rb,cb);
    D3DDDICB_CREATECONTEXTVIRTUAL args{};args.EngineAffinity=1;
    failCreate=true;assert(a.open(args)==E_OUTOFMEMORY && !a.handle());
    failCreate=false;assert(a.open(args)==S_OK && !memcmp(&seen,&ra,sizeof(ra)));
    HANDLE ah=a.handle();unsigned count=creates;
    assert(a.open(args)==E_UNEXPECTED && creates==count);
    assert(b.open(args)==S_OK && !memcmp(&seen,&rb,sizeof(rb)) && b.handle()!=ah);
    failDestroy=true;assert(a.close()==E_FAIL && a.handle()==ah && destroyed==ah && !memcmp(&seen,&ra,sizeof(ra)));
    failDestroy=false;assert(a.close()==S_OK && !a.handle());
    count=destroys;assert(a.close()==S_OK && destroys==count);
    assert(b.close()==S_OK && !b.handle() && !memcmp(&seen,&rb,sizeof(rb)));
    cb.pfnDestroyContextCb=nullptr;native12::QueueContext bad(ra,cb);
    count=creates;assert(bad.open(args)==E_INVALIDARG && creates==count);
    puts("queue-context ownership/failure tests passed");
}
