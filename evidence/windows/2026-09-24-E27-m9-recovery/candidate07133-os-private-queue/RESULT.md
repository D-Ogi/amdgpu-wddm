# M426 - OS-private paging queue on KMD0.7.133.1

All runtime paging builders now reserve queue slots in the OS private buffer.
Direct capacity accounts for both command data and per-start slots; native
records reserve128bytes. Constructors clear ownership before publication,
pointers advance over the full aligned record, and logical updates retain
pre-publication validation. Every supported nonempty paging submit borrows its
slot; the per-submit nonpaged allocation and private-data copy are removed.
Legacy/empty records are rejected at this internal entry, whose actual physical
and virtual callers require nonempty driver-built ranges.

Host:907971 actual builder/packet checks and711 actual queue checks pass.
Direct/native records exported by the real builders enter the actual queue
without any pool allocator mock. A completion callback immediately overwrites
the released slot; reversing release/publication fails123 checks. Full WDK
build passes. The first migrated fixture used obsolete private capacities and
one incorrectly rewritten array access (76 failures); correcting test storage
and that access restored the original875868 checks, then format/integration
witnesses extended the suite. A generator comment anchor was also updated;
the initial generator failure and failed fixture output are preserved.

Hardware, unit A:

- Installed0.7.133.1 SYS SHA256
  37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87.
- One PnP disable/install/enable. Boot11:44:14 and DWM4448/start13:58:18
  retained through18:16:21. M412D3D/M414RADV identities unchanged.
  No OS/DWM/AC restart. Final temperature66.8C,1000MHz/820mV preflight passes.
- Existing direct/IB and translated/remap startup controls pass.
-64MiB and1GiB each pass three eviction/restore cycles and four complete
  GPU word-oracle readbacks. Eight shader CPU hashes and both complete E14
  model outputs match, with7/7 and23/23 layers offloaded. Native/worker exits0.
-30673 OS-private queue admissions equal30673 actual SDMA submissions and
  completions. GFX6706/6706. Zero timeouts/refusals; noTDR.
- Native98transfers/92fills,13118906368bytes. The1GiB phase adds33transfers,
  21fills and exactly10GiB native data. Zero capture plans reserved or heap.
  Context capture arenas and the allocating physical fallback still exist.
-1GiB restore+wait1766/1609/1578ms versus132's2328/1562/1469ms. This is one
  ordered run per build, not evidence of a speedup or Windows/Linux parity.

A model-artifact collector encountered an SSH banner timeout after the worker
had completed. Missing files were fetched sequentially from the same run;
no workload was repeated or machine reset. All14 artifacts were validated.
Raw logs preserve original bytes except irrelevant PCI instance/interface
identifiers. The private scanout BMP is hash-recorded only, not visually reviewed.

This closes the source-level per-submit pool dependency for driver-built paging
records and demonstrates the exercised OS path. Forced preemption/cancellation,
device generation and full OS buffer lifetime, capture resource bounds,
PFN/cache aliases, cold/power startup and matched Windows/Linux performance
remain open. Full M9 is not complete.12GiB/75%-of-physical-memory residency is
not tested here; the current physical VRAM carve-out is8GiB.
