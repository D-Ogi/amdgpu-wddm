# M349 - Opt-in RLC reload reset prepared; hardware acceptance absent

Candidate 0.7.110.1, SYS SHA256
4743B3CE3D177970BD33F30F00E01B242E00EDDCDB2C5353014E808FDA48D9D1.
No deployment or lab state change belongs to this result.

The imported gfx_v10_0_rlc_reset callback body matches the local AMD reference
with only whitespace and the shim delay symbol normalized. See
callback-comparison.json for source hashes and the exact comparison body.
This establishes source correspondence, not a working reset on unit A.

The default-off EnableRlcReloadReset setting selects an experiment before PSP
initialization in unpublished full WDDM startup. The KMD serializes against GFX
paging and GART, requires an existing unstarted GFX object and enabled GART,
and propagates sequence faults. The shim selects only disabled, busy RLC;
all CP ME/PFP/CE, MEC1/MEC2 and SDMA0/1 halt bits must be set. The original
callback asserts/deasserts only SOFT_RESET_RLC with two 50 us delays. Busy is
read afterwards; remaining busy refuses startup. No reset is selected when
RLC is enabled or busy is clear. This guard is a project-specific experiment;
ordinary AMD resume does not call this reset callback. It is not an idle,
DMA-completion, memory-release or general GPU-reset proof.

Validation:
- Ordinary replay: exact 354 + 35 writes, 24 documented address exceptions;
  all four negative controls rejected.
- Isolated shim model: 11 scenarios, zero failures, including each missing
  halt bit, no-write cases, two-write success and stuck-busy refusal.
  Busy clearing is explicitly modeled; real MMIO behavior and delay timing
  are not validated by this model.
- Actual startup coordinator: 327 checks, zero failures. Reset refusal unwinds
  before PSP load. This does not directly execute the KMD locking wrapper.
- WDK build succeeds; package checks: 25 passed, zero errors or warnings.
- General raw MMIO write permission remains limited to the two scratch registers;
  the reset register is added only to the GFX sequence allow-list.

The next hardware trial must verify exact installed SYS, keep the new gate
explicitly controlled, establish first-start GPU byte correctness, preserve
stop logs, and perform one same-boot warm startup with the experimental gate.
Capture the preflight halt words, reset result/status, before/after RLC state,
PSP results and scheduler checkpoints. A successful startup also needs the
unchanged GPU byte-control workload. A refused reset is a measured precondition
or postcondition failure, not a reason to bypass the guard. Recover persistent
logs before another change if transport is lost. Close the experimental gate
alongside execution gates during recovery.

M348 remains the latest hardware result: 109 first start passes, warm reentry
fails. DMA cache/alias/resource/lifetime and matched performance acceptance
remain open. No source-model result here changes those conclusions.
