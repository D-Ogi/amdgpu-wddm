# Pressure-before-query revision2, unit A, 2026-09-23

Probe SHA256 E41BDFB3821FCAB3647A75FF658DA6E7D30DE622367758333D37C31CA8DB6433. Unchanged full WDDM0798, no reboot/reinitialization. Raw outputs and source snapshot included. Native cmd exit capture:64KiB0 and1GiB1.

Small control passes3cycles and initial full GPU readback.1GiB initial GPU readback passes1024copies; competing1GiB allocation dirtied/CPU-verified, then preserved allocation residency3(NOTRESIDENT). After competitor release, MakeResident returns PENDING fence7009, tool expires5s with fence7008. All driver GPU work completed at final snapshot,GFX24095/24095,SDMA275423/275423,zero timeouts/refusals/noTDR. Post-eviction1GiB content is not verified. This isolates a tool wait expiration, not its ultimate cause. No KMD timeout change.

MS enriched d3dkmthk.md12491:Evict decrements residency references. d3dukmdt.md4681 clarifies EvictOnlyIfNecessary=0 asks for earliest-opportunity eviction; it is not a fixed synchronous deadline. New probe revision measures a60s aggregate MakeResident wait and eliminates redundant CPU reads of WC source/pressure; full GPU readback checks remain.
