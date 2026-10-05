# Candidate0775 logical PTE publication and paging translation

2026-09-23, HEAD bed764d plus uncommitted worktree. No lab access/deployment.

WddmPublishPagingRecord preflights DMA capacity and private-record address/size
before VidMmCommitPagingUpdate. The latter re-encodes the immutable OS update into
the existing locked snapshot, applies only the accepted entry range to registered
paging-process tables, and ignores unregistered application table identities.
No GPU-visible table is written. The helper then copies/advances the OS buffers.
PnP excludes stop while this DDI is active; borrowed OS pointers must remain valid.
Unexpected failures retain the existing INVALID_PARAMETER publication path;
that OS-facing error contract remains unresolved, not newly certified.

PagingResolve now uses VidMmTranslatePaging, which walks only logical registered
entries under the shared lifetime lock. It never falls back to live GPU memory.
VidMmTranslate still reads actual retained VRAM for CPU Present/diagnostic needs.
Both share address bounds and AMD PTE decoding. Copy-PTE operations, power/reset,
real OS concurrent construction and cache policy still require implementation or
validation. The retained NC mapping is unchanged (M190 remains open).

Verification:
- Extracted actual builder, initialization, logical/live walkers and publication
  helper with real packet/PTE/private-record code:6768 checks,0 failures.
- Focused M191 ordering scenario:539 checks,0 failures; published update changes
  logical resolution from MC100006000 to100007000 while modeled GPU RAM stays old.
  Refused private/DMA capacity, base overflow and malformed batch do not publish
  logical state. Full512 updates publish480 then32 with no early suffix visibility.
- Mutation omitting logical commit fails515 checks, exit1. The default suite now
  includes the ordering requirement (previous optional failing test).
- Existing private-record tests and CPU ownership controls pass. Ownership model
  directly extracts the old live walker wrapper; logical wrapper uses the same
  primitives but has not had a separate concurrent OS/kernel exercise.
- Full WDK build/sign passes. No full OS DDI dispatch or hardware execution test.

Candidate package: scratch/build/bc250kmd-0775/package-umd,0.7.75.1. SYS SHA256:
7A1E46CA5C8C276C91A99EEB1CC77221E3A5E9CE8537F5A199B93B630DB32B04.
Installed lab last verified0773 with gates closed. Candidate not deployed.

MS contract ordering references are archived by description/links in prior
paging-pt-shadow evidence. This closes the demonstrated M191 construction bug at
source/host scope, not full M9 or hardware paging acceptance.
