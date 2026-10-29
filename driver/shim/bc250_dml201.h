/* AMD DML201 planning only. Caller owns floating-point state and heap workspace. */
#ifndef BC250_DML201_H
#define BC250_DML201_H
#include <stddef.h>
#if !defined(__cplusplus) && !defined(bool)
typedef unsigned char bool;
#endif
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
#endif
