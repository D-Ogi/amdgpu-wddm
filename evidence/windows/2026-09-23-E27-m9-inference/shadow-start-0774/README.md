# Candidate0774 logical page-table storage lifecycle

2026-09-23. HEAD bed764d plus current worktree. No lab access or deployment.

VidMmStart reserves caller-owned logical storage before readiness. The current
WDDM2 paging-process1GiB VA and four9-bit table levels need515 table pages;
1031 hash slots keep occupancy below50%. Storage is paged pool, used only at
PASSIVE_LEVEL under CpuUpdateLock. No per-callback allocation/growth. A pool
failure stops before mapping, a later mapping failure frees pool, and normal
stop drains users before releasing mapping and storage.

CPU_VIRTUAL initialization obtains the physical table identity using
MmGetPhysicalAddress on the OS-borrowed valid mapping of the pinned table. This
identity indexes logical metadata; it does not produce a DMA address, nor retain
the borrowed pointer. Complete input encoding and capacity preflight precede
CPU writes; only then are encoded logical entries registered. Unknown entries
remain unknown. Immediate pre-RUN GPU_PHYSICAL updates mirror already registered
tables; unregistered application tables are not added. Later queued GPU updates
and the transfer resolver are NOT connected yet. M191 remains failing.

References:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmgetphysicaladdress
- MS DDI repo7515063cea4c9e98db6a92986c5b4ddb0463fd16 / WDK26100.

Verification:
- Actual extracted initialization/update/stop routes:6229 checks,0 failures;
  physical-identity lookup is modeled, not a kernel mapping test. Includes pool
  failure, map failure cleanup, partial initialization, known bootstrap changes,
  registration exhaustion refusing before writes, and reset/release.
- Portable shadow capacity/storage:557 checks,0 failures plus kernel compile.
- Extracted wrappers with Windows SRW model: exclusive-lock removal fails10,
  shared-reader removal fails6; actual wrappers pass0, including pool release
  after held readers/writers drain.
- Explicit queued-PTE ordering acceptance:8 checks,1 failure, exit1 (still open).
- Full WDK build/sign passes. Post-build source change is comment only.

Package scratch/build/bc250kmd-0774/package-umd, version0.7.74.1. SYS SHA256:
3F5238EFC3FED7591F0C20D73E83C3FBEA3A2BD923333666C80B5DA2735DE452.
Installed lab last verified0773, closed gates. This candidate is not deployed.

Remaining: queued-batch logical publication only after private/DMA acceptance,
resolver integration, copy-PTE semantics, power/reset/start failure coverage,
actual CPU mapping contract/cache policy and OS-facing error handling. Internal
capacity status is not a new authorized BuildPagingBuffer return code; inherited
outer error handling remains unresolved. No full M9 acceptance claim.
