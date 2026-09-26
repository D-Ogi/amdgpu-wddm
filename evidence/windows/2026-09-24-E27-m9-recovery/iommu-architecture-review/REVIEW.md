# BD-029: graphics IOMMU and DMA-remapping architecture review

2026-09-24, source-only review by Codex memory sub-agent. No production changes, capability/INF changes, firmware changes, lab access, or hardware tests. This is an engineering proposal, not measured IOMMU support. Proposed backlog status: TRIAGED (implementation epic); not FIXED and not merely NEEDS-LAB.

## Conclusion

The existing driver depends on identity system-memory DMA addressing. Supporting remapping requires a graphics-specific migration through Dxgkrnl memory ownership, address descriptor lists (ADLs), paging, and domain-switch quiescence. Adding IoGetDmaAdapter or an INF value is not the architecture described by Microsoft for a WDDM display miniport.

The original BD-029 statement needs two qualifications:

- Absence of an ordinary DMA adapter is not by itself proof that Windows has disabled graphics remapping. The graphics contracts use Dxgkrnl callbacks and capability negotiation. INF DmaRemappingCompatible=3 is a declaration; Dxgkrnl and HAL choose the actual mode. This review did not measure the current Windows domain mode.
- Identity isolation can preserve numerical CPU/device addresses while requiring every accessed page to be tracked and mapped. Nonidentity remapping additionally invalidates numerical CPU-PA/device-address comparisons. Not every ring register needs a direct substitution: our rings commonly use a GART GPU address, whose backing PTE is the place requiring a device-logical page address.

Keep the existing GpuMmu architecture. Do not enable the separate process-VA IoMmu model just to obtain DMA isolation.

## Primary references and identity

All paths below are relative to P:/bc-250. Line references describe files read during this review, not future edits. The driver tree was dirty over HEAD bed764da5192be7132d646be0e6331c1edeb30fd; no claim that HEAD alone reproduces the reviewed source. Display work continued concurrently, so search by the named functions when lines move.

PROVENANCE: MicrosoftDocs/windows-driver-docs text CC-BY-4.0, code MIT; MicrosoftDocs/windows-driver-docs-ddi text CC-BY-4.0, code MIT; original AMD amdgpu files inspected under their MIT notices in the Linux GPL-2.0 tree. No source was imported.

| ID | Exact local source and revision | Relevant contract / public source |
|---|---|---|
| MS1 | ref/windows-driver-docs/windows-driver-docs-pr/display/iommu-model.md:13-30, staging 110f60eaf2ac5836e644d320c1e92c1011f2af5e | WDDM2.0 shared process VA/PASID model, [upstream](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-model) |
| MS2 | Same directory, iommu-based-gpu-isolation.md:60-99,114-135,269-291, same revision | Ownership, domain switching, reserved ranges, tracked allocations, [upstream](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-based-gpu-isolation) |
| MS3 | Same directory, iommu-dma-remapping.md:13-46,48-66,68-145,149-170, same revision | Nonidentity remapping, ADLs, new memory callbacks, INF, [upstream](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/iommu-dma-remapping) |
| DDI1 | ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/ns-d3dkmddi-dxgk_iommu_caps.md:43-75, revision 7515063cea4c9e98db6a92986c5b4ddb0463fd16 | Isolation vs remapping caps; pre-StartDevice query, [upstream](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-dxgk_iommu_caps) |
| DDI2 | Same directory/revision, ns-d3dkmddi-dxgk_adl.md:43-67 and ns-d3dkmddi-_dxgkarg_buildpagingbuffer.md:430-475 | ADL representation and MapApertureSegment2, [ADL](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-dxgk_adl), [paging](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_buildpagingbuffer) |
| DDI3 | Same directory/revision, ns-d3dkmddi-dxgkargcb_create_physical_memory_object.md and ns-d3dkmddi-dxgkargcb_allocate_adl.md:45-77 | Physical and adapter memory object owners, CPU view vs logical view, [create](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-dxgkargcb_create_physical_memory_object), [allocate ADL](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-dxgkargcb_allocate_adl) |
| WDK1 | toolchain/nuget/microsoft.windows.wdk.x64/c/Include/10.0.26100.0/shared/d3dkmddi.h:2254-2290,3065-3120,4621,5040-5058,10008-10226 | Exact VIDMMCAPS/physical/IOMMU caps, ADL, MapAperture2 and memory callback declarations |
| WDK2 | Same WDK, km/dispmprt.h:2138-2177,2957-2958 | DXGKRNL_INTERFACE callbacks under WDDM2.4/WDDM2.9 guards and exclusive-access DDI slots |
| AMD1 | ref/linux-src/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c:75-105,337-367; amdgpu_ttm.c:825-838,888-923, Linux v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449 | DMA mapping results populate GART, dummy page is DMA-mapped; [GART source](https://github.com/torvalds/linux/blob/7d0a66e4bb9081d75c82ec4957c50034cb0ea449/drivers/gpu/drm/amd/amdgpu/amdgpu_gart.c) |

### Three distinct Microsoft models

1. WDDM2.0 IoMmuSupported means GPU requests carry a process PASID and shared CPU/GPU VA. It is not the opt-in bit for isolation beneath our existing GpuMmu page tables (MS1).
2. WDDM2.4 isolation places a logical adapter in a Dxgkrnl-managed domain and tracks GPU-accessible memory. Its original mappings are 1:1; equal numbers do not remove the ownership requirement (MS2, MS3:17).
3. New DMA remapping supports nonidentity logical addresses and ADLs. The conceptual guide labels the feature Windows 11 22H2/WDDM3.0; the exact headers expose these structures/callbacks beginning at WDDM2.9, and DDI pages list Windows Server 2022/WDDM2.9. Keep these source labels separate when choosing the supported OS/interface; do not assume bumping one version macro establishes support (MS3, WDK1/2).

A domain belongs to the logical adapter, shared by its physical adapters. Dxgkrnl owns mapping and page locking. VidMm handles its own backing stores, mapped allocations and monitored fences; the KMD must not independently DMA-map those pages a second time as if they were its private allocation (MS2:60-81).

### Two documentation discrepancies checked

Local-first review found these discrepancies, which justified two narrow online checks. The live pages reproduced the local DDI behavior; no new dependency was fetched.

- The broad guide says callbacks must run at IRQL <= APC_LEVEL. WDK1's individual new memory callbacks carry PASSIVE_LEVEL SAL; the individual [CreatePhysicalMemoryObject DDI](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkcb_createphysicalmemoryobject) also requires PASSIVE_LEVEL. Allocate/map/free these objects at PASSIVE_LEVEL; no allocation callback inside a spinlock or elevated paging/interrupt path.
- WDK1's MapPhysicalMemory fields Offset and Size are annotated _In_, but local and live [MapPhysicalMemory parameter remarks](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-dxgkargcb_map_physical_memory) describe both as input/output. Preserve the returned base, offset and size; access requested data at base plus returned offset and unmap the returned view. Start with aligned requests, then verify an offset case against actual callback output. Do not infer an ABI layout change from annotation differences.

## Current address and ownership audit

All driver paths in this table start at bc250-win/driver/. These are concrete inspected sites, not a claim to have exhaustively catalogued every consumer.

| Owner/path | Current assumption | Required separation |
|---|---|---|
| kmd/bc250kmd.h:52; kmd/wddm.c Wddm caps around1843/1891, node metadata2076, initialization4752 | Build asserts WDDM2.0, advertises WDDMv2/GpuMmu and node IoMmuSupported FALSE; newer callback slots are outside current interface | Negotiate a supported newer graphics interface and check structure/callback lengths and required DDIs; do not merely expose newer bits through old layouts |
| kmd/gpumem.c bc250_shim_mem_alloc:401-465 | GTT allocation via MmAllocateContiguousMemorySpecifyCache; MmGetPhysicalAddress plus page offsets goes directly into bc250_gart_bind | CPU allocation/view and device-logical page list need distinct owners. GART entries take the mapped device addresses. The returned entry->Mc is gart_start+offset, not a CPU PA; retain that distinction for ring consumers |
| kmd/gart.c:408-415,145 | Dummy page uses an ordinary cached contiguous allocation; DummyPhysical is programmed as dummy_page_dma | Map this long-lived backing through Dxgkrnl too; keep it mapped while any unbound GART entry can reference it |
| shim/bc250_ih.c:433-456; shim/bc250_sdma.c:843-885,1210-1237; shim/bc250_gfx.c allocations and ring/MQD fields | IH, SDMA and GFX objects consume the allocator's CPU pointer and GART .mc addresses | Central allocator migration covers backing mapping, but teardown and all autonomous hardware consumers must preserve the mapping lifetime. Do not feed an ADL page directly into a register currently requiring GART/MC space |
| kmd/vidmm.c VidMmCommitPagingAperture:212-228; kmd/gfx.c GfxPagingMdlAddress:1855-1873 and aperture builder2827; kmd/wddm.c map dispatch3213 | Old MAP_APERTURE_SEGMENT supplies MDL PFNs, stored in aperture state and encoded into GART | Implement negotiated MAP_APERTURE_SEGMENT2 with ADL count, flags, offset and page units; support contiguous and list forms. Preserve mapping ownership until GPU unmap retirement |
| shim/bc250_pte.c bc250_pte_address:110-119; kmd/vidmm.c VidMmSystemLimit:70-83 | System-segment addresses are labelled CPU physical and bounded by installed physical RAM maximum; source cites old no-IVRS observation M47 | Separate device-address representability from CPU physical extent validation. Under remapping the translated system target is not a CPU pointer or a CPU physical-range proof |
| kmd/vidmm.c table checks353-357 and CPU table walk612-617/712-737 | Table memory CPU ownership is checked using actual CPU PA; system leaf diagnostic probe uses encoded GPU address as MmCopyMemory PHYSICAL input | CPU table identity checks can remain CPU checks where that is the actual contract. GPU system leaf addresses cannot be inverted into CPU PA; diagnostic reads require an owned CPU view of the same backing object or must report unavailable |
| kmd/wddm.c WddmNativeDmaMapping:2885 and mapping observation3500; kmd/gfx.c SDMA validation1648-1649 | GPU translation must equal MmGetPhysicalAddress(CPU buffer) | Preserve the same-bytes proof through OS-provided paired views / owned mapping identity, not numeric equality across domains. CPU_PA==IOVA is only an identity-mode observation |
| kmd/paging_aperture_state.h:11-24; WddmNativeRangeDisjoint and native source/destination checks | Distinct aperture slots alias iff physical address slices overlap | Device address inequality does not prove different backing pages if two logical mappings alias one backing object. Retain canonical backing-object/range identity where available; no permissive alias fast path until an authoritative identity is available |
| kmd/gpumem.c VRAM branch400-405; kmd/vidmm.c SegmentPhysical and kmd/wddm.c segment descriptions1760-1816 | CPU BAR/physical mapping, local GPU MC offset and segment-relative address are already different concepts; current segments do not set PopulatedFromSystemMemory | Audit which reserved carve-out accesses traverse the IOMMU on this APU; do not globally translate local MC addresses or turn local VRAM into arbitrary system PFNs |

The historical Linux M47 no-IVRS measurement is useful for its exact firmware/configuration baseline. It is not a permanent assertion about a future IOMMU-enabled firmware setup and was not remeasured here.

AMD1 independently confirms the conceptual boundary: Linux gets a DMA mapping for the dummy page and system backing, then uses DMA addresses in GART PTEs. That is hardware-usage guidance, not a reason to port Linux dma_map APIs or ordinary Windows DMA-adapter ownership into WDDM.

## Staged implementation proposal

### 1. Address and lifetime boundary, still on the current lab configuration

Introduce explicit internal views for CPU VA/CPU backing identity, device-logical page list, GPU GART/MC address, and segment-relative offset. Keep allocation owner, domain/generation and accepted-command retirement associated with those views. Do not make raw PFN-to-PA substitution a global utility.

Start with private GTT allocator and dummy page, because they feed IH, writeback, fences, ring storage and MQDs through a small boundary. Preserve current content controls and allocation/retirement behavior first. This can be developed and host-tested before any remapping support is advertised.

### 2. New Dxgkrnl memory backend and version negotiation

Choose a coherent target interface supported by the intended Windows build; WDK2.9 contains the required new callbacks, while the documented client remapping feature is WDDM3.x. Audit required newer-interface DDIs and existing size assertions before changing the advertised WDDM version.

For private allocations use CreatePhysicalMemoryObject -> MapPhysicalMemory for a CPU view, plus AllocateAdl on hAdapterMemoryObject for GPU-visible pages. OpenPhysicalMemoryObject is needed when creating without an adapter or opening against another adapter. Free the ADL, unmap CPU views and close/destroy handles only after every GPU consumer has stopped; avoid close plus destroy of the same adapter handle. Keep the old allocation backend only as an explicitly selected legacy path, not silently during an advertised remapping session.

Prefer a contiguous ADL representation where useful; it does not require physically contiguous CPU RAM under remapping. Keep cache type and BD-021/022 coherence work separate from address translation. CPU/device sharing still requires the existing cache and ordering contract.

Do not adopt the older AllocateContiguousMemory/AllocatePagesForMdl/MapMdlToIoMmu callbacks as the final nonidentity design: MS3 expressly disallows them for drivers using the new logical remapping model. An identity-isolation-only interim step is possible but adds a backend that would then need replacement.

### 3. OS-owned paging and alias correctness

Support MapApertureSegment2 and MapAperture2Supported, consuming ADL page numbers with the supplied AdlOffset. The optional CpuVisibleAddress is borrowed until the corresponding unmap. Request it only where CPU access is needed and the allocation truly follows the physically accessed aperture path: MS3 requires AccessedPhysically and MapApertureCpuVisible in addition to MapAperture2Supported. Do not mark every GPUVA allocation physically accessed just to get a CPU pointer.

For UPDATE_PAGE_TABLE and GPUVA paging, retain the address domain dictated by the negotiated OS contract. The older local DXGK_PTE DDI describes PageAddress as a system memory address or segment offset; it is not an authoritative CPU-PA inverse for remapped mode. Before enabling that mode, resolve and document the precise new-interface PTE/transfer pairing used by our GPUVA path and the OS DMA-buffer CPU/GPU pairing. A nonidentity host fixture is useful but cannot settle an undocumented runtime pairing.

Migrate the native DMA-buffer identity checks and alias graph alongside the new address view. For driver-owned objects, paired views plus backing-object/range identity supply the proof. For OS-owned mappings, use only identities actually guaranteed by VidMm; distinct IOVAs alone are insufficient. Retain the existing conservative path where no proof is available. Do not make the mapper appear functional by deleting these safeguards.

### 4. Reserved memory, pre-start caps and exclusive access

Implement pre-StartDevice PHYSICAL_MEMORY_CAPS and IOMMU_CAPS queries without depending on MMIO/resources acquired later. Report real accessible bounds, including the weakest active system-memory consumer, rather than blindly using an all-ones value or the installed RAM limit. IommuIsolationSupported/Required must match the related VIDMM caps. Leave process-VA/PASID requirements unset unless separately implemented.

Inventory firmware-reserved GPU-accessible system memory. MS2 maps PopulatedFromSystemMemory segments, and obtains private reserved ranges through the two-pass HARDWARERESERVEDRANGES query. Such ranges must not overlap NTOS-managed memory. The hidden APU VRAM carve-out, private tail and CPU BAR are not interchangeable; establish each hardware access path before declaring ranges. Power-save scratch must use the documented tracked framebuffer save services if needed.

Implement BeginExclusiveAccess/EndExclusiveAccess: from successful begin through end there must be no device reads or writes to system memory. A completed user fence is insufficient if IH, writeback, CP/SDMA queues or firmware can still generate traffic. Dxgkrnl already suspends scheduling and flushes active workloads, and allows suppressing interrupts/vsync during the pair. The KMD still owns its asynchronous consumers and restart ordering. Preallocate required state at PASSIVE_LEVEL; do not require a system-memory command fetch while the domain is changing.

Only after these pieces work together add the truthful support declarations, including DmaRemappingCompatible=3. No registry bypass or fallback-success option is an acceptance condition.

### 5. Positive acceptance controls

1. Host: simulate one backing allocation with CPU PA different from device address, plus a stable CPU pointer. Encode a system GART/PTE target from the device address while local MC translation remains unchanged. Identity mode must pass the same content/extent tests.
2. Host: cover ADL contiguous and fragmented lists, nonzero AdlOffset and page offset, plus paired CPU view offset. Confirm a GPU-command reference prevents early ADL/object release. These are bounded success paths, not an exhaustive error matrix.
3. Host: use two distinct logical mappings of the same backing page. Confirm copy/fill alias handling preserves source bytes or takes the conservative path. Keep an ordinary disjoint copy as the positive control.
4. Existing lab configuration, before firmware changes: run current private allocator, IRQ/ring/fence and paging content controls through the new ownership backend, recording actual interface/callback selection and mapping lifetimes. This proves the backend on that mode only.
5. Future authorized remapping session: capture actual active graphics domain/mode and at least one paired allocation whose logical address differs from its CPU backing address. A booted desktop, INF flag, or IOMMU menu setting alone does not prove nonidentity operation. Do not fabricate a lower HighestVisibleAddress just to force remapping.
6. With that positive mode witness, run CPU->GPU->CPU data controls through GFX and SDMA, fragmented system allocations, aperture map/unmap/remap, local VRAM controls, normal DWM scanout and interrupt/writeback progress. Repeat with ordinary memory pressure and lifetime drain; confirm contents and absence of IOMMU faults.
7. Finally validate domain detach/attach and supported stop/start/power transitions while quiescence and resource lifetimes are observed. This is a separate hardware acceptance step, not implied by host tests or a single inference run.

## Boundaries still requiring implementation or hardware evidence

No source-only review can prove BC-250 requester/domain routing, active Windows remapping mode, nonidentity runtime address pairs, correct reserved-carve-out treatment, quiescence of every autonomous engine, or successful resume. IVRS publication/requester scopes must be checked in a future explicitly authorized firmware configuration. This task changed none of it.

The precise OS-owned GPUVA PTE/DMA-buffer pairing and backing-alias identity under the selected modern DDI remain implementation prerequisites. The ADL aperture contract is explicit; extending that certainty to every old physical-address field without evidence would be unsafe. A remaining documentation gap should be researched narrowly before coding that part, using primary sources.

No build or production regression test was run because the only deliverable is this report. Validation consisted of current source inspection, local primary contract comparison, exact WDK declarations and the two targeted online discrepancy checks above. Existing memory reports BD-020/021/022 and all production files were left unchanged.

## Proposed existing-ID comment

- 2026-09-24 Codex: BD-029 triaged in scratch/m9/bd029/REVIEW.md. WDDM graphics remapping uses Dxgkrnl memory objects/ADLs, capability negotiation and exclusive-access quiescence; missing IoGetDmaAdapter or an INF value alone does not establish the current mode. Audited private GTT/dummy, OS aperture, PTE/CPU-PA checks, alias identities and local-MC boundaries. Staged prerequisites and positive nonidentity acceptance controls recorded. No implementation or lab change; support remains unimplemented. Recommend TRIAGED.
