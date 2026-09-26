# M279: WDDM physical transfer permutation integration

Date 2026-09-23. Host-only source integration, base commit bed764da5192be7132d646be0e6331c1edeb30fd, captured working-tree sources. No lab operation or deployment.

WddmBuildPhysicalTransfer calls the M278 complete-plan builder for overlapping indirect endpoints. A whole-page bijection fitting one submission is published as one private record. Successful publication advances the byte count and terminal multipass token; a repeated terminal token emits nothing. DMA/private capacity shortage returns without publishing a cycle prefix or advancing progress. Identity permutations need no packets. Partial, non-bijective and oversized alias graphs remain unimplemented; existing refusal policy is preserved, not claimed to satisfy the restricted DDI error contract.

Procedure: driver/shim/test/run_paging.ps1 -KmdRouting -Out P:/bc-250/scratch/m9/permutation-ddi-host. The 12 M278 byte-oracle fixtures now call the actual WDDM transfer helper, decode published DMA PTE/COPY packets, and compare against initial page bytes. Added independent DMA/private capacity shortages, exact pointer/remaining-size/progress checks, private-record coverage and completed-token repeat. The former swapped-page refusal test now expects an insufficient buffer: its 256 DWORD capacity cannot contain three reserved transactions. No change to data-oracle expectations.

Result: 14039 checks, zero failures. Negative control: same command with -BreakCycleRestore and separate output directory substitutes original source page for saved VRAM bytes during restore in the extracted test only. All eight nonidentity byte oracles fail, native exit 1; packet counts and bookkeeping still pass. No production mutation.

WDK build.ps1 -Kits P:/bc-250/toolchain/nuget -Out P:/bc-250/scratch/build/permutation-ddi-dev passes and signs development SYS SHA256 0C88CA522095A189E92E773DA74E76154727A80F27E4E17D1CECC3B6114A2FF0. Revision remains 100, not a deployment candidate. GPU ordering, real OS alias callback delivery and pending/cancel lifetime are not established by these host tests. Full M9 and warm reentry remain open.
