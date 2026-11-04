// The phases of BC250_DEVICE.RetainedPowerPhase: the retained power transaction of a started full-WDDM adapter
// (power.c, GpuSetPowerRetained). The field stays a LONG, because it is read and written with Interlocked calls.
//
//   ACTIVE -> SUSPENDING -> SUSPENDED      the way down (DxgkDdiSetPowerState to D1-D3)
//   SUSPENDED -> RESTORING -> ACTIVE       the way back (D0)
//   SUSPENDING or RESTORING -> FAILED      a step failed
//
// FAILED ends the power transaction for this start. GpuSetPowerRetained refuses each later call, and nothing leaves
// FAILED until the next start sets ACTIVE (pnp.c). The bus driver takes the power after a failed way down all the
// same (dispmprt.md), so the adapter meets the next D0 with engines that it did not save. The pauses of pnp.c's
// Bc250SetPowerState stay in force until the next start: the fan stays with the board curve, DPM at its floor and the
// hang detector silent. The CPU temperature cap of the driver is not applied again, so the chip runs at its firmware
// limit (BD-293).
//
// Many readers test only "not ACTIVE" (the value is not zero). ACTIVE stays zero for them.
#ifndef BC250_POWER_PHASE_H
#define BC250_POWER_PHASE_H

#define BC250_POWER_ACTIVE      0L
#define BC250_POWER_SUSPENDING  1L
#define BC250_POWER_SUSPENDED   2L
#define BC250_POWER_RESTORING   3L
#define BC250_POWER_FAILED      4L

#endif
