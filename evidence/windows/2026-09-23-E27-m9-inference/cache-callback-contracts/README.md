# Candidate OS mapping callbacks and M190

2026-09-23 contract inspection. Source remains0781; no code/build/lab changes.
Local enriched WDK26100 declarations and MS source descriptions are captured here;
original docs revision7515063cea4c9e98db6a92986c5b4ddb0463fd16. Line slices preserve
source text and may include adjacent sections. Primary web pages checked below.

1. DXGKCB_MAPFRAMEBUFFERPOINTER maps a subregion of a per-adapter section used by
   framebuffer save/restore. The driver copies hardware contents to/from this
   section through a preallocated transfer buffer. Its output is not documented
   as a pointer to arbitrary live local page-table storage. It is not a direct
   replacement for VidMm's retained physical table map.
2. DXGKCB_MAPPHYSICALMEMORY uses the cache type selected when its physical-memory
   object was created. CREATE_PHYSICAL_MEMORY_OBJECT supports IO_SPACE with a
   driver-supplied physical base and size, but still requires a chosen CacheType.
   The inspected contract does not promise discovery/matching of cache attributes
   for an already-existing independent CPU_VIRTUAL pointer owned by VidMm.
3. These are newer interfaces (framebuffer callback WDDM2.4; physical-memory API
   WDDM2.9). This driver advertises WDDM2.0 and copies the interface for that version;
   their availability cannot be assumed simply because local headers contain them.
   Upgrading the interface requires broader capability/DDI validation.

Conclusion limited to these contracts: neither callback establishes the missing
cache guarantee. Do not switch APIs and declare M190 closed. This is not a claim
that no Windows mechanism can provide the needed information, nor a hardware
restriction. MmGetCacheAttribute/Ex declarations found in M206 remain a separate
candidate for bounded diagnostics; a query after creating our own alias is not
independent proof of a particular borrowed pointer's effective cache attributes.

Remaining implementation choices require stronger evidence: obtain a documented
or measured consistent mapping policy, or avoid simultaneous independently chosen
CPU mappings. Any no-alias design must retain immediate CPU_VIRTUAL initialization,
GPU_PHYSICAL queued updates, system paging construction state, application Present
address resolution, diagnostics and lifetime guarantees. Merely turning off these
operations or returning empty success would not meet M9.

Sources:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkargcb_mapframebufferpointer
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkcb_mapphysicalmemory
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkargcb_create_physical_memory_object

No new hardware or performance acceptance. Other independent M9 work remains:
legacy physical ADL transfer/fill, restricted BuildPagingBuffer error semantics,
actual GPU halt/reset/reentry, OS concurrency/ownership and1GiB/performance tests.
