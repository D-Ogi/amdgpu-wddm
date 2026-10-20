// One serialized connector discovery. No allocation, Windows calls or PHY writes.
#pragma once
#include "bc250_ddc.h"
#include "../kmd/edid.h"

enum bc250_sink_kind { BC250_SINK_NONE, BC250_SINK_DP, BC250_SINK_HDMI, BC250_SINK_DVI };
enum bc250_sink_phase { BC250_SINK_PHASE_NONE, BC250_SINK_PHASE_AUX_OPEN, BC250_SINK_PHASE_DPCD,
    BC250_SINK_PHASE_AUX_EDID, BC250_SINK_PHASE_DDC_OPEN, BC250_SINK_PHASE_ADAPTER,
    BC250_SINK_PHASE_DDC_EDID, BC250_SINK_PHASE_PARSE, BC250_SINK_PHASE_READY };
typedef struct BC250_SINK {
    unsigned Kind, Phase;
    long Status, CleanupStatus, AuxStatus, DdcStatus;
    unsigned long EdidLength, MaxPixelClockKhz, TargetCount;
    unsigned char Dpcd[16], Edid[BC250_EDID_MAX_BYTES];
    BC250_DUAL_MODE_ADAPTER Adapter;
    BC250_EDID_INFO Info;
    BC250_TARGET_TIMING Targets[BC250_EDID_MAX_MODES];
} BC250_SINK;

typedef struct BC250_SINK_GUARD {
    void* Context;
    // Powered, not stopping, and no connector generation change. Access callbacks
    // enforce it on every transaction too; cleanup must remain possible on unplug.
    int (*Current)(void* Context);
} BC250_SINK_GUARD;

// Io is already owned by the passive operation. Restore uses that same physical
// owner even if Current becomes false. A failed cleanup prevents a second protocol.
// Result is caller-allocated nonpaged workspace, not a kernel stack local.
// MaxDpKhz bounds a DP sink, MaxTmdsKhz an HDMI or DVI sink (behind a dual-mode adapter): each is the ceiling of
// that protocol's route, under the sink's own limit.
long Bc250SinkDiscover(BC250_DISP_IO* Io,const BC250_SINK_GUARD* Guard,unsigned long MaxDpKhz,
    unsigned long MaxTmdsKhz,unsigned long MaxWidth,unsigned long MaxHeight,BC250_SINK* Result);
