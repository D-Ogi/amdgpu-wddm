// DCN201 DP0 training-symbol control. PHY power and lane drive belong to ATOM.
#pragma once
#include "../kmd/display_io.h"
long Bc250DpPhyConfigure(BC250_DISP_IO *Io,unsigned Lanes,int Enhanced);
long Bc250DpPhyPattern(BC250_DISP_IO *Io,unsigned Pattern,int Enhanced);
long Bc250DpPhyDisableTraining(BC250_DISP_IO *Io);
