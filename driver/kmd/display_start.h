// The display-start state of one adapter start (BD-341). It says what the start found on the display side, and every
// part of the driver that depends on it reads this one value: the start sequence (pnp.c), the memory layout (vram.c),
// the connector and its child reports (connector.c), the DCN stop (dcn.c), the D0 restore (power.c), the PnP release
// and the start record (guard.c, value DisplayStart). No reader tests the POST geometry for zeros instead.
// docs/design/wddm-startup.md, "Start without a firmware display", has the contract, and docs/design/cold-modeset.md
// the plan for the cold mode set.
//
// Plain C with no WDK types, so that the host tests include it as it is.
#pragma once

#define BC250_DISPLAY_START_UNKNOWN 0ul     // no POST answer yet in this start
#define BC250_DISPLAY_START_INHERITED 1ul   // the firmware lit a pipe, and this driver keeps it (M3 sets no mode)
#define BC250_DISPLAY_START_NO_POST 2ul     // the firmware handed over no display: no pipe, no framebuffer, no timing
// The state says what the start found, and it does not change until the stop. A mode that this driver lights later,
// also on a NO_POST start (the cold-modeset plan), is a runtime mode: RuntimeDisplayChanged and RuntimePost, as after
// a connector commit on an inherited start. So the cold mode set adds no state here.

// The answer of DxgkCbAcquirePostDisplayOwnership as a state. A failure, or a zero in any of the four values that a
// scan-out needs, is no display: the firmware lit nothing that this driver could keep.
static __inline unsigned long Bc250DisplayStartClassify(long AcquireStatus, unsigned long Width, unsigned long Height,
                                                        unsigned long Pitch, unsigned long long Address)
{
    if (AcquireStatus < 0 || Width == 0 || Height == 0 || Pitch == 0 || Address == 0)
        return BC250_DISPLAY_START_NO_POST;
    return BC250_DISPLAY_START_INHERITED;
}

static __inline const char* Bc250DisplayStartText(unsigned long State)
{
    switch (State) {
    case BC250_DISPLAY_START_INHERITED: return "inherited";
    case BC250_DISPLAY_START_NO_POST: return "no-post";
    default: return "unknown";
    }
}
