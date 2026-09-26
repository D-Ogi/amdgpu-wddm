# Follow-up to the owner-relayed driver review (2026-09-24)

The numbered items below retain the review's identifiers. A source concern is
not automatically a demonstrated hardware failure. Current deployment remains
M432/KMD134; M433-M435 source work has not been deployed.

| Item | Current assessment and next action |
|---|---|
| 1: primary pitch | Source mismatch verified: standard primary uses Width*4 while inherited HUBP pitch remains unchanged. Implement one validated pitch/height/extent across primary allocation, HUBP programming, AddressAllowed, FillSurface and scanout mapping. Include1680/1440/1366 controls and actual allocation bounds. A live out-of-bounds CPU write has not been reproduced. Highest display priority. |
| 2: bugcheck output | Review open: SystemDisplayWrite targets POST while normal flips may scan another surface. Inspect high-IRQL crash callback constraints and retain a usable crash-time target; a PASSIVE mapping routine cannot be reused blindly. |
| 3: GetScanLine | Review open: use actual OTG position/timing for hardware-vsync mode, after confirming fields and blanking origin. Timer timestamps do not establish the hardware scan line. |
| 4: deferred vblank | Source drops the report when completed-primary snapshot is unavailable. Establish the actually scanned address and report each real vblank without falsely retiring an unlatched flip. Preserve M431's generation protection. |
| 5: flip completion | Extend FLIP_PENDING observation with the documented earliest-in-use address; verify AMD DCN201 path and field units. Read failures must not be mistaken for completed hardware work. |
| 6: unresolved POST framebuffer | M435 removes WddmMemoryLayout's empty-reservation fallback. Unknown POST-to-VRAM identity or zero extent now refuses the layout; a verified physical path remains usable when BAR0 is absent. Existing GART CheckWindow also rejects unresolved POST, so an actual runtime overlapping segment was not demonstrated. |
| 7: handoff after restore failure | Review open: carry the actual DCN restore result through StopDeviceAndReleasePostDisplayOwnership and preserve required storage/lifetime on failure; returning success is not proof that Basic Display owns a usable surface. |
| 8: source visibility | Review open: clearing POST is not hardware blanking after a flip. Tie visibility to the active scanout/pipe using verified DCN sequencing. |
| 9: conflicting VRAM sizes | M435 compares GC FB_LOCATION size with named NBIO RCC_CONFIG_MEMSIZE before publishing VRAM; RunSetup also compares actual shim mc/real size and MC base against that geometry. 8/12/16GiB model controls pass; no12GiB hardware claim. |
| 10: diagnostic fill in full WDDM | Claimed escape path is already blocked: display.c WddmDiagnosticAllowed refuses RUN_DCNFLIP whenever FullWddm is true, independent of VidPnFlipEnabled. M432 observed that earlier dispatcher refusal. Internal reservation-aware fill bounds remain part of item1, but the reported full-WDDM escape route is not reachable in this source. |
| 11: GOP scratch / D7 | Reviewer corrects the earlier assumption: donor vram_usagebyfirmware is zero. Do not reserve a presumed GOP workspace after ExitBootServices solely because GOP used it earlier. Verify actual live firmware consumers; retain GART/TMR/ring disjointness. Own-unit evidence remains L35. |
| 12: local PTE snoop | Review open: compare the exact Linux PTE flag selection and our OS CacheCoherent contract, including local versus system pages. Physical shared memory is not cache-coherence evidence. |
| 13: legacy clock limits | Deployed legacy reader/CLI has wider constants than hardware.md. New M434 reader contains no mailbox write implementation. Client migration must enforce1500MHz/900mV ceilings; initial native integration is fixed1000MHz/820mV. Do not deploy reader-only migration. |
| 14: upclock ordering | Review accepted as a policy requirement: frequency-first AMD commit is not a general safe upclock transition. Fixed native1000/820 preparation must also establish its entry operating-point assumptions. Implement a transition-aware policy before allowing arbitrary upward changes; on partial failure never raise frequency to roll back a downclock. |
| 15a: message0x0F | Remove raw-message client dependence; native public owner exposes typed operations only. No claim that every legacy allowed message complies with hardware.md. |
| 15b: two mailbox owners | M433-M434 prepare one native owner and remove direct writes from new reader source. Handover, clients and startup call sites are not yet deployed. No runtime exclusivity claim. |
| 15c: Mesa firmware versions | Review open: trace each reported version to its loaded blob/header. P5's SMU change alone does not establish changed ME/PFP/MEC/RLC blob versions, since those are loaded by the driver. |
| 15d: RLC header version | Review open: validate supported header revisions before interpreting RLC fields, with the exact current firmware as a positive control. |
| 15e: legacy timeout | Old reader uses sleep-based polling and can return transport success with a failing response. New reader source removes that path. M433 transport checks firmware success and both elapsed/iteration bounds; native timing still needs lab validation. |

The12GiB test task also needs removal of fixed8GiB assumptions from host fixtures
and qai_bridge, with generated/sample geometries for both sizes. A12G carve-out
still yields about11.5864GiB application capacity under current reservations;
it does not meet the owner's12GiB model-residency requirement by itself.

IOMMU remapping is a separate DMA-address ownership project covering ring/fence,
GART and PTE backing and paging checks. Do not enable the firmware option or add
DmaRemappingCompatible before the implementation and hardware evidence exist.

Next: finish item1's complete geometry path, then active scanout/completion and
handoff. Resume native SMU client/startup integration with item14's transition
policy and validated native temperature. This triage does not replace the M9
DMA audit, cold/repeated-start, lifetime, recovery or performance gates.
