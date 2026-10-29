// SST RGB8 at RBR, HBR, HBR2 and HBR3. The caller owns the PHY and AUX for the entire call.
// Hardware callbacks return zero on success and must each have a finite bound.
#pragma once
#include "bc250_atom_types.h"

enum BC250_DP_TRAIN_STATUS {
    BC250_DP_TRAIN_OK, BC250_DP_TRAIN_ARGUMENT, BC250_DP_TRAIN_UNSUPPORTED,
    BC250_DP_TRAIN_CANCELLED, BC250_DP_TRAIN_TIMEOUT, BC250_DP_TRAIN_IO_ERROR,
    BC250_DP_TRAIN_CR_FAILED, BC250_DP_TRAIN_EQ_FAILED
};
typedef struct BC250_DP_TRAIN_IO {
    void *Context;
    int (*Current)(void *Context); // same connector generation, powered and not stopping
    int (*Read)(void *Context, uint32_t Address, uint8_t *Data, unsigned Length);
    int (*Write)(void *Context, uint32_t Address, const uint8_t *Data, unsigned Length);
    int (*Enable)(void *Context, unsigned Rate, unsigned Lanes, int Enhanced);
    int (*Pattern)(void *Context, unsigned Pattern); // 1 to 4 TPS1 to TPS4, 0 normal symbols
    int (*Drive)(void *Context, unsigned Swing, unsigned PreEmphasis);
    int (*Disable)(void *Context); // unconditional hardware cleanup, even on cancellation
    uint64_t (*NowUs)(void *Context);
    int (*DelayUs)(void *Context, unsigned Us); // passive wait for long delays
} BC250_DP_TRAIN_IO;

// One rate the training tried: its link rate (DPCD LINK_BW_SET), the EQ pattern (2 to 4), the phase it ended in
// (1 CR, 2 EQ) and the AMD link_training_result.
typedef struct BC250_DP_TRAIN_ATTEMPT {
    unsigned Rate, EqPattern, Phase, AmdResult;
} BC250_DP_TRAIN_ATTEMPT;

typedef struct BC250_DP_TRAIN_RESULT {
    enum BC250_DP_TRAIN_STATUS Status;
    unsigned Rate, Lanes, Enhanced, Attempts, Phase, CrSamples, EqSamples;
    unsigned Swing, PreEmphasis, Reads, Writes, WaitUs, AmdResult;
    unsigned CleanupFailed;
    unsigned FirstRate, EqPattern;      // the rate tried first; the EQ pattern of the last attempt
    BC250_DP_TRAIN_ATTEMPT Tried[4];    // Attempts of them, in order; rates only go down
    uint8_t LastStatus[6];
    uint64_t ElapsedUs;
} BC250_DP_TRAIN_RESULT;

// The most any 8b/10b link carries at RGB8: HBR3 x4, 8.1 Gbps x 4 lanes x 8/10 / 24 bits. Above the 655.35 MHz an
// EDID detailed timing can state, and under the 1200 MHz DISPCLK of dcn201_resource.c.
#define BC250_DP_MAX_PIXEL_KHZ 1080000u

// Capabilities are the first 16 DPCD bytes. No inferred EDID bandwidth or rate.
uint32_t Bc250DpMaxPixelClock(const uint8_t Caps[16]);
// The rate Bc250DpTrain tries first for this pixel clock, 0 when no rate of these caps carries it.
unsigned Bc250DpFirstRate(const uint8_t Caps[16], uint32_t PixelKHz);

// GitHub #45 review: the first downstream port of a DP branch device, as Linux get_active_converter_info reads it
// (DPCD 0x005, then the detailed capabilities at 0x080 on DPCD 1.1 and later).
enum BC250_DP_DOWNSTREAM {
    BC250_DP_DOWNSTREAM_NONE,   // no branch device: the receiver is the sink
    BC250_DP_DOWNSTREAM_DP,     // a branch device with a DP port (a hub or a repeater): the DP link is the limit
    BC250_DP_DOWNSTREAM_VGA,    // active converters: they end the DP link and have their own pixel-clock limit
    BC250_DP_DOWNSTREAM_DVI,
    BC250_DP_DOWNSTREAM_HDMI,   // HDMI and DP++ downstream ports (DP 1.4: a DP++ port is an HDMI port)
    BC250_DP_DOWNSTREAM_OTHER   // a type this driver has no limit for
};
// The pixel clock the converter on a converter port takes when it states none, or when it is a type without one.
#define BC250_DP_CONVERTER_DEFAULT_KHZ 300000u
// DPCD DP_DOWNSTREAM_PORT_0, the 16 bytes of detailed capabilities that Port below holds.
#define BC250_DP_DOWNSTREAM_PORT_DPCD 0x80u
// Whether DPCD 0x080 is to be read for these caps: a branch device on DPCD 1.1 or later.
int Bc250DpDownstreamPortReadable(const uint8_t Caps[16]);
// The pixel-clock limit of the converter behind the DP receiver, 0 when there is none (no branch device, or a DP
// downstream port). Port is DPCD 0x080-0x08F, NULL when it was not read. Type receives enum BC250_DP_DOWNSTREAM.
// A DVI or HDMI port with detailed capabilities states its maximum TMDS clock (byte 1, 2.5 MHz units), a VGA port
// its maximum pixel rate (byte 1, 8 MHz units). Without that byte, or at 0, or for another type, the limit is
// BC250_DP_CONVERTER_DEFAULT_KHZ.
uint32_t Bc250DpDownstreamLimit(const uint8_t Caps[16], const uint8_t *Port, unsigned *Type);
// What a DP sink takes: Bc250DpMaxPixelClock under the converter limit. 0 when the caps are not valid.
uint32_t Bc250DpSinkMaxPixelClock(const uint8_t Caps[16], const uint8_t *Port, unsigned *Type, uint32_t *ConverterKHz);
enum BC250_DP_TRAIN_STATUS Bc250DpTrain(const BC250_DP_TRAIN_IO *Io,
    const uint8_t Caps[16], uint32_t PixelKHz, BC250_DP_TRAIN_RESULT *Result);
