# Candidate 0.7.100.1 startup, unit A, 2026-09-23

Installed oem74.inf, SYS8017E7BF050AACE3F16F06097A3BF2BF53229045240C0CE55049706637A3C6F6. Signed build and full-model packagecheck pass (25checks, pre-existing InfVerif1199 note). Closed-gate install restores deviceOK; subsequent one-shot full startup succeeds, native task completes. All eight CP checkpoints returned success; final persisted disk snapshot includes their results and SDMA/interrupt completion. First post-start summary has292/292paging jobs, zero timeouts/refusals. Info escape confirms FULL WDDM TABLE, version0x00070064. FullWddm0 on disk is the consumed one-shot, not the active DDI table.

Boot remains15:39:09 throughout. The owner had previously removed AC; this session had not run GPU inference before the candidate start. Consequently this is NOT acceptance of repeated initialization after GPU work, and does not establish why0799 hung. Native accelerated display gates remain closed. Device and SSH healthy at final collection16:05:26,1000MHz/VID116,71.4C.

Only private adapter instance suffixes were redacted from text. Raw files remain in scratch/m9. No BIOS changes or OS reboot issued.
