// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once

// Per-display-IP plane capabilities (BD-223 Part Y). Shared overlay paths take
// their plane, pipe, scaler and surface limits from this table and never from
// board literals. The key is the DC version that Linux derives from the PCI
// device ID (resource_parse_asic_id, dc/core/dc_resource.c of ref/linux-src
// v6.18 7d0a66e4bb90). A device without a row has no overlay capability: the
// caller answers one plane and refuses every overlay group.
//
// The values of a row are imported from that version's resource file. A value
// that our lab measurement has not shown is marked so in the row; the release
// caps answer only what is proven.

enum BC250_DCN_VERSION {
    BC250_DCN_NONE = 0,
    BC250_DCN_2_01 = 201,      // DCN_VERSION_2_01 (Cyan Skillfish)
};

// NV12 as a scanout plane. Linux dcn201 plane_cap lists nv12 false; the DPP
// has the 4:2:0 path. Our lab witness (Y0) decides. Until it passes, the
// overlay planner may describe NV12 for the lab witness, and the release caps
// answer no YUV plane.
enum BC250_DCN_NV12 {
    BC250_DCN_NV12_NONE = 0,   // no NV12 plane at all
    BC250_DCN_NV12_WITNESS,    // planner admits it for the lab witness only
    BC250_DCN_NV12_PROVEN,     // a passed hardware witness: release may answer it
};

typedef struct BC250_DCN_CAPS {
    unsigned Version;          // enum BC250_DCN_VERSION
    unsigned Pipes;            // pool->base.pipe_count
    unsigned TimingGenerators; // res_cap num_timing_generator
    unsigned MaxSlavePlanes, MaxSlaveRgbPlanes, MaxSlaveYuvPlanes;  // dc->caps
    unsigned MinSurfaceDimension;                   // plane_cap min_width/min_height
    unsigned MaxSurfaceWidth, MaxSurfaceHeight;     // inclusive, pixels
    unsigned PitchAlignBytes, MaxPitchBytes;        // linear RGB surface
    unsigned MaxUpscaleArgb, MaxUpscaleNv12;        // 1/1000 units, 16000 = 16x
    unsigned MaxDownscaleArgb, MaxDownscaleNv12;    // 1/1000 units, 250 = 4x
    unsigned MaxDownscaleSourceWidth;               // debug max_downscale_src_width
    unsigned Nv12;             // enum BC250_DCN_NV12
} BC250_DCN_CAPS;

// dcn201_resource.c: res_cap_dnc201, plane_cap, debug_defaults_drv and
// dcn201_resource_construct (pipe_count 4, max_slave_*_planes 1). Surface and
// pitch limits are the DCN2 HUBP register field widths that the shared
// contracts already use (bc250_nv12_layout.h, dcn_overlay.c).
static const BC250_DCN_CAPS Bc250DcnCaps201 = {
    BC250_DCN_2_01, 4u, 2u, 1u, 1u, 1u,
    64u, 16383u, 16383u, 256u, 65536u,
    16000u, 16000u, 250u, 250u, 3840u,
    BC250_DCN_NV12_WITNESS,
};

// PCI vendor/device to the DC version, as resource_parse_asic_id does for
// FAMILY_NV. Other devices return NULL until their row is imported.
static inline const BC250_DCN_CAPS* Bc250DcnCapsForPci(unsigned VendorId, unsigned DeviceId)
{
    if (VendorId != 0x1002u) return 0;
    switch (DeviceId) {
    case 0x13FEu: case 0x143Fu: case 0x13F9u: case 0x13FAu:
    case 0x13FBu: case 0x13FCu: case 0x13DBu:
        return &Bc250DcnCaps201;
    default:
        return 0;
    }
}

// The overlay planes that the release caps may answer for this row: the
// primary plus the slave planes, never more than the pipes, and a YUV slave
// plane only after a passed NV12 witness. Proven is 0 until the hardware
// witnesses (A1, Y0) pass: then the answer is one RGB plane.
typedef struct BC250_DCN_PLANE_ANSWER { unsigned Planes, RgbPlanes, YuvPlanes; } BC250_DCN_PLANE_ANSWER;
static inline BC250_DCN_PLANE_ANSWER Bc250DcnPlaneAnswer(const BC250_DCN_CAPS* Caps, unsigned Proven)
{
    BC250_DCN_PLANE_ANSWER a = {1u, 1u, 0u};
    unsigned slaves;
    if (!Caps || !Proven || Caps->Pipes < 2u) return a;
    slaves = Caps->MaxSlavePlanes < Caps->Pipes - 1u ? Caps->MaxSlavePlanes : Caps->Pipes - 1u;
    a.Planes = 1u + slaves;
    a.RgbPlanes = 1u + (Caps->MaxSlaveRgbPlanes < slaves ? Caps->MaxSlaveRgbPlanes : slaves);
    if (Caps->Nv12 == BC250_DCN_NV12_PROVEN)
        a.YuvPlanes = Caps->MaxSlaveYuvPlanes < slaves ? Caps->MaxSlaveYuvPlanes : slaves;
    return a;
}
