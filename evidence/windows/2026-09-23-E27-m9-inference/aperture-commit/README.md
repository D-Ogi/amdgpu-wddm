# Logical aperture publication integration, 2026-09-23

VidMmStartLayout allocates512KiB logical aperture storage with the retained table state; VidMmStop frees it under CpuUpdateLock after CPU access exclusion. No storage is read by GPU. Accepted MAP/UNMAP records copy only the emitted page slice from OS-owned MDL PFNs, under the existing logical-state lock. WddmPublishPagingRecordCore checks DMA capacity/private header first, commits logical state, then copies/publishes the packet and advances pointers. WddmBuildAperture supplies exact Start/Next multipass indices. No MDL pointer retained.

Host extracted actual KMD builders/publication/VidMm start-stop:13689checks0failures. Earlier13685-check deliberate omission of aperture commit gives5failures, proving map/unmap observations depend on the integration. Tests cover accepted map identity, multipass, failed private-header publication leaving mapping unchanged, unmapping a previously valid page, and start/stop allocation balance. Portable module533checks from M259 remain relevant.

WDK signed development build passes: scratch/build/aperture-commit-dev/package/bc250kmd.sys SHA2565E13110A5FC45A1A59244314D59D0367D5D44E227A74745D07F930115EBCB56A. Existing version0798 retained for DEV ONLY; not installed, not an official versioned candidate. Lab unchanged from M258.

Still open: segment2 endpoint consumption, lookup/publication ordering over complete transfers, cross-page aliases, pending-buffer cancellation/lifetime and runtime callbacks. Lock-protected logical state is not proof of hardware completion or OS page pinning. Integration does not claim physical aperture Transfer support yet.
