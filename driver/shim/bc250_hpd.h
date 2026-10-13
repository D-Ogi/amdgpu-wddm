// DCN201 HPD0 only. Caller serializes these short register operations with ISR.
#pragma once
#include "../kmd/display_io.h"

typedef struct BC250_HPD {
    BC250_DISP_IO* Io;
    unsigned long BeforeControl, BeforeInterrupt;
    int Saved, Armed;
    unsigned Events; // last acknowledged IRQ: bit0 long edge, bit1 DP HPDRX
    long Error;
} BC250_HPD;

long Bc250HpdArm(BC250_HPD* Hpd, BC250_DISP_IO* Io);
long Bc250HpdDisarm(BC250_HPD* Hpd);
// 1 when an owned HPD or HPDRX interrupt was acknowledged, 0 if none, -1 on I/O
// failure. Sense is physical state; Rx is a short DP sink interrupt, not unplug.
int Bc250HpdInterrupt(BC250_HPD* Hpd, int* Sense, int* Rx);
long Bc250HpdSense(BC250_HPD* Hpd, int* Sense);
