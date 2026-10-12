# HIP allocation cap on unit A

The consumer cap lets Gemma 12B load and complete inference at UMA 8192 MiB.
Ten allocation controls identify the refusal boundary. Six failures have matching ETW execution witnesses.
This change does not support arbitrary contiguous HIP allocations above that boundary.

PROVENANCE: llama.cpp, MIT, base b86d2f07542b29ab099aed34fd6b6d1b2fd4b81c with the recorded HIP patches.

## Allocation contract and controls

The measured Windows images are 10.0.22621.6199. Their hashes and PDB identities are in [os-gate.json](os-gate.json).
Offline decoding identifies a size check in `dxgmms2!VIDMM_GLOBAL::CreateOneAllocation`, RVA `0xa9632`.
It refuses a rounded size above `0xffff0000` unless internal allocation bit `0x400` is set.
The bit producer leads to standard allocation type 5, VGPU. This is not a CPU visibility flag.

The local contract is WDK 10.0.26100.0, `d3dkmdt.h`, `D3DKMDT_STANDARDALLOCATION_VGPU`.
The [public allocation type definition](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ne-d3dkmdt-_d3dkmdt_standardallocation_type) describes that virtual GPU type.
The private size predicate is a finding for these exact images, not a public limit for every Windows build.

Layer 1 rounds allocations to 256 KiB. Its largest admissible aligned request is therefore `0xfffc0000` bytes.
The controls use the same layer 1 allocation path, with GPU submission disabled.
Each process allocates once, optionally maps and unmaps, then releases its allocation and device.
No control reads or writes the allocated memory.

| Requested bytes | Device allocation | Mappable allocation |
|---:|---|---|
| 65,536 | Success | Success, map and unmap |
| 4,294,705,152 | Success | Success, map and unmap |
| 4,294,705,153 | C000000D | C000000D |
| 4,294,967,296 | C000000D | C000000D |
| 6,960,120,064 | C000000D | C000000D |

[admission-controls.json](admission-controls.json) retains the probe output, PID, timestamps and closure for each control.
[etw-size-refusals.txt](etw-size-refusals.txt) retains six matching DxgKrnl event 494 records.
Each has the size-refusal message and EventData1 equal to 3602, matching the decoded branch.
These controls run in session 0. They do not test presentation.

The measured cause differs from K84's CPU-visible allocation restriction and the earlier shared-allocation checks.
The previous Vulkan capacity result did not establish admission of one large HIP allocation.
Changing allocation visibility does not bypass the measured size gate.

## Candidate and correctness

The patch supplies the ordinary buffer type's `get_max_size` callback.
The existing tensor planner splits at tensor boundaries. An allocation guard also rejects oversized fallback requests.
Allocation flags and preferred placement remain unchanged. A single oversized tensor still fails explicitly.
Internal pools and multi-device split buffers remain outside this change.

Backend FB0E315E uses the corrected HIP headers from M868 and the new consumer cap.
[artifact-pins.json](artifact-pins.json) records the exact artifacts and model hash.
Host controls compile the actual planner, callback and allocation function in ordinary and opt-in modes.
A guard-deleted mutant must compile and fail assertions. No host GPU execution is needed.

HIP and upstream Vulkan produce the same 16-token raw completion, `0111111111111111`, for the same greedy prompt.
Strict CPU matches its first two tokens. Its complete two-token output precedes a bounded cleanup timeout.
This is not a clean CPU trial pass or a general certification of model output.

TinyLlama's strict CPU control completes and matches the earlier corrected HIP 16-token output.
The earlier `-ngl 0` reference alone did not exclude GPU operation offload.
The new control also uses `--device none --no-op-offload --no-kv-offload`.

## Performance measurements

One repetition per arm, same model and dynamic clock policy:

| Backend and option | pp512 tokens/s | tg128 tokens/s | Supervisor |
|---|---:|---:|---|
| Vulkan, FA on | 256.38 | 37.64 | Pass |
| HIP, FA on, ordinary client | 69.03 | 2.08 | Workload bound reached |
| HIP, FA on, profiling client | 69.17 | 2.04 | Pass |
| HIP, FA off, profiling client | 209.59 | 19.20 | Pass |

The profiled A/B changes only the FA option. DPM responds dynamically, so clocks and temperatures differ.
Generation samples show 1000 MHz with FA on, and 1500 then 1200 MHz with FA off.
The FA-off arm reaches a sampled 87.6 C and later cools. These are not measurements at a constant clock.

The HIP build disables GPU Flash Attention. Forced FA remains enabled in the graph and creates 98 graph splits.
FA off reduces that count to two. Source routes unsupported operations to another supporting backend.
The retained log does not identify every node's assigned backend.
Vulkan uses a newer llama.cpp revision, so its rate is a stack contrast, not an isolated driver comparison.

All 128 measured FA-on generation steps issue 1048 dispatches and 49 non-fast waits each.
FA off issues 1203 dispatches and still 49 non-fast waits per step.
Thus neither the wait count nor a wait per dispatch explains the measured rate difference alone.
The cap and two model buffers are unchanged between these arms.

The profiling client reads existing process counters around decode and preserves the original synchronization calls.
Its intervals are CPU wall times, not GPU kernel durations. [trials.json](trials.json) includes thermal samples and outcomes.
The two profile JSONL files retain all measured intervals and warmups.
Per-kernel GPU timing remains unmeasured.

## Placement and closure limits

Twenty-five successful budget samples show 7,190,900,736 bytes requested in local memory and zero demotion counters.
The process also uses 845,393,920 bytes of non-local memory.
Paging records identify model-sized allocations of 4,289,200,128 and 2,671,247,360 bytes.
Their initial mappings are absent. Process totals cannot prove each allocation's complete physical placement.
The paging capture has a 94-record boundary gap and an interrupted final footer.

All final health gates pass with no new GPU fault, timeout, TDR or bugcheck.
The installed KMD, runtime slots, UMA and policy remain unchanged. No owned workload or trial task remains running.
The lab was returned to the integrator after collection.

An initial scheduled launch timed out before the allocation probe ran.
Two early direct controls lacked a reliable process exit receipt and were repeated.
An overly broad ETW reader reached its host bound and was closed before the filtered controls.
Those attempts do not count as positive controls. The failed consumer arms remain in the trial record.

Raw ETL, OS images, weights and full internal mapping logs remain outside the repository.
[redaction.json](redaction.json) lists the selected records and redactions.
