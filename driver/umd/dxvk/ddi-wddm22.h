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
// The arguments of the DXGI 1.6.1 Present as the arguments of the DXGI 1.2 Present, which is where this shell
// presents (ddi-present.cpp). One surface only: several surfaces per Present belong to stereo and multi-plane
// swap chains, which this driver does not create. The dirty rectangles and the rotation hint are hints and this
// shell takes neither - it presents the buffer as the application wrote it and rotates nothing. A pure function,
// so the host test can read every field it carries (ddi-wddm22-test.cpp). False where one Present cannot carry.
inline bool present_arguments_1_6_1(const DXGI1_6_1_DDI_ARG_PRESENT &from,DXGI_DDI_ARG_PRESENT &to) {
    if (from.SurfacesToPresent!=1 || !from.phSurfacesToPresent) return false;
    to=DXGI_DDI_ARG_PRESENT{};
    to.hDevice=from.hDevice;
    to.hSurfaceToPresent=from.phSurfacesToPresent[0].hSurface;
    to.SrcSubResourceIndex=from.phSurfacesToPresent[0].SubResourceIndex;
    to.hDstResource=from.hDstResource;
    to.DstSubResourceIndex=from.DstSubResourceIndex;
    to.pDXGIContext=from.pDXGIContext;
    to.Flags=from.Flags;
    to.FlipInterval=from.FlipInterval;
    return true;
}
}
