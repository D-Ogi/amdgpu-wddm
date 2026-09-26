# System-page virtual paging integration, local0766

2026-09-23. No lab access or deployment. Source base HEAD bed764d with captured
worktree changes. The prior PTE/TLB/transaction constructors are now connected to
GfxPagingBuild for virtual copy/fill operations whose page walker returns system RAM.

paging_window.c validates a two-page aperture immediately after the64MiB driver GTT
allocation limit, checks actual aperture/table extent and48-bit MC arithmetic. gpumem
and this window share one boundary constant. No driver-owned GTT allocation overlaps
these slots. The initialized live GART BO determines the table MC address.

Actual PagingResolve keeps local physical-to-MC conversion and tags representable
system physical addresses for PagingEmit. Emit maps source/destination independently
to the two slots using existing AMD GART PTE flags; local/system, system/local,
system/system and system fill use complete map/barrier/TLB/operation/cleanup sequences.
Local/local keeps direct packets. Physical pages are borrowed from VidMm's resident
paging operation, not newly allocated/pinned by this code. Legacy physical ADL paths
and their complete DDI contract still require review; this change covers virtual paths.

Driver fence-page slot5 is a private marker, separate from completion slot4. Markers
are1+3*commandDWORDoffset and successive phases use+1/+2; offsets include earlier
BuildPagingBuffer calls in the same OS DMA buffer. GfxSubmitPaging resets slot5 after
claiming the one-in-flight state and successfully reserving ring space, before packet
publication/doorbell. Previous actual hardware completion is required for that claim;
timeout leaves the path failed. CPU memory barrier precedes publication. Marker/page
lifetime shares the existing protected engine state. Runtime coherency still untested.

Host integration test extracts actual resolver/emitter code and links actual AMD
emitters and page-stream builder.364assertions pass including prior335packet checks,
four system/local/fill routes, local fast path, physical-field refusal, missing window,
command-offset markers and five scattered page pairs in three bounded batches with
exactly-once progress. Host page-table translation is stubbed. Window bounds/48-bit
arithmetic tests and builder lifetime regressions pass. This does not execute SDMA,
verify OS page ownership, replay cache coherency or prove1GiB paging.

Full KMD build/sign passes, package scratch/build/bc250kmd-0766/package-umd,0.7.66.1.
SYS SHA256886DE21300492A956BEB8E746FF0CF625FF030043A7F4C15ECDE2BB9EDAED269.
Not deployed. Needed next: review errors/refusal and hardware-lifetime contracts,
small hardware positive controls with actual fence/data/TLB-cleanup witnesses, then
pressure/eviction and1GiB acceptance. No redactions.
