# VSync diagnostic samples

The KMD retains interrupt entry, DCN handler entry, successful VUPDATE ACK and
returned CRTC_VSYNC notification times in device-owned nonpaged fields. These
are independent atomic samples, not a transaction or a frozen pre-reset record.
A returned notification callback does not prove acceptance by the scheduler.

The ISR counts mutually exclusive early exits: no MMIO, flip disabled, unarmed,
no VUPDATE event, read failure and ACK failure. Existing acknowledged-event and
interrupt counters remain available. The last successfully read value is the
raw OTG_GLOBAL_SYNC_STATUS word; an early exit leaves that value unchanged.
The existing periodic summary emits the samples; there is no per-frame logging,
additional MMIO access, change to acknowledgement writes or recovery policy.
CollectDbgInfo retains its existing Level Zero behavior and does not freeze them.

Times use [KeQueryInterruptTime](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kequeryinterrupttime),
which is permitted at any IRQL and returns 100 ns units at clock-tick resolution
(Microsoft reference checked 2026-09-27). Zero means unsampled since device start.
Use differences within a device start; timestamps alone do not prove continuous
IRQ delivery. Counters can wrap and a concurrent summary can cross an update.

Interpretation requires consecutive samples: a stale IRQ entry time locates a
gap before ISR entry; fresh ISR/DCN entry but stale ACK requires examining the
exit counters; fresh ACK but stale notification points downstream of the ISR.
These observations narrow an investigation but do not identify a hardware cause.
The fields add no public escape ABI. Dump analysis needs the exact build PDB.

Validation uses run_display_visibility.ps1, extracting the real DCN ISR and
checking successful ACK, each early exit, preserved counters and DPC queueing.
The host harness is deterministic; it does not validate hardware delivery,
concurrency, performance or recovery after a real timeout.
