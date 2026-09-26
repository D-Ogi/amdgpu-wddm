# M432 - Candidate 0.7.134.1 DCN/VidPn hardware control

Unit A, 2026-09-24. One PnP disable/install/enable, no Windows, DWM or AC restart.
Full-WDDM SYS SHA256:
`8DC2172B803F965F6F91C1FAB7116DEB777369F9A4A6FA85FD9441B2F1597534`.
Package built and signed in scratch/build/bc250kmd-07134/package-umd.
Previous133 remains available for rollback, SHA37A52F95... (M426/M428).

## Result

- Preflight19:33:13: full133, no errors, DWM2036,66.5C.
- PnP enable19:38:34: full134, all startup SDMA controls pass, guard confirmed,
  one-shot Full consumed to0 and both SDMA startup-control gates returned to0.
- Through19:44:36 the OS boot remains18:23:50; DWM2036 remains since18:24:54.
  M412 Mesa main/LLVM23 llvmpipe DLL loaded with unchanged SHA D438EA42... .
  This is CPU JIT D3D rendering with hardware DCN scanout, not GPU D3D.
- D3D control exits0: shared red/blue textures have0/2048 mismatches each;
  green texture0/307200,60 presents, device-removed status0. Actual scanout
  preview shows the green window, desktop and whole overlay. It is not proof
  of uninterrupted temporal smoothness; owner feedback remains pending.
- Original64MiB probe, SHA E9566E49..., exits0: three evict/restore cycles,
  four complete GPU readbacks, all words match, fences64/128/192/256.
- Eight Vulkan shaders have CPU-matching hashes; stories15M and TinyLlama
  outputs exactly match E14 references. Every native exit0; actual ICD witness
  identifies retained RADV/ACO SHA DB886B8D... . This is correctness coverage,
  not a new matched Windows/Linux performance result.
- Final GFX2610/2610; SDMA6051/6051; zero timeouts/refusals, no TDR.
  DCN533 hardware flips,0 refused,0 ACK timeouts;21550 acknowledged vsync events,
  no failed IRQ acknowledgement. Eleven ACK-counter snapshots in final.log
  all show0. Clocks1000MHz/VID116 (820mV) verified; final temperature66.6C.

## Diagnostic refusal and observer correction

One `dcnflip restore` returned D3DKMTEscape STATUS_DEVICE_BUSY (0x80000011),
CLI exit1. guard-control.ps1 incorrectly expected CLI exit3 plus the inner DCN
reason string, so that script exits1. Preserve this failure as an observer
expectation error; it is not a second driver transition or content failure.

Source inspection identifies the earlier existing WddmDiagnosticAllowed gate
in display.c: full WDDM refuses this command before calling DcnFlipEscape. Thus
this hardware trial validates dispatcher refusal, not the new M431 inner guard.
The latter remains actual-source host-tested. The old broad claim that escape
and DDI could interleave in full WDDM overlooked this existing dispatcher gate.
No bypass or repeated diagnostic attempt was made. The successful final read
shows the original OS/DWM, continued flips and no errors after the refusal.

## Limits and next work

M430-M431 dcn.c/wddm.c/bc250kmd.h exactly match the hardware-build inputs; only
version/INF were advanced to134. Source tests and negative controls are in the
adjacent dcn-lock-ack-review and vidpn-flip-publication evidence directories.
The normal path uses no ACK-timeout fallback on this run; actual timeout/error
handling and generation races are not forced on hardware. No30-minute or
physical-input acceptance is claimed. Owner observation was requested and was
not yet received when this evidence was finalized.

Current deployment is workspace STATE.md. The M9 cold/OS-boot gate, native SMU
ownership/readiness, cache/PFN/lifetime, capture fallback, cancellation/preemption
and matched performance requirements remain open. 6GiB residency was measured
on133, not rerun here; no12GiB claim. D5/D6 and IRQ enable/ISR synchronization
remain in docs/research/bios-analysis-followup.md.

The initial read-only preflight was mistakenly rejected by automatic approval
review as a PnP mutation. Full script/helper inspection established its read-only
scope; the same operation was then allowed, and the owner also explicitly
approved it. No rejected action was bypassed. Later PnP was separately reviewed
and ran within the standing lab authorization.
