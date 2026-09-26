# M280: actual WDDM page-cycle multipass

Date 2026-09-23. Base commit bed764da5192be7132d646be0e6331c1edeb30fd; captured working-tree sources. Host-only implementation and validation, no lab operations.

The M278/M279 builder now uses PagingPermutationBatch to pack whole cycles under actual remaining DMA/private/ring capacity. MultipassOffset names the next cycle's minimum original source index (cycle seed), or the page count when done. Seeds increase deterministically. It is not a contiguous completed-byte prefix: earlier cycles may contain later source indices. Initial token zero permits skipped identity pages. Every resume rebuilds the physical permutation plan and locates the exact seed. Moved diagnostics count logical cursor progress, including identity pages, not staging-copy traffic. All cycles are checked against fresh hardware capacity before the first publication; a single oversized cycle remains unresolved and does not induce endless empty retries.

Scratch has no live value between accepted buffers. Paging FIFO retirement must still establish the runtime ordering already required by the design. OS cancellation, arbitrary alias graphs, stable physical identities across callbacks and hardware coherence are not proven by this host test.

Procedure: driver/shim/test/run_paging.ps1 -KmdRouting -Out P:/bc-250/scratch/m9/permutation-multipass-host. Existing 14039 checks stay green. New fixtures use eight high physical page identities, permutation [0,7,2,5,6,3,4,1], cycles (1,7),(3,5),(4,6), and identity pages 0,2. Four MDL/aperture combinations times three independent capacity limits (DMA/private/hardware ring) force exactly three accepted buffers. Tokens are 3,4,8; byte accounting totals32768. Decode actual published PTE/COPY packets, replay bytes against initial-snapshot oracle and overwrite scratch between buffers. A 512 DWORD hardware ring admits each cycle even though the full plan cannot fit. Result:14343 checks, zero failures.

Negative control: -BreakCycleRestore substitutes source page0 for VRAM scratch only in the generated test. Same14343checks produce20byte-oracle failures (eight single-buffer and twelve multipass), native exit1. Production source is not mutated.

WDK build/sign passes: build.ps1 -Kits P:/bc-250/toolchain/nuget -Out P:/bc-250/scratch/build/permutation-multipass-dev. SYS SHA256 F10E67D012DD15C659AA04FF85F3B144510EABD4BA8D061109D09E5CFF35C8AE. Version remains100, development only, not deployed. No general alias or full M9 completion claim.
