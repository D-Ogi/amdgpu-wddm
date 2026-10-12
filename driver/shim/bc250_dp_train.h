// SST RGB8 at RBR/HBR. The caller owns the PHY and AUX for the entire call.
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
    int (*Pattern)(void *Context, unsigned Pattern); // 1 TPS1, 2 TPS2, 0 normal symbols
    int (*Drive)(void *Context, unsigned Swing, unsigned PreEmphasis);
    int (*Disable)(void *Context); // unconditional hardware cleanup, even on cancellation
    uint64_t (*NowUs)(void *Context);
    int (*DelayUs)(void *Context, unsigned Us); // passive wait for long delays
} BC250_DP_TRAIN_IO;

typedef struct BC250_DP_TRAIN_RESULT {
    enum BC250_DP_TRAIN_STATUS Status;
    unsigned Rate, Lanes, Enhanced, Attempts, Phase, CrSamples, EqSamples;
    unsigned Swing, PreEmphasis, Reads, Writes, WaitUs, AmdResult;
    unsigned CleanupFailed;
    uint8_t LastStatus[6];
    uint64_t ElapsedUs;
} BC250_DP_TRAIN_RESULT;

// Capabilities are the first 16 DPCD bytes. No inferred EDID bandwidth or rate.
uint32_t Bc250DpMaxPixelClock(const uint8_t Caps[16]);
enum BC250_DP_TRAIN_STATUS Bc250DpTrain(const BC250_DP_TRAIN_IO *Io,
    const uint8_t Caps[16], uint32_t PixelKHz, BC250_DP_TRAIN_RESULT *Result);
