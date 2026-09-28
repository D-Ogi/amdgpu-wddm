// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-device-create.h"
namespace bc250::umd {
// The deployment layer supplies artifact-bound capabilities and absolute DLL
// paths. This transaction does not guess caps or load Vulkan at adapter open.
struct AdapterConfiguration {
    const wchar_t *engine_path;
    const wchar_t *icd_path;
    AdapterCaps caps;
};
HRESULT open_render_adapter(D3D10DDIARG_OPENADAPTER &,const AdapterConfiguration &) noexcept;
}
