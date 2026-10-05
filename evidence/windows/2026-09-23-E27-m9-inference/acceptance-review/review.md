# M9 acceptance status

Reviewed 2026-09-23 after M311. This is the current acceptance index for the DMA audit and startup plan; historical progress entries do not override these gates. Facts and immutable evidence remain in [facts.md](../facts.md).

## Current hardware and source state

The last verified installation is0.7.101.1/oem75 from M288. Its full start lost SSH observation. The owner subsequently reported a desktop after AC removal, but no persistent startup checkpoint has been recovered. Both configured TCP22 routes still fail during M312. The installed runtime state is unknown; no additional start/reset was attempted.

Development source includes M289-M310; M311 adds actual self-table tests. Latest built development SYS is M310 `AA55F016D6AFCCA7DC4DA07C932266F8E942A48AB38437B9750B21072B0A3AFF`, not deployed. Last default actual-source host routing test is M311,328759checks PASS. This does not establish current Windows behavior.

## Requirement-by-requirement status

| Requirement | Evidence that exists | What still prevents acceptance |
|---|---|---|
| Shared initialization, prepared firmware, explicit phase ownership and unwind (startup1-2) | M233-M240 source/host failure and interrupt controls; M241 automatic admission integration | These controls do not prove current07101 startup or device remove/recreation containment after failed hardware retirement. |
| Engines and queues ready before first supported OS work; no production CLI RUN dependency (startup3-4) | M241 integration; M250/M251 actual early paging/queue delivery; M271 cold-session07100 completion | Current07101 full-start checkpoints missing; latest paging source undeployed. Earliest callbacks on the candidate must execute, not merely build. |
| Cold start, repeated start after workload, partial failure, map/transfer/unmap ownership (startup5) | M271 cold-session CP1..8 and paging; earlier retention tests | Warm reentry remains unresolved. Missing current hardware retirement and mapping-preservation proof; desktop/DWM recovery is insufficient. |
| Physical/system/fragmented bytes, private/DMA ranges, multipass and table ordering | Host integration through M311; earlier M257 real1GiB residency cycles and M283 shader visibility on07100 | Current captured paths require live OS/GPU acceptance. Host byte oracle cannot prove cache visibility or retained PFN lifetime. |
| Aliases | Equal-offset graph cycles; unequal-offset compatible-direction dependencies including repeated source reads; actual self-table host walks | Conflicting-direction unequal intervals and repeated destinations remain unsupported. MS descriptions inspected in M303 do not define arbitrary overlap snapshot semantics; do not present robustness coverage as observed OS demand. |
| Restricted BuildPagingBuffer outcomes and resource readiness | Capacity exhaustion separated from builder errors; exact publication ordering tested | Current capture allocates paged pool during the DDI. Allocation failure and unsupported dependencies propagate internally through the actual outer return path. A guaranteed resource/operation design is missing; never replace these errors with empty success or an infinite insufficient-buffer retry. |
| CPU aperture/cache policy | Separate table storage and prior mapping work; M287 physical-range cache query instrumentation | Current07101 query logs missing. Physical-range cache type alone does not prove actual Windows mapping aliases. All overlapping views and actual coherency require acceptance. |
| Context/capture/queue lifetime and honest completion | Captures owned by context, accepted progress, completion detach, stop cleanup; host submission/retirement tests | Actual OS cancellation, concurrent delivery, PFN ownership and device-generation transitions remain unverified. No timeout-based claim of hardware halt is allowed. |
| Paging pressure and performance | M257 full1GiB GPU readback and M283 shader test on older builds; M274 Windows TinyLlama below Linux | Rerun equivalent workloads on accepted new driver with loader/clock witnesses, complete outputs and hardware fences. Windows superiority is not achieved. |

## Next actions, in order

1. Recover SSH, then read the already prepared `scratch/m9/candidate07101-recover.ps1` breadcrumbs before any hardware mutation. Determine the actual last startup stage and retain raw logs. No blind repeat of the start command.
2. Resolve startup/reentry using those observations, then establish one stable candidate session for real paging acceptance. Honor STOP, clock/voltage/temperature and overlay preflight; avoid routine OS resets.
3. Complete the paging resource/status design and remaining supported dependency shapes. Pool allocation in a live paging callback is a concrete outstanding source condition. A reservation/reuse strategy must preserve simultaneous context ownership and cancellation; choosing an arbitrary cap and retrying forever is not a solution.
4. Validate source-to-physical ownership, cache aliases, table ordering, complete fences and pressure on that candidate. Then profile and compare equivalent Windows/Linux inference workloads.

Host-only work can continue on step3 while remote access is unavailable. No completion claim follows from this index. See [M312 source review](../../evidence/windows/2026-09-23-E27-m9-inference/acceptance-review/source-state.json) for the exact inspected worktree hashes.
