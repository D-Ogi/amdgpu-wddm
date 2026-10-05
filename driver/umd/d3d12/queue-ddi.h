// SPDX-License-Identifier: MIT
#pragma once
#include "device-state.h"
#include <atomic>
#include <cstdint>
namespace native12 {
// Private storage has one fixed size. Sizing does not judge the request;
// pfnCreateCommandQueue validates it before the first write.
inline SIZE_T APIENTRY queue_size(D3D12DDI_HDEVICE,const D3D12DDIARG_CREATECOMMANDQUEUE_0050*) {
    return sizeof(QueueSlot);
}
inline HRESULT APIENTRY queue_create(D3D12DDI_HDEVICE h,const D3D12DDIARG_CREATECOMMANDQUEUE_0050* args,
                                     D3D12DDI_HCOMMANDQUEUE q,D3D12DDI_HRTCOMMANDQUEUE runtime) {
    if(!h.pDrvPrivate || !args || !q.pDrvPrivate ||
       reinterpret_cast<uintptr_t>(q.pDrvPrivate)%alignof(QueueSlot))return E_INVALIDARG;
    auto d=static_cast<Device*>(h.pDrvPrivate);
    if(d->lost.load())return D3DDDIERR_DEVICEREMOVED;
    {ContextRequest probe;HRESULT refused=probe.prepare(*args);if(FAILED(refused))return refused;}
    // Runtime-owned storage is uninitialized. Never read an old pointer from it.
    auto slot=new(q.pDrvPrivate) QueueSlot{};
    HRESULT hr=d->queues.create(*args,runtime,d->callbacks,*slot);
    if(FAILED(hr))slot->~QueueSlot();
    return hr;
}
inline void APIENTRY queue_destroy(D3D12DDI_HDEVICE h,D3D12DDI_HCOMMANDQUEUE q) {
    if(!h.pDrvPrivate || !q.pDrvPrivate)return;
    auto d=static_cast<Device*>(h.pDrvPrivate);auto slot=static_cast<QueueSlot*>(q.pDrvPrivate);
    HRESULT hr=d->queues.destroy(*slot);slot->~QueueSlot();
    if(FAILED(hr))d->remove();
}
// DDI0092 retains the0088 core-device table layout. This installs only queue entries;
// the caller must complete the rest of the table before publishing it to the runtime.
inline void install_queue_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088& table) noexcept {
    table.pfnCalcPrivateCommandQueueSize=queue_size;
    table.pfnCreateCommandQueue=queue_create;
    table.pfnDestroyCommandQueue=queue_destroy;
}
}
