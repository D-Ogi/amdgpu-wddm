# M423 - Exact physical disjointness admits fragmented native paging

Unit A,2026-09-24,1000MHz/820mV. Candidate0.7.132.1,
SYS E7AF5A02A3DEDA2CAD25E7D6A789FDA3C2406666D537F583D8ACAB14BDA49652.

## Problem and change

The131 native DMA check treated every physical address between an endpoint's
minimum and maximum page as data backing. Fragmented system allocations have
holes. A separate OS DMA allocation inside a hole therefore forced the physical
capture path despite having no shared bytes. An actual-page-table fixture
reproduces this: old source refuses the valid copy, producing1failure in875856
checks. Its source matches the archived131source; the generated failing fixture
and log are preserved.

The corrected path keeps the inexpensive bounding test, but when it intersects
a DMA span, walks actual endpoint pages to prove disjointness. It allocates no
capture storage and retains true-overlap refusal. This handles fragmented data
on either copy endpoint; all875868checks pass, including true shared-page
controls in both directions. WDK26100build passes. DMA identity, read/write
permissions, CSA/IB separation, typed metadata, root/TLB/fence ordering and
page-table/alias fallbacks remain in force.

## Hardware result

One PnP transition from131, no OS/DWM/ACreset. Existing six startup controls
pass. Initial native transfers2/fills8 and exact disjoint proofs2 replace the
previous startup capture work. Captured plans remain0reserved/0heap.

-64MiB VRAM:3pressure/restore cycles,4whole GPU readbacks, fences64/128/192/256,
 native exit0. Afterward native9transfers/12fills;0capture plans.
-8shader CPU hashes/MLPargmax and both full E14model reference outputs match,
 stories15M7/7 and TinyLlama23/23GPUlayers; worker/native exits0. Current M414RADV
 module witnessed. After models native65transfers/71fills;0capture plans.
-1GiB VRAM:3cycles, nonresident3/resident1 in each;4whole GPU readbacks,
 fences1024/2048/3072/4096 and native exit0. During this stage native transfers
 increase65to100, fills71to90 and native data bytes by exactly10GiB. Capture
 plans stay0. This supersedes131's large-transfer admission gap for this workload.

Final GFX6706/6706,paging30672/30672,zero timeout/refusal/noTDR. Native totals
100transfers/90fills/13118906368bytes,82exact disjoint proofs,0reserved/0heap
capture plans. The preallocated context arena still exists and physical fallback
still exists;0plans in this workload is not removal of all capture resources.

## Observed restoration timing

Same probe binary E9566E49...,1GiB VRAM, same1000MHz/820mV and OS/DWM session:

| Build | Cycle1 | Cycle2 | Cycle3 | Median |
|---|---:|---:|---:|---:|
|131, captured physical path |8000ms |7719ms |7703ms |7719ms |
|132, native path |2328ms |1562ms |1469ms |1562ms |

The median ratio is4.94. These are three cycles in one ordered run per build,
not independent randomized repetitions; physical allocation placement and prior
workload differ. The measurement covers MakeResident plus its paging wait,
not inference throughput, pure SDMA bandwidth or a Windows/Linux comparison.
Raw131comparison data remains in M422evidence; comparison.json records scope.

## Retained state and limits

Final17:21:23,boot11:44:14,DWM4448since13:58:18/responding,66.8C.
M412D3D D438EA42...CPUllvmpipe/LLVM23.1.2 and M414RADV DB886B8D... retained.
Display gates1,Full0consumed,guard0,SDMA diagnostic gates0. Scanout captured
privately/hashonly; no new manual visual acceptance. All workloads/handles
terminal. An early observer mixed formatted objects and omitted its selected
stage strings; its live process/fence observation remains valid. The second
observer uses explicit formatting, and final raw artifacts independently prove
all stage results. No retry or reset was caused by this formatting issue.

Raw logs retain their encoding; only PCI instance/interface identifiers are
redacted. Source/hash/validation/manifest files identify this measurement.
Full M9 remains open: general resource guarantees and the allocating physical
fallback, remaining alias/dependency cases, actual cache/PFN ownership,
cancellation/device-generation/partial-failure retirement, forced CSA preemption,
cold/power startup and matched Windows/Linux performance need acceptance.
