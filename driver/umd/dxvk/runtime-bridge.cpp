/**************************************************************************
 *
 * Copyright 2012-2021 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 **************************************************************************/

// Derived from the G0 Mesa Device.cpp runtime bridge at 71f2e28c1b14.
// Gallium Device replaced with runtime-only state; entry uses RuntimeDomain.
#include "runtime-bridge.h"
#include "ddi-error-policy.h"
namespace bc250::umd {
// The device is gone. This is the one report that happens outside a DDI entry's own code: the loss is
// detected inside the engine's submission path, under whichever entry is running. That entry's page
// decides whether the runtime accepts device removal here, so the status passes through the entry
// class that enter_context put on this thread. A capability-check entry accepts no status at all, and
// then the report waits for the next entry whose page allows it (BD-071 review).
static HRESULT Bc250HostLost(HostBridge *s)
{
   s->device_lost=true;
   s->submission_failed=true;
   if (!s->lost_reported) {
      const HRESULT reported=ddi_class_status(ddi_entry_class,D3DDDIERR_DEVICEREMOVED);
      if (reported!=S_OK && s->device->UMCallbacks.pfnSetErrorCb) {
         s->lost_reported=true;
         s->device->UMCallbacks.pfnSetErrorCb(s->device->hRTCoreLayer,reported);
      }
   }
   return D3DDDIERR_DEVICEREMOVED;
}

static bool Bc250DeviceLostResult(HRESULT hr)
{
   return hr==D3DDDIERR_DEVICEREMOVED || hr==DXGI_ERROR_DEVICE_REMOVED ||
          hr==DXGI_ERROR_DEVICE_RESET || hr==DXGI_ERROR_DEVICE_HUNG ||
          hr==DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

static HRESULT Bc250HostStatus(HostBridge *s)
{
   // A loss already latched still needs its report when a check-type entry swallowed the first one.
   if (s->device_lost) return s->lost_reported ? D3DDDIERR_DEVICEREMOVED : Bc250HostLost(s);
   for (const auto &p:s->progress) {
      if (!p.cpu_address) continue;
      UINT64 observed=*(const volatile UINT64 *)p.cpu_address;

      if (observed==UINT64_MAX) return Bc250HostLost(s);
   }
   if ((s->present_cpu && *(const volatile UINT64 *)s->present_cpu==UINT64_MAX) ||
       (s->device->pagingFence && *s->device->pagingFence==UINT64_MAX))
      return Bc250HostLost(s);
   return S_OK;
}

static HANDLE Bc250HostContext(HostBridge *s, UINT token)
{
   return token && token <= 16 ? s->contexts[token - 1] : NULL;
}

static HRESULT Bc250HostOperation(HostBridge *s, uint32_t op, void *argument)
{
   auto &cb = s->device->KTCallbacks;
   HANDLE rt = s->device->hDevice;
   if (op==BC250_HOST_CHECK_STATUS) return Bc250HostStatus(s);
   if (op==BC250_HOST_REPORT_LOST) return Bc250HostLost(s);
   if (!argument) return E_INVALIDARG;
#define HOST_CALL(name, arg) (cb.pfn##name##Cb ? cb.pfn##name##Cb(rt, arg) : E_NOTIMPL)
   switch (op) {
   case BC250_HOST_PUBLISH_PROGRESS: {
      auto *a=(bc250_host_progress *)argument;
      if (!Bc250HostContext(s,a->context) || !a->sync || !a->value || !a->cpu_address || a->value==UINT64_MAX) return E_INVALIDARG;
      auto &old=s->progress[a->context-1];
      if (old.sync && (old.sync!=a->sync || old.value>=a->value)) return E_INVALIDARG;
      old=*a;
      return S_OK;
   }
   case BC250_HOST_CREATE_PAGING: {
      auto *a = (bc250_host_paging *)argument;
      D3DDDICB_CREATEPAGINGQUEUE b = {};
      HRESULT hr = HOST_CALL(CreatePagingQueue, &b);
      a->queue=b.hPagingQueue; a->sync=b.hSyncObject; a->cpu_address=b.FenceValueCPUVirtualAddress;
      return hr;
   }
   case BC250_HOST_DESTROY_PAGING: {
      D3DDDI_DESTROYPAGINGQUEUE b = {};
      b.hPagingQueue=((bc250_host_paging *)argument)->queue;
      return HOST_CALL(DestroyPagingQueue, &b);
   }
   case BC250_HOST_CreateAllocation2: {
      auto *a=(D3DKMT_CREATEALLOCATION *)argument;
      D3DDDICB_ALLOCATE b = {};
      b.pPrivateDriverData=a->pPrivateDriverData; b.PrivateDriverDataSize=a->PrivateDriverDataSize;
      b.NumAllocations=a->NumAllocations; b.pAllocationInfo2=a->pAllocationInfo2;
      HRESULT hr=HOST_CALL(Allocate, &b);
      a->hResource=b.hKMResource;
      return hr;
   }
   case BC250_HOST_DestroyAllocation2: {
      auto *a=(D3DKMT_DESTROYALLOCATION2 *)argument;
      if (a->hResource || a->Flags.Value) return E_NOTIMPL;
      D3DDDICB_DEALLOCATE b = {};
      b.NumAllocations=a->AllocationCount; b.HandleList=a->phAllocationList;
      return HOST_CALL(Deallocate, &b);
   }
   case BC250_HOST_ReserveGpuVirtualAddress:
      return HOST_CALL(ReserveGpuVirtualAddress, (D3DDDI_RESERVEGPUVIRTUALADDRESS *)argument);
   case BC250_HOST_MapGpuVirtualAddress:
      return HOST_CALL(MapGpuVirtualAddress, (D3DDDI_MAPGPUVIRTUALADDRESS *)argument);
   case BC250_HOST_MakeResident:
      return HOST_CALL(MakeResident, (D3DDDI_MAKERESIDENT *)argument);
   case BC250_HOST_FreeGpuVirtualAddress: {
      auto *a=(D3DKMT_FREEGPUVIRTUALADDRESS *)argument;
      D3DDDICB_FREEGPUVIRTUALADDRESS b = {};
      b.BaseAddress=a->BaseAddress; b.Size=a->Size;
      return HOST_CALL(FreeGpuVirtualAddress, &b);
   }
   case BC250_HOST_Evict: {
      auto *a=(D3DKMT_EVICT *)argument;
      D3DDDICB_EVICT b = {};
      b.NumAllocations=a->NumAllocations; b.AllocationList=a->AllocationList; b.Flags=a->Flags;
      HRESULT hr=HOST_CALL(Evict, &b); a->NumBytesToTrim=b.NumBytesToTrim; return hr;
   }
   case BC250_HOST_Lock2: {
      auto *a=(D3DKMT_LOCK2 *)argument;
      D3DDDICB_LOCK2 b = {};
      b.hAllocation=a->hAllocation; b.Flags.Value=a->Flags.Value;
      HRESULT hr=HOST_CALL(Lock2, &b); a->pData=b.pData; return hr;
   }
   case BC250_HOST_Unlock2: {
      D3DDDICB_UNLOCK2 b = {};
      b.hAllocation=((D3DKMT_UNLOCK2 *)argument)->hAllocation;
      return HOST_CALL(Unlock2, &b);
   }
   case BC250_HOST_CreateContextVirtual: {
      auto *a=(D3DKMT_CREATECONTEXTVIRTUAL *)argument;
      unsigned slot=0; while (slot<16 && s->contexts[slot]) ++slot;
      if (slot==16) return E_OUTOFMEMORY;
      D3DDDICB_CREATECONTEXTVIRTUAL b = {};
      b.NodeOrdinal=a->NodeOrdinal; b.EngineAffinity=a->EngineAffinity; b.Flags=a->Flags;
      b.pPrivateDriverData=a->pPrivateDriverData; b.PrivateDriverDataSize=a->PrivateDriverDataSize;
      HRESULT hr=HOST_CALL(CreateContextVirtual, &b);
      if (SUCCEEDED(hr)) { s->contexts[slot]=b.hContext; s->present_waited[slot]=0; a->hContext=slot+1; }
      return hr;
   }
   case BC250_HOST_DestroyContext: {
      auto *a=(D3DKMT_DESTROYCONTEXT *)argument;
      D3DDDICB_DESTROYCONTEXT b = {};
      b.hContext=Bc250HostContext(s,a->hContext);
      if (!b.hContext) return E_INVALIDARG;
      HRESULT hr=HOST_CALL(DestroyContext, &b);
      if (SUCCEEDED(hr)) {
         s->contexts[a->hContext-1]=NULL;
         s->progress[a->hContext-1]={};
      }
      return hr;
   }
   case BC250_HOST_CreateSynchronizationObject2: {
      auto *a=(D3DKMT_CREATESYNCHRONIZATIONOBJECT2 *)argument;
      if (a->Info.Flags.Shared || a->Info.Flags.NtSecuritySharing) return E_NOTIMPL;
      D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 b = {};
      b.Info=a->Info;
      HRESULT hr=HOST_CALL(CreateSynchronizationObject2, &b);
      a->Info=b.Info; a->hSyncObject=b.hSyncObject; return hr;
   }
   case BC250_HOST_DestroySynchronizationObject: {
      D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT b = {};
      b.hSyncObject=((D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *)argument)->hSyncObject;
      HRESULT hr=HOST_CALL(DestroySynchronizationObject, &b);
      if (SUCCEEDED(hr)) for (auto &p:s->progress) if (p.sync==b.hSyncObject) p={};
      return hr;
   }
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      auto *a=(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *)argument;
      D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU b = {};
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray;
      b.FenceValueArray=a->FenceValueArray; b.hAsyncEvent=a->hAsyncEvent; b.Flags=a->Flags;
      return HOST_CALL(WaitForSynchronizationObjectFromCpu, &b);
   }
   case BC250_HOST_SignalSynchronizationObjectFromCpu: {
      auto *a=(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU *)argument;
      D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMCPU b = {};
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray; b.FenceValueArray=a->FenceValueArray;
      return HOST_CALL(SignalSynchronizationObjectFromCpu, &b);
   }
   case BC250_HOST_WaitForSynchronizationObjectFromGpu: {
      auto *a=(D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *)argument;
      D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU b = {};
      b.hContext=Bc250HostContext(s,a->hContext); if (!b.hContext) return E_INVALIDARG;
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray;
      b.MonitoredFenceValueArray=a->MonitoredFenceValueArray;
      return HOST_CALL(WaitForSynchronizationObjectFromGpu, &b);
   }
   case BC250_HOST_SignalSynchronizationObjectFromGpu2: {
      auto *a=(D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 *)argument;
      if (a->BroadcastContextCount > D3DDDI_MAX_BROADCAST_CONTEXT || a->Flags.Value) return E_NOTIMPL;
      HANDLE contexts[D3DDDI_MAX_BROADCAST_CONTEXT] = {};
      for (UINT i=0;i<a->BroadcastContextCount;++i) {
         contexts[i]=Bc250HostContext(s,a->BroadcastContextArray[i]); if (!contexts[i]) return E_INVALIDARG;
      }
      D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 b = {};
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray;
      b.BroadcastContextCount=a->BroadcastContextCount; b.BroadcastContextArray=contexts;
      b.MonitoredFenceValueArray=a->MonitoredFenceValueArray;
      return HOST_CALL(SignalSynchronizationObjectFromGpu2, &b);
   }
   case BC250_HOST_UpdateGpuVirtualAddress: {
      // Sparse binding: the runtime queues wait(FenceValue), the page-table updates and
      // signal(FenceValue+1) on the context. The kernel validates ranges and backing.
      HRESULT health=Bc250HostStatus(s);
      if (FAILED(health)) return health;

      auto *a=(D3DKMT_UPDATEGPUVIRTUALADDRESS *)argument;
      if (!a->NumOperations || !a->Operations || a->Reserved0 || a->Reserved1 || !a->hFenceObject ||
          !a->FenceValue || a->FenceValue>=UINT64_MAX-1) return E_INVALIDARG;
      if (a->Flags.Value) return E_NOTIMPL;
      D3DDDICB_UPDATEGPUVIRTUALADDRESS b = {};
      b.hContext=Bc250HostContext(s,a->hContext); if (!b.hContext) return E_INVALIDARG;
      b.hFenceObject=a->hFenceObject; b.NumOperations=a->NumOperations; b.Operations=a->Operations;
      b.FenceValue=a->FenceValue;
      return HOST_CALL(UpdateGpuVirtualAddress, &b);
   }
   case BC250_HOST_SubmitCommand: {
      HRESULT health=Bc250HostStatus(s);
      if (FAILED(health)) return health;

      auto *a=(D3DKMT_SUBMITCOMMAND *)argument;
      if (a->BroadcastContextCount>D3DDDI_MAX_BROADCAST_CONTEXT || a->NumPrimaries>D3DDDI_MAX_WRITTEN_PRIMARIES || a->Flags.NullRendering || a->Flags.PresentRedirected || a->Flags.NoKmdAccess || a->Flags.Reserved || a->PresentHistoryToken || a->NumHistoryBuffers) return E_NOTIMPL;
      D3DDDICB_SUBMITCOMMAND b = {};
      b.Commands=a->Commands; b.CommandLength=a->CommandLength;
      b.pPrivateDriverData=a->pPrivateDriverData; b.PrivateDriverDataSize=a->PrivateDriverDataSize;
      b.BroadcastContextCount=a->BroadcastContextCount;
      for (UINT i=0;i<a->BroadcastContextCount;++i) {
         b.BroadcastContext[i]=Bc250HostContext(s,a->BroadcastContext[i]); if (!b.BroadcastContext[i]) return E_INVALIDARG;
         UINT token=a->BroadcastContext[i];
         if (s->present_value>s->present_waited[token-1]) {
            D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait={};
            wait.hContext=b.BroadcastContext[i]; wait.ObjectCount=1;
            wait.ObjectHandleArray=&s->present_sync; wait.MonitoredFenceValueArray=&s->present_value;
            HRESULT hr=HOST_CALL(WaitForSynchronizationObjectFromGpu, &wait);
            if (FAILED(hr)) return hr;
            s->present_waited[token-1]=s->present_value;
         }
      }
      b.NumPrimaries=a->NumPrimaries;
      for (UINT i=0;i<a->NumPrimaries;++i) b.WrittenPrimaries[i]=a->WrittenPrimaries[i];
      return HOST_CALL(SubmitCommand, &b);
   }
   default: return E_NOTIMPL;
   }
#undef HOST_CALL
}

int32_t host_dispatch(void *userdata, uint32_t operation, void *argument)
{
   auto *s=(HostBridge *)userdata;
   if (!s || !s->device || !s->device->domain.entered()) {
      return (int32_t)0xc000000d;
   }
   HRESULT hr=Bc250HostOperation(s,operation,argument);
   if (Bc250DeviceLostResult(hr)) {
      Bc250HostLost(s);
      return (int32_t)0xc00002b6;
   }
   if (FAILED(hr) && (operation==BC250_HOST_SubmitCommand || operation==BC250_HOST_SignalSynchronizationObjectFromGpu2 || operation==BC250_HOST_UpdateGpuVirtualAddress || operation==BC250_HOST_PUBLISH_PROGRESS))
      s->submission_failed=true;
   if (operation<64) ++s->calls[operation];
   if (hr==E_PENDING && (operation==BC250_HOST_MapGpuVirtualAddress || operation==BC250_HOST_MakeResident)) return 0x103;
   return SUCCEEDED(hr) ? 0 : hr==E_NOTIMPL ? (int32_t)0xc00000bb :
          hr==E_OUTOFMEMORY ? (int32_t)0xc0000017 :
          hr==E_INVALIDARG ? (int32_t)0xc000000d : (int32_t)0xc0000001;
}

HRESULT queue_present_wait(HostBridge &bridge)
{
   auto *s=&bridge;
   auto *device=s->device;
   if (!device || !device->domain.entered()) return E_INVALIDARG;
   if (!s || !device->domain.entered() || !device->present_context) return E_INVALIDARG;
   if (FAILED(Bc250HostStatus(s)) || s->submission_failed) return DXGI_ERROR_DEVICE_REMOVED;
   if (!device->KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb ||
       !device->KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb ||
       !device->KTCallbacks.pfnCreateSynchronizationObject2Cb) return E_NOTIMPL;
   if (!s->present_sync) {
      D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 create={};
      create.Info.Type=D3DDDI_MONITORED_FENCE;
      create.Info.MonitoredFence.EngineAffinity=1;
      HRESULT hr=device->KTCallbacks.pfnCreateSynchronizationObject2Cb(device->hDevice,&create);
      if (FAILED(hr)) return hr;
      s->present_sync=create.hSyncObject;
      s->present_cpu=(const UINT64 *)create.Info.MonitoredFence.FenceValueCPUVirtualAddress;
   }
   D3DKMT_HANDLE objects[16]={};
   UINT64 values[16]={};
   UINT count=0;
   for (const auto &p:s->progress) {
      if (!p.sync) continue;
      if (!Bc250HostContext(s,p.context)) return E_FAIL;
      objects[count]=p.sync; values[count]=p.value; ++count;
   }
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU wait={};
   wait.hContext=device->present_context; wait.ObjectCount=count;
   wait.ObjectHandleArray=objects; wait.MonitoredFenceValueArray=values;
   HRESULT hr=count ? device->KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb(device->hDevice,&wait) : S_OK;

   return hr;
}

HRESULT signal_present(HostBridge &bridge)
{
   auto *s=&bridge;
   auto *device=s->device;
   if (!device || !device->domain.entered()) return E_INVALIDARG;
   if (!s || !device->domain.entered() || !s->present_sync) return E_INVALIDARG;
   if (!device->present_context || !device->KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb) return E_INVALIDARG;
   if (FAILED(Bc250HostStatus(s)) || s->submission_failed) return DXGI_ERROR_DEVICE_REMOVED;
   UINT64 value=s->present_value+1;
   if (!value || value==UINT64_MAX) return E_FAIL;
   D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 signal={};
   signal.ObjectCount=1; signal.ObjectHandleArray=&s->present_sync;
   signal.BroadcastContextCount=1; signal.BroadcastContextArray=&device->present_context;
   signal.MonitoredFenceValueArray=&value;
   HRESULT hr=device->KTCallbacks.pfnSignalSynchronizationObjectFromGpu2Cb(device->hDevice,&signal);
   if (SUCCEEDED(hr)) s->present_value=value;
   else s->submission_failed=true;
   return hr;
}

HRESULT wait_present_idle(HostBridge &bridge)
{
   return wait_present_value(bridge,bridge.present_value);
}

HRESULT wait_present_value(HostBridge &bridge, UINT64 value)
{
   auto *s=&bridge;
   auto *device=s->device;
   if (!device || !device->domain.entered()) return E_INVALIDARG;
   if (!s || !value) return S_OK;
   // Only a value this bridge has signalled can be waited for.
   if (value>s->present_value) return E_INVALIDARG;
   if (FAILED(Bc250HostStatus(s))) return D3DDDIERR_DEVICEREMOVED;
   // BD-065: the Present shadow waits for a Present two frames back, normally retired already.
   if (s->present_cpu && *(const volatile UINT64 *)s->present_cpu>=value) return Bc250HostStatus(s);
   if (!device->domain.entered() || !device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb) return E_INVALIDARG;
   HANDLE event=CreateEventW(NULL,FALSE,FALSE,NULL);
   if (!event) return HRESULT_FROM_WIN32(GetLastError());
   D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMCPU wait={};
   wait.ObjectCount=1; wait.ObjectHandleArray=&s->present_sync; wait.FenceValueArray=&value;
   wait.hAsyncEvent=event;
   HRESULT hr=device->KTCallbacks.pfnWaitForSynchronizationObjectFromCpuCb(device->hDevice,&wait);
   if (SUCCEEDED(hr) && WaitForSingleObject(event,10000)!=WAIT_OBJECT_0) hr=DXGI_ERROR_DEVICE_HUNG;
   CloseHandle(event);
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(s);
   return SUCCEEDED(hr) ? Bc250HostStatus(s) : hr;
}

HRESULT present_runtime(HostBridge &bridge, D3DKMT_HANDLE source,
    D3DKMT_HANDLE destination, void *dxgi_context, FlushEngine flush, void *engine)
{
   auto *device=bridge.device;
   if (!device || !device->domain.entered() || !device->present_context || !source || !flush)
      return E_INVALIDARG;
   if (!device->DXGICallbacks || !device->DXGICallbacks->pfnPresentCb) return E_NOTIMPL;
   if (FAILED(Bc250HostStatus(&bridge)) || bridge.submission_failed)
      return DXGI_ERROR_DEVICE_REMOVED;
   HRESULT hr=flush(engine);
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(&bridge);
   if (FAILED(hr)) return hr;
   hr=queue_present_wait(bridge);
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(&bridge);
   if (FAILED(hr)) return hr;
   DXGIDDICB_PRESENT present={};
   present.hSrcAllocation=source;
   present.hDstAllocation=destination;
   present.hContext=device->present_context;
   present.pDXGIContext=dxgi_context;
   const HRESULT result=device->DXGICallbacks->pfnPresentCb(device->hDevice,&present);
   if (Bc250DeviceLostResult(result)) return Bc250HostLost(&bridge);
   if (FAILED(result)) return result;
   hr=signal_present(bridge);
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(&bridge);
   return FAILED(hr) ? hr : result;
}

}
