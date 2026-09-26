# Page permutation planner, host only, 2026-09-23

A local foundation for the currently refused indirect alias cycles. Input is a permutation of normalized distinct physical page identities, not raw addresses. The planner walks inverse edges and emits SAVE(first page), reverse predecessor copies, RESTORE(saved page) per nontrivial cycle. Identity edges are skipped. One scratch page suffices; O(N) planning after normalization, at most N+floor(N/2) actions. Caller owns inverse/visited/actions storage; no source contents or pointers retained.

MSVC /W4 /WX /O2 /std:c11: all46233permutations of1..8identities pass an independent full-initial-snapshot oracle. Each page identity carries17distinct words; each action is replayed as a separate simulated DMA buffer, preserving scratch. Mutation skips SAVE writes only in generated test source:78522page-payload mismatches,exit1. This validates scheduling, not actual4096-byte SDMA packets, MMIO or kernel lifetime.

NOT integrated into KMD build/admission and NOT deployed. Existing multi-page aliases still refuse. Unaligned ranges, duplicate physical destinations, non-permutation dependency graphs, normalization and command/multipass ownership remain open.

Integration constraint: do not reuse PagingCopyStaging as cycle scratch. The per-slice mapped-copy path may overwrite it, and other accepted buffers can interleave between BuildPagingBuffer callbacks. A saved cycle page needs distinct storage owned through its final hardware fence, with pending-buffer retention/cancellation/generation semantics. A multipass token alone is not ownership. Do not emit a partial cyclic transfer until that lifetime is implemented. Whole-range logical source snapshot semantics remain the correctness oracle for general aliases.

No lab operation or driver binary change this turn. Healthy07100/v2 state from M273/M274 remains the last hardware observation.
