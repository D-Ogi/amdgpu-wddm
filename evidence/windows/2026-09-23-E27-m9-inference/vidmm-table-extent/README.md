# VidMm independent table extent

2026-09-23, source base bed764d plus ongoing uncommitted M9 work. Host only.
VidMmStartLayout accepts independently bounded local application and table ranges.
It validates IDs/extents and rejects overlap before reservation. SegmentPhysical,
SegmentLength and the retained NC mapping now describe table storage; Pte.vram_*
describe application storage and optional Pte.table_* describe the second segment.
The existing VidMmStart wrapper supplies identical ranges and remains the WDDM
call site, so current runtime enumeration/placement is unchanged.

Root and GPU_PHYSICAL destination validation require the table segment ID.
Logical/live directory walks remain confined to table storage, while leaves may
name either local segment. ProbeIb source bounds follow the same separation;
its system-page path excludes both local domains. ProbeIb mapping cache attributes
remain a separate unresolved concern; this change does not fix those aliases.

Actual startup, CPU initialization, encoder and walkers are host tested with
application range[0,1MiB), table range[1MiB,1MiB+32KiB) within modeled VRAM.
Captured MmMapIoSpaceEx range covers only tables. Directory entries reference
segment3; application leaves reference1; another paging leaf references3.
Tests verify both walkers, root rejection, encoded table destinations, stop cleanup
and overlap/ID/bounds rejection before mapping.17 new checks,8693 total pass.
Field-level host structs and mocked allocation/mapping/locks do not prove WDDM
layout delivery or concurrent lifecycle. Full WDK dev build/sign passes.
Dev SYS6E38953F5354F849A470C456126742614BA1B9B5C5919967F75B30D369E7C851
scratch/build/vidmm-layout-dev retains0779: DO NOT DEPLOY. Official0779 unchanged.

No lab access, reset or new runtime acceptance. Next: choose/document table capacity,
update WDDM enumeration/segment masks/table IDs and call the new initializer as one
consistent layout. CPU_VIRTUAL borrowed mapping attributes and registration bounds
still need review; table isolation alone is not M190 cache-policy closure. Present
OS aliases, error contract, physical ADL, GPU recovery and1GiB/performance remain.
