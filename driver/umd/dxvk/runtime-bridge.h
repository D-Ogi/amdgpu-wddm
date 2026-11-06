// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <winternl.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#include "runtime-domain.h"
#include "engine-worker-gate.h"
#include "host-bootstrap.h"
#include "adapter-presence.h"
#include <atomic>
namespace bc250::umd {
struct RuntimeDevice {
    RuntimeDomain domain;
    // BD-327: the engine's pipeline workers of this device (engine-worker-gate.h). The engine's Services.Shell is
    // this object, so that its WorkerScope service finds the gate.
    EngineWorkerGate workers;
    HANDLE hDevice = nullptr;
    HANDLE present_context = nullptr;
    D3D10DDI_HRTCORELAYER hRTCoreLayer = {};
    volatile UINT64 *pagingFence = nullptr;
    // Runtime may replace entries between DDI calls; retain its table, not a copy.
    const DXGI_DDI_BASE_CALLBACKS *DXGICallbacks = nullptr;
    D3DDDI_DEVICECALLBACKS KTCallbacks = {};
    D3D10DDI_CORELAYER_DEVICECALLBACKS UMCallbacks = {};
    // BD-253: the identity the hosted ICD knows this device by (bc250_host.identity, bc250_host_import.identity).
    // The ICD keeps one winsys for each identity in the process and gives it again to a new instance with the same
    // identity (radv_wddm2_winsys_create). The runtime's hDevice cannot be that identity: the runtime can give
    // the same handle to a new device after it destroys the old one, while the old device's instance and winsys
    // stay alive because its cleanup failed (retire_device_handle keeps such an owner). The new device then gets
    // the old winsys, with the old device's paging queue and host callbacks. The address of this object does not
    // have that problem: the owner that holds it is not freed while an instance of it is alive.
    // legacy_host_identity (the experiment switch "host-identity-off") gives hDevice as before, for a lab
    // comparison.
    bool legacy_host_identity = false;
    void *host_identity() { return legacy_host_identity ? hDevice : static_cast<void *>(this); }
};
// BD-198: a monitored fence that the bridge created for the hosted ICD, with the value its CPU address gave right
// after the creation. The loss line names the fence of the read that saw the lost-device value with these fields, and
// counts the recorded fences that read that value at the loss. The record goes when the ICD destroys the fence.
struct HostFence {
   D3DKMT_HANDLE handle;
   const volatile UINT64 *cpu;
   UINT64 initial, at_create;
   ULONGLONG created_ms;
};
inline constexpr unsigned kHostFenceRecords=32;
// Zero initialize; lifetime and cleanup belong to the UMD device. Context tokens
// are bridge-local, not kernel handles. No DXVK worker may enter a runtime scope: a pipeline worker gets only the
// calls of host_dispatch's worker path (BD-327, RuntimeDevice::workers).
struct HostBridge {
   RuntimeDevice *device;
   HANDLE contexts[16];
   unsigned calls[64];
   bc250_host_progress progress[16];
   bool submission_failed;
   bool device_lost;
   // The loss has reached the runtime through pfnSetErrorCb. A check-type entry may not report device
   // removal, so a loss detected inside one stays unreported until an entry whose page allows it runs.
   bool lost_reported;
   const UINT64 *present_cpu;
   D3DKMT_HANDLE present_sync;
   UINT64 present_value, present_waited[16];
   // BD-253: settle_device_work did its one bounded wait for this device. The answer itself is not kept: each call
   // reads the fences again.
   bool drain_tried;
   // BD-198: the calls of this device that queued GPU work which reads or writes the device's memory: SubmitCommand
   // and UpdateGpuVirtualAddress of the hosted ICD, and the present callback. Each call counts before it reaches the
   // runtime, whatever its status, so a refused call also counts. Fence signals and fence waits do not count: they
   // touch only the fence storage of the kernel. The kernel's own paging work (MakeResident) does not count either:
   // VidMm defers the destruction of an allocation until its paging work is done (allocation-usage-tracking.md).
   unsigned gpu_work_calls;
   HostFence fences[kHostFenceRecords];
   // Monitored fences that got no record because every record was in use.
   unsigned fences_unrecorded;
   // BD-179: the LUID of the adapter that runs this device (host_descriptor sets it), and the check that the
   // adapter is still in the system (adapter-presence.h). adapter_state is one of AdapterCheck; the check runs at
   // most once in kAdapterCheckMs, from the host status. Several threads read and write both fields, always through
   // std::atomic_ref: the bridge stays a plain structure that a close can reset with an assignment.
   UINT64 adapter_luid;
   ULONGLONG adapter_next_check_ms;
   unsigned adapter_state;
   // The kernel entries of the check; null means those of gdi32.dll. Only the tests set it.
   const AdapterPresenceKmt *adapter_kmt;
   // BD-327: host calls of the engine's pipeline workers of this device that reached the runtime, and those that
   // the worker path refused (an operation outside worker_admits_call, or a gate that close revoked). Workers and
   // DDI threads read and write them through std::atomic_ref, as adapter_state.
   unsigned worker_calls;
   unsigned worker_refused;
};
// BD-327: TRUE when a pipeline worker of the engine may make this host call: driver-internal memory work of
// hosted RADV only. A non-shared allocation with no runtime resource and no existing memory, its destruction, its
// GPU addresses, residency and CPU mapping, the synchronous wait for the paging fence of that work, and the status
// check. No submission, context, sync object creation, present, progress publication or loss report.
bool worker_admits_call(uint32_t operation, const void *argument) noexcept;
// BD-327: one line for the device (DestroyDevice): its worker host calls, and the host calls that threads outside a
// scope made since the last such line, by thread name. Both flavours write it, to the game log only. The lab flavour
// also writes a note for each of the first 16 worker calls that the filter refuses.
void report_host_scope_calls(HostBridge &bridge) noexcept;
// BD-179: the states of the adapter check. The check is armed only when it first finds the adapter of the device in
// the list: a LUID that the list never had (a wrong LUID, a list that this process cannot read) never reports a loss.
enum AdapterCheck : unsigned { kAdapterUnchecked=0, kAdapterSeen=1, kAdapterLeft=2 };
inline constexpr ULONGLONG kAdapterCheckMs=500;
// BD-179: TRUE when the adapter of this device left the system after the check had seen it.
inline bool adapter_left(const HostBridge &bridge) {
   return std::atomic_ref<unsigned>(const_cast<unsigned &>(bridge.adapter_state)).load(std::memory_order_acquire)==
          kAdapterLeft;
}
// Latch a loss whose status the caller already carries: the entry that got it reports it through its
// own error class, so the bridge must not report it a second time.
inline void latch_device_lost(HostBridge &bridge) { bridge.device_lost=true; bridge.lost_reported=true; }
// BD-253: TRUE when the kernel has put this device in its error state: a monitored fence of the bridge (the present
// fence or a progress fence of the hosted ICD) reads the lost-device value, which only the kernel writes, and only
// when it ends the device's work. A latched loss alone is not this proof: a refused submission or a wait that
// timed out also latches one while earlier work can still run.
// BD-179: the adapter of the device left the system (adapter_left) is the other proof. The kernel stopped that
// adapter, and the stop resets it (adapter-presence.h), so no work of the device runs. The fences of such a device
// never read the lost-device value, and without this proof each device of the old adapter would stay kept until
// kKeptDeviceLimit refused every new device of the process, also on the new adapter.
inline bool kernel_ended_device_work(const HostBridge &bridge) {
   if (adapter_left(bridge)) return true;
   if (bridge.present_cpu && *(const volatile UINT64 *)bridge.present_cpu==UINT64_MAX) return true;
   for (const auto &p:bridge.progress)
      if (p.cpu_address && *(const volatile UINT64 *)p.cpu_address==UINT64_MAX) return true;
   return false;
}
// BD-198: TRUE when this device never queued GPU work that uses its memory (HostBridge::gpu_work_calls). Then no work
// of the device can run, so a lost device can free its memory at once. Windows Terminal and the XAML of explorer lose a
// device at its first submission, before the submission reaches the kernel: the ICD reads the lost-device value in
// the progress fence and stops (radv_wddm2_cs.c, "progress fence before submit"). Such a device has no published
// progress record, so kernel_ended_device_work cannot give its proof, and before this check each such device cost a
// 2 s wait in DestroyDevice and stayed kept for the life of the process.
inline bool device_queued_no_work(const HostBridge &bridge) { return bridge.gpu_work_calls==0; }
// BD-258: the last loss that a bridge of this process latched, for the line of a CreateDevice refusal. The texts are
// copies: the site text belongs to the ICD module, which a closed device unloads.
struct DeviceLossRecord {
   char reason[64];
   char site[48];
   unsigned op;
   HRESULT hr;
   ULONGLONG at_ms;
   unsigned losses;
};
DeviceLossRecord last_device_loss() noexcept;
// The statuses that report a lost device.
inline bool device_lost_status(HRESULT hr) {
   return hr==D3DDDIERR_DEVICEREMOVED || hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET ||
          hr==DXGI_ERROR_DEVICE_HUNG || hr==DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
// BD-253: TRUE when no work of a lost device runs any more, so that its close can free what the work used. There are
// two proofs: kernel_ended_device_work, and device_queued_no_work (BD-198), which needs no wait. The values of the host records (bc250_host_progress, present_value) are not a
// proof: the ICD publishes a progress value only after its submission and its signal succeeded
// (radv_wddm2_progress_signalled), so a record can stay at k-1 while an accepted submission k still runs (an accepted
// IB whose signal or publish failed). Each call reads the fences again, so a later kernel error is seen and an earlier
// TRUE is not kept. With timeout_ms nonzero, the first such call of a bridge waits at most timeout_ms for the proof;
// every other call returns at once. Only DeviceOwner::close waits: the other callers (DestroyResource, Present) run on
// the threads of the application's UI. A device without the proof stays kept (retire_device_handle), as before. One
// line for each wait, in both flavours.
bool settle_device_work(HostBridge &bridge, DWORD timeout_ms);
using FlushEngine = HRESULT (*)(void *);
// The per-application VSync setting (docs/design/per-app-graphics-settings.md): when valid, the Present callback
// carries interval as DXGIDDICB_PRESENT.SyncIntervalOverride in place of the application's sync interval.
struct PresentSyncOverride { bool valid=false; DXGI_DDI_FLIP_INTERVAL_TYPE interval=DXGI_DDI_FLIP_INTERVAL_IMMEDIATE; };
// callback, when given, receives the status of pfnPresentCbDXGI, or S_OK when the present did not reach it: the runtime
// keeps the kernel status of that callback for itself, so the DDI entry must know which step failed (present-contract.h).
// step, when given, receives the step that ended the present: 'H' a lost host or a failed submission before the first
// step, 'F' the flush, 'W' the GPU wait, 'P' the callback, 'S' the fence signal, or 0 when each step succeeded.
HRESULT present_runtime(HostBridge &bridge, D3DKMT_HANDLE source,
    D3DKMT_HANDLE destination, void *dxgi_context, FlushEngine flush, void *engine,
    PresentSyncOverride sync=PresentSyncOverride{}, HRESULT *callback=nullptr, char *step=nullptr);
HRESULT queue_present_wait(HostBridge &bridge);
HRESULT signal_present(HostBridge &bridge);
// Destruction/readback only. Steady-state Present uses GPU waits above.
HRESULT wait_present_idle(HostBridge &bridge);
// CPU wait until the Present that signalled value has retired; value must be one this bridge signalled.
// Returns at once when the fence already reached it. Used before the engine writes a Present shadow again.
HRESULT wait_present_value(HostBridge &bridge, UINT64 value);
int32_t host_dispatch(void *userdata, uint32_t operation, void *argument);
// Descriptor is copied by the hosted ICD, but userdata and device remain borrowed.
// Only call with a live, initialized bridge. Device destruction must first drain
// the engine and destroy its Vulkan objects while this callback is still valid.
inline bc250_host host_descriptor(HostBridge &bridge, uint64_t adapter_luid) {
    bc250_host host{};
    host.sType = BC250_HOST_STYPE;
    host.version = BC250_HOST_VERSION;
    host.size = sizeof(host);
    host.adapter_luid = adapter_luid;
    bridge.adapter_luid = adapter_luid;
    host.identity = bridge.device->host_identity();
    host.userdata = &bridge;
    host.dispatch = host_dispatch;
    return host;
}
}
