// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
// The handle must own a heap-allocated DeviceOwner. Failure retains ownership;
// the owner also keeps its module references until cleanup succeeds.
HRESULT retire_device_handle(DdiDeviceHandle &handle) noexcept;
void install_lifecycle_ddi(D3D11_1DDI_DEVICEFUNCS &table);
// BD-253: the runtime device handles of the owners this process keeps because their cleanup failed (a failed
// DestroyDevice, a failed CreateDevice whose cleanup failed too). Such an owner keeps its hosted instance and
// winsys, while the runtime can give the same handle to a new device. The CreateDevice line says whether a new
// device got such a handle. The record holds a bounded number of handles, the oldest first; a handle past the
// bound is not recorded, which only makes the line say "no".
void note_retained_runtime_handle(HANDLE runtime) noexcept;
bool retained_runtime_handle(HANDLE runtime) noexcept;
unsigned retained_device_count() noexcept;
// BD-253: the second bound of the kept devices. A lost device whose work ended is freed (DeviceOwner::close), so a
// kept device is one whose cleanup failed for another reason, or whose work did not end in the bounded wait. Each
// one keeps its hosted instance, its winsys, its kernel objects and the ICD's threads for the life of the process.
// An application that creates a device again after each loss (the XAML of explorer, BD-253 tester.34) would add one
// for each loss, without end. When the process has kept this number of devices, CreateDevice fails at once
// (kKeptDeviceStatus), so the application stops or uses its fallback. The count only goes up: a kept device that a
// later retire frees still counts. 8 permits a few recoveries from real losses (each one TDR trial gives one loss),
// and at 8 the process holds at most 8 hosted instances, with up to 4 cache threads in each.
inline constexpr unsigned kKeptDeviceLimit=8;
// The status of that refusal. E_FAIL, not DXGI_ERROR_DEVICE_REMOVED: in tester.33, a CreateDevice that failed with
// E_FAIL (a vkCreateDevice that failed on the winsys of a kept device) stopped the XAML retries of explorer. A device
// removed status tells the application that a device was lost, and a lost device is what started that loop.
inline constexpr HRESULT kKeptDeviceStatus=E_FAIL;
bool kept_device_limit_reached() noexcept;
// BD-258: the third bound, of the devices that are lost soon after their creation. In tester.34 explorer created a
// second device, the kernel put that device in error after its first submission, and explorer created a new device
// after each loss: more than 1000 devices in one lab capture, each one closed without error, so the kept-device bound
// above did not stop it. A retired device is a short lost device when its loss was latched and it lived less than
// kShortLostDeviceMs from CreateDevice to DestroyDevice. A retired device that is not one resets the count. When the
// count gets to kShortLostDeviceLimit, CreateDevice fails at once with kKeptDeviceStatus, as at the kept-device bound.
// The count only goes down at a retire, and a refused CreateDevice retires nothing, so the refusal stays for the life
// of the process. A loss after a long life (one TDR in a game) resets the count instead, so a process can recover from
// any number of such losses.
inline constexpr unsigned kShortLostDeviceLimit=8;
inline constexpr ULONGLONG kShortLostDeviceMs=10000;
// lost: the loss was latched before the close. lifetime_ms: from CreateDevice to DestroyDevice.
void note_retired_device(bool lost,ULONGLONG lifetime_ms) noexcept;
unsigned short_lost_device_count() noexcept;
bool short_lost_device_limit_reached() noexcept;
}
