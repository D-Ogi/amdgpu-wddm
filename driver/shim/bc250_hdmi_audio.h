// HDMI stream setup for an already validated, inherited RGB 8bpc TMDS link.
// Does not change the video mode, PHY, PLL or AVI InfoFrame.
#pragma once
#include "../kmd/dpaudio_seq.h"
long bc250_hdmi_audio_enable(BC250_AZ_IO *io, unsigned long stream,
    unsigned long pixel_clock_100hz, BC250_DPAUDIO_STREAM_RESULT *result);
long bc250_hdmi_audio_disable(BC250_AZ_IO *io, unsigned long stream,
    BC250_DPAUDIO_STREAM_RESULT *result);
int bc250_hdmi_audio_clock(unsigned long pixel_clock_100hz, unsigned long n[3], unsigned long cts[3]);
