# Queued PTE update / CPU translation counterexample

2026-09-23, HEAD bed764d plus uncommitted candidate0773 source. Host only.
No KMD source change, lab access, deployment, reboot or hardware execution.

Commands:
```
pwsh -NoProfile -File driver/shim/test/run_paging.ps1 -KmdRouting -Out P:/bc-250/scratch/build/queued-pte-ordering
P:/bc-250/scratch/build/queued-pte-ordering/paging_packets.exe --queued-pte-ordering
```
Default regression suite:6221 checks,0 failures. Explicit ordering probe:8 checks,
1 failure, exit1. This is an unresolved failing acceptance probe, NOT a pass.

The generator now optionally routes the actual PagingResolve through the actual
VidMmTranslate wrapper and walker, previously mocked for routing tests. Actual
GfxPagingBuildUpdate and AMD encoding generate one leaf update. Root and three
lower levels are modeled in32KiB host storage. Initial leaf points to page6.
The update queues a WRITE_LINEAR to change it to page7 without a CPU write.
A subsequent resolver before submission yields MC0x100006000, not0x100007000.
Applying only the encoded payload to modeled RAM makes the same resolver return
page7. An actual immediate CPU_VIRTUAL update also resolves page7 correctly.
No GPU interpreter, cache simulation or full OS callback trace is involved.

Interpretation is conditional: constructing a physical transfer after a queued
PTE remap but before that remap executes can capture an obsolete physical page.
This probe does NOT prove Windows issues that sequence for the paging-process
root. MS specifies immediate CPU_VIRTUAL for paging-process initialization, and
advertised mode for other updates. Need distinguish initialization from later
updates and establish actual scheduling/dependency behavior. The production
builder records physical addresses at construction, so later GPU PTE execution
cannot repair an already encoded stale transfer address.

References: enriched ref/ddi-display/d3dkmddi.md WDK26100, original MS DDI repo
7515063cea4c9e98db6a92986c5b4ddb0463fd16, UPDATEPAGETABLE.UpdateMode and
TRANSFER_VIRTUAL.SourceAddress/DestinationAddress. The latter use paging-process
virtual addresses; initialization specifically uses CPU_VIRTUAL/pDmaBuffer=NULL.

Next acceptance requirement: prove that every resolved page reflects all required
preceding PTE updates, using documented guarantees plus an OS trace, or change
construction to preserve the correct ordered translation. Do not restore eager
CPU writes to live GPU_PHYSICAL tables merely to make this test pass.
