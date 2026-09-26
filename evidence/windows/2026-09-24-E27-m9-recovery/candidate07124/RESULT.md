# M398 - Candidate124 warm failure persists after the pure observer checkpoint

Unit A, 2026-09-24. SYS13858D3FF1018C2F1F42636210B525F960DC1FF9C8A1FF66311B01CD8B81F351,
version0.7.124.1. STOP absent; current123display-only boot11:01:13 verified.
Installation closes gates, verifies clock/temperature, hash/version and passes
without an OS restart. First full start completes request/read/ACK, MMHUB and
publication checkpoints. The64KiB probe passes three eviction/restoration
cycles with GPU readbacks. Full outputs preserved before stop.

Stop succeeds: pre/post SDMA reset quiescence0, retirementGFXHUB0/fault0.
Read-only inspection confirms display-onlystage61,CLI0,sameboot11:01:13.
One warm start at11:16:11 losesSSH after PnPenable. Configured addresses have
zero listeners; subnet discovery finds one other listener,zero pinnedlab matches.
Old warm SSH69020 remains locally alive; it is terminated only after confirmed
AC recovery. Native4294967295 is transport termination, not remote completion.

Latest snapshot ring-20260924-091611-963.log contains after-rlc-resume0.303s,
before-gfx-visibility0.307s, after-invalidate-request0.309s/fault0 and visibility
observer returned after-invalidate-request0.311s. No after-request-read checkpoint.
Prior snapshot ends at the request sample. The observer now contains no MMIO.
The interval still includes completion of persistence, return to the shim,
and the required dummy request read before its next observation. Thus the
diagnostic reads removed by124 are not necessary for this warm failure;
this does not prove a specific stalled CPU instruction or that the request
read may be omitted. All11PSPcommands succeed; warm RLCbusy again first appears
after SDMA0firmwarecommand2, before host GFX invalidation.

One recorded AC recovery returns Windows boot11:17:48,124display-onlystage61,
CLI success, FullWddm0. Other diagnostic/execution gates remain set as recorded.
USBloaderOFF,plugON. No second warm trial or post-recovery reset.
Copied logs redact interface and PCI instance identities only.

Warm recovery and ideal M9 remain open. Next review source-derived GFXHUB,
RLC and firmware reload dependencies before changing another candidate.
Retain request/dummyread/ACK and publication guarantees; do not rerun124unchanged.
