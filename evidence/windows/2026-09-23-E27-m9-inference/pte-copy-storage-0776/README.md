# Private staging and single-range PTE-copy construction, candidate 0776

Source base: bed764d plus the ongoing uncommitted M9 changes captured here.
Host-only verification, 2026-09-23. No lab access, deployment or reboot.

Gfx SetUp reserves a private 4 KiB VRAM staging page before engine stages.
Allocation failure unwinds setup; invalid size/alignment descriptors are freed.
PagingReady requires the reservation. Teardown frees it under existing ownership.
CPU exclusion is not proof of GPU retirement; actual halt/reset remains open.

GfxPagingBuildCopyRange resolves 64 KiB-aligned GPU table VAs through logical
paging translation, checks indices/count within the current 512-entry tables,
converts local physical identities to MC addresses and constructs the staged
copy with accumulated DMA offset, live ring and outer-fence capacity accounting.
It returns physical entry identities only on success. Current page-table segments
are local VRAM; system tables are refused. This helper is not yet called by the
WDDM DDI. Array/multipass progress and accepted logical-copy publication remain.

Results: 26 storage/setup checks pass; 8552 packet/routing checks pass.
The new range cases mock logical translation; they do not prove full OS delivery.
Existing lifetime negative control fails and actual locked code passes.
Full WDK build/sign passes. Candidate package remains local at
scratch/build/bc250kmd-0776/package-umd. SYS SHA256:
4ADFFCE861BC345C620F6DA5B9EE3D6EB99E26223AA017398A92653F0F34C2FB

No GPU execution, barrier/coherency, OS concurrency, resource retirement or
performance acceptance is claimed. Cache alias policy, outer DDI return contract,
physical ADL, recovery/reentry and real 1 GiB paging acceptance remain unresolved.
