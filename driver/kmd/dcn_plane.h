// BD-223: one multiplane-overlay plane on pipe 0, with its own source and destination rectangles. The plane is
// read whole from its allocation, scaled 1:1 or up by DSCL0 into its destination rectangle, and MPCC0 fills the
// rest of the active area with black, as the source-mode scaler of dcn_scale.c does for a smaller desktop mode.
// docs/design/scaled-flip.md has the design, the Microsoft contract and the stages.
//
// Plain C over dcn_scale.h, no WDK header: wddm.c translates DXGK_MULTIPLANE_OVERLAY_PLANE_WITH_SOURCE and
// DXGK_MULTIPLANE_OVERLAY_PLANE2 into BC250_PLANE_REQUEST, and driver\kmd\test\dcn_plane_test.c gives it numbers.
// The answer is what Check tells Windows, so it says yes only for what this driver programs: a refused plane is
// composed by DWM, as before this work.
#pragma once
#include "dcn_scale.h"

// A RECT of the DDI, in the coordinates the DDI gives it: Src in the allocation, Dst and Clip in the VidPN source.
typedef struct _BC250_PLANE_RECT {
    long Left, Top, Right, Bottom;
} BC250_PLANE_RECT;

// The plane, already reduced by wddm.c to what this code decides on. Every flag is 0 or 1.
typedef struct _BC250_PLANE_REQUEST {
    unsigned long PlaneCount;                   // the planes of the request; this stage admits one
    unsigned long LayerIndex;
    int Enabled;
    unsigned long SurfaceWidth, SurfaceHeight;  // the allocation's own description
    unsigned long PlaneFormat;                  // plane_format.h BC250_PLANE_FORMAT_*, NONE for any other format
    int TenBitSource;                           // the committed source mode is 10-bit (scan-out caps flag)
    unsigned long SourceModeWidth, SourceModeHeight;    // the committed VidPN source mode
    unsigned long TargetWidth, TargetHeight;    // the active area of the timing on the cable
    BC250_PLANE_RECT Src, Dst, Clip;
    int Mirror;                                 // DXGK_MULTIPLANE_OVERLAY_FLAGS has a flip bit
    int Rotated;                                // Rotation is not D3DDDI_ROTATION_IDENTITY
    int AlphaBlend;                             // Blend is not DXGK_MULTIPLANE_OVERLAY_BLEND_OPAQUE
    int Interlaced;                             // VideoFrameFormat is not PROGRESSIVE
    int Stereo;                                 // StereoFormat is not MONO, or a stereo field is set
    int NotSrgb;                                // ColorSpaceType is not RGB_FULL_G22_NONE_P709
    int StretchQualityKnown;                    // StretchQuality is BILINEAR or HIGH
} BC250_PLANE_REQUEST;

// Why a plane is refused. A log line and a support file carry the number, so this is a vocabulary: append, never
// renumber or reuse one (the rule of enum bc250_pipe_gate).
enum bc250_plane_verdict {
    BC250_PLANE_OK = 0,
    BC250_PLANE_ARGS = 1,               // no request, or an empty or inverted rectangle
    BC250_PLANE_COUNT = 2,              // more than one plane: this stage has plane 0 only
    BC250_PLANE_LAYER = 3,              // LayerIndex is not 0
    BC250_PLANE_DISABLED = 4,           // a disabled plane 0 (a black screen needs OTG blanking, not this path)
    BC250_PLANE_SOURCE_MODE = 5,        // the source mode is scaled to the timing already (stage A); no plane on it
    BC250_PLANE_FORMAT = 6,             // not an 8-bit plane format, or 10-bit under an 8-bit source mode
    BC250_PLANE_SURFACE = 7,            // the allocation is smaller than 64x64 or larger than the scaler's fields
    BC250_PLANE_SRC_CROP = 8,           // SrcRect is not the whole allocation
    BC250_PLANE_DST_BOUNDS = 9,         // DstRect is not inside the source mode
    BC250_PLANE_CLIP = 10,              // ClipRect cuts DstRect
    BC250_PLANE_MIRROR = 11,
    BC250_PLANE_ROTATION = 12,
    BC250_PLANE_BLEND = 13,             // alpha blending asked for
    BC250_PLANE_INTERLACED = 14,
    BC250_PLANE_STEREO = 15,
    BC250_PLANE_COLOR_SPACE = 16,       // not sRGB: no input CSC is programmed here
    BC250_PLANE_DOWNSCALE = 17,         // DstRect smaller than SrcRect in a direction (DPP clock and DCHUB: not here)
    BC250_PLANE_UPSCALE = 18,           // more than 16 times up (DCN 2.0.1 plane cap max_upscale_factor 16000)
    BC250_PLANE_SCALER = 19,            // the scaler plan refused the rectangles (dcn_scale.c)
    BC250_PLANE_DML = 20,               // DML refused the scaled pipe, or could not run
    BC250_PLANE_CLOCKS = 21,            // DML needs a DISPCLK or DPPCLK above the programmed one
    BC250_PLANE_STRETCH_QUALITY = 22,   // StretchQuality is neither BILINEAR nor HIGH
    BC250_PLANE_VERDICTS
};
const char* Bc250PlaneVerdictText(unsigned long Verdict);

// The DCN 2.0.1 plane caps of Linux dcn201_resource.c (plane_cap: min_width, min_height, max_upscale_factor for
// ARGB8888 in thousandths).
#define BC250_PLANE_MIN_DIMENSION 64ul
#define BC250_PLANE_MAX_UPSCALE_MILLI 16000ul

// What the scan-out of an admitted plane needs. Identity is a plane of the source mode's own size at its origin:
// the native 1:1 shape of dcn_scale.c (Bc250ScalerRestoreNative), which DirectFlip programs today.
typedef struct _BC250_PLANE_PLAN {
    BC250_SCALER_PLAN Scaler;
    int Identity;
    // The DML inputs of the scaled pipe (bc250_dml201.h): the viewport, the recout and the taps.
    unsigned long DmlViewportWidth, DmlViewportHeight, DmlRecoutWidth, DmlRecoutHeight, DmlHTaps, DmlVTaps;
} BC250_PLANE_PLAN;

// The geometry and the pipe-local limits, with no register access and no floating point (DIRQL-safe). On
// BC250_PLANE_OK *Plan is complete; on a refusal *Plan is zero. The DML answer is a second step
// (Bc250PlaneDmlVerdict), because DML runs at PASSIVE_LEVEL with the floating-point state saved.
unsigned long Bc250PlanePrepare(const BC250_PLANE_REQUEST* Request, BC250_PLANE_PLAN* Plan);

// The DML step. DmlStatus is Bc250Dml201Plan's return, DmlValid its Valid, the clocks its answer, and the budget is
// what the committed mode programmed (connector_link.c). An identity plane needs no DML: it is the committed pipe.
unsigned long Bc250PlaneDmlVerdict(const BC250_PLANE_PLAN* Plan, int DmlStatus, unsigned long DmlValid,
                                   unsigned long DispClockKhz, unsigned long DppClockKhz,
                                   unsigned long BudgetDispClockKhz, unsigned long BudgetDppClockKhz);

// Two plans that program the same pipe shape (the address aside): a flip between them writes no scaler register.
int Bc250PlaneSameShape(const BC250_PLANE_PLAN* A, const BC250_PLANE_PLAN* B);
