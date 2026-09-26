# BD-022 cache-alias review

2026-09-24. Candidate137 source snapshot: scratch/m9/display137-build-source.
Read-only review and host controls; no driver changes, deployment or lab access.
Recommended existing backlog status: NEEDS-LAB. The M200 source defect is already
fixed; the separate M190 alias policy remains open.

## Current ownership and mapping requests

| Domain | Current requests / ownership | Assessment |
| --- | --- | --- |
| POST physical pages | display.c:13-35 records WC success or NC fallback in FramebufferCacheProtect. vram.c:162-181 matches aliases at physical-page granularity; mixed-domain maps refuse. | Historical M200 unconditional-NC POST diagnostic path is gone. |
| Surface reads/writes | VramMapCpuRange at vram.c:185; DCN dump/fill/scanout and WDDM Present source/destination/seed use it. | POST overlaps agree. Non-POST pages still default to NC; this does not establish agreement with VidMm's independent CPU maps. |
| Local table segment | vidmm.c:104-145 maps only dedicated table extent NC. Application allocations exclude this segment. CPU_VIRTUAL initialization at 302-350 writes through the OS-owned borrowed pointer. | Whole-application retained mapping from M190 is gone, but table aliases remain. The borrowed pointer's actual cache attributes are unknown. |
| Application local segment | No whole-segment retained VidMm map. wddm.c:1753 and 1780 advertise local/table segments CpuVisible. Ordinary local allocations have Cached=0; Cached=1 is restricted to GTT by umd_blob.c:120. | Cached=0 describes backing store, not every resident mapping. Present/DCN/diagnostic NC views can still coexist with OS maps of local pages. |
| Driver-reserved tail | gpumem.c:400 pool and :418 GART table NC; gart.c:107 zeroing NC; psp.c:153 NC. Allocator/reservation bounds separate tail from OS application/table segments. | Inspected CPU mappings agree with one another; no WC change justified. This is not a new hardware alias measurement. |
| OS system GTT | gpumem.c:424 and gart.c:408 allocate MmCached. VidMmProbeIb at vidmm.c:721 uses physical MmCopyMemory for system pages, not a new NC view. | Preserve cached allocations and existing copy policy. CPU map attributes and GPU SNOOPED are separate questions (BD-021). |
| BAR5 and doorbells | mmio.c:125 and gpumem.c:140 NC | Keep MMIO outside any VRAM cache-policy change. |

BAR0 and the hidden carve-out can reach the same VRAM bytes through different
CPU physical address ranges. Matching offset alone is not proof that the CPU
translations name an identical physical address, nor proof that mixed aliases
through those windows are harmless. No such hardware conclusion is drawn here.

## Contracts and original AMD reference

Local Microsoft DDI snapshot 7515063cea4c9e98db6a92986c5b4ddb0463fd16,
ref/ddi-display/d3dkmddi.md:18223-18229 specifies Cached as backing-store policy.
Cached defaults to write-combined backing, and VidMm provides appropriate flushes
for use in a non-coherent segment; it does not specify the borrowed table VA PAT.

Local conceptual snapshot 110f60eaf2ac5836e644d320c1e92c1011f2af5e:
- display/gpu-segments.md says VidMm directly maps CPU-visible memory segments.
- display/system-paging-process.md:34 requires immediate CPU_VIRTUAL initialization
  even when normal updates use GPU_PHYSICAL. It cannot simply be dropped or queued.
- UPDATEPAGETABLE local DDI line65 defines CpuVirtual, without a cache-attribute
  contract in the inspected entry.

The wdm memory-cache reference pages are absent from the local display-only DDI
checkout (rg file search confirmed). Rechecked official pages on 2026-09-24:
- [MEMORY_CACHING_TYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_memory_caching_type), updated2024-02-22:
  aliases of the same physical address must have consistent cache behavior.
- [MmMapIoSpaceEx](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmmapiospaceex), updated2022-02-25:
  accepts one of NC/WC with access flags. Its return value does not reveal an
  independent mapping's effective cache type.
- [MmMapLockedPagesSpecifyCache](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmmaplockedpagesspecifycache), updated2023-01-13:
  mappings of properly locked MDLs generally reuse an established page cache type.
  This does not authorize synthesizing an MDL for hidden firmware carve-out pages
  or assuming such an MDL establishes the borrowed VidMm pointer's attributes.

Original AMD at linux-src commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449:
- amdgpu_vram_mgr.c:607-610 chooses cached VRAM only with CPU-connected XGMI,
  otherwise write-combined.
- amdgpu_ttm.c:1959-1972 maps the normal VRAM aperture using ioremap_wc (the
  CPU-connected case uses ioremap_cache). This describes Linux-owned mappings,
  not the cache type selected by Windows VidMm.

PROVENANCE: Linux AMD files carry MIT notices within the GPL-2.0 kernel repository;
review only, no imported code. Microsoft sources used for contract checks.

## What historical evidence actually establishes

M201/M202 fixed POST-aware source selection, not all OS-owned aliases. M205 split
application and table storage; M206 removed optional system-IB NC aliases. These
implementations remain in candidate137.

M317 evidence, candidate07102-start/start.raw.log:321-334, returns0xC0000141 from
MmGetCacheAttribute for physical table addresses. The accompanying type6 is not
a valid observation. This already-tested physical query is not an effective
per-VA PAT measurement and rerunning it alone is not a closure plan.

## Host validation

scratch/m9/bd022/run-post-cache.cmd extracts the actual frozen137
DisplayMapFramebuffer/DisplayUnmapFramebuffer/VramMappingProtection/
VramMapCpuRange into the existing project harness. /W4 /WX compile succeeds:
- Actual selector: 2072 checks, 0 failures.
- Deliberate always-NC regression: 2072 checks, 2049 failures, expected nonzero exit.

Log: scratch/m9/bd022/post-cache.log. Generated source and full mutation output:
scratch/build/bd022/actual.c and mutation.log. No new test suite or production
change. These controls verify requested mapping flags, not actual OS/PAT behavior.
The display agent confirmed its concurrent BD-003/011 work will not change cache
attributes; DisplayMap/Unmap edits concern bugcheck readiness only.

## Required next decision / dependency

1. To retain independent CPU views, establish their actual mapping attributes for
   the same local physical pages: borrowed table VA versus retained table VA, and
   UMD/VidMm allocation VA versus Present/DCN view. Include page size and effective
   x86 PAT/MTRR interpretation. Successful GPU readback or MmGetPhysicalAddress
   alone cannot answer this. Live KDNET is explicitly disallowed without a new
   per-run authorization; no such measurement was attempted here.
2. Alternatively eliminate aliases by design. The existing logical shadow covers
   the paging-process construction view, not every executed application table.
   Removing SegmentMapping now would break pre-RUN GPU_PHYSICAL bootstrap writes,
   committed application VA walks used by CPU Present, and IB diagnostic walks.
   A replacement must cover those operations and immediate CPU_VIRTUAL init,
   with queued-versus-executed state and retirement kept distinct. Surface CPU
   access is a second ownership problem; removing the table map alone is incomplete.
3. Do not replace every NC request with WC based on Linux or Cached=0, and do not
   convert private tail/MMIO/GTT indiscriminately. No bounded correct code fix is
   established by the available source contracts, so no speculative patch is made.

## Proposed existing-ID comment

- 2026-09-24 Codex: Rechecked candidate137. Historical M200 POST diagnostic conflict
  is already source-fixed (M201/M202): actual POST selector passes2072checks and
  an always-NC mutation fails2049. Dedicated table storage removes the old broad
  application mapping, but M190 persists for retained NC versus borrowed
  CPU_VIRTUAL table maps and independent VidMm versus Present/DCN application
  views. Cached=0 and Linux WC do not establish Windows effective attributes;
  M317 physical queries already failed0xC0000141. No production change or new
  hardware conclusion. NEEDS-LAB for same-page actual-attribute evidence, or a
  complete alias-removal design preserving bootstrap/Present/table-walk behavior.
  Review and controls: scratch/m9/bd022/REVIEW.md, post-cache.log.
