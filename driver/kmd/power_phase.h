// The phases of BC250_DEVICE.RetainedPowerPhase: the retained power transaction of a started full-WDDM adapter
// (power.c, GpuSetPowerRetained). The field stays a LONG, because it is read and written with Interlocked calls.
//
//   ACTIVE -> SUSPENDING -> SUSPENDED      the way down (DxgkDdiSetPowerState to D1-D3)
//   SUSPENDED -> RESTORING -> ACTIVE       the way back (D0)
//   SUSPENDING or RESTORING -> FAILED      a step failed
//   SUSPENDING -> SUSPENDED_UNSAVED        BD-188 D-2: a step failed while the power goes all the same (a D3 target or
//                                          a known S3/S4, power_loss.h); the later steps ran in their forced form
//   SUSPENDED_UNSAVED -> RESTORING         the D0 witness proved that the power was lost: the full restore
//   SUSPENDED_UNSAVED -> RESTORING -> FAILED  the witness did not prove it (power kept, or no proof)
//
// FAILED ends the power transaction for this start. GpuSetPowerRetained refuses each later call, and nothing leaves
// FAILED until the next start sets ACTIVE (pnp.c). The bus driver takes the power after a failed way down all the
// same (dispmprt.md), so the adapter meets the next D0 with engines that it did not save. The pauses of pnp.c's
// Bc250SetPowerState stay in force until the next start: the fan stays with the board curve, DPM at its floor and the
// hang detector silent. The D0 in FAILED starts the SMU owner again for the CPU surface alone, so the driver's CPU
// temperature cap goes in again (power.c, PowerFailedThermal, BD-293). A way down in FAILED stops that owner.
//
// Many readers test only "not ACTIVE" (the value is not zero). ACTIVE stays zero for them.
#ifndef BC250_POWER_PHASE_H
#define BC250_POWER_PHASE_H

#define BC250_POWER_ACTIVE      0L
#define BC250_POWER_SUSPENDING  1L
#define BC250_POWER_SUSPENDED   2L
#define BC250_POWER_RESTORING   3L
#define BC250_POWER_FAILED      4L
#define BC250_POWER_SUSPENDED_UNSAVED 5L

// BD-293 lab arm: one step of the way down, numbered 1-5 in GpuSetPowerRetained. In the lab flavour, the step that
// LabPowerDownFault names (guard.c, GuardLabPowerDownFault) does not run and gives STATUS_IO_TIMEOUT, once per load.
// In the release flavour, and in the host tests, the macro is the call itself.
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
#define BC250_POWER_DOWN_STEP(Step, Call) (GuardLabPowerDownFault(Step) ? STATUS_IO_TIMEOUT : (Call))
#else
#define BC250_POWER_DOWN_STEP(Step, Call) (Call)
#endif

#endif
