# Allocation-free CPU PTE snapshot, local0772

The CPU updater now uses512entries embedded in the existing static VidMm state,
not a per-call pool allocation. An exclusive push lock serializes the snapshot
across complete encode/map/write operations; VidMmStop joins active CPU writers
before closing readiness gates. Lock order is optional device GfxPagingLock then
VidMm snapshot lock; no inverse acquisition. PnP start/reinitialization serialization
remains an assumption of the existing single-adapter global state.

Why: BuildPagingBuffer documentation provides only SUCCESS, ALLOCATION_BUSY for
specific busy-allocation situations, and INSUFFICIENT_DMA_BUFFER for capacity.
It describes no arbitrary heap-allocation retry. Removing that allocation makes
the internal path less fallible without inventing an error mapping. Physical
MmMapIoSpaceEx still may fail; the OS-facing failure policy remains unresolved.
The prior0771 allocation-failure test describes the historical implementation;
that allocation no longer exists here.

Verified primary references2026-09-23:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_buildpagingbuffer
https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/bug-check-0x119---video-scheduler-internal-error
The latter describes0x5 as faulting system/paging commands, not an exhaustive map
from every possible BuildPagingBuffer return code. No unsupported status-return
experiment was performed. Microsoft sample code was inspected, not imported.

Validation:6213host packet/routing/CPU checks PASS. Actual CPU update wrapper and
VidMmStop extracted into a Windows SRW-lock model: a second writer cannot enter
while the first holds its snapshot; stop waits for the writer; later updates
refuse after stop. Lock-disabled mutation FAIL4violations; actual wrappers PASS0.
CPU encoder/write behavior is covered separately by the existing route harness;
the concurrency test models only their ownership boundary. Not a kernel scheduler
or hardware lifecycle proof. Existing paging-builder lifetime regression also
passes with its unlocked mutation failing. Its extractor was narrowed to the
actual function body so subsequent added helpers are not accidentally included.

Full WDK build/sign PASS, candidate0.7.72.1 NOT DEPLOYED:
scratch/build/bc250kmd-0772/package-umd.
SYS SHA256 A9F98BC8875F19F070938F11E65A01B152F541315911C06DE8F599B883229D26.
Post-build source edit corrects only a comment. No lab access/reset/agents/USB.

OPEN: outer CPU_VIRTUAL errors remain ignored and other BuildPagingBuffer helper
failures folded to SUCCESS; physical mapping failures, unsupported operations,
real GPU reset/reentry, OS/GPU ordering and full M9 runtime gates remain open.
