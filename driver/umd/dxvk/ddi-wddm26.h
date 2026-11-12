// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
// The WDDM 2.6 D3D11 device table (BD-371). It extends the WDDM 2.2 table: the same entries at the same offsets,
// one of them retyped (RelocateDeviceFuncs, which now carries this table), and two appended scan-out entries,
// QueryScanoutCaps and PrepareScanoutTransformation.
//
// Why the shell needs it: D3D10DDIARG_CREATEDEVICE has no device table newer than this one, so the runtime gives
// a driver that negotiates a later interface (the shell offers WDDM 2.9, ddi-negotiation.h) this layout. The
// WDK header names no other table for those interfaces.
//
// The scan-out entries: QueryScanoutCaps answers D3DWDDM2_6DDI_SCANOUT_FLAG_NONE for every surface, so the
// runtime is told that no surface needs or wants a scan-out transformation. The runtime then has no reason to
// call PrepareScanoutTransformation, and that entry reports once and loses the device (ddi-unexpected.h).
D3DWDDM2_6DDI_DEVICEFUNCS make_wddm2_6_device_table();
// The answer of QueryScanoutCaps for every resource, source and plane. A host test reads it directly.
inline constexpr D3DWDDM2_6DDI_SCANOUT_FLAGS kScanoutFlags=D3DWDDM2_6DDI_SCANOUT_FLAG_NONE;
}
