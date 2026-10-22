// DCN201 requestor and single-output dentist programming. No floating arithmetic.
#pragma once
#include "../kmd/display_io.h"
#include "bc250_dml201.h"

typedef struct BC250_DCN201_CLOCKS {
    unsigned int CrystalClockKhz, DchubRefClockKhz, DpRefClockKhz;
    unsigned int DentistVcoKhz, DispClockKhz, DppClockKhz;
} BC250_DCN201_CLOCKS;

// CrystalClockKhz is the ATOM DCE reference clock. No fallback frequency exists.
long Bc250Dcn201ReadClocks(BC250_DISP_IO* Io, unsigned int CrystalClockKhz,
    BC250_DCN201_CLOCKS* Clocks);
// Blank=1 also waits for HUBP0_NO_OUTSTANDING_REQ. Failed blanking must not be
// followed by timing writes. Blank=0 is only for the fully replayed active pipe.
long Bc250Dcn201PlaneBlank(BC250_DISP_IO* Io, int Blank);
// Same blank/drain contract, explicit HUBP0 or HUBP1. Other indices refuse.
long Bc250Dcn201PlaneBlankAt(BC250_DISP_IO* Io, unsigned Pipe, int Blank);
// Caller serializes the complete modeset, blanks/drains HUBP0, and locks/stops
// OTG0 before this call. Applies dentist clocks, RQ/DLG/TTU, and A-D watermarks.
// It does not change surface format/address/pitch, scaling, PHY, stream or OTG.
// A failed write requires full old-plan replay; HUBP remains blank throughout.
// Clocks must come from ReadClocks for the currently powered DCN201 instance.
long Bc250Dcn201PipeApply(BC250_DISP_IO* Io, const BC250_DML201_PLAN* Plan,
    const BC250_DCN201_CLOCKS* Clocks);
// Blanked setup/recovery only, NOT a live MPO attribute update. Caller owns all
// display writers, a stopped/locked OTG0, residency and the DCF/SOC/DRAM service
// contract. Both HUBPs must be powered, blanked and drained, even for Count=1.
// Plan is a private immutable PlanGroup result; Pipes maps its entries to unique
// physical HUBP0/HUBP1. Clocks must match current ReadClocks and satisfy every
// plane's DISP/DPP requirement. No clock changes, locks, unblank or allocations.
// Validates all fields before MMIO, programs each requestor then shared A-D
// watermarks once. A write failure needs whole old-group replay before unblank;
// success does not establish latching, retirement, complete MPO or HLK support.
long Bc250Dcn201GroupProgramBlanked(BC250_DISP_IO* Io,
    const BC250_DML201_GROUP_PLAN* Plan, const unsigned* Pipes,
    const BC250_DCN201_CLOCKS* Clocks);
// Set MPC0's timing-dependent flow count, OPP0 clock/edge and black DPG.
// Call while HUBP is blank/drained, after timing programming and before CRTC enable.
long Bc250Dcn201OutputProgram(BC250_DISP_IO* Io, const BC250_DML201_PLAN* Plan);
// Change OPP0 black/video output on a running CRTC; waits for the DPG latch.
long Bc250Dcn201OutputBlank(BC250_DISP_IO* Io, unsigned int Width,
    unsigned int Height, int Blank);
// Bind OTG0 -> DIG FE0 -> BE0. Mode is 0=DP SST, 2=DVI single-link, 3=HDMI.
// The caller keeps the old stream disabled and repeats mode setup after ATOM TX.
long Bc250Dcn201Route(BC250_DISP_IO* Io, unsigned int Mode, int Connect);
