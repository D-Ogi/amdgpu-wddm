// DCN201 OTG0 timing operations. The passive display owner supplies serialization,
// a blank/quiescent plane, admitted display bandwidth, and cancellation-aware I/O.
#pragma once
#include "../kmd/display_io.h"
#include "../kmd/edid.h"

typedef struct BC250_OPTC_SYNC {
    unsigned long VReady, VStartup, VUpdateOffset, VUpdateWidth;
} BC250_OPTC_SYNC;

// Admission only; no register access. RGB8 progressive, one OPP, no stereo/DSC.
int Bc250OptcTimingValid(const BC250_TARGET_TIMING* Timing,const BC250_OPTC_SYNC* Sync);
// These return the FIRST I/O failure; AMD's void helpers cannot hide a failed
// register access. A failure after any write requires a full old-mode replay.
long Bc250OptcDisable(BC250_DISP_IO* Io);
long Bc250OptcProgram(BC250_DISP_IO* Io,const BC250_TARGET_TIMING* Timing,
    const BC250_OPTC_SYNC* Sync,int DisplayPort);
long Bc250OptcEnable(BC250_DISP_IO* Io);
// Read-only inverse of optc1_program_timing for the admitted progressive RGB
// path. Caller supplies the measured clock and excludes concurrent modesets.
// Two identical snapshots are required; every refusal clears the output.
long Bc250OptcReadTiming(BC250_DISP_IO* Io,unsigned long PixelKhz,BC250_TARGET_TIMING* Timing);
