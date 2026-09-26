# M400 - Candidate125 full-stop policy executes, warm failure remains

Unit A,2026-09-24. Installed0.7.125.1,
SYS52A2E0FA025CCA857AE1ED918D90B26CAB64CC0CCD30DF4E4AF4101EC45EFA6E.
STOP absent, exact package verified, clock/temperature preflights pass.
Installed into recovered boot11:17:48 without baseline OS reset.
First full start and64KiB3cycle eviction/restoration GPU oracle pass.
Complete control outputs and before-warm logs preserved before stopping.

Stop shows SDMA pre/post-reset quiescence0, GFXHUB retirement0/fault0,
GART hardware disable status0, then the new message:
'gart: full stop retains disabled translation state'. No final firmware-state
restore message. Display-only stage61 and CLI0 in unchanged boot establish
that this policy ran and returned. The log witnesses calls and status; it is
not an independent readback of every disabled hardware bit.

One warm start at11:29:26 losesSSH after PnP enable. Subnet discovery finds
one other listener,zero pinnedlab matches. Recovery independently checks both
configured OS endpoint sets unavailable. One recorded OFF8sON cycle restores
Windows boot11:30:53. Old SSH55072 terminated only after confirmed powercycle;
its4294967295 result is local transport termination,not remote success/failure.

Latest ring-20260924-092926-938.log persists after-RLC-resume0.303s,
after-invalidate-request0.309/fault0, pure observer-return checkpoint0.311s.
No after-request-read checkpoint. This matches M398's observed interval.
Removing final boot-state restoration does not resolve the failure. Exact
stalled instruction remains unproved: persistence completion and required
request read still lie after the last checkpoint. Do not omit the dummy read.
Warm PSP again completes11commands, RLCbusy first observed afterSDMA0command2.

Recovered125display-onlystage61,CLI0,FullWddm0,UnconfirmedStarts2; other
execution/diagnostic gates as captured. USBloaderOFF,plugON. No second trial
or post-recovery reset. Telemetry at09:32:03UTC reports119.9W/0.726A/242.8V;
rawDPS retained, scale uncalibrated, measurement age unknown, not OS proof.
Interface/PCI instance identities redacted in copied logs.

Next investigate firmware reload dependencies and reset-state differences,
using source and existing Linux evidence. M351 already warns that generic
MODE1 dispatch may return success with an absent callback on this APU; do not
treat that as a working reset reference or add speculative register masks.
Full M9 and warm recovery remain open; do not repeat125unchanged.
