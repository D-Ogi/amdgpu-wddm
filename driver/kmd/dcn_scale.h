// Pipe 0's scaler and viewport for a desktop source mode smaller than the native timing (display modes, stage A).
// Plain C over display_io.h: modeset.c runs it with BAR5, driver\kmd\test\dcn_scale_test.c with a register file.
//
// The OTG timing never changes here. A source mode of SourceWidth x SourceHeight is read whole by HUBP0 (the
// viewport), scaled up by DSCL0 into a rectangle of the native active area (the recout), and MPCC0 fills the rest
// of the active area with its background colour (black). The math and the register sequence are Linux amdgpu's
// for DCN 2.0.1 (MIT, v6.18), reduced to one pipe, RGB, no rotation, no mirror and an upscale or 1:1 only:
//   core/dc_resource.c calculate_scaling_ratios, calculate_init_and_vp;
//   dpp/dcn201/dcn201_dpp.c dpp201_get_optimal_number_of_taps;
//   dpp/dcn20/dcn20_dpp.c dscl2_calc_lb_num_partitions;
//   dpp/dcn10/dcn10_dpp_dscl.c dpp1_dscl_get_dscl_mode, dpp1_dscl_find_lb_memory_config, dpp1_dscl_set_lb,
//     dpp1_dscl_set_scaler_filter, dpp1_dscl_set_scl_filter, dpp1_dscl_set_manual_ratio_init,
//     dpp1_dscl_set_scaler_manual_scale;
//   hubp/dcn10/dcn10_hubp.c min_set_viewport; dce/dce_scl_filters.c filter_4tap_64p_upscale (the only filter an
//   upscale with 4 taps uses; extracted, not retyped: scl_filter_4tap_64p_upscale.inc).
// docs/design/display-modes.md has the design and the deviations.
#pragma once
#include "display_io.h"

// How a smaller source mode is placed on the native active area (the VidPN scaling of its present path).
enum bc250_scaling {
    BC250_SCALING_IDENTITY = 0,         // source size == native size: the firmware's own shape
    BC250_SCALING_CENTERED,             // 1:1 pixels in the middle, black around (no scaler ratio)
    BC250_SCALING_STRETCHED,            // the whole active area, aspect ratio not kept
    BC250_SCALING_ASPECT,               // as large as fits with the aspect ratio kept, centered, black bars
    BC250_SCALING_CUSTOM,               // retained firmware transform only; never programmed by ScalerPlan
    BC250_SCALING_COUNT
};
#define BC250_SCALING_BIT(s) (1ul << (s))

typedef struct _BC250_RECT {
    unsigned long X, Y, Width, Height;
} BC250_RECT;

// Everything the register writes need, computed before the first write.
typedef struct _BC250_SCALER_PLAN {
    unsigned long SourceWidth, SourceHeight, TargetWidth, TargetHeight, Scaling;
    BC250_RECT Viewport;                // in the source surface
    BC250_RECT Recout;                  // in the native active area
    unsigned long RatioH19, RatioV19;   // source / recout, unsigned 3.19 fixed point
    unsigned long InitH19, InitV19;     // (ratio + taps + 1) / 2, 19 fraction bits
    unsigned long TapsH, TapsV, TapsHC, TapsVC;
    unsigned long DsclMode;             // 0 = 444 bypass (1:1), 1 = 444 RGB scaling enabled
    unsigned long LbConfig;             // LB_MEMORY_CTRL.MEMORY_CONFIG
    int FilterH, FilterV;               // the 4-tap upscale filter is loaded for this direction
} BC250_SCALER_PLAN;

// The scaler limits of this code (not of the hardware): a source mode is never larger than the target, and a
// ratio is 1 or below (dpp201_get_optimal_number_of_taps allows up to 8:1 down, which needs DPP clock and DCHUB
// request changes that stage A does not make).
#define BC250_SCALE_MIN_DIMENSION 16ul
#define BC250_SCALE_MAX_DIMENSION 0x3FFFul      // the 14-bit width/height fields of the viewport and recout

// The recout for a scaling. Returns BC250_DISP_STATUS_SUCCESS, _INVALID for a zero or oversized dimension or an
// unknown scaling, or _NOT_SUPPORTED when the source is larger than the target in either direction, or Identity
// is asked for a source of another size.
long Bc250ScalingRecout(unsigned long SourceWidth, unsigned long SourceHeight, unsigned long TargetWidth,
                        unsigned long TargetHeight, unsigned long Scaling, BC250_RECT* Recout);
// The whole plan (Linux's scaler_data for this pipe). Same returns as Bc250ScalingRecout.
long Bc250ScalerPlan(unsigned long SourceWidth, unsigned long SourceHeight, unsigned long TargetWidth,
                     unsigned long TargetHeight, unsigned long Scaling, BC250_SCALER_PLAN* Plan);
// The scalings display.c offers for a source mode on a target, as BC250_SCALING_BIT()s, for a display-modes
// level (edid.h BC250_DISPLAY_MODES_*). 0 when the mode is not offered at all. The native size reports identity
// and every scaling of its level (each is the 1:1 shape), so that a scaling dxgkrnl pinned for the monitor never
// hides the native mode; a smaller size reports centered (level CENTERED) or centered, stretched and aspect ratio
// (level SCALED), also for a size of the native aspect ratio.
unsigned long Bc250ScalingSupport(unsigned long Level, unsigned long SourceWidth, unsigned long SourceHeight,
                                  unsigned long TargetWidth, unsigned long TargetHeight);
// The scaling to program when Windows leaves it unpinned or asks for one the level does not offer: Identity for
// the native size, else aspect-ratio (level SCALED) or centered (level CENTERED).
unsigned long Bc250ScalingDefault(unsigned long Level, unsigned long SourceWidth, unsigned long SourceHeight,
                                  unsigned long TargetWidth, unsigned long TargetHeight);
const char* Bc250ScalingName(unsigned long Scaling);

// ---- the pipe the firmware lit -------------------------------------------------------------------------------
//
// Read only. The driver changes pipe 0 only when it has the shape this code expects: one pipe, the viewport and
// recout covering the whole active area, MPCC 0 fed by DPP 0, OPTC segment 0 fed by OPP 0, and the scaler's
// coefficient memory not forced off.
typedef struct _BC250_PIPE_SHAPE {
    unsigned long ViewportStart, ViewportDimension, RecoutStart, RecoutSize, MpcSize;
    unsigned long SclMode, MemPwrCtrl, MpccTopSel, OdmSource, HubpCntl;
} BC250_PIPE_SHAPE;

enum bc250_pipe_verdict {
    BC250_PIPE_OK = 0,
    BC250_PIPE_VIEWPORT,                // the viewport is not 0,0,W,H
    BC250_PIPE_RECOUT,                  // the recout is not 0,0,W,H
    BC250_PIPE_MPC_SIZE,                // MPC_SIZE is not W,H
    BC250_PIPE_DSCL_MODE,               // DSCL_MODE is neither 0 (444 bypass) nor 1 (444 RGB)
    BC250_PIPE_MPCC_TOP,                // MPCC 0 is not fed by DPP 0
    BC250_PIPE_ODM,                     // OPTC segment 0 is not fed by OPP 0
    BC250_PIPE_COEF_POWER,              // the coefficient memory is forced off (scaling refused, centered allowed)
    BC250_PIPE_VERDICT_COUNT
};

long Bc250PipeShapeRead(BC250_DISP_IO* Io, BC250_PIPE_SHAPE* Shape);

// BD-124: read-only admission of a firmware-scaled surface. Valid is a per-read
// bit mask, so an unreadable register is distinguishable from a measured zero.
typedef struct _BC250_INHERITED_PIPE {
    BC250_PIPE_SHAPE Shape;
    unsigned long RatioH, RatioV, Taps, SurfaceConfig, TilingConfig, Pitch, Otg1;
    unsigned long SurfaceControl, MpccBottom, MpccControl, MpcOut;
    unsigned long Autocal, DsclControl;
    unsigned long Valid;
} BC250_INHERITED_PIPE;
#define BC250_INHERITED_PIPE_READS 23ul
#define BC250_INHERITED_PIPE_ALL ((1ul << BC250_INHERITED_PIPE_READS) - 1ul)
long Bc250InheritedPipeRead(BC250_DISP_IO* Io, BC250_INHERITED_PIPE* Pipe);
int Bc250InheritedPipeSame(const BC250_INHERITED_PIPE* A, const BC250_INHERITED_PIPE* B);
// BD-169: which check refused the pipe, and the word that check read. A support
// file from the field carries these two numbers, so the gate numbers are a
// vocabulary: append, never renumber or reuse one. Word is the register word for
// a register gate and the argument for an argument gate (the comments say which).
enum bc250_pipe_gate {
    BC250_PIPE_GATE_NONE = 0,               // the pipe is supported; Word is 0
    BC250_PIPE_GATE_READS = 1,              // not every register read back (Word: the Valid mask)
    BC250_PIPE_GATE_SOURCE_SIZE = 2,        // the POST source size is out of range (Word: width << 16 | height)
    BC250_PIPE_GATE_TARGET_SIZE = 3,        // the active size is out of range (Word: width << 16 | height)
    BC250_PIPE_GATE_PITCH_ARG = 4,          // the POST pitch is short or unaligned (Word: the pitch)
    BC250_PIPE_GATE_VIEWPORT_START = 5,     // PRI_VIEWPORT_START is not 0,0
    BC250_PIPE_GATE_VIEWPORT_SIZE = 6,      // PRI_VIEWPORT_DIMENSION is not the source size
    BC250_PIPE_GATE_MPC_SIZE = 7,           // MPC_SIZE is not the target size
    BC250_PIPE_GATE_MPCC_TOP = 8,           // MPCC 0 is not fed by DPP 0
    BC250_PIPE_GATE_MPC_OUT = 9,            // OPP 0 is not fed by MPCC 0
    BC250_PIPE_GATE_DCC = 10,               // the primary surface is DCC-compressed
    BC250_PIPE_GATE_ODM = 11,               // OPTC segment 0 is not fed by OPP 0
    BC250_PIPE_GATE_OTG1 = 12,              // OTG1 is also running
    BC250_PIPE_GATE_ROTATION = 13,          // the surface is rotated or mirrored (Word: SURFACE_CONFIG)
    BC250_PIPE_GATE_TILING = 14,            // the fetch layout is not linear (Word: TILING_CONFIG)
    BC250_PIPE_GATE_PITCH_REG = 15,         // DCSURF_SURFACE_PITCH is not the POST pitch
    BC250_PIPE_GATE_MPCC_MODE = 16,         // MPCC_MODE is neither 1 nor 2 (Word: MPCC_CONTROL)
    BC250_PIPE_GATE_MPCC_BOTTOM = 17,       // MPCC_MODE 2 with a connected bottom layer (Word: MPCC_BOT_SEL)
    BC250_PIPE_GATE_PIXEL_FORMAT = 18,      // SURFACE_PIXEL_FORMAT is not 8 (RGB8888) (Word: SURFACE_CONFIG)
    BC250_PIPE_GATE_RECOUT = 19,            // the recout is empty or outside the target (Word: RECOUT_SIZE)
    BC250_PIPE_GATE_AUTOCAL_MODE = 20,      // AUTOCAL_MODE is centering or replication (Word: DSCL_AUTOCAL)
    BC250_PIPE_GATE_AUTOCAL_PIPE = 21,      // AUTOCAL_NUM_PIPE/PIPE_ID are set (Word: DSCL_AUTOCAL)
    BC250_PIPE_GATE_AUTOCAL_DSCL = 22,      // automatic scale without DSCL_MODE 444 RGB (Word: SCL_MODE)
    BC250_PIPE_GATE_AUTOCAL_UPSCALE = 23,   // automatic scale with a recout under the source (Word: RECOUT_SIZE)
    BC250_PIPE_GATE_AUTOCAL_COEF = 24,      // automatic scale with the coefficient memory off (Word: MEM_PWR_CTRL)
    BC250_PIPE_GATE_BYPASS_RECOUT = 25,     // scaler bypass with a recout other than the source (Word: RECOUT_SIZE)
    BC250_PIPE_GATE_COEF_POWER = 26,        // manual scale with the coefficient memory off (Word: MEM_PWR_CTRL)
    BC250_PIPE_GATE_RATIO_H_ZERO = 27,      // the horizontal ratio register is zero (Word: the ratio)
    BC250_PIPE_GATE_RATIO_V_ZERO = 28,      // the vertical ratio register is zero (Word: the ratio)
    BC250_PIPE_GATE_RATIO_H = 29,           // the horizontal ratio is neither the recout's nor unity (Word: the ratio)
    BC250_PIPE_GATE_RATIO_V = 30,           // the vertical ratio is neither the recout's nor unity (Word: the ratio)
    BC250_PIPE_GATE_RATIO_UPSCALE = 31,     // a unity-ratio pipe with a recout under the source (Word: RECOUT_SIZE)
    BC250_PIPE_GATE_DSCL_MODE = 32,         // DSCL_MODE is neither bypass nor 444 RGB (Word: SCL_MODE)
    // Set by the pipe's reader (modeset.c), not by the classifier, which never
    // saw the pipe in these two cases.
    BC250_PIPE_GATE_UNSTABLE = 33,          // the two reads disagreed (Word: the Valid mask)
    BC250_PIPE_GATE_COUNT
};
typedef struct _BC250_INHERITED_PIPE_REASON {
    unsigned long Gate;                     // enum bc250_pipe_gate: the first check that refused
    unsigned long Word;                     // the word that check read
} BC250_INHERITED_PIPE_REASON;
const char* Bc250InheritedPipeGateText(unsigned long Gate);

// A set of equivalent VidPN transformations for exactly the saved tuple.
// Custom preserves the firmware image without claiming a standard scaling
// transform (a non-standard rectangle or a manual unity-ratio enlarged recout).
// It never grants permission to program a different pipe. Return 0 if
// the readback does not prove a linear, unrotated RGB source feeding OTG0.
// The Ex form names the first check that refused, for the start record; the
// plain form is the same decision without it.
unsigned long Bc250InheritedPipeSupport(const BC250_INHERITED_PIPE* Pipe,
    unsigned long SourceWidth, unsigned long SourceHeight, unsigned long Pitch,
    unsigned long TargetWidth, unsigned long TargetHeight);
unsigned long Bc250InheritedPipeSupportEx(const BC250_INHERITED_PIPE* Pipe,
    unsigned long SourceWidth, unsigned long SourceHeight, unsigned long Pitch,
    unsigned long TargetWidth, unsigned long TargetHeight, BC250_INHERITED_PIPE_REASON* Reason);
// The verdict for a native active size; *MaxLevel is the highest display-modes level the shape allows.
unsigned long Bc250PipeShapeCheck(const BC250_PIPE_SHAPE* Shape, unsigned long NativeWidth,
                                  unsigned long NativeHeight, unsigned long* MaxLevel);
const char* Bc250PipeVerdictText(unsigned long Verdict);

// ---- the register writes ---------------------------------------------------------------------------------------
//
// A live pipe update selects OTG0 then takes OTG_MASTER_UPDATE_LOCK as optc1_lock
// does (at most ten 1 us stalls), then unlocks and triggers. Every write
// is to a register of gen_regs.py's display write table. The underflow status of HUBP0 is read after the trigger
// and returned (it is the first sign of a DCHUB request setting this code does not program; the design names it).
typedef struct _BC250_SCALER_RESULT {
    unsigned long LockWaitUs;
    unsigned long CoefWrites;           // SCL_COEF_RAM_TAP_DATA writes
    unsigned long SclModeBefore, SclModeAfter;
    unsigned long HubpCntlAfter;        // HUBP0_DCHUBP_CNTL after the trigger
    unsigned long LockStatus;          // raw register at wait termination or stopped-pipe admission
    unsigned long LockStatusValid;     // zero when no sample was taken or the final read failed
} BC250_SCALER_RESULT;

long Bc250ScalerProgram(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* Plan, BC250_SCALER_RESULT* Result);
// Caller owns the pipe and has completed optc1_disable_crtc. Verify MASTER_EN=0,
// OTG_BUSY=0 and no held update lock before programming. No lock, stall or
// manual trigger: the caller programs timing and enables the OTG afterwards.
long Bc250ScalerProgramStopped(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* Plan, BC250_SCALER_RESULT* Result);
// The 1:1 shape for the native size (Linux's shape for a source of the native size: 444 bypass, full viewport
// and recout). The quiet restore at stop, reset and bugcheck uses this.
long Bc250ScalerRestoreNative(BC250_DISP_IO* Io, unsigned long NativeWidth, unsigned long NativeHeight,
                              BC250_SCALER_RESULT* Result);

// The filter table, for the test: 33 phases x 4 taps of filter_4tap_64p_upscale.
#define BC250_SCL_PHASES 33ul
#define BC250_SCL_TAPS 4ul
extern const unsigned short g_Bc250SclFilter4tapUpscale[BC250_SCL_PHASES * BC250_SCL_TAPS];
