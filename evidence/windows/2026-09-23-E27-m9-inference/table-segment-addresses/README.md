# Dedicated table-segment address groundwork

2026-09-23. Source base bed764d plus ongoing uncommitted M9 work. Host only.
The PTE context now optionally describes a second local segment with independent
physical base/extent and ID. Size0 preserves the existing single-segment behavior.
Directory references and leaf mappings both resolve this origin, because the
paging process can map table storage as data. Configuration rejects ambiguous or
unrepresentable IDs, unaligned/wrapping extents and overlap with application VRAM.
The existing AMD PTE encoder still checks hardware address masks and flags.

2048 page/unit/kind combinations decode to independently computed physical
identities. Disabled-segment rejection is a control. Boundary, collision,
alignment, overlap and wrap tests plus the existing PTE suite pass.8676 routing
regressions pass with second segment disabled. Full WDK dev build/sign passes.
Dev SYS888DFB81A924BBD2B7252F0D9D8C4921254448396C8875DD3E81FB8972AF2143
Output scratch/build/table-segment-dev retains0779 version: DO NOT DEPLOY.
Official0779 candidate remains unchanged; last installed0773 remains unchanged.

Not runtime configured. WDDM still enumerates the old segments and table IDs.
Before integration, jointly update enumeration, application segment exclusion,
root validation, table physical destination checks, logical/live walks and retained
CPU map extent. PageTableSegmentId and PagingProcessPageTableSegmentId may select
separate segments (local enriched d3dkmddi.md24799-24805, WDK26100; original MS docs
commit7515063cea4c9e98db6a92986c5b4ddb0463fd16). No OS placement/runtime claim.

Separate table storage removes application/table physical overlap only after the
layout is actually changed. It does NOT establish the cache attributes of borrowed
CPU_VIRTUAL OS mappings; M190 still requires matching policy or no alias. Present
application mappings likewise remain unresolved. No full M9 acceptance, hardware
execution, recovery or performance conclusion follows from this groundwork.
