// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
// Builds the implemented rendering entries from a zeroed table. Still not a
// publishable device table: shared imports, DXGI and remaining
// DDI requirements must be completed before OpenAdapter advertises it.
D3D11_1DDI_DEVICEFUNCS make_render_device_table();
}
