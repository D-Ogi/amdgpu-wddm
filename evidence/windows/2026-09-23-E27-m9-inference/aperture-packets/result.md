# M230: ordered aperture update packets

2026-09-23. Source/host only.

bc250_sdma_paging_set_aperture emits explicit PTE WRITE_LINEAR, a noninterrupting
fence/memory poll, and VMID0 GART invalidation through the existing AMD-derived
helpers. Entire reservation precedes output. Emission29+2*N DWORDs, reservation
rounded to16 DWORDs. Validates PTE count/alignment/48bit interval, nonzero marker,
marker alignment/48bit interval/non-overlap with PTEs and initialized invalidate
request/ACK fields. Caller supplies encoded scattered MDL or repeated DummyPage
entries; NULL does not implicitly produce zero unmap entries.

13634 checks pass (+1795), including counts1,2,7,16,31,128,480,497,512, byte-for-byte
comparison against separate ordered-update and GART-flush constructors, every
PTE payload, tails, one-DWORD-short capacity for each count and all48 capacities
below a two-page reservation. Invalid fields preserve output. Repeated nonzero
dummy-page payload and maximum nonzero single marker work. Initial host build
caught potentially uninitialized flushWords in a short-circuit control; outputs
are now initialized before reference construction.

Full WDK development build/sign passes; retained version0791:
P:/bc-250/scratch/build/aperture-packets-dev/package-umd
SYS SHA256:0E33A45A14B4B00ED6FDCC3D6BC90E2F83EC530EA7847C44A608902736162816
Development-only, do not replace/deploy official0791 with this artifact.

Not yet connected to map/unmap DDI. Next capture/validate live aperture geometry,
convert OS MDL pages and CacheCoherent/DummyPage fields to PTEs, split counts to
live-ring capacity, publish per-buffer records and preserve page-index progress.
Hardware memory ordering, OS page lifetime and CPU aperture policy remain open.
No lab access, deployment, reboot or USB changes.
