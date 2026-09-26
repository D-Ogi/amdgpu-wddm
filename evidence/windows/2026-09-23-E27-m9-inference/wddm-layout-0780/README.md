# WDDM isolated table layout, candidate0780

2026-09-23, base bed764d plus ongoing uncommitted M9 work. No lab access.
WddmMemoryLayout is the single calculation used by startup, QUERYSEGMENT4 and
PAGETABLELEVELDESC. Segment1 is application VRAM,2 remains the aperture,3 is table
storage. All four hierarchy levels and paging-process levels select3. Allocation
masks still select1 or2; table3 is excluded from ordinary allocations. VidMmStart
now calls VidMmStartLayout so the retained map covers only the table extent.

Capacity policy:1/32 of usable VRAM, rounded down64KiB, minimum4MiB. Application
and table ranges start/end on64KiB boundaries and exclude the full POST scanout
and32MiB private tail. Tiny layouts unable to leave application space refuse.
This is a driver budget, not a hardware limit or a proven ideal capacity. VidMm
placement/pressure and possible allocation exhaustion require runtime validation.
Changing the layout requires a device restart; no existing allocations are migrated.

CPU_VIRTUAL initialization now checks its borrowed mapping's physical identity
against the table extent/alignment before any CPU write or shadow registration.
Host foreign application-page, end-of-table and unaligned identity cases refuse
without changing destination data/registration count. The OS still owns pointer
validity/cache attributes; this is not a cache-policy or OS fault-contract fix.

8922 host checks pass. Actual layout is exercised for1..32GiB models, small/closed
and invalid framebuffer cases. Actual two-pass QuerySegment4 and level descriptor
functions run with field-level WDK models; count-only outputs, descriptor stride
padding, distinct origins, four table IDs and malformed capacities are checked.
Existing separate-extent startup/walker tests remain. WddmStart itself is source
reviewed/WDK compiled, not host executed. Full0780 build/sign passes:
SYS2B9E16BA540AA2B6372C858FE4A48BCCF30F5FB496D34DA284E2134082373EA5
scratch/build/bc250kmd-0780/package-umd, NOT DEPLOYED. Last installed0773 unchanged.

M190 remains open: NC mapping of isolated tables can still alias borrowed OS
CPU_VIRTUAL mappings with unknown cache attributes. Present application mappings
also remain potentially incompatible with OS aliases. No hardware cache, GPU
retirement, OS concurrency or1GiB/performance acceptance is claimed. Required
paging operations/error policy and successful GPU reset/reentry remain unfinished.
