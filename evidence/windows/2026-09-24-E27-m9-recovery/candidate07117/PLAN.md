# Candidate 0.7.117.1 hardware plan

Hypothesis: RLC-scoped SDMA freeze/idle, halt and UTC_L1 disable before owner GTT retirement permit a correct subsequent full startup without an OS restart.

Use final package scratch/build/bc250kmd-07117-final/package-umd, SYS SHA256 1CD6BA41D8B4C8F2BCBF5DB0C47524488AFF496BD2D0A1A18C2F8BF259448F4D. Source and host validation: M370. Windows unit A at 1000 MHz / 820 mV, temperature below 85 C. Verify current boot and no full initialization since recovery, STOP absent, execution gates closed. Install display-only, first full startup and unchanged 64 KiB VRAM residency content control. Preserve all logs and validate four complete readbacks and hardware counters before instrumented PnP stop.

Require SDMA reload quiescence result 0 / fault 0 and inspect retained RLC and retirement observations before one warm startup. On failed quiescence, do not attempt warm startup. On success, repeat content control into fresh output paths. Compare PSP command 2 observations with M367. No unchanged failed retry. Preserve streamed logs; on unreachable lab independently verify connectivity then use authorized single AC recovery, collect persisted evidence and restore closed-gate display-only. Power alone is not liveness evidence. No firmware boot changes or live KDNET.

Success requires complete warm startup and correct content, not merely PSP completion or a busy-bit change. Failure narrows the sequence; it does not prove hardware impossibility. Broader M9 acceptance remains separate and open.
