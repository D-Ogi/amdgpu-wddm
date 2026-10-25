// SPDX-License-Identifier: MIT
#pragma once
#include "harness.h"
#if defined(D3D12DDI_BUILD_VERSION_0119)
#include "ddi-modern-core.h"

namespace harness {
// Every test in one process publishes the same immutable CORE callbacks and
// error hook. A separate local hook in each test would fail the identity gate.
inline void APIENTRY modern_core_device_error(D3D12DDI_HDEVICE device, HRESULT hr) {
    auto* shell = static_cast<ShellDevice*>(device.pDrvPrivate);
    if (shell && shell->magic == kShellDeviceMagic && shell->context) shell->context->report(hr);
}
inline bool modern_core_table(const Env& env, D3D12DDI_DEVICE_FUNCS_CORE_0119& core) {
    return native12::upgrade_core(env.core, &core, modern_core_device_error, engine_ddi::modern_core_entries());
}
} // namespace harness
#endif
