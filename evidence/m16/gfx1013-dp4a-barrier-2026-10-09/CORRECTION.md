# Correction, 2026-10-10: the optimizer mechanism is an inference, not a measurement

`evidence/` is immutable (`docs/01-evidence-rules.md`: "Never edit afterwards. A correction is
a new file."), so `README.md` and the retained assembly, diffs and JSON records next to it stay
as the series wrote them. This file names what the write-up explains more firmly than it
measured. The independent audit of 2026-10-10 raised it in its appendix on M843 to M849, and
`codex.md` states the same limit: "The exact optimizer-pass explanation remains inference
without IR/remarks". The fact row [M849](../../../docs/facts/icd.md#m849) carries the corrected
wording.

## What stands

Every count stands. The audit reconstructed the histogram of all five retained disassemblies
and each one equals its JSON record: 1025 instructions and 40 `ds_read2_b32` without the patch,
2292 and 180 with it, and 1025 and 40 again with the `asm` branch switched off. The instruction
lines of the unpatched build and of the `asm`-off build are equal, byte for byte and address
for address. The gfx1010 pair equals the gfx1013 pair the same way. The arithmetic stands too:
2292 / 1025 = 2.2361, 180 / 40 = 4.5, 611456 - 599168 = 12288 bytes and 214 - 200 = 14 vector
registers.

The intervention also stands. One branch condition of `ggml_cuda_dp4a` (`common.cuh:717`),
renamed to a macro that nothing defines, removes the whole difference in this pinned build with
FlashAttention off. That is a controlled three-build comparison, and it is what the fact claims.

## What is corrected

The section "Why the assembly costs this much" explains the counts by decisions of the
compiler. Those sentences are an inference over the five disassemblies. A disassembly records
the code the compiler emitted. It does not record why a pass did or did not fire.

| Section | The write-up says | What the evidence holds |
|---|---|---|
| Why the assembly costs this much | "Out of plain C the compiler sees the byte extraction of all of them at once, so it shares the work" | Two instruction histograms that differ. The common subexpression elimination of the byte extraction is a plausible reading of them, not a recorded pass decision |
| Why the assembly costs this much | "it cannot know that two blocks read the same bytes, so it cannot common up the extraction and it cannot merge the reads across them" | The same two histograms. No pass was asked what it did with the `asm` block |
| Why the assembly costs this much | "14 more VGPRs, because the two scratch registers of the block are live per dot product rather than shared" | A register count of 200 against 214. The split of that difference over the two scratch registers of the `asm` block was not isolated |

The whole-kernel claim is unaffected: the `asm`-off build is equal to the unpatched build in
every figure, so this one branch is the source of the difference. What stays open is the
attribution of each part of the difference to a named pass.

## What would test the attribution

`-Rpass=.*` and `-Rpass-missed=.*` over the same three builds, or `-save-temps` and a read of
the intermediate IR, which records the decisions the disassembly does not. A narrower
intervention is the other route: one `asm` block in a kernel of two dot products, where the
shared operand is visible by inspection. Neither needs a lab unit, a GPU or a run of the code.
Both are compile-level work on this development machine, like the three builds themselves.

## Also noted by the same audit

- M847's older extrapolation, that the eight independent dot products of the isolated probe
  represent the general matrix-multiply shape, is already corrected: M849 measured the
  shared-operand case and holds a `supersedes` edge of partial scope to M847. The limit that
  M847's own detail already states, that the MMQ translation unit differs by more than the dot
  product and that the measurement did not isolate why, is the same limit this file labels.
- M848 needs no correction. The host and device launch bounds reproduce from the source copy,
  and `AMDGPUUsage.rst:2477-2486` makes the larger host launch undefined behaviour: "If the
  actual block or workgroup size exceeds the limit at any point during the execution, the
  behavior is undefined."
- The M843 item of the same appendix has its own correction, beside its own evidence:
  `evidence/windows/2026-10-08-d3d9on12/CORRECTION.md`.

Nie wszystko złoto, co się świeci - not all that glitters is gold: a count is a measurement,
and the story about the count is not.
