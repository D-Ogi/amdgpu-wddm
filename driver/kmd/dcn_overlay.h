// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "dcn_geometry.h"
#include "dcn_cnvc_color.h"
#include "dcn_mpc.h"
#include "../shim/bc250_dml201.h"

typedef struct BC250_OVERLAY_RECT { int Left, Top, Right, Bottom; } BC250_OVERLAY_RECT;
typedef struct BC250_OVERLAY_INPUT {
    unsigned LayerIndex, Format, Color, Filter;
    // Format: BGRA8/RGBA8/BGRX8 or BC250_CNVC_NV12. All other formats refuse.
    // Flags are exclusions, not permission bits. The caller normalizes WDDM
    // rotation/mirror/alpha/interlace/stereo/DCC/tiling and unknown flags here.
    unsigned UnsupportedFlags;
    unsigned Width, Height, PitchBytes, ChromaPitchBytes;
    unsigned long long AllocationMcAddress, AllocationBytes, SurfaceOffset, ChromaOffset;
    BC250_OVERLAY_RECT Source, Destination, Clip;
} BC250_OVERLAY_INPUT;

typedef struct BC250_OVERLAY_REQUEST {
    unsigned Count, SourceId, PostCompositionChanged;
    unsigned long long McBase, VramPhysical, VramLength;
    BC250_DML201_INPUT Timing;
    BC250_OVERLAY_INPUT Plane[2]; // Array order is not layer order or pipe order.
} BC250_OVERLAY_REQUEST;

typedef struct BC250_OVERLAY_PLAN {
    unsigned Ready, LayerByPipe[2];
    BC250_SURFACE_LAYOUT Rgb;
    BC250_NV12_LAYOUT Nv12;
    BC250_DCN_GEOMETRY Geometry[2];
    BC250_CNVC_PLAN Color[2];
    BC250_MPC_PLAN Mpc;
    BC250_DML201_GROUP_PLAN Dml;
} BC250_OVERLAY_PLAN;

enum BC250_OVERLAY_RESULT {
    BC250_OVERLAY_OK, BC250_OVERLAY_ARGUMENT, BC250_OVERLAY_TOPOLOGY,
    BC250_OVERLAY_FLAGS, BC250_OVERLAY_FORMAT, BC250_OVERLAY_LAYOUT,
    BC250_OVERLAY_COLOR, BC250_OVERLAY_GEOMETRY, BC250_OVERLAY_DML
};

// Private CPU-only preparation for one RGB pipe 0 and one NV12 pipe 1 on OTG0.
// LayerIndex 0 is topmost in WDDM; pipe identity stays tied to storage/cursor,
// not to array order. MPC gets the inverse z-order. Both planes are opaque.
// Uses actual allocation offsets and byte pitches, never an upload-layout guess.
// PASSIVE only with caller-owned FPU state, private DML workspace and output
// storage (several KiB; not the kernel stack). Input/output may alias; workspace
// must not overlap either. Output is cleared on every refusal. No pointers to
// request, workspace or temporary DML inputs survive the call.
//
// This joins existing preparation contracts; it is NOT Check3 admission. It
// does not establish chroma siting for a DXGI color space, CM normalization,
// power/clock readiness, coefficient/CSC bank ownership, latch or retirement.
// No MMIO, DDI or caps change. AMD DCN201 still advertises NV12 false. A live
// owner needs the Y0 witnesses and an atomic recovery/retirement implementation.
unsigned Bc250OverlayPrepare(const BC250_OVERLAY_REQUEST* Request,
    void* Workspace, size_t WorkspaceBytes, BC250_OVERLAY_PLAN* Plan);
