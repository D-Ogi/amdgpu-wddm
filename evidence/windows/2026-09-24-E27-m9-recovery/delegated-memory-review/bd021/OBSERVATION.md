# BD-021 observation instrumentation (source only)

2026-09-24. Requested by main after the bounded BD-021/BD-022 source reviews.
Existing BD-021 stays NEEDS-LAB; this change prepares observation, not a cache fix.
No mapping policy, PTE encoding, capability, version, driver ABI or lab change.

## Changed files

- driver/kmd/vidmm.c: count validated encoding attempts by input page-table level
  and domain (system, application local, dedicated table local), separately for
  CacheCoherent=1/0 and mismatch with encoded SNOOPED. Include CPU_VIRTUAL /
  pre-RUN CPU initialization and normal GPU_PHYSICAL encoding. Repeat counts
  output entries and GPU batches count only their selected slice. Invalid entries
  do not count. Totals may include re-encoding during logical publication or
  encoding before a later destination/resource failure: they are not unique
  pages, successful submissions, GPU retirement or residency.
- Per-callback local accumulation uses72bytes of counters; each nonempty bucket
  gets one atomic add, at most9 per callback. No per-entry added interlocked call.
- VidMmSummary emits level, actual segment ID, coherent, noncoherent and mismatch
  totals for nonempty buckets. Independent atomic reads are an approximate live
  observation, not one synchronized snapshot. Counters reset at VidMmStartLayout.
- experiments/E27-m9-inference/generate-paging-route-test.py extracts the actual
  helper, adds a focused --pte-observations entry point, and avoids duplicate
  extraction from the startup range.
- paging-route-test-prefix.c updates the private model's counter fields.
- paging-route-test-suffix.c adds71 bounded observations and updates historical
  positive paging fixtures to supply a resolved, nonempty POST framebuffer.

## Validation and pre-existing fixture drift

1. Existing frozen137 with its old fixture inputs:689052checks,34567failures.
   New instrumentation and new tests before fixture repair:689123checks, identical
   34567failures. First failure was the small layout's deliberate missing POST
   being used as a positive control; subsequent fixtures omitted POST entirely.
2. Main authorized fixture correction after that baseline. Supply a tiny valid
   POST surface and reserve its first64KiB. Preserve the small model's original
   application/table capacity by adding the reserved64KiB to its total VRAM.
   Keep an explicit unknown-POST refusal. Update the staged-copy byte oracle to
   index relative to the advertised application origin, rather than assume zero.
   No production guard or expected copy behavior weakened.
3. Frozen137 source with corrected fixtures, no observation change:
   907964checks,0failures (baseline-fixtures-fixed.log).
4. Current source with the same fixtures plus71 new controls:
   908035checks,0failures (observation-test-v4.log).
5. Focused --pte-observations:71checks,0failures (observation-focused.log).
   Deliberately attributing local buckets to system in generated code:
   71checks,19failures (observation-mutation-focused.log). No production mutation.
6. Actual vidmm.c compiles using the project's WDK26100 /kernel /W4 /WX flags
   (kernel-compile.log). This is a translation-unit check, not a combined KMD link;
   main will build the combined candidate after other agents' changes are frozen.

An initial harness extraction duplicated the helper and failed compilation;
observation-test.log retains that mistake. v2 retains the failing old fixtures;
v3 retains a partial fixture repair; final v4 passes. No logs overwritten.

Source snapshot: scratch/m9/bd021-observation-source with SHA256SUMS.txt.
Logs and generated-code helpers: scratch/m9/bd021. No shared status files edited.

## Proposed existing-ID comment

- 2026-09-24 Codex: Added source-only PTE observation counters by level and
  system/application-local/table-local segment, coherent/noncoherent and encoded
  snoop mismatch. Covers CPU initialization and GPU encoding with per-callback
  aggregation, no cache-policy change. Current routing908035/0; frozen137 with
  corrected POST fixtures907964/0; new focused71/0, local-domain mutation19fails;
  WDK kernel compile passes. Await normal lab paging trial; counts are attempts,
  not residency or executed pages. BD-021 remains NEEDS-LAB.
  Review: scratch/m9/bd021/OBSERVATION.md.
