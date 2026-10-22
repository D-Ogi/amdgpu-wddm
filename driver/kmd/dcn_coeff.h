// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "display_io.h"
#include "../shim/bc250_scaler_plan.h"

enum BC250_COEF_PHASE {
    BC250_COEF_IDLE, BC250_COEF_READY, BC250_COEF_SUBMITTED, BC250_COEF_POISONED
};
typedef struct BC250_COEF_STATE {
    uint64_t Generation;
    uint32_t Phase, Pipe, CurrentBank, TargetBank, ChromaMode;
} BC250_COEF_STATE;

// Zero-initialize once per device object, one state per pipe. The display
// owner serializes every call and every other scaler writer. Plans are private
// preparation storage. Stage runs at PASSIVE_LEVEL with OTG0's lock already
// acknowledged, for a pipe positively known to feed that OTG. It never stalls,
// releases the lock or changes SCL_MODE. Memory must already be powered on.
// Every pair gets an explicit selector (unlike AMD's auto-increment stream),
// so staging does not depend on the currently programmed tap count.
// Caller may release the OTG lock after staging; READY reserves the bank until
// cancellation or submission. No other path may write that pipe's coefficients.
long Bc250CoefStageLocked(BC250_DISP_IO* Io, BC250_COEF_STATE* State,
    unsigned Pipe, unsigned ChromaMode, const BC250_SCALER_PREPARED* Plan,
    uint64_t* Ticket);

// Short commit step under the same acknowledged OTG0 lock as all geometry and
// surface-address writes. Caller must validate ALL plane state before this call.
// Success means the selector was written, not latched. A write failure poisons
// ownership because it may have reached hardware. Caller restores coherent
// state while still holding the lock; it must not retry only the selector.
long Bc250CoefSelectLocked(BC250_DISP_IO* Io, BC250_COEF_STATE* State, uint64_t Ticket);

// Poll once, no wait. Call after the commit unlock; current AND selected bank
// must match the submitted target. This retires BANK ownership only, never a
// Windows PresentId or a scanout allocation. Those need whole-plane evidence.
long Bc250CoefObserve(BC250_DISP_IO* Io, BC250_COEF_STATE* State, uint64_t Ticket);
long Bc250CoefCancel(BC250_COEF_STATE* State, uint64_t Ticket);

// Only after the display owner has quiesced/reset the hardware and excluded all
// writers. Preserves monotonically increasing tickets, including reset/stop.
// Exhaustion fails closed; never zero/reinitialize a live state to recycle it.
long Bc250CoefInvalidateQuiesced(BC250_COEF_STATE* State);
