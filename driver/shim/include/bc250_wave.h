/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_SHIM_WAVE_H
#define BC250_SHIM_WAVE_H

#include "amdgpu.h"
#include "../../kmd/wave_snapshot.h"

/* Zero continues. Otherwise return BC250_WAVE_READ_ERROR and/or
 * BC250_WAVE_BUDGET_EXPIRED. The callback must notice a backend access failure
 * before its returned value is treated as hardware data. It must not sleep. */
typedef unsigned int (*bc250_wave_stop_fn)(void *context);

/* For the kernel wrapper's checked cleanup after a failed backend sequence. */
unsigned int bc250_gfx_wave_broadcast_value(void);

/* Caller owns the GFX register/lifecycle lock, an initialized snapshot and a
 * dedicated register backend. Geometry and capture counters are overwritten;
 * metadata, timing, result and the unused record tail remain caller-owned.
 * Returns 0 for a complete observation, -1 for partial/error/unsupported.
 * Only GC10.1.3 with the existing 2-SE, 2-SA, 5-WGP topology is admitted.
 * No SQ_CMD or SQ_IND_DATA write is issued. GRBM broadcast is attempted on
 * every admitted exit; the caller must also restore selectors through a
 * checked path that still runs after the normal backend has latched a fault.
 * The caller saves/restores SQ_IND_INDEX. No read here halts wave execution. */
int bc250_gfx_read_waves(struct amdgpu_device *adev,
                        BC250_WAVE_SNAPSHOT *snapshot,
                        bc250_wave_stop_fn stop, void *context);

#endif
