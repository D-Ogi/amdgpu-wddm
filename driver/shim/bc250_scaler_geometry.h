// Checked geometry only. No MMIO, allocation, coefficient selection or admission.
// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: Linux amdgpu DC, MIT, v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
#pragma once
#include "bc250_dc_types.h"

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

// Coordinates are half-open. Destination and clip are in active-output pixels
// (not relative to each other) and may start outside the output. A DDI caller
// must normalize its rectangles to this coordinate system before this call.
// Output is zero on EMPTY/refusal. Source must be within the allocation.
// Both axes are planned independently. The caller supplies already selected taps
// and validates DML, format/chroma, rotation, coefficient and line-buffer limits.
// This computes a full-resolution component, not subsampled YUV chroma.
// It does not advertise a capability or make the existing RGB writer MPO-ready.
int Bc250ScalerAxisPlan(const BC250_SCALER_AXIS_INPUT* Input,
                       BC250_SCALER_AXIS_PLAN* Plan);

// ComponentDivisor=1 for full-resolution, 2 for 4:2:0 chroma. Inputs remain
// in full-resolution source pixels, outputs use component pixels. The ratio
// uses the original source extent, not the rounded chroma viewport extent.
// AMD requires the source origin to align to ComponentDivisor. Odd origins
// are explicitly UNSUPPORTED here pending a phase-correct extension, not an
// implicit rounding down and not a claim of complete MPO clipping coverage.
int Bc250ScalerComponentAxisPlan(const BC250_SCALER_AXIS_INPUT* Input,
                                uint32_t ComponentDivisor,
                                BC250_SCALER_AXIS_PLAN* Plan);

typedef struct BC250_SCALER_PLANE_INPUT {
    BC250_SCALER_AXIS_INPUT Horizontal, Vertical;
    uint32_t ChromaDivisor; // 1 = RGB/4:4:4 geometry, 2 = 4:2:0 geometry
    uint32_t ChromaTapsH, ChromaTapsV;
} BC250_SCALER_PLANE_INPUT;

typedef struct BC250_SCALER_PLANE_PLAN {
    BC250_SCALER_AXIS_PLAN LumaH, LumaV, ChromaH, ChromaV;
} BC250_SCALER_PLANE_PLAN;

// Publish all four components only on success; otherwise clear the entire plan.
// Empty means at least one axis is fully clipped, after all inputs are checked.
// This does not admit a pixel format or choose the chroma siting/colour matrix.
int Bc250ScalerPlaneGeometry(const BC250_SCALER_PLANE_INPUT* Input,
                            BC250_SCALER_PLANE_PLAN* Plan);
