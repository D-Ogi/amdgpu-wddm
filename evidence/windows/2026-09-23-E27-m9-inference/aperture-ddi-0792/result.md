# M231: map/unmap aperture DDI publication, candidate0792

2026-09-23. Source/host only.

GfxPagingBuildAperture validates the entire advertised page interval, MDL page
span or aligned representable DummyPage, then takes the engine lifetime lock.
Live GART geometry must match Device.WddmAperture. A batch contains at most256
encoded PTEs and also fits the aligned live-ring budget with29 DWORDs overhead.
PFNs are independently resolved before any packet output. Map uses MdlOffset as
a page index; partial MDL boundary pages are valid page identities here. Map
CacheCoherent selects the cached TT snoop flag; false clears SNOOPED. Unmap
repeats the supplied DummyPage with valid cached-system flags.

WddmBuildAperture validates segment2 and reserved flags, emits into OS private
storage, preflights/publishes the record and only then advances MultipassOffset
by accepted pages. Partial accepted batches return INSUFFICIENT_DMA_BUFFER.
New early DDI branches and per-operation batch counters are compiled; they do not
claim full restricted-error compliance. No logical application-table commit is
needed: these PTEs belong to the dedicated VMID0 GART partition.

13678 host checks pass (+44). Actual builder and publication with modeled WDK
fields cover nonzero GART base, fragmented PFNs, skipped MdlOffset page, both
snoop settings,9+3 small batches,256+256+7 large batches, partial final MDL page,
private-header/capacity refusal, reserved flags, extent/MDL bounds, bad PFN,
live-geometry mismatch, valid repeated dummy mappings and unaligned dummy refusal.
Outer DDI counters/OS delivery are compiled only. Initial host PMDL declaration
order and WDK bitfield-to-BOOLEAN conversion warnings were corrected.

Full WDK build/sign passes:
P:/bc-250/scratch/build/bc250kmd-0792/package-umd, version0.7.92.1
SYS SHA256:22C317BC3B135E352A803B6F01C42B93B8E0C16B4C41BF746C7DF29F3E9CA0F1
Not deployed. Fresh device session required for descriptor/token changes.

Important remaining integration gap: this builder requires PagingReady, which
currently appears only after explicit engine RUN reaches stage8. It does not
handle OS map requests during pre-RUN startup. Startup/resource advertisement,
hardware initialization and restoration of mappings must be reconciled before
full-WDDM deployment; returning DEVICE_NOT_READY is not a final DDI solution.
CPU aperture mapping policy, hardware cache/barrier behavior, OS MDL/dummy lifetime,
physical transfers involving segment2, cross-engine serialization, GPU recovery,
arbitrary alias dependencies and pressure/performance acceptance remain open.
No lab access, reboot or USB change.
