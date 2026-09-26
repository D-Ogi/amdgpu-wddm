# O2 - Adaptive CPU, SDMA and GPU execution

Status: accepted research direction,2026-09-24, requested by the owner.
No implementation or speedup claim. This is optional O2 in the
[main roadmap](../00-goal-and-roadmap.md), not a new prerequisite for M9.

## Decision and realistic scope

Investigate adaptation for operations whose semantics and implementations the
UMD controls. Begin with avoiding redundant copies and selecting an execution
engine for known buffer operations. An application/runtime integration can
later expose coarse compute tasks with validated CPU and GPU implementations.
Transparent migration of arbitrary game shaders or game logic is out of scope
for the first prototype. The driver does not own the game's CPU task graph.

BC-250 has a shared GDDR6 memory pool; UMA does not mean one physical chip,
one address space, identical CPU/GPU bandwidth, or automatically coherent
cached mappings. The current Windows model still has a VRAM carveout, GTT,
page tables and separate allocation/cache policies. Existing M9 acceptance
explicitly leaves complete cache-alias/PFN ownership proof open.
Microsoft distinguishes UMA from CacheCoherentUMA and cautions that UMA alone
does not guarantee every GPU resource can be freely accessed by the CPU.
[Architecture1](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_feature_data_architecture1).

The engineering opportunity is to reduce execution cost on shared storage,
including copies that can legally disappear. Moving GPU work to a saturated
CPU can worsen a CPU-limited game. CPU and GPU may also compete for memory
bandwidth and power. Conversely, a small CPU-accessible operation might finish
before a GPU submission can start. These are hypotheses to measure on this
board, not conclusions from UMA or utilization percentages alone.

## Candidate operations

| Operation | Initial assessment | Boundary |
|---|---|---|
| Upload/readback storage choice and eliminating redundant copies | Highest priority | Preserve residency, cache attributes, API visibility and resource lifetime; shared physical storage does not automatically make two API resources aliases. |
| Linear buffer copies and fills | Good bounded CPU/SDMA/GPU comparison | Only CPU-accessible, appropriately cached mappings with known ownership. Include waiting, mapping and cache costs. Do not execute a recorded command early. |
| Known format conversion, packing or driver meta operations | Later selective prototype | Require matching layout and validated implementations. GPU tiling/compression metadata can make a CPU path expensive. |
| Coarse compute tasks in a cooperating runtime | Promising separate integration | Runtime knows the dependency graph and can offer CPU/GPU versions; include result placement and the following consumer. |
| Arbitrary game draws or SPIR-V shaders on CPU | Defer | Requires a compatible CPU backend plus textures, derivatives/subgroups, atomics, precision, queries and synchronization semantics. Having llvmpipe and RADV available does not provide transparent interchangeability. |
| Game simulation, AI or physics absent from submitted GPU work | Application responsibility | A graphics driver cannot infer and relocate arbitrary CPU application code. |

## Architecture to evaluate

Keep execution selection in the UMD or cooperating runtime, where operation
semantics and resource properties are available. The KMD continues to enforce
VidMm/VidSch ownership, ordering and honest completion. Never turn a paging or
DPC callback into an unbounded CPU compute worker. Do not bypass OS scheduling
to obtain a favorable benchmark.

Use a small cost model before considering machine learning:

- CPU cost: producer wait, mapping/cache work, execution, completion publication
  and delay imposed on application CPU threads.
- SDMA/GPU cost: queue wait, submission, transfer/dispatch, dependencies/cache
  work and the next consumer's wait.
- Include data size/layout, cacheability, recent queue latency, CPU headroom,
  clocks and memory contention. Utilization alone is not a decision rule.

Calibrate on private scratch data. Never replay a live application command
for profiling: atomics, writes, queries and external side effects are observable.
Use bounded sampling of normal work, confidence margins and hysteresis so
small timing changes do not switch paths every frame. Cache profiles by
operation shape, resource properties and exact driver/compiler version;
expire them after relevant changes. Log sizes/timings/path decisions, not
application data. Provide forced CPU/SDMA/GPU modes for controlled comparisons.

CPU execution must occupy the same legal position in the submission dependency
graph, and signal completion only after its data is visible. Check semaphore,
fence, resource-layout and timestamp/query semantics. Vulkan explicitly separates
memory visibility from merely sharing storage; see
[Synchronization and cache control](https://docs.vulkan.org/spec/latest/chapters/synchronization.html).

## Research gates

| Gate | Deliverable | Acceptance |
|---|---|---|
| O2.1 | Map the legal shared-memory paths and benchmark CPU/SDMA/GPU buffer operations | Compare equivalent complete operations over small-to-large sizes, cached/WC mappings, CPU/GPU producers and consumers, idle and contended load. Preserve byte oracles and actual fence completion. Establish CPU access bandwidth/cache behavior on unit A. |
| O2.2 | Optional UMD selector for one or two proven operations | Compare against each forced path and the best fixed policy on a predefined mixed workload. A/B order, warmup, repetitions and limits declared before the run. Demonstrate repeatable end-to-end benefit beyond measurement noise, acceptable P95/P99 latency and bounded CPU/profiling cost. Otherwise retain a static policy. |
| O2.3 | Decide whether to expand into runtime-level compute adaptation | Requires a successful O2.2 and an explicit cooperating runtime/task class. Game claims require a real GPU graphics baseline from M10/M13 plus application frame-time measurements. |

Report end-to-end latency, P50/P95/P99 frame times where applicable, CPU time,
queue waits and content correctness. Keep matched clocks and memory pressure.
Whole-lab plug power is supporting telemetry; its current scales/update age
are not a calibrated per-kernel energy meter. A kernel microbenchmark win alone
cannot justify promoting a policy that delays the application's critical path.

Priority: correctness and a working native GPU path first. O2.1 can accompany
memory/performance work after its cache and ownership prerequisites are proven.
Sparse residency is a separate M12.1 capability and does not automatically
provide CPU/GPU load balancing.
