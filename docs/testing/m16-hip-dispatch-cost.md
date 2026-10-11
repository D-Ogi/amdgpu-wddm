# M16 HIP dispatch-cost records

This page indexes the historical performance arms and their corrected interpretation.
It does not prescribe a new lab run or report current application performance.

## Question and controls

The trials compare one submission per dispatch with batching, barrier policies and a compute-state cache.
Vector addition before and after the series checks that the tested runtime executes correct GPU work.
A dependent kernel chain checks results for each policy.
The repeated unbatched arm provides a drift control.

The plan's early prediction for the deepest batch did not match the measured command size.
[M856](../facts/icd.md#m856) records that result and the limits that affected batch depth.
The prediction is not a requirement that later measurements must satisfy.

## Retained records

| Series | Evidence | Facts |
|---|---|---|
| Initial batching and barrier arms | [First series](../../evidence/m16/perf-2026-10-09/README.md), [raw measurements](../../evidence/m16/perf-2026-10-09/measurements.jsonl) | [M853](../facts/icd.md#m853) through [M857](../facts/icd.md#m857) |
| Defaults, state-cache and copy controls | [Repeated series](../../evidence/m16/perf-rerun-2026-10-09/README.md), [raw measurements](../../evidence/m16/perf-rerun-2026-10-09/measurements.jsonl) | [M862](../facts/icd.md#m862), [M863](../facts/icd.md#m863), [M864](../facts/icd.md#m864) |
| Audit of timing and evidence scope | [Correction](../../evidence/m16/perf-rerun-2026-10-09/CORRECTION-2026-10-10.md) | The current facts incorporate this narrower interpretation. |

The existing records already preserve these measurements. This page does not duplicate or replace their raw files.

## Timing boundary

The launch loop measures elapsed time, including waits for kernel arguments and command slots.
It does not separate processor-active time from time blocked by GPU progress.
End-to-end chain time includes completion of dependent work.
Neither measure is an isolated shader time or a direct CPU cost per application token.

The source performs a barrier between dispatches on this path.
Host threads can overlap waits without overlapping their GPU dispatches.
The [thread record](m16-hip-step3.md) preserves that distinction.

An illustrative kernel count multiplied by these timings remains a modelled scale estimate.
It does not establish an application's kernel count, submission count or bottleneck.
Those require that application's counters and a trace on the tested runtime.

## Remaining limits

The copy difference from the first build did not reproduce on the later build.
[M864](../facts/icd.md#m864) records that outcome without assigning a cause.
The old performance plan's pending rerun is not evidence that a newer runtime has the same timings.
No new measurement accompanies this documentation change.
