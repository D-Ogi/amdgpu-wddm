# M16 HIP step 3: threads and application records

This page indexes the historical step-3 trials. It does not describe the current release's HIP acceptance.
The interface and submission contract are in the [route-B design](../design/m16-hip-route-b.md).

## Acceptance scope

Part 3A checks progress by other threads during a fence wait and safe stream/event destruction.
It also checks whether the runtime adds excessive delay against serialised execution on the same device.
The corrected client permits a parallel-to-serialised ratio up to 1.1.
It does not require fourfold acceleration from four threads.
One submission context and barriers between dispatches can serialize GPU work while host waits overlap.

Part 3B checks BLAS results, benchmark execution, generated text against Vulkan, and the variant without inline assembly.
A successful vector or GEMM control does not establish correctness of the application's other kernels.
Likewise, device enumeration does not establish successful dispatch or correct text.

## Records

| Trial | Evidence | Facts and interpretation |
|---|---|---|
| First step-3 attempt | [2026-10-09 record](../../evidence/m16/step3b-2026-10-09/README.md) and its [correction](../../evidence/m16/step3b-2026-10-09/CORRECTION-2026-10-10.md) | [M858](../facts/icd.md#m858), [M859](../facts/icd.md#m859), [M861](../facts/icd.md#m861) distinguish the GEMM control, application refusal and thread-client criterion. |
| Offline dispatch-packet investigation | [Preserved controls and scan](../../evidence/m16/bd110-dispatch-packet-2026-10-09/README.md) | [M860](../facts/icd.md#m860) bounds what the mock and metadata scan prove. |
| Repeat with the packet change and corrected client | [Later trial](../../evidence/m16/step3b-rerun-2026-10-09/README.md) | [M861](../facts/icd.md#m861) records the thread passes. [M859](../facts/icd.md#m859) and [M860](../facts/icd.md#m860) retain the application refusals. |

The repeated trial retains each wrapper console beside the child's output.
That permits checks of commands, logged environment assignments, exits and observed duration.
The first record lacks some of those witnesses. Its correction remains part of the record.

## What the repeats resolve

The corrected client passes the thread criterion on the tested runtime.
That closes the historical criterion question, not acceptance of every thread count or multiple processes.
The application logs distinguish a metadata-note refusal from a scratch-support refusal.
The Vulkan reference completes, while the HIP side produces no text in this trial.
The result therefore does not satisfy the cross-backend text criterion.

Later runtime changes require their own artifact pins and results.
The [dispatch-cost page](m16-hip-dispatch-cost.md) explains why historical timings cannot substitute for a current application profile.
