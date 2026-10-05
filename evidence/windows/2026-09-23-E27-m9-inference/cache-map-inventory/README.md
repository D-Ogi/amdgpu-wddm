# CPU cache mapping inventory, source0777

2026-09-23. Base bed764d plus uncommitted M9 work; source inspection only.
No lab access, PTE/PAT measurement, corruption reproduction or code change.

## Concrete diagnostic alias path

DisplayMapFramebuffer maps Post.PhysicAddress through PAGE_WRITECOMBINE first,
falling back to PAGE_NOCACHE only on failure. The successful cache choice is not
stored in BC250_DEVICE. Vram Access accepts a read at BAR0 offset0 with gates open
and maps that page PAGE_NOCACHE unconditionally. When Post.PhysicAddress equals
Bar0Physical and the WC mapping succeeded, both calls request different cache
attributes for the same physical page while the framebuffer remains mapped.
This is a conditional source counterexample, not a measured live alias or cause
of previous black screens. The physical carve-out and BAR0 are different CPU
physical ranges; do not claim that their equal VRAM offsets alone prove identical
CPU physical page aliases. The conflict above uses the same BAR0 physical address.

## Inventory by physical ownership

| Domain | Existing driver mapping requests | Other ownership |
|---|---|---|
| POST framebuffer CPU physical range | Display WC with NC fallback; Vram Access NC for BAR0 or physical diagnostics | Firmware/display mapping lifetime |
| VidMm local segment | Retained VidMm NC map; CPU Present source/destination NC; DCN scanout/dump NC; VidMmProbeIb and Vram diagnostics NC | VidMm creates direct CPU virtual mappings for CPU-visible allocations; actual cache attributes still unmeasured |
| Reserved firmware surface outside VidMm | Display sample, Present seed and DCN NC mappings through carve-out | POST may instead name BAR0, a distinct physical window |
| Driver reserved tail | GpuMem VRAM pool/GART table, PSP and GART NC mappings | Excluded from VidMm local segment; cannot infer that all tail subranges are disjoint without allocator bounds |
| Ordinary system GTT pages | MmAllocateContiguousMemorySpecifyCache MmCached | GPU snoop/cache requirements separate from CPU alias matching |
| Registers/doorbells | NC | Must not be changed to WC as part of a blanket VRAM replacement |

M190 remains unresolved: Flags.Cached=0 states allocation backing-store defaults,
not measured attributes of every resident local allocation or borrowed CPU_VIRTUAL
page-table pointer. Switching only the retained segment mapping to WC is therefore
not a complete or established fix.

## Implementation requirements derived from this inspection

1. Track the actual successful POST mapping cache choice, including fallback.
   Diagnostics of the same physical pages must reuse the existing mapping or
   select identical attributes, including partial first/last-page overlaps.
2. Centralize physical VRAM mapping policy across Present, DCN, diagnostics and
   page tables. Keep MMIO/doorbells and system-memory allocation outside that policy.
3. Resolve OS-owned alias attributes or isolate page-table storage in a separate
   non-application segment before retaining a broad map of application VRAM.
   Segment split must update PTE encoding, root validation and VA walkers together.
4. Validate full mapping lifetimes and GPU ordering; matching CPU attributes alone
   does not prove GPU coherency or establish a black-screen root cause.

## Microsoft sources inspected

https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_memory_caching_type
requires matching caching behavior when different virtual ranges name the same
physical address. Checked2026-09-23, page updated2024-02-22.
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/gpu-segments
says VidMm directly maps CPU-visible memory-segment allocations. Checked2026-09-23,
page updated2024-12-19. Neither source measures this driver's live mapping attributes.
Local enriched d3dkmddi.md Cached field describes backing-store caching defaults;
original docs commit7515063cea4c9e98db6a92986c5b4ddb0463fd16, WDK26100.
