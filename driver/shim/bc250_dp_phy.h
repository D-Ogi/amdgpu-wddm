// DCN201 DP0 training-symbol control. PHY power and lane drive belong to ATOM.
#pragma once
#include "../kmd/display_io.h"
long Bc250DpPhyConfigure(BC250_DISP_IO *Io,unsigned Lanes,int Enhanced);
// Pattern 1 to 4 sends TPS1 to TPS4, 0 returns to normal symbols.
long Bc250DpPhyPattern(BC250_DISP_IO *Io,unsigned Pattern,int Enhanced);
long Bc250DpPhyDisableTraining(BC250_DISP_IO *Io);
