# Complete-cycle batching, host only, 2026-09-23

Current WddmGpuFencePaging owns copied private packets in FIFO jobs and dispatches only one job until its hardware fence arrives. This creates a possible bounded scratch lifetime: SAVE, all predecessor copies and RESTORE of a cycle must belong to the same accepted hardware submission. Then unrelated jobs can overwrite scratch between complete-cycle batches without corrupting the transfer.

Added PagingPermutationBatch to the M275 planner. It packs only whole cycles under a caller-supplied action budget. If the next cycle cannot fit an empty buffer it returns NeedCycle and its required action count, NOT More with zero progress. This is a planning result, not a DDI status. The caller must derive the action budget from proven packet/DMA/private capacities and must not translate NeedCycle into endless insufficient-buffer retries.

MSVC /W4 /WX /O2 /std:c11 passes all46233permutations1..8 at each of12budgets:554796batch scenarios plus original direct replays, zero snapshot-oracle failures. Scratch is deliberately overwritten between batches to model interleaved jobs. When a complete cycle exceeds the test budget, the test explicitly supplies a larger simulated buffer. This is NOT proof that a larger real ring reservation exists.

Not wired into KMD build or hardware admission. Oversized cycles require retained per-transfer storage or another correct execution architecture. A distinct cycle scratch page remains needed when the actual per-slice emitter can use its own staging. OS lifetime for arbitrary split cycles, unaligned/duplicate endpoints and physical normalization remain open. No lab changes or GPU runs.

Local Microsoft d3dkmddi.md BuildPagingBuffer section lines4132/4144 describes MultipassOffset preservation and repeated TransferStart across a sub-transfer; it does not establish scratch ownership. Source review is limited to those documented properties, not a claim that Windows never interleaves transfers.
