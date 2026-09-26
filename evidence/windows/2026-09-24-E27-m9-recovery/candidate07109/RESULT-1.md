# M348 - First control and changed retirement ordering pass

Exact109 installed closed, then one normal shutdown/verified8secondACcycle.
Boot2026-09-24T05:12:37. First full startup passes;64KiB control passes3cycles,
4GPUwordreadbacks, graphics4/4 and paging1095/1095, no timeouts/refusals/TDR.

New stop order is witnessed: after-stop/beforePSPunload/afterTMR/afterPSPring
all have CNTL0/STATUS2 0x8. PSPunload succeeds. GART hardware-disable status0;
RLC still0/0x8. First GTT unbind keeps0/0x8; first GFXHUBflush changes STATUS2
to0x01000008, persistent across remaining cleanup and firmware restore282writes.
All RLCreadstatuses0, no retention/TLB-failure message, finalstage79. Device
restart returns healthy closed-gate display-only in the same boot.

Thus phase ordering changed as intended, but it did NOT eliminate the observed
post-flush busy bit. It remains insufficient evidence to predict next startup.
A single separate warm full-start trial is next, after preserving these logs.
No warm success/failure yet. Smart-plug telemetry is auxiliary and uncalibrated.
