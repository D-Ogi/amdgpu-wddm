# Single-instance SDMA retained reset primitive

2026-09-25. Source/host/build only. No hardware access, deployment, version change,
DDI wiring or OS-fence bookkeeping change. Parent owns subsequent coordination.

## Change

Only production files changed for this task:
- driver/shim/bc250_sdma.c
- driver/shim/include/bc250_sdma.h

New API:

    int bc250_sdma_reset_retained_instance(struct amdgpu_device *adev,
        u32 instance, struct bc250_sdma_reset_receipt *receipt);

The receipt records selected instance, last stage, prior software WPTR in dwords
and final hardware RPTR/WPTR in bytes. BC250_SDMA_RESET_PROGRAMMED is deliberately
not READY. Return 0 means sequence and empty-transport readbacks passed; a fresh
hardware content/fence oracle remains necessary before accepting work.

Sequence:
1. Validate one of the existing two prepared rings and its own WB slots.
2. Source-derived conditional RLC scope, selected queue stop/freeze/halt, exit.
3. Existing AMD single-instance GRBM soft reset, 50 us, readbacks.
4. Second scope and selected re-quiescence. Readback confirms queue/cache off
   and HALT before CPU modifies the transport. This reuses the established
   reset_for_reload policy but selects only the requested instance.
5. Clear only this ring to NOP; clear its software transport and two WB slots.
6. Unfreeze and program the existing per-instance resume sequence with explicit
   reset_transport=true. It skips E15's hardware-WPTR adoption entirely.
7. Verify zero hardware RPTR/WPTR/WB plus ring/IB/cache enabled, HALT/FREEZE clear.

The shared resume function takes an extra internal bool. The old startup call
passes false, retaining its measured E15 adoption behavior and register trace.
No allocation/free, firmware load, TMR/GART change, doorbell submission, fence
completion or old packet replay occurs in the new API. No production caller is
added. A failure keeps responsibility for backing and admission with the caller;
it is not a guarantee that every intermediate hardware setting was unwound.
The function must not be invoked with a PLAN backend because it changes ring/WB
memory. The caller must exclude same-instance CPU publishers/readers and retain
backing, firmware and translation throughout. Backend MMIO stores must order
prior CPU stores before enabling the queue; current KMD mmio.c uses
WRITE_REGISTER_ULONG, not a no-fence accessor.

## Reference and explicit composition

PROVENANCE: existing AMD amdgpu SDMA5 register sequences, AMD MIT, v6.18
7d0a66e4bb9081d75c82ec4957c50034cb0ea449. The GPL-labelled September 2026
patch series was read for its stale-transport policy; its code was not imported.

- ref/linux-src/drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c:
  gfx_resume_instance:688, soft_reset_engine:1330, stop_queue:1566,
  restore_queue:1615. New MMIO uses only the existing imported helper/accessors
  and original AMD named registers/fields; no register offsets were invented.
- ref/web-docs/sdma-reset-2026-09__WARN-GPL-newer-than-linux-src/series-thread.txt:
  patches 5/6 describe clearing ring and CPU/WB pointers after reset, before
  restart. The older restore=true saved-WPTR policy is deliberately not used
  by this zero-transport helper. Empty transport has its own final readback.
- M388/M389, evidence/linux/2026-09-24-E29-sdma-reset: measured successful
  timeout-driven live reset of physical SDMA1 on this unit. It is not a Windows
  measurement, not SDMA0 acceptance, and not this extra post-reset quiescence
  composition. The helper needs a separate hardware oracle.
- Existing helper bc250_sdma_reset_for_reload already composes post-reset
  quiescence. The new instance-scoped composition preserves firmware/other ring
  ownership instead of using whole-GFX stop/start or retained-power APIs.

## Tests and negative controls

All focused tests call the compiled production shim, not a copied function.
Scratch test.c uses existing backend_trace/backend_mem plus explicitly declared
register/freeze/reset models, and original AMD register headers.

- Existing full run_gfx.ps1: EXACT MATCH, 354 + 35 original trace writes;
  all existing control runs fail as expected; SDMA pointer/adoption, reset,
  quiescence and RLC policy checks pass. Kernel-flag shim compilation passes.
  replay.log contains the full result.
- Focused /W4 /WX fixture: 57994 checks, 0 failures (focused.log).
  Both selected instances; exact zero-gating register write order; sibling ring,
  descriptors, WB and register state preserved; original selected ring content
  and pointers discarded before unfreeze; conditional RLC scope; reset defaults
  intentionally disturb halt/queue state and must be re-quiesced; freeze/idle
  refusal preserves old CPU transport; post-reset halt mismatch refuses clear;
  sticky nonzero hardware WPTR refuses success without adoption; bounds rejected
  before MMIO; no doorbells are emitted.
- omit-clear mutation: compiles, 57994 checks / 20480 expected failures.
- wrong-engine mutation: compiles, 66168 checks / 8268 expected failures.
- allow-adoption mutation: compiles, 57994 checks / 2 expected failures.
  Mutation sources/logs and mutate.py are preserved here; each run exits 1.

Host models do not prove hardware reset completion or absence of pending bus
transactions. They establish code order, ownership isolation and refusal paths.
There is no fresh GPU job execution in this fixture.

## Frozen full WDK build

source/ holds 481 inputs with SHA256SUMS. The original build.ps1 is retained;
build-unsigned.ps1 stops after compile/link/stackbudget and before INF/catalog,
certificate access or signing. wdk.log records success. The linker can discard
the unreferenced new API; kernel compilation still checks its implementation.
No runtime use of the helper follows from producing this SYS.

Output: P:/bc-250/scratch/build/sdma-single-recovery-dev/package/bc250kmd.sys
SHA256: 0B218D0C52D23539A90F416895012FB7FD2D8F63EE837A9F02BF2705DDC5DDBE
Authenticode: NotSigned. Version unchanged.
Stack check: 568 unwind functions, largest fixed frame 3992 bytes;
existing warnings are recorded in wdk.log, no new stack-budget violation.

Reproduce: run_gfx.ps1 -Out <this-dir>/replay, then run.ps1.
run.ps1 -SdmaSource <mutation.c> -Out <separate-dir> reruns one negative control.
freeze.py describes the frozen build inputs; do not overwrite the existing
snapshot. Production before-images are bc250_sdma.before.c/.h.

## Next measurable step and limits

The parent can add a bounded pre-WDDM-publication diagnostic transaction after
normal GPU startup. First idle reset SDMA0, then fresh marker/fence and full copy
readback on retained addresses. Next separate test may reset an owned blocked
memory-poll job and prove its old marker remains poisoned after release, while
new SDMA and CP shader content controls pass. Keep all backing throughout.
Do not set OS ResetEngine success from this receipt alone. Node recovery
synchronization, scheduler replay/fence reconciliation, paging-TDR escalation to
adapter-wide reset, and adapter no-memory-access recovery remain unimplemented.

## Exact source hashes

- bc250-win\driver\shim\bc250_sdma.c SHA256 C760060822FC7F1F6997ADEF359E62B390C4BC8B76B504B39EE7C4D87A23FA1E
- bc250-win\driver\shim\include\bc250_sdma.h SHA256 7F7D58FA7C93CCD1D8BF9861D4B11CB4BC708EF926DAF6C7E6A4C1C4D126E3AC
- scratch\m9\sdma-single-recovery\test.c SHA256 AB1E6F738677926687EC374F2A73540AFFEB3F7BE84BFCEF8D28C69587C953FA
