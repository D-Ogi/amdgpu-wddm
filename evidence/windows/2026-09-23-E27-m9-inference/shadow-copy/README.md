# Logical PTE copy metadata groundwork

2026-09-23, HEAD bed764d plus worktree. No lab access or deployment.

Pinned Microsoft DDI repo7515063cea4c9e98db6a92986c5b4ddb0463fd16,
enriched d3dkmddi.md WDK26100. Primary pages checked2026-09-23:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_buildpagingbuffer_copy_range
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_buildpagingbuffer_copypagetableentries

Copy ranges carry64KiB-aligned GPU virtual table addresses and entry indices,
not physical table identifiers. NumRanges describes the range array. A future
DDI implementation must translate these addresses in the appropriate context;
passing them directly to the logical physical-table index would be incorrect.
Current WDDM callback has no COPY_PAGE_TABLE_ENTRIES handler.

New PagingPtShadowCopy takes resolved physical table identities. It does not
allocate/register new tables. Copy into an unregistered destination returns
MISSING, leaving storage unchanged. Unknown source entries clear destination
Known bits, preventing stale destination data from looking valid. Known zero
remains known zero. Same-table overlap uses snapshot/memmove semantics without
a512-entry stack buffer. This is our conservative helper contract, not a claim
that Microsoft specifies overlap behavior or SDMA implements memmove natively.
GPU-side execution must preserve the chosen semantics if overlap is admitted.
Caller owns serialization and invokes it only for accepted copy command batches.

Validation:15935 core checks pass, including512 overlap/offset/count combinations
against an independent snapshot oracle spanning the64-entry bitmap boundary,
whole-table copy, known zero, unknown source, untouched neighbors, missing target
and invalid inputs without partial mutation. Kernel-flag compile passes. Existing
6768 route checks, including M191, pass. Full development build/sign passes.

Not runtime wired. Dev package retains0775; DO NOT DEPLOY shadow-copy-dev.
SYS SHA2565554E771B8B3139F1FB3BA14163205FAA297C1EADC1E019061AAC7E06C2FC095.
Official0775 candidate unchanged; lab last verified0773, gates closed.

Next: COPY_PAGE_TABLE_ENTRIES builder, GPU-copy ordering/overlap treatment, range
multipass and capacity preflight, publication of matching logical metadata, OS
context/lifetime validation. Unknown logical data must remain a refusal to resolve,
not a fallback to old GPU tables or a claim of successful hardware copying.
