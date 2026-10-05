# M462 - Private retained GTT mapping reconstruction

2026-09-25, source/host only. No deployment or lab calls.

GpuMemRebuildRetainedGtt reconstructs the driver-reserved GART prefix from
retained contiguous nonpaged allocations without resetting the AMD device,
allocation owners, backing, bump pointers or lifetime flags. It preflights live
entry ranges and MC identities, invalidates the private prefix through the
existing AMD unbind API, and binds only Used/Gtt/Bound entries that are neither
Retired nor TranslationsRetired. Windows-owned entries outside the private
prefix remain untouched. Identity DMA matches the current allocator contract;
this is not IOMMU remapping support.

Caller must hold GartLock with every consumer halted, including IH. TlbDirty
remains set even on success. No hardware flush or global-ready claim is made;
the coordinator must configure hubs and commit GFX visibility after RLC at the
measured startup boundary. OS-owned page-table restoration remains separate.
This helper is not yet called by a power coordinator or SetPowerState.

Actual-source host22/0: poisoned table reconstruction, independent expected
mapping addresses, unmodified OS tail, retained owner/allocation metadata,
repeated reconstruction, absent/retired entries, preflight refusal and partial
bind failure. Binder/unbinder are mocked with an independent table oracle:
these tests prove orchestration/ranges, not AMD PTE encoding or TLB visibility.
Retired-entry resurrection mutation:36 checks,3 failures. Clearing TlbDirty
before hardware commit:22 checks,2 failures. Initial negative harness lacked
FALSE; fixed before the retained final runs. Raw final logs retained.

WDK build/sign passes, undeployed SYS SHA256:
B2F9C444F72A6F3EF16E3448D73F94C8BC2ED5499E258E6AA6D3262A6E76D620.
Build scratch/build/m462-retained-gtt still carries version145. Accepted lab
SYS ED7B2E07 remains unchanged. No firmware writes or power transitions.

Existing allocator and driver/shim/bc250_gart.c define retained contiguous
backing, private prefix and AMD binding API; no new register/SMU command or
upstream code copied. Remaining: retained hub disable/configure, private VRAM
and firmware restore, actual GFX/SDMA restoration, coordinator/display/health
integration and a real power-loss resume with live application handles.
