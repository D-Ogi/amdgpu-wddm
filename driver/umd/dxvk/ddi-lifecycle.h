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
}
