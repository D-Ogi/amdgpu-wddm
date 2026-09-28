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
    DXGI_DDI_BASE_CALLBACKS DXGICallbacks = {};
    D3DDDI_DEVICECALLBACKS KTCallbacks = {};
    D3D10DDI_CORELAYER_DEVICECALLBACKS UMCallbacks = {};
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
   const UINT64 *present_cpu;
   D3DKMT_HANDLE present_sync;
   UINT64 present_value, present_waited[16];
};
using FlushEngine = HRESULT (*)(void *);
HRESULT present_runtime(HostBridge &bridge, D3DKMT_HANDLE source,
    D3DKMT_HANDLE destination, void *dxgi_context, FlushEngine flush, void *engine);
HRESULT queue_present_wait(HostBridge &bridge);
HRESULT signal_present(HostBridge &bridge);
// Destruction/readback only. Steady-state Present uses GPU waits above.
HRESULT wait_present_idle(HostBridge &bridge);
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
    host.identity = bridge.device->hDevice;
    host.userdata = &bridge;
    host.dispatch = host_dispatch;
    return host;
}
}
