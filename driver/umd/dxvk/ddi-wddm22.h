// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
// The WDDM 2.2 D3D11 table (BD-099). It extends the WDDM 2.0 table: the same entries at the same offsets,
// one of them retyped (RelocateDeviceFuncs, which now carries this table), and six appended - the two sync
// tokens of WDDM 2.1 (AcquireResource, ReleaseResource) and the four shader-cache-session entries of
// WDDM 2.2. Offered only by an FL12 adapter, and only while the process does not ask for the WDDM 2.0
// interface (ddi-experiment.h).
//
// Why the shell offers it at all: the D3D11 runtime hands the driver the whole DXGIDDICB_PRESENT, with
// SyncIntervalOverrideValid and SyncIntervalOverride in it, only at this interface and build. Below it the
// runtime copies a shorter structure, so the per-application VSync of a D3D11 program needed vertical-blank
// waits of the shell instead (docs/design/per-app-graphics-settings.md, vblank-pacer.h).
D3DWDDM2_2DDI_DEVICEFUNCS make_wddm2_2_device_table();
inline constexpr bool dxgi1_6_1_table_layout_supported(UINT interfaceVersion,UINT buildVersion) {
    return IS_DXGI1_6_1_BASE_FUNCTIONS(interfaceVersion,buildVersion<<16);
}
// The DXGI table paired with it: the DXGI 1.4 entries, with OfferResources1 in place of OfferResources,
// Present1 and PresentMultiplaneOverlay1 on their 1_6_1 arguments, and ReclaimResources1 appended.
DXGI1_6_1_DDI_BASE_FUNCTIONS make_dxgi1_6_1_device_table();
}
