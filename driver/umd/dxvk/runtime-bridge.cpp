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
#include "diagnostics.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
namespace bc250::umd {
namespace {
std::mutex g_loss_mutex;
DeviceLossRecord g_last_loss{};
void Bc250RecordLoss(const char *reason, const char *site, unsigned op, HRESULT hr) noexcept
{
   std::lock_guard<std::mutex> lock(g_loss_mutex);
   snprintf(g_last_loss.reason,sizeof(g_last_loss.reason),"%s",reason ? reason : "-");
   snprintf(g_last_loss.site,sizeof(g_last_loss.site),"%s",site ? site : "-");
   g_last_loss.op=op; g_last_loss.hr=hr; g_last_loss.at_ms=GetTickCount64(); ++g_last_loss.losses;
}
}
DeviceLossRecord last_device_loss() noexcept
{
   std::lock_guard<std::mutex> lock(g_loss_mutex);
   return g_last_loss;
}
static HostFence *Bc250FindFence(HostBridge *s, D3DKMT_HANDLE handle)
{
   if (!handle) return nullptr;
   for (auto &f:s->fences) if (f.handle==handle) return &f;
   return nullptr;
}
// BD-198: a monitored fence of the ICD gets a record when it is created. A fence that does not read its initial value
// right after its creation gets one line, in both flavours: then the kernel gave the ICD a fence whose storage another
// writer had already changed.
static void Bc250RecordFence(HostBridge *s, D3DKMT_HANDLE handle, const D3DDDI_SYNCHRONIZATIONOBJECTINFO2 &info)
{
   if (info.Type!=D3DDDI_MONITORED_FENCE || !handle || !info.MonitoredFence.FenceValueCPUVirtualAddress) return;
   const auto *cpu=static_cast<const volatile UINT64 *>(info.MonitoredFence.FenceValueCPUVirtualAddress);
   HostFence *slot=Bc250FindFence(s,handle);
   if (!slot) for (auto &f:s->fences) if (!f.handle) { slot=&f; break; }
   if (!slot) { ++s->fences_unrecorded; return; }
   *slot={handle,cpu,info.MonitoredFence.InitialFenceValue,*cpu,GetTickCount64()};
   if (slot->at_create!=slot->initial) {
      static std::atomic_uint notes{16};
      device_life_note(notes,"BC250 BD-198: monitored fence %u reads %llu right after its creation with %llu: "
         "runtime=%p identity=%p thread=%lu\n",unsigned(handle),(unsigned long long)slot->at_create,
         (unsigned long long)slot->initial,s->device ? s->device->hDevice : nullptr,
         s->device ? s->device->host_identity() : nullptr,(unsigned long)GetCurrentThreadId());
   }
}
// BD-253: one line when a bridge first latches a loss, in both flavours (an ordinary error report, at most 16
// lines per process): the reason, the host operation and its status where one caused it, the device and the
// thread. The engine's own line ("Command submission failed: VK_ERROR_DEVICE_LOST") does not name the cause.
// BD-258: the line also names the read of the ICD that saw the lost-device value (site, the sync object), and what
// the fences this bridge knows read at that time. When every known fence reads the lost value, the kernel put the
// device in error. When only the fence of the site does, something else wrote that value to that one fence.
static const char *Bc250FenceState(const volatile UINT64 *cpu)
{
   return !cpu ? "none" : *cpu==UINT64_MAX ? "lost" : "live";
}
// BD-198: the line also names the fence of the site from the bridge's records (its value right after its creation, its
// value now, its age) and counts the recorded fences of the device that read the lost value. At a loss before the
// first submission the progress records are still empty, so only these records tell the three cases apart: every
// fence of the device reads the lost value (the kernel ended the device), only the fence of the site reads it (one
// writer changed one fence), or the fence read it already at its creation. The GPU work count says whether the device
// ever queued work (device_queued_no_work).
static void Bc250HostLossNote(HostBridge *s, const char *reason, unsigned op, HRESULT hr,
   const bc250_host_lost_report *report=nullptr)
{
   unsigned known=0, lost=0;
   for (const auto &p:s->progress) {
      if (!p.cpu_address) continue;
      ++known; if (*(const volatile UINT64 *)p.cpu_address==UINT64_MAX) ++lost;
   }
   unsigned recorded=0, recorded_lost=0;
   for (const auto &f:s->fences) {
      if (!f.handle) continue;
      ++recorded; if (*f.cpu==UINT64_MAX) ++recorded_lost;
   }
   const HostFence *site=report ? Bc250FindFence(s,report->handle) : nullptr;
   const ULONGLONG now=GetTickCount64();
   static std::atomic_uint notes{16};
   device_life_note(notes,"BC250 BD-253: host loss latched: reason=%s op=%u hr=%08X site=%s handle=%u runtime=%p "
      "identity=%p thread=%lu; fences at the loss: progress lost %u of %u, present %s, paging %s; site fence: "
      "recorded=%u at-create=%llu now=%llu age-ms=%llu; device fences lost %u of %u (unrecorded %u); "
      "gpu-work-calls=%u\n",reason,op,
      unsigned(hr),report && report->site ? report->site : "-",report ? report->handle : 0u,
      s->device ? s->device->hDevice : nullptr,s->device ? s->device->host_identity() : nullptr,
      (unsigned long)GetCurrentThreadId(),lost,known,Bc250FenceState(s->present_cpu),
      Bc250FenceState(s->device ? s->device->pagingFence : nullptr),unsigned(site!=nullptr),
      site ? (unsigned long long)site->at_create : 0ull,site ? (unsigned long long)*site->cpu : 0ull,
      site && now>=site->created_ms ? (unsigned long long)(now-site->created_ms) : 0ull,recorded_lost,recorded,
      s->fences_unrecorded,s->gpu_work_calls);
   Bc250RecordLoss(reason,report ? report->site : nullptr,op,hr);
}
// The device is gone. This is the one report that happens outside a DDI entry's own code: the loss is
// detected inside the engine's submission path, under whichever entry is running. That entry's page
// decides whether the runtime accepts device removal here, so the status passes through the entry
// class that enter_context put on this thread. A capability-check entry accepts no status at all, and
// then the report waits for the next entry whose page allows it (BD-071 review).
static HRESULT Bc250HostLost(HostBridge *s, const char *reason="unnamed", unsigned op=~0u, HRESULT hr=S_OK,
   const bc250_host_lost_report *report=nullptr)
{
   if (!s->device_lost) Bc250HostLossNote(s,reason,op,hr,report);
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
   return device_lost_status(hr);
}

// BD-179: TRUE when the adapter of this device left the system after the check had seen it there (adapter-presence.h).
// One thread reads the adapter list at most once in kAdapterCheckMs; every other call reads only the state.
static bool Bc250AdapterLeft(HostBridge *s)
{
   if (!s->adapter_luid) return false;
   std::atomic_ref<unsigned> state(s->adapter_state);
   const unsigned before=state.load(std::memory_order_acquire);
   if (before==kAdapterLeft) return true;
   std::atomic_ref<ULONGLONG> next(s->adapter_next_check_ms);
   const ULONGLONG now=GetTickCount64();
   ULONGLONG due=next.load(std::memory_order_relaxed);
   if (now<due || !next.compare_exchange_strong(due,now+kAdapterCheckMs,std::memory_order_relaxed)) return false;
   const AdapterPresence presence=probe_adapter_presence(s->adapter_luid,
      s->adapter_kmt ? *s->adapter_kmt : system_adapter_presence_kmt());
   if (presence==AdapterPresence::present) {
      unsigned unchecked=kAdapterUnchecked;
      (void)state.compare_exchange_strong(unchecked,kAdapterSeen,std::memory_order_acq_rel);
      return false;
   }
   if (presence!=AdapterPresence::gone || before!=kAdapterSeen) return false;
   state.store(kAdapterLeft,std::memory_order_release);
   static std::atomic_uint notes{16};
   device_life_note(notes,"BC250 BD-179: the adapter of this device left the system: luid=%llx runtime=%p identity=%p "
      "thread=%lu\n",(unsigned long long)s->adapter_luid,s->device ? s->device->hDevice : nullptr,
      s->device ? s->device->host_identity() : nullptr,(unsigned long)GetCurrentThreadId());
   return true;
}

static HRESULT Bc250HostStatus(HostBridge *s)
{
   // A loss already latched still needs its report when a check-type entry swallowed the first one.
   if (s->device_lost) return s->lost_reported ? D3DDDIERR_DEVICEREMOVED : Bc250HostLost(s,"an earlier loss of this device");
   for (const auto &p:s->progress) {
      if (!p.cpu_address) continue;
      UINT64 observed=*(const volatile UINT64 *)p.cpu_address;

      if (observed==UINT64_MAX) return Bc250HostLost(s,"progress fence reads the lost-device value");
   }
   if (s->present_cpu && *(const volatile UINT64 *)s->present_cpu==UINT64_MAX)
      return Bc250HostLost(s,"present fence reads the lost-device value");
   if (s->device->pagingFence && *s->device->pagingFence==UINT64_MAX)
      return Bc250HostLost(s,"paging fence reads the lost-device value");
   if (Bc250AdapterLeft(s)) return Bc250HostLost(s,"the adapter of this device left the system");
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
   if (op==BC250_HOST_REPORT_LOST) {
      // The argument is optional: an ICD before BD-258 passes 0.
      const auto *report=static_cast<const bc250_host_lost_report *>(argument);
      if (report && (report->sType!=BC250_HOST_LOST_REPORT_STYPE || report->size<sizeof(*report))) report=nullptr;
      return Bc250HostLost(s,"the ICD reports a loss",op,S_OK,report);
   }
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
      // BD-200: AssumeNotInUse is the one flag admitted (D3DDDICB_DESTROYALLOCATION2FLAGS, d3dukmdt.h line 579,
      // WDK 10.0.26100: AssumeNotInUse 0x1, SynchronousDestroy 0x2, Reserved, SystemUseOnly). The ICD sets it only
      // for a BO it held until the progress values captured at its destroy retired (radv_wddm2_bo.c,
      // buffer_destroy_queued). It goes to pfnDeallocate2Cb, whose D3DDDICB_DEALLOCATE2 carries the same flags
      // (d3dumddi.h line 4045); flags 0 keep the plain Deallocate, which is the same call with all flags clear
      // (d3dumddi.md, pfnDeallocate2Cb). Every other bit stays refused.
      D3DDDICB_DESTROYALLOCATION2FLAGS assume = {}; assume.AssumeNotInUse = 1;
      if (a->hResource || (a->Flags.Value & ~assume.Value)) return E_NOTIMPL;
      if (a->Flags.AssumeNotInUse) {
         D3DDDICB_DEALLOCATE2 b = {};
         b.NumAllocations=a->AllocationCount; b.HandleList=a->phAllocationList; b.Flags=assume;
         return HOST_CALL(Deallocate2, &b);
      }
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
      a->Info=b.Info; a->hSyncObject=b.hSyncObject;
      if (SUCCEEDED(hr)) Bc250RecordFence(s,b.hSyncObject,b.Info);
      return hr;
   }
   case BC250_HOST_DestroySynchronizationObject: {
      D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT b = {};
      b.hSyncObject=((D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *)argument)->hSyncObject;
      HRESULT hr=HOST_CALL(DestroySynchronizationObject, &b);
      if (SUCCEEDED(hr)) {
         for (auto &p:s->progress) if (p.sync==b.hSyncObject) p={};
         if (HostFence *f=Bc250FindFence(s,b.hSyncObject)) *f={};
      }
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
      // The CPU callback has no flags member. The ICD signals a value above the current
      // timeline value; its AllowFenceRewind bit needs no callback equivalent here.
      D3DDDICB_SIGNALFLAGS rewind = {}; rewind.AllowFenceRewind=1;
      if (a->Flags.Value & ~rewind.Value) return E_NOTIMPL;
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
      // Vulkan timeline wait-before-signal submits may queue a smaller value after a
      // larger one. Preserve the WDK AllowFenceRewind flag, as the D3D12 bridge does.
      D3DDDICB_SIGNALFLAGS rewind = {}; rewind.AllowFenceRewind=1;
      if (a->BroadcastContextCount > D3DDDI_MAX_BROADCAST_CONTEXT ||
          (a->Flags.Value & ~rewind.Value)) return E_NOTIMPL;
      HANDLE contexts[D3DDDI_MAX_BROADCAST_CONTEXT] = {};
      for (UINT i=0;i<a->BroadcastContextCount;++i) {
         contexts[i]=Bc250HostContext(s,a->BroadcastContextArray[i]); if (!contexts[i]) return E_INVALIDARG;
      }
      D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 b = {};
      b.ObjectCount=a->ObjectCount; b.ObjectHandleArray=a->ObjectHandleArray;
      b.BroadcastContextCount=a->BroadcastContextCount; b.BroadcastContextArray=contexts;
      b.MonitoredFenceValueArray=a->MonitoredFenceValueArray;
      b.Flags.AllowFenceRewind=a->Flags.AllowFenceRewind;
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
      ++s->gpu_work_calls;
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
      ++s->gpu_work_calls;
      return HOST_CALL(SubmitCommand, &b);
   }
   default: return E_NOTIMPL;
   }
#undef HOST_CALL
}

// BD-327: the D3DKMT_CREATEALLOCATIONFLAGS bits (d3dkmthk.h, SDK 10.0.26100) that a worker's allocation must not
// have: shared or cross-adapter memory, memory the caller already owns, a standard allocation, a swap chain buffer.
// CreateResource, NonSecure and ReadOnly stay allowed: hosted RADV sets CreateResource and NonSecure on each of its
// allocations (radv_wddm2_bo.c), and the runtime callback creates no runtime resource for them (hResource is 0).
static constexpr UINT kWorkerRefusedAllocationFlags=
   0x00000002u | // CreateShared
   0x00000008u | // CreateProtected
   0x00000010u | // RestrictSharedAccess
   0x00000020u | // ExistingSysMem
   0x00000040u | // NtSecuritySharing
   0x00000400u | // SwapChainBackBuffer
   0x00000800u | // CrossAdapter
   0x00001000u | // OpenCrossAdapter
   0x00002000u | // PartialSharedCreation
   0x00010000u | // StandardAllocation
   0x00020000u | // ExistingSection
   0x00200000u;  // SharedDisplayable
static_assert(sizeof(D3DKMT_CREATEALLOCATIONFLAGS)==sizeof(UINT));
bool worker_admits_call(uint32_t operation, const void *argument) noexcept
{
   if (!argument) return operation==BC250_HOST_CHECK_STATUS;
   switch (operation) {
   case BC250_HOST_CHECK_STATUS: return true;
   case BC250_HOST_CreateAllocation2: {
      const auto *a=static_cast<const D3DKMT_CREATEALLOCATION *>(argument);
      UINT flags=0; std::memcpy(&flags,&a->Flags,sizeof(flags));
      if ((flags & kWorkerRefusedAllocationFlags) || a->hResource || a->pPrivateRuntimeData || a->PrivateRuntimeDataSize ||
          a->NumAllocations!=1 || !a->pAllocationInfo2) return false;
      return a->pAllocationInfo2[0].pSystemMem==nullptr;
   }
   case BC250_HOST_DestroyAllocation2: {
      const auto *a=static_cast<const D3DKMT_DESTROYALLOCATION2 *>(argument);
      // BD-200: flags 0 or AssumeNotInUse alone, as the dispatch admits them.
      D3DDDICB_DESTROYALLOCATION2FLAGS assume = {}; assume.AssumeNotInUse = 1;
      return !a->hResource && !(a->Flags.Value & ~assume.Value);
   }
   case BC250_HOST_WaitForSynchronizationObjectFromCpu: {
      // The paging fence wait of an allocation: one object, no event (the call returns when the fence reached it).
      const auto *a=static_cast<const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *>(argument);
      return a->ObjectCount==1 && !a->hAsyncEvent;
   }
   case BC250_HOST_ReserveGpuVirtualAddress: case BC250_HOST_MapGpuVirtualAddress:
   case BC250_HOST_FreeGpuVirtualAddress: case BC250_HOST_MakeResident:
   case BC250_HOST_Lock2: case BC250_HOST_Unlock2:
      return true;
   default: return false;
   }
}
namespace {
// BD-327: host calls refused because the calling thread was in no scope of the device, by thread name (the
// description the thread set, for example dxvk-shader-n). Kept only while the process writes the AMDGPU_WDDM_LOG
// sink (the game log, both flavours), which is the only reader. A fixed table: the names after the first 15 count
// as "other". report_host_scope_calls prints it and starts it again.
struct RefusedByThread { char name[40]; unsigned count; unsigned last_op; };
std::mutex g_refused_mutex;
RefusedByThread g_refused[16];
unsigned g_refused_rows;
void note_refused_thread(uint32_t operation) noexcept
{
   if (!amdgpu_wddm_log::enabled()) return;
   char name[40]="unnamed";
   PWSTR wide=nullptr;
   if (SUCCEEDED(GetThreadDescription(GetCurrentThread(),&wide)) && wide) {
      if (wide[0] && !WideCharToMultiByte(CP_UTF8,0,wide,-1,name,sizeof(name),nullptr,nullptr))
         std::snprintf(name,sizeof(name),"unreadable");
      LocalFree(wide);
   }
   // One word in the line: the list separates its entries with spaces and a count with '='.
   for (char *p=name;*p;++p) if (*p==' ' || *p=='=' || *p=='(' || static_cast<unsigned char>(*p)<0x20) *p='_';
   std::lock_guard<std::mutex> lock(g_refused_mutex);
   for (unsigned i=0;i<g_refused_rows;++i) {
      if (!std::strcmp(g_refused[i].name,name)) { ++g_refused[i].count; g_refused[i].last_op=operation; return; }
   }
   const unsigned other=sizeof(g_refused)/sizeof(g_refused[0])-1;
   const bool named=g_refused_rows<other;
   RefusedByThread &row=g_refused[named ? g_refused_rows++ : other];
   if (!row.count) std::snprintf(row.name,sizeof(row.name),"%s",named ? name : "other");
   ++row.count; row.last_op=operation;
}
}
void report_host_scope_calls(HostBridge &bridge) noexcept
{
   if (!amdgpu_wddm_log::enabled()) return;
   char list[512]; size_t used=0; list[0]=0;
   {
      std::lock_guard<std::mutex> lock(g_refused_mutex);
      for (const auto &row:g_refused) {
         if (!row.count || used>=sizeof(list)-1) continue;
         const int n=std::snprintf(list+used,sizeof(list)-used,"%s%s=%u(op %u)",used ? " " : "",row.name,row.count,
            row.last_op);
         if (n>0) used=(std::min)(sizeof(list)-1,used+size_t(n));
      }
      for (auto &row:g_refused) row={};
      g_refused_rows=0;
   }
   // One line for each DestroyDevice, with no bound of its own: kit-bd327-diag counts these lines.
   amdgpu_wddm_log::print("BC250 BD-327: host scope calls: device=%p runtime=%p identity=%p worker admitted=%u refused=%u "
      "gate=%s; refused outside a scope since the last line, by thread: %s\n",
      static_cast<void *>(bridge.device),bridge.device ? bridge.device->hDevice : nullptr,
      bridge.device ? bridge.device->host_identity() : nullptr,std::atomic_ref<unsigned>(bridge.worker_calls).load(std::memory_order_relaxed),
      std::atomic_ref<unsigned>(bridge.worker_refused).load(std::memory_order_relaxed),
      bridge.device && bridge.device->workers.revoked() ? "revoked" : "open",used ? list : "none");
}
// BD-327: a call of a pipeline worker of this device's engine (EngineWorkerGate). It runs outside the RuntimeDomain:
// no runtime error callback (pfnSetErrorCb belongs to a DDI thread), no loss latch and no call counter of the DDI
// path. A lost-device status goes back to hosted RADV as STATUS_DEVICE_REMOVED; the next DDI entry of the device
// finds the loss itself (Bc250HostStatus reads the same fences).
static int32_t Bc250WorkerDispatch(HostBridge &s, uint32_t operation, void *argument)
{
   std::atomic_ref<unsigned> refused(s.worker_refused);
   if (!worker_admits_call(operation,argument)) {
      refused.fetch_add(1,std::memory_order_relaxed);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
      static std::atomic_uint notes{16};
      device_life_note(notes,"BC250 BD-327: worker host op %u refused: not a driver-internal memory call: runtime=%p "
         "identity=%p thread=%lu\n",operation,s.device->hDevice,s.device->host_identity(),
         (unsigned long)GetCurrentThreadId());
#endif
      return RuntimeDomain::wrong_domain;
   }
   if (operation==BC250_HOST_CHECK_STATUS) {
      // Read-only: a loss already latched, or the kernel's proof that the device's work ended.
      const bool lost=std::atomic_ref<bool>(s.device_lost).load(std::memory_order_relaxed) ||
         kernel_ended_device_work(s);
      return lost ? (int32_t)0xc00002b6 : 0;
   }
   EngineWorkerGate &gate=s.device->workers;
   if (!gate.enter()) {
      refused.fetch_add(1,std::memory_order_relaxed);
      return RuntimeDomain::wrong_domain;
   }
   const HRESULT hr=Bc250HostOperation(&s,operation,argument);
   gate.leave();
   std::atomic_ref<unsigned>(s.worker_calls).fetch_add(1,std::memory_order_relaxed);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
   // The positive witness of BD-327: the host scope line comes only at DestroyDevice, and a game can end with its
   // device alive (RotTR, kit-bd327-diag5 arm G), so the first admitted calls of the process also get a line of
   // their own. kit-bd327-diag6 counts these lines by runtime.
   static std::atomic_uint admitted{16};
   device_life_note(admitted,"BC250 BD-327: worker host op %u admitted: hr=%08X runtime=%p identity=%p thread=%lu\n",
      operation,static_cast<unsigned>(hr),s.device->hDevice,s.device->host_identity(),
      (unsigned long)GetCurrentThreadId());
#endif
   if (Bc250DeviceLostResult(hr)) return (int32_t)0xc00002b6;
   if (hr==E_PENDING && (operation==BC250_HOST_MapGpuVirtualAddress || operation==BC250_HOST_MakeResident)) return 0x103;
   return SUCCEEDED(hr) ? 0 : hr==E_NOTIMPL ? (int32_t)0xc00000bb :
          hr==E_OUTOFMEMORY ? (int32_t)0xc0000017 :
          hr==E_INVALIDARG ? (int32_t)0xc000000d : (int32_t)0xc0000001;
}

int32_t host_dispatch(void *userdata, uint32_t operation, void *argument)
{
   auto *s=(HostBridge *)userdata;
   if (s && s->device && !s->device->domain.entered() && s->device->workers.bound())
      return Bc250WorkerDispatch(*s,operation,argument);
   if (!s || !s->device || !s->device->domain.entered()) {
      note_refused_thread(operation);
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
      // BD-253, lab flavour (BD-167): a call that reached a device whose runtime scope this thread has not
      // entered, for example a winsys that the hosted ICD kept for another device with the same identity.
      static std::atomic_uint wrong{16};
      device_life_note(wrong,"BC250 BD-253: host op %u refused: not in this device's runtime scope: runtime=%p "
         "identity=%p thread=%lu\n",operation,s && s->device ? s->device->hDevice : nullptr,
         s && s->device ? s->device->host_identity() : nullptr,(unsigned long)GetCurrentThreadId());
#endif
      return (int32_t)0xc000000d;
   }
   HRESULT hr=Bc250HostOperation(s,operation,argument);
   if (Bc250DeviceLostResult(hr)) {
      Bc250HostLost(s,"a host operation returned a lost-device status",operation,hr);
      return (int32_t)0xc00002b6;
   }
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
   // BD-253, lab flavour: each other failed operation, at most 32 lines per process. E_PENDING of a mapping or a
   // residency request is the normal paging answer, not a failure.
   if (FAILED(hr) && hr!=E_NOTIMPL && !(hr==E_PENDING && (operation==BC250_HOST_MapGpuVirtualAddress ||
       operation==BC250_HOST_MakeResident))) {
      static std::atomic_uint failed{32};
      device_life_note(failed,"BC250 BD-253: host op %u failed hr=%08X runtime=%p identity=%p thread=%lu\n",
         operation,unsigned(hr),s->device->hDevice,s->device->host_identity(),(unsigned long)GetCurrentThreadId());
   }
#endif
   if (FAILED(hr) && (operation==BC250_HOST_SubmitCommand || operation==BC250_HOST_SignalSynchronizationObjectFromGpu2 || operation==BC250_HOST_UpdateGpuVirtualAddress || operation==BC250_HOST_PUBLISH_PROGRESS)) {
      // BD-253: in both flavours, one line when a submission-path failure first stops this device's Present.
      if (!s->submission_failed) {
         static std::atomic_uint stops{16};
         device_life_note(stops,"BC250 BD-253: submission stopped: host op %u failed hr=%08X runtime=%p identity=%p "
            "thread=%lu\n",operation,unsigned(hr),s->device->hDevice,s->device->host_identity(),
            (unsigned long)GetCurrentThreadId());
      }
      s->submission_failed=true;
   }
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

bool settle_device_work(HostBridge &bridge, DWORD timeout_ms)
{
   if (kernel_ended_device_work(bridge)) return true;
   if (device_queued_no_work(bridge)) {
      // BD-198: one line for each such device, in both flavours, in place of the wait line.
      if (!bridge.drain_tried) {
         bridge.drain_tried=true;
         static std::atomic_uint none{16};
         device_life_note(none,"BC250 BD-198: lost device queued no GPU work: no wait, its memory is free at once: "
            "runtime=%p identity=%p\n",bridge.device ? bridge.device->hDevice : nullptr,
            bridge.device ? bridge.device->host_identity() : nullptr);
      }
      return true;
   }
   if (!timeout_ms || bridge.drain_tried) return false;
   bridge.drain_tried=true;
   const ULONGLONG start=GetTickCount64();
   bool drained=false;
   while (!drained && GetTickCount64()-start<timeout_ms) { Sleep(1); drained=kernel_ended_device_work(bridge); }
   static std::atomic_uint notes{16};
   device_life_note(notes,"BC250 BD-253: lost device work %s after %llu ms: kernel-ended=%u runtime=%p identity=%p\n",
      drained ? "ended" : "did not end",(unsigned long long)(GetTickCount64()-start),
      unsigned(kernel_ended_device_work(bridge)),bridge.device ? bridge.device->hDevice : nullptr,
      bridge.device ? bridge.device->host_identity() : nullptr);
   return drained;
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
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(s,"present fence wait",~0u,hr);
   return SUCCEEDED(hr) ? Bc250HostStatus(s) : hr;
}

HRESULT present_runtime(HostBridge &bridge, D3DKMT_HANDLE source,
    D3DKMT_HANDLE destination, void *dxgi_context, FlushEngine flush, void *engine,
    PresentSyncOverride sync, HRESULT *callback, char *step)
{
   if (callback) *callback=S_OK;
   char ignored=0; char &at=step ? *step : ignored;
   at=0;
   auto *device=bridge.device;
   if (!device || !device->domain.entered() || !device->present_context || !source || !flush)
      return E_INVALIDARG;
   if (!device->DXGICallbacks || !device->DXGICallbacks->pfnPresentCb) return E_NOTIMPL;
   at='H';
   if (FAILED(Bc250HostStatus(&bridge)) || bridge.submission_failed)
      return DXGI_ERROR_DEVICE_REMOVED;
   at='F';
   HRESULT hr=flush(engine);
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(&bridge,"Present: engine flush",~0u,hr);
   if (FAILED(hr)) return hr;
   at='W';
   hr=queue_present_wait(bridge);
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(&bridge,"Present: queue wait",~0u,hr);
   if (FAILED(hr)) return hr;
   at='P';
   DXGIDDICB_PRESENT present={};
   present.hSrcAllocation=source;
   present.hDstAllocation=destination;
   present.hContext=device->present_context;
   present.pDXGIContext=dxgi_context;
   if (sync.valid) {
      present.SyncIntervalOverrideValid=TRUE;
      present.SyncIntervalOverride=sync.interval;
   }
   ++bridge.gpu_work_calls;
   const HRESULT result=device->DXGICallbacks->pfnPresentCb(device->hDevice,&present);
   if (callback) *callback=result;
   if (Bc250DeviceLostResult(result)) return Bc250HostLost(&bridge,"Present: present callback",~0u,result);
   if (FAILED(result)) return result;
   at='S';
   hr=signal_present(bridge);
   if (Bc250DeviceLostResult(hr)) return Bc250HostLost(&bridge,"Present: present fence signal",~0u,hr);
   if (FAILED(hr)) return hr;
   at=0;
   return result;
}

}
