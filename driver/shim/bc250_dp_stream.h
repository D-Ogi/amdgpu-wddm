// DCN201 DP SST RGB8 stream. Caller serializes the link and quiesces flips.
#pragma once
#include "../kmd/display_io.h"
#include "../kmd/edid.h"

long Bc250DpStreamProgram(BC250_DISP_IO* Io,const BC250_TARGET_TIMING* Timing);
long Bc250DpStreamBlank(BC250_DISP_IO* Io);
// Rate is the DPCD 8b/10b link rate code (6 = RBR, 10 = HBR).
long Bc250DpStreamUnblank(BC250_DISP_IO* Io,unsigned PixelClockKhz,unsigned Rate);
