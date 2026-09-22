# ADR 0013: node layout - 3D, and a copy node on SDMA that also pages

Date: 2026-09-22. Status: **accepted** (split out of ADR 0010 point 5 at the owner's suggestion).

## Context

The full table reports one node (`DXGK_ENGINE_TYPE_3D`) and names it as the paging node
(`MemoryManagementCaps.PagingNode`), and paging is answered in software. With real paging, every eviction and every
upload VidMm asks for would queue on the same ring as the game's rendering. The number and kind of nodes is visible
to user mode: a Vulkan ICD builds its queue families on it. Of everything in ADR 0010 this is the choice that gets
harder to change with each line of user-mode code written against it.

## Decision

1. Two nodes: node 0 `DXGK_ENGINE_TYPE_3D` on the gfx ring (graphics and compute; the compute rings of facts M79
   stay a later, additive node), node 1 `DXGK_ENGINE_TYPE_COPY` on SDMA.
2. Node 1 is the paging node. `BuildPagingBuffer` emits SDMA copies and fills; page table updates stay CPU writes
   through the VidMm path stage B proved (facts M73).
3. The ICD exposes node 1 as its transfer queue family.
4. SDMA's seven known defects (`driver/shim/test/sdma_faults.c`) are closed and its trap interrupt is identified on
   the IH ring before node 1 is reported to dxgkrnl. Until then the table keeps one node and software paging.
5. This layout is settled before the user-mode contract of stage D is written down.

## Consequences

- Stage D of ADR 0008 starts with SDMA, not with the ICD.
- Adding the compute node later is additive; removing or renumbering a node is not. Nothing in user mode may
  hardcode a node ordinal other than through the adapter's reported metadata.
