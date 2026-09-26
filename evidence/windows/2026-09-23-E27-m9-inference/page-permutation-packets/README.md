# M278: complete page-permutation packet builder (host only)

Date: 2026-09-23. Base commit: bed764da5192be7132d646be0e6331c1edeb30fd; captured working-tree sources are authoritative for this test. No lab mutation, deployment, GPU work or OS restart.

GfxPagingBuildPermutation captures full physical identities from MDL/aperture endpoints, normalizes a distinct-page bijection, plans cycles and emits actual SDMA mapped-transfer packets. The whole plan must fit one hardware submission. Cycle scratch is the engine-owned VRAM page, with per-slice staging disabled, and no cycle value crosses a submission boundary. Oversized/non-bijective/partial-page transfers remain unsupported by this helper. The WDDM transfer route does not call it yet. STATUS_NOT_SUPPORTED here is not a claim about acceptable BuildPagingBuffer DDI returns.

The zero-capacity emitter query returns aligned reservation size (96 DWORDs here), while each actual mapped copy emits 83 DWORDs. Emission accepts a positive count at most the reservation, and advances by actual size. Capacity checks occur before emitting any prefix.

Procedure: run driver/shim/test/run_paging.ps1 -KmdRouting -Out P:/bc-250/scratch/m9/permutation-packets-host. Positive control: the existing extracted KMD suite passed 13887 checks before adding the new packet oracle. Added 12 fixtures: two-page swap, three-page cycle and identity, each across four MDL/aperture endpoint combinations. Decode actual PTE and COPY packet addresses, replay SAVE/COPY/RESTORE into byte arrays, compare all page bytes against an independent initial snapshot; verify no emitted prefix on insufficient capacity, untouched tail and released lifetime locks.

Result: 14003 checks, zero failures. These tests exercise generated packet data, not real GPU ordering/cache behavior. WDK build/sign passed with build.ps1 -Kits P:/bc-250/toolchain/nuget -Out P:/bc-250/scratch/build/permutation-packets-verified. SYS SHA256 890B9B4B8318C14267E4C8DEEDEBCA0BDD914EBADCE5BECC56E3F315ED32B43F. Development build retains revision 100, not a deployment candidate. No new hardware support is claimed.

Read-only lab check at 16:32:47 returned boot 15:39:09, display Status OK, EnableFullWddm 0, LastStage 50, UnconfirmedStarts 2. No new reboot observed. This does not establish warm-reentry recovery.
