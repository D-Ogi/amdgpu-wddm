// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once

// Integer-only boundary shared by WDDM and the connector's FPU-owning planner.
// It includes no planner (shim) header: wddm.c includes the amdgpu shim, whose bool
// macro collides with the DC type header of the planner (C4142 as an error).
// The scaler filter of a plane. dcn_overlay.c asserts that the values are those of
// enum BC250_SCALER_FILTER_KIND (shim/bc250_scaler_filter.h).
enum BC250_OVERLAY_FILTER {
    BC250_OVERLAY_FILTER_AMD = 0,
    BC250_OVERLAY_FILTER_LINEAR = 1
};
typedef struct BC250_OVERLAY_RECT { int Left, Top, Right, Bottom; } BC250_OVERLAY_RECT;
typedef struct BC250_OVERLAY_INPUT {
    unsigned LayerIndex, Format, Color, Filter;
    // Normalized exclusions: rotation, mirror, alpha, interlace, stereo, tiling
    // and every unrecognized WDDM attribute refuse before DML.
    unsigned UnsupportedFlags;
    unsigned Width, Height, PitchBytes, ChromaPitchBytes;
    unsigned long long AllocationMcAddress, AllocationBytes, SurfaceOffset, ChromaOffset;
    BC250_OVERLAY_RECT Source, Destination, Clip;
} BC250_OVERLAY_INPUT;
typedef struct BC250_OVERLAY_DESCRIPTION {
    unsigned Count, SourceId, PostCompositionChanged;
    BC250_OVERLAY_INPUT Plane[2];
} BC250_OVERLAY_DESCRIPTION;
// The answer of the CPU-only Check3 preflight of the connector (ConnectorLinkOverlayCheck).
typedef struct _BC250_LINK_OVERLAY_CHECK {
    unsigned Result, Described, Generation, DispClockKhz, DppClockKhz;
    unsigned BudgetDispKhz, BudgetDppKhz, WithinCurrentBudget;
} BC250_LINK_OVERLAY_CHECK;
