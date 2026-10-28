// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <winternl.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#include "runtime-domain.h"
#include "host-bootstrap.h"
namespace bc250::umd {
struct RuntimeDevice {
    RuntimeDomain domain;
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
// Zero initialize; lifetime and cleanup belong to the UMD device. Context tokens
// are bridge-local, not kernel handles. No DXVK worker may enter a runtime scope.
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
};
// Latch a loss whose status the caller already carries: the entry that got it reports it through its
// own error class, so the bridge must not report it a second time.
inline void latch_device_lost(HostBridge &bridge) { bridge.device_lost=true; bridge.lost_reported=true; }
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
    host.identity = bridge.device->host_identity();
    host.userdata = &bridge;
    host.dispatch = host_dispatch;
    return host;
}
}
