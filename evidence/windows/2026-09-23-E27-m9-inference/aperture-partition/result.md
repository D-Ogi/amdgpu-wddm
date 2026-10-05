# M228: disjoint OS aperture geometry

2026-09-23. Source/host only; no hardware access.

Current QuerySegment4 advertises256MiB at MC base0. gpumem reserves driver GTT
below64MiB; transient paging maps two pages immediately after64MiB. Map/unmap
aperture is still unimplemented. Directly implementing OS PTE writes at the
existing base could overwrite driver or transient mappings. This is a source
integration hazard, not measured hardware corruption.

PagingApertureInit derives a separate256MiB extent after both private consumers:
offset67117056 bytes, PTE offset131088 bytes. It validates the full MC/PTE backing
range, alignment and48-bit exclusive limits before returning any usable extent.
PagingApertureRange validates nonempty page intervals and derives independent
MC and PTE addresses without wrap, accepting the exact last page and full extent.
Failure clears outputs. No allocation, MMIO, mapping or descriptor change occurs.

11832 host checks pass (+86), including independent numeric extent controls,
adjacency to the existing window, start/end/overflow/short-table cases and
sampled ranges across the entire aperture. Full WDK development build passes.
Development-only package retains0790, NOT a replacement official artifact:
P:/bc-250/scratch/build/aperture-partition-dev/package-umd
SYS SHA256:AAC5A9540A54EEF52D9367491075839B3657E2971A9ED5BE5D727F66A58601E2
Official0790 candidate and installed0773 are unchanged; no deployment.

Next integration: obtain/capture actual GART geometry through GartDevice under
its documented lock/lifetime, then use one layout in both QuerySegment4 and the
map/unmap builder. GartStart precedes WddmStart, but setup is lazy in GartDevice;
GartStart alone does not establish gart_start/table geometry. Do not independently
retype AMD placement logic or assume base0. Required map/unmap must respect MDL
page offsets, CacheCoherent flags, DummyPage replication, bounded multipass,
PTE-write ordering and GART invalidation. The geometry helper alone does not
establish OS lifetime, hardware map execution, cache policy or safe restart.
