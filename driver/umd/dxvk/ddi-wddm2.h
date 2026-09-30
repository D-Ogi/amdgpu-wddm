// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
// The WDDM 2.0 D3D11 table (FL12_0/12_1). It extends the D3D11.1 table: the same
// entries at the same offsets, twelve of them retyped (Flush, RelocateDeviceFuncs,
// SRV/RTV/UAV with a plane slice, rasterizer with conservative mode, query with a
// context type) and thirteen appended (tiled resources, markers, content protection,
// resource layout, shader comments). Offered only by an FL12 adapter.
D3DWDDM2_0DDI_DEVICEFUNCS make_wddm2_0_device_table();
inline constexpr bool dxgi1_4_table_layout_supported(UINT interfaceVersion,[[maybe_unused]] UINT buildVersion) {
    return IS_DXGI1_4_BASE_FUNCTIONS(interfaceVersion,buildVersion);
}
// The DXGI 1.4 table paired with it: the DXGI 1.2 entries, Present1 through Present,
// and the WDDM 1.3/2.0 additions without multiplane overlays.
DXGI1_4_DDI_BASE_FUNCTIONS make_dxgi1_4_device_table();
}
