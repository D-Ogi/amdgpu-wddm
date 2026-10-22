/* AMD DML201 planning only. Caller owns floating-point state and heap workspace. */
#ifndef BC250_DML201_H
#define BC250_DML201_H
#include <stddef.h>
#include "bc250_scaler_plan.h"
/* bc250_scaler_plan.h supplies the common host/kernel boolean type. */
#include "dml201/display_mode_structs.h"

enum { BC250_DML201_OK = 0, BC250_DML201_INVALID = -1,
       BC250_DML201_UNSUPPORTED = -2, BC250_DML201_ASSERTION = -3 };
enum { BC250_DML201_HDMI = 0, BC250_DML201_DP = 1 };
typedef struct BC250_DML201_INPUT {
    unsigned int HActive, HTotal, HFrontPorch, HSyncWidth;
    unsigned int VActive, VTotal, VFrontPorch, VSyncWidth;
    unsigned int PixelClockKhz, PitchPixels, Output, DpLanes;
    unsigned int CursorWidth, AudioSampleRateKhz;
    /* Measured/firmware-derived inputs, never silently replaced by constants. */
    unsigned int DchubRefClockKhz, DentistVcoKhz;
    /* BD-223: a scaled plane. All six zero is the full-size 1:1 pipe of the
     * active area. Otherwise all six are set: the viewport the pipe reads, the
     * recout it fills (inside the active area) and the scaler taps for each
     * direction (1 only for a 1:1 direction). PitchPixels then bounds the
     * viewport width, not the active width. */
    unsigned int ViewportWidth, ViewportHeight, RecoutWidth, RecoutHeight;
    unsigned int HTaps, VTaps;
} BC250_DML201_INPUT;
typedef struct BC250_DML201_WATERMARKS {
    unsigned int UrgentNs, StutterEnterExitNs, StutterExitNs;
    unsigned int DramClockChangeNs, MemoryTripNs;
} BC250_DML201_WATERMARKS;
typedef struct BC250_DML201_PLAN {
    unsigned int Valid, VoltageLevel, BboxDefaults, Assertions, FirstAssertionLine;
    unsigned int VStartup, VReady, VUpdateOffset, VUpdateWidth;
    unsigned int DispClockKhz, DppClockKhz, DcfClockKhz, SocClockKhz;
    unsigned int DcfDeepSleepKhz, FabricClockKhz, DramClockKhz;
    unsigned int PStateSupported, CStateEnabled;
    display_rq_regs_st Rq;
    display_dlg_regs_st Dlg;
    display_ttu_regs_st Ttu;
    display_pipe_dest_params_st Destination;
    BC250_DML201_WATERMARKS Watermarks[4];
} BC250_DML201_PLAN;
size_t Bc250Dml201WorkspaceSize(void);
/* Workspace must be naturally aligned, private to this call, and at least the
 * returned size. All output is zero/invalid on failure except diagnostic fields.
 * One progressive linear RGB8 pipe, physical addressing, no DCC: full size,
 * or the scaled plane of the six BD-223 fields (validScale says which sets).
 * BboxDefaults=1 identifies the pinned AMD DCN201 SoC/IP model, not measurement.
 * Success is a computed plan, not a hardware admission or application result. */
int Bc250Dml201Plan(const BC250_DML201_INPUT *input, void *workspace,
                   size_t bytes, BC250_DML201_PLAN *plan);

enum { BC250_DML201_RGB32 = 0, BC250_DML201_NV12 = 1 };
/* One output, up to two distinct unsplit RGB32-storage or NV12 planes. RGB8
 * and RGB10A2 have the same storage bandwidth; this does not admit HDR.
 * Scaler points to the immutable result of Bc250ScalerPrepare for this same
 * surface and output. SourceWidth/Height are the original source rectangle
 * extents, before destination clipping, not the resulting viewport extents.
 */
typedef struct BC250_DML201_PLANE_INPUT {
    const BC250_SCALER_PREPARED *Scaler;
    unsigned int SurfaceWidth, SurfaceHeight, PitchPixels;
    unsigned int SourceWidth, SourceHeight;
    unsigned int StorageBits; /* RGB: 32 per pixel. NV12: 8 per luma sample. */
    unsigned int Format;
    /* NV12: pitch in two-byte interleaved UV samples, not bytes or luma
     * pixels. Chroma dimensions are half the even surface dimensions.
     * Y and UV byte pitches must each be a multiple of 256.
     * RGB requires zero. Allocation/address and color admission are separate. */
    unsigned int ChromaPitchSamples;
} BC250_DML201_PLANE_INPUT;
typedef struct BC250_DML201_GROUP_PLAN {
    unsigned int Valid, Count, Assertions, FirstAssertionLine;
    /* Each entry has its own RQ/DLG/TTU, destination and DPP clock. Shared
     * watermarks and global clocks are identical and computed with every pipe.
     * Keep the whole group together through hardware admission and commit. */
    BC250_DML201_PLAN Plane[2];
} BC250_DML201_GROUP_PLAN;

/* Same workspace/FPU contract as Plan. One OTG, one DPP per plane, no split,
 * ODM, interlace, compression, rotation or mirror. NV12 uses separate Y/UV
 * fetch geometry and no cursor on that pipe, as in the AMD video path. The
 * caller must establish that those exclusions match the surface transaction.
 * Count is 1 or 2; an all-disabled output uses the owner's blank/drain path.
 * timing.PitchPixels is ignored; each plane supplies its own pitch. Output
 * timing remains 8-bpc RGB. Inputs, workspace and output must not overlap.
 * Failure clears the group
 * except assertion diagnostics. No capability or hardware admission implied. */
int Bc250Dml201PlanGroup(const BC250_DML201_INPUT *timing,
                        const BC250_DML201_PLANE_INPUT *planes, unsigned int count,
                        void *workspace, size_t bytes, BC250_DML201_GROUP_PLAN *plan);
#endif
