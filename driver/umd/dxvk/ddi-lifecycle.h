// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
// The handle must own a heap-allocated DeviceOwner. Failure retains ownership;
// callers must retain engine/ICD module references too until cleanup succeeds.
HRESULT retire_device_handle(DdiDeviceHandle &handle) noexcept;
void install_lifecycle_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
