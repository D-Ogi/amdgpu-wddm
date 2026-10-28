// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "ddi-error-policy.h"
namespace bc250::umd {
// BD-170. What PresentDXGI (and Present1, which forwards to it) may return: S_OK or D3DDDIERR_DEVICEREMOVED, and
// nothing else. The DDI pages name no status for this entry; the runtime's own code does. d3d11.dll 10.0.22621.5415
// (unit A; the same code as 10.0.22621.6493), NDXGI::CDevice::PresentImpl: after the driver's Present returns,
// D3DDDIERR_DEVICEREMOVED removes the device, and every other status but S_OK, a success code such as S_FALSE
// included, calls NDXGI::CDevice::DriverInternalError. That removes the device with the reason
// DXGI_ERROR_DRIVER_INTERNAL_ERROR, and IDXGISwapChain::Present returns DXGI_ERROR_DEVICE_REMOVED. This is a
// decision of the user-mode runtime: no TDR, no kernel event, no system log record.
//
// The status of pfnPresentCbDXGI is not the driver's to return. The runtime's PresentCB keeps the kernel status of
// the present in its thread data, and PresentImpl maps it after the driver returns S_OK: occluded, mode changed,
// still drawing, and an invalid parameter as S_OK or DXGI_ERROR_INVALID_CALL. A refused callback therefore
// returns S_OK from the entry. A lost device stays a lost device. A failure of the shell's own steps (flush, the
// GPU wait before the present, the fence signal after it, the arguments) is a status this entry cannot carry, so
// it is device removal, the strongest status the entry allows.
//
// BD-253: without this rule, a refused callback removed the device. GOG Galaxy (Qt 6.7 RHI, flip-model swap chains)
// lost its D3D11 device on each Present after its window came back from the tray, and a new device lost itself the
// same way at its first Present, with no GPU fault and no TDR. The kernel can refuse a present, for example one to
// an occluded window; the runtime reports that refusal to the application itself (PresentImpl maps
// STATUS_GRAPHICS_PRESENT_OCCLUDED and the other present statuses), so it is no reason to remove the device.
//
// result: the status of the entry's work; callback: the status pfnPresentCbDXGI returned, S_OK where the entry
// did not reach it (present_runtime).
constexpr HRESULT present_ddi_status(HRESULT result,HRESULT callback) {
    if (ddi_device_status(result)==D3DDDIERR_DEVICEREMOVED) return D3DDDIERR_DEVICEREMOVED;
    if (SUCCEEDED(result)) return S_OK;
    if (FAILED(callback) && result==callback) return S_OK;
    return D3DDDIERR_DEVICEREMOVED;
}
// True where present_ddi_status gives S_OK for a failed present: the runtime got a refusal from the kernel.
constexpr bool present_callback_refused(HRESULT result,HRESULT callback) {
    return FAILED(result) && present_ddi_status(result,callback)==S_OK;
}
}
