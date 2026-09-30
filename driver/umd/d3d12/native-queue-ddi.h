// SPDX-License-Identifier: MIT
#pragma once
#include "queue-engine.h"

namespace native12 {
// Called on runtime-owned, live queue storage. Validates the slot's registry
// membership and serial; does not turn arbitrary stale memory into a safe handle.
Device* resolve_queue_device(D3D12DDI_HCOMMANDQUEUE) noexcept;
// Present: the native context of the queue, which must be a live queue of the given device.
HRESULT queue_present_context(D3D12DDI_HCOMMANDQUEUE, Device& expected, HANDLE* context) noexcept;
void install_native_queue_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088&) noexcept;
HRESULT fill_native_queue_table(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001*,
    SIZE_T = sizeof(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001)) noexcept;
HRESULT fill_native_extended_table(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021*,
    SIZE_T = sizeof(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021)) noexcept;
} // namespace native12
