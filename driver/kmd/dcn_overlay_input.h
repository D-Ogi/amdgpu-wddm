// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once

// Integer-only boundary shared by WDDM and the connector's FPU-owning planner.
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
