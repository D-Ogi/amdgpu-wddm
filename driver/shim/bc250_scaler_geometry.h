// Checked geometry only. No MMIO, allocation, coefficient selection or admission.
// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: Linux amdgpu DC, MIT, v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
#pragma once
#include <stdint.h>

typedef struct BC250_SCALER_AXIS_INPUT {
    uint32_t SurfaceSize, ActiveSize;
    int32_t SourceBegin, SourceEnd;
    int32_t DestinationBegin, DestinationEnd;
    int32_t ClipBegin, ClipEnd;
    uint32_t Taps, Mirror;
} BC250_SCALER_AXIS_INPUT;

typedef struct BC250_SCALER_AXIS_PLAN {
    uint32_t ViewportBegin, ViewportSize;
    uint32_t RecoutBegin, RecoutSize;
    uint32_t Ratio19, Init19;
} BC250_SCALER_AXIS_PLAN;

enum BC250_SCALER_GEOMETRY_STATUS {
    BC250_SCALER_GEOMETRY_OK = 0,
    BC250_SCALER_GEOMETRY_EMPTY,
    BC250_SCALER_GEOMETRY_INVALID,
    BC250_SCALER_GEOMETRY_UNSUPPORTED
};

// Coordinates are half-open. Destination and clip may start outside the output.
// Output is zero on EMPTY/refusal. Source must be within the allocation.
// Both axes are planned independently. The caller supplies already selected taps
// and validates DML, format/chroma, rotation, coefficient and line-buffer limits.
// This computes a full-resolution component, not subsampled YUV chroma.
// It does not advertise a capability or make the existing RGB writer MPO-ready.
int Bc250ScalerAxisPlan(const BC250_SCALER_AXIS_INPUT* Input,
                       BC250_SCALER_AXIS_PLAN* Plan);
