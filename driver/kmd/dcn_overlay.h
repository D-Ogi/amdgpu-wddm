// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "dcn_geometry.h"
#include "dcn_cnvc_color.h"
#include "dcn_mpc.h"
#include "dcn_overlay_input.h"
#include "dcn_caps.h"
#include "../shim/bc250_dml201.h"


typedef struct BC250_OVERLAY_REQUEST {
    unsigned Count, SourceId, PostCompositionChanged;
    const BC250_DCN_CAPS* Caps;   // the display IP row (dcn_caps.h); NULL refuses
    unsigned long long McBase, VramPhysical, VramLength;
    BC250_DML201_INPUT Timing;
    BC250_OVERLAY_INPUT Plane[2]; // Array order is not layer order or pipe order.
} BC250_OVERLAY_REQUEST;

typedef struct BC250_OVERLAY_PLAN {
    unsigned Ready, Described, LayerByPipe[2];
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
// storage (several KiB; not the kernel stack). The planner reads its inputs in
// place: output that overlaps the request (or the description, timing or caps
// row) is refused with ARGUMENT, and the workspace must not overlap either.
// Output is cleared on every refusal. No pointers to request, workspace or
// temporary DML inputs survive the call.
//
// This joins existing preparation contracts; it is NOT Check3 admission. It
// does not establish chroma siting for a DXGI color space, CM normalization,
// power/clock readiness, coefficient/CSC bank assignment, latch or retirement.
// No MMIO, DDI or caps change. Plane count, surface and pitch limits, the
// downscale source width and NV12 come from Request->Caps (dcn_caps.h); a row
// whose Nv12 is NONE refuses the NV12 pipe. AMD DCN201 still advertises NV12 false. A live
// path needs the Y0 witnesses and an atomic recovery/retirement implementation.
unsigned Bc250OverlayPrepare(const BC250_OVERLAY_REQUEST* Request,
    void* Workspace, size_t WorkspaceBytes, BC250_OVERLAY_PLAN* Plan);

// Check3 has storage metadata, not resident MC addresses. Validate its relative
// spans and prepare geometry/color/group DML without fabricating an address.
// Success sets Described, never Ready; Rgb/Nv12 physical plans stay zero.
// The connector supplies current timing under its operation lock and owns FPU.
// Caps is the row of the detected display IP; without one nothing is described.
unsigned Bc250OverlayDescribe(const BC250_OVERLAY_DESCRIPTION* Description,
    const BC250_DCN_CAPS* Caps, const BC250_DML201_INPUT* Timing, void* Workspace, size_t WorkspaceBytes,
    BC250_OVERLAY_PLAN* Plan);
