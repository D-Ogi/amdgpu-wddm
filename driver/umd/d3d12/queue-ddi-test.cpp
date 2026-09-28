// SPDX-License-Identifier: MIT
#include "queue-ddi.h"
#include <cassert>
#include <cstdio>
static unsigned creates,errors;
static bool failCreate,failDestroy;
static D3D12DDI_HRTCOMMANDQUEUE seen{};
static HRESULT APIENTRY create(D3D12DDI_HRTCOMMANDQUEUE q,D3DDDICB_CREATECONTEXTVIRTUAL* a) {
    seen=q;++creates;if(failCreate)return E_OUTOFMEMORY;
    a->hContext=reinterpret_cast<HANDLE>(UINT_PTR(creates));return S_OK;
}
static HRESULT APIENTRY destroy(D3D12DDI_HRTCOMMANDQUEUE q,const D3DDDICB_DESTROYCONTEXT*) {seen=q;return failDestroy?E_FAIL:S_OK;}
static void APIENTRY error(D3D10DDI_HRTDEVICE,HRESULT hr){assert(hr==D3DDDIERR_DEVICEREMOVED);++errors;}
int main(){
    native12::Device device;device.callbacks.pfnCreateContextVirtualCb=create;
    device.callbacks.pfnDestroyContextCb=destroy;device.callbacks.pfnSetErrorCb=error;
    D3D12DDI_HDEVICE hd{};hd.pDrvPrivate=&device;
    D3D12DDI_DEVICE_FUNCS_CORE_0088 table{};native12::install_queue_entries(table);
    D3D12DDIARG_CREATECOMMANDQUEUE_0050 args{};args.QueueFlags=D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    SIZE_T size=table.pfnCalcPrivateCommandQueueSize(hd,&args);assert(size==sizeof(native12::QueueSlot));
    D3D12DDI_HCOMMANDQUEUE a{},b{};a.pDrvPrivate=::operator new(size);b.pDrvPrivate=::operator new(size);
    memset(a.pDrvPrivate,0xcd,size);memset(b.pDrvPrivate,0xcd,size);
    D3D12DDI_HRTCOMMANDQUEUE ra{},rb{};UINT_PTR av=11,bv=12;memcpy(&ra,&av,sizeof(ra));memcpy(&rb,&bv,sizeof(rb));
    failCreate=true;assert(table.pfnCreateCommandQueue(hd,&args,a,ra)==E_OUTOFMEMORY && device.queues.empty());
    failCreate=false;assert(table.pfnCreateCommandQueue(hd,&args,a,ra)==S_OK && !memcmp(&seen,&ra,sizeof(ra)));
    assert(table.pfnCreateCommandQueue(hd,&args,b,rb)==S_OK && !memcmp(&seen,&rb,sizeof(rb)));
    failDestroy=true;table.pfnDestroyCommandQueue(hd,a);::operator delete(a.pDrvPrivate);
    assert(errors==1 && device.lost.load() && !memcmp(&seen,&ra,sizeof(ra)));
    assert(table.pfnCreateCommandQueue(hd,&args,{},ra)==E_INVALIDARG);
    assert(table.pfnCalcPrivateCommandQueueSize(hd,&args)==0);
    failDestroy=false;table.pfnDestroyCommandQueue(hd,b);::operator delete(b.pDrvPrivate);
    assert(!memcmp(&seen,&rb,sizeof(rb)) && !device.queues.empty());
    unsigned unresolved=0;
    assert(device.queues.discard_retired_metadata(unresolved)==S_FALSE && unresolved==1 && device.queues.empty());
    assert(!memcmp(&seen,&rb,sizeof(rb))); // Last callback remains the still-valid B destruction.
    puts("typed queue DDI creation/destruction tests passed");
}
