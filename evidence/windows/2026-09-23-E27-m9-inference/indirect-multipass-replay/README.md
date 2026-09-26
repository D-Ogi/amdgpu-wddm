# M263: indirect multi-buffer byte replay

Date: 2026-09-23. Host-only; no lab mutation. Base commit bed764da5192be7132d646be0e6331c1edeb30fd, with uncommitted sources. actual-route-tested.c contains the exact generated translation unit of the passing run. M262 retains the unchanged classifier/KMD source snapshots.

Hypothesis: disjoint fragmented MDL/aperture physical transfers preserve all requested bytes and untouched boundaries when DMA or private-buffer capacity requires multiple DDI calls.

Procedure: run driver/shim/test/run_paging.ps1 -KmdRouting. New case_indirect_multipass_bytes uses four endpoint combinations (MDL/MDL, aperture/MDL, MDL/aperture, aperture/aperture), each with an independently limiting DMA or private buffer. Three fragmented pages per side use addresses above 4 GiB. Aperture source/destination offsets17/31 differ; transfers span8192bytes. Each accepted call emits one mapped slice and advances the resume token. The test decodes physical PTE addresses and SDMA source/destination/count into separate sparse byte backing, then compares all24576bytes against an independently computed copy, including untouched tails. Private records must cover exactly the accepted command range and unused DMA storage must stay unchanged.

Result:13884checks,0failures. Eight byte replay scenarios pass. The initial test decoder mistakenly retained MTYPE bits48-50 in its address mask; it was corrected to48address bits using the existing AMD PTE definition before this passing run. No driver change was needed.

Control: -KmdRouting -RepeatMdlSourcePage modifies only the generated test translation unit so MDL source resolution repeats the first page.13885checks,9failures, including four new final byte-oracle failures. The other failures are existing regression controls. This is deliberately failing test code, never a candidate driver patch.

Scope: construction, publication, buffer limits, resume tokens and packet-derived byte contents. This does not emulate hardware ordering, TLB/cache behavior, OS page pinning, cancellation or live legacy aperture callbacks. No driver rebuild/deployment; M259-M262 still local.
