# E35: bounded GFX submit pipeline

Hypothesis: CP read-pointer accounting, ordered KMD completion records and per-slot
RADV gather retirement can remove the unconditional previous-submit wait without
rewriting unread commands or reporting work that did not finish.

Scope: Windows implementation and host verification first. No Linux transition,
no on-ring TLB change, no multi-IB BC2S, no SDMA or paging queue redesign.
Keep the MMIO flush; drain before switching a busy VMID to another root.

Procedure, written before tests:
1. Snapshot the dirty source tree before edits (scratch/gfx-submit-pipeline/before.json).
2. Test the actual ring allocator against a synthetic CP read pointer: seven aligned
   slots in 2048 dwords, refusal without mutation, one-slot advance, physical and
   counter wrap, padding. Test cumulative fence wrap and ordered completion state.
3. Build the GFX replay and KMD with WDK 26100; build the current Mesa RADV ICD.
4. Preserve an incremental patch against the snapshot, source identities and logs.
5. Runtime promotion needs a separate bounded Windows control: two independent
   output buffers, overlapping accepted jobs, correct output/fence order; root
   switch and queue saturation controls. TinyLlama/performance acceptance follows
   those controls; neither compilation nor host tests establish a speedup.

Expected: host controls preserve unread ring words and per-job ownership. A failure
blocks deployment. G0/M9 status does not change from this side fix.

Result: M537 records the passing bounded content control and unresolved overlap/performance acceptance. M538 records compiler/analysis fixes and E14 exact-artifact smoke. See evidence/windows/2026-09-26-E35-gfx-submit-pipeline and evidence/windows/2026-09-26-E35-quality-gates.

## Authorized Windows control (owner, 2026-09-26)

The owner requested deployment while Witcher 3 installs. Do not terminate its
installer or restart Windows for the planned transition. Use PnP disable/install/
enable for the identified GPU, preserve UMD/ICD registration and native paging.
152 is based on the deployed151 source, not unrelated workspace recovery edits.
First compare the queue functions from that exact source in the host harness.

Run pipeline-control (16 independent 16 MiB mapped buffers, repeated GPU fills,
16 distinct patterns, finite fences) through the registered candidate RADV. A
post-doorbell KMD sample with observed < prior is the overlap witness; an API
NOT_READY sample alone does not establish hardware overlap. Check all 67,108,864
words. Then E14's eight existing compute checks, then two concurrent instances
for process/root changes if the first checks pass. Stop at the first timeout or
mismatch. Restore the baseline ICD after the bounded run. Keep clocks1000/820,
monitor temperature and health, and preserve the OS boot ID. No performance
conclusion while the game installer is active. No Linux session.

## Compiler contract gate (2026-09-26)

The standalone M536 winsys call in radv_device.c lacked its public header and
still used three arguments after the function gained a fourth. MSVC accepted
that implicit declaration; enumeration passed, while vkCreateDevice failed and
its early cleanup dereferenced an uninitialized shader-arena list (C0000005).
The E35 run003 reached all 16 GPU submissions after correcting the call and
including the shared header, but timed out at the process deadline. It is not
a passing pipeline result.

Apply radv-prototype-contract.patch on the recorded pre-fix source. It adds the
header, correct host argument and fatal MSVC C4013; C4020/C4024 were already fatal.
Build through build-radv-checked.cmd with the existing build CMD, generated
compile_commands.json and a fresh output directory as its three arguments.
The supplied build CMD must initialize MSVC and propagate compiler failures.
The wrapper propagates build and gate failures. verify-prototype-gate.py checks
every RADV C command, rejects absent/disabled protections, and compiles valid,
undeclared-call and wrong-arity controls using the actual warning configuration.
Checked build005 passed: 108 RADV C commands, one configuration, all controls.

This is a compiler gate, not an installed CI service or a substitute for runtime
validation. Candidate promotion must additionally exercise standalone instance,
device, allocation, submit/fence and content checks; hosted mode has a separate
gate. Enumeration alone cannot approve an ICD. No claim of certification.

The compiler gate now also supports KMD/UMD command filters and is used by tools/quality/quick.cmd. See docs/research/build-quality-gates.md for the current scope, cache and candidate profile.
