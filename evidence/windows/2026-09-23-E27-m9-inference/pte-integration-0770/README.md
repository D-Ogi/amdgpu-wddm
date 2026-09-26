# Local0770 ordered GPU_PHYSICAL page-table updates

Candidate0.7.70.1 built/signed, NOT DEPLOYED:
scratch/build/bc250kmd-0770/package-umd.
SYS C4C9DCE75C31627D86ED3E78AFCDA750F4944536C9D145451DBDA57AFEA9483C.
No lab access, reset, agents or USB changes.

VidMmEncodePageTable validates the entire source operation and encodes a selected
entry range with the existing AMD translator. It checks level, table/segment
extent, source, Repeat, unsupported64KB flag and progress. The destination starts
at table physical base plus8*(StartIndex+progress). Bad later entries prevent even
the first batch's publication. Input pointers retain the DDI's OS validity contract.

GfxPagingBuildUpdate converts destination physical to MC, budgets the actual
ring/private capacity, emits explicit PTE values followed by a fence/poll barrier,
and reports entry progress separately from command bytes. Full512 entries split
480+32; initial497-entry theoretical capacity is capped480 to keep the fixed
kernel frame under4096bytes. First build exceeded that limit at4248; corrected
build's largest fixed frame3992, warning retained by the stack checker, PASS.
The full call-chain stack consumption has not been dynamically measured.

PagingCpuBootstrap starts true in GfxStart and is closed under the existing
exclusive GfxPagingLock before the first non-PLAN RUN setup. It is never reopened
on FINI/failure. While true, the builder validates the update, performs the old
CPU update and a memory barrier under the shared lock; RUN cannot overlap.
After close, missing/not-ready engine is an error, not a CPU fallback. This
assumes the new driver instance does not inherit active app VMIDs from an unsafe
previous teardown; that broader hardware lifecycle remains unresolved.

CPU_VIRTUAL paging-process updates remain immediate at the DDI entry, even with
NULL DMA buffer. GPU_PHYSICAL dispatch now uses the new builder and existing
per-buffer records, pointer/size publication and multipass; separate update-batch
counter avoids treating entry progress as data bytes moved.

Validation:6197host checks pass (actual root resolver, encoder, update builder,
real AMD translator/packet/budget functions). Includes480+32, value coverage,
Repeat/StartIndex, MC conversion, marker offsets, full ring, late invalid source,
bootstrap dispatch and no CPU fallback after close. WDK structures are field-level
host substitutes, locks are counted mocks, CPU write routine is stubbed; real ABI
checked by full kernel build. Bootstrap flag transition in RUN reviewed in source,
not exercised by this extracted harness. PTE translator and private-record suites
also pass. Post-build changes only add/update comments.

OPEN: old CPU update is void and may drop mapping failures; bootstrap caller still
cannot observe that failure. Outer BuildPagingBuffer still folds helper failures
into SUCCESS, including after readiness is lost. Fix these error contracts before
runtime acceptance. Full DDI dispatch/publication is source/compile reviewed, not
dynamically covered. Actual SDMA page-table visibility, OS page lifetime and
cross-engine TLB/paging completion require hardware evidence. No full M9 claim.
