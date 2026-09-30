# Native D3D12 queue contract probe

Build with `build.ps1 -Kits <workspace>/toolchain/nuget -Out <workspace>/scratch/build/d3d12queue`.
Run `amdgpu_wddm_d3d12_queue.exe --warp` for the software positive control or `--lab` for the BC-250 adapter (PCI1002:13fe). The program explicitly loads System32/d3d12.dll. It does not load the vkd3d engine directly or change driver registration.

The sequence creates two direct queues and two fences. Queue A signals done=1; CPU observes completion. Queue B waits on gate=1 and then signals done=2. During a100ms observation, done must stay1 and its event must time out. CPU signals gate=1; done must then reach2 within5s. The device must remain healthy. Any failed assertion produces a nonzero exit. The gate is released even after a failed blocked-state assertion.

Run under the existing bounded-child Job supervisor: the5s waits do not bound driver calls or COM teardown. All lab trials remain at most180s and require exact baseline/rollback and sibling-module admission from the surrounding runner. This client is not permission to deploy an unfinished DDI shell. Output is flushed after each operation so a stopped attempt retains its last boundary.

Host validation,2026-09-28: /W4 /WX build, help and malformed CLI controls pass. WARP completes the sequence, done stays1 during WAIT_TIMEOUT and becomes2 after CPU release; process exits0 and the20s Job supervisor verifies an empty Job. Raw receipt: scratch/m15/queue-client001. No native lab run yet.

This measures API ordering for a no-command-list sequence. It does not establish which native DDI callbacks or KMD contexts implement the ordering. The T0 logging shell must identify those, including the association with hRTCommandQueue. Copy/compute, GPU cross-queue dependencies, residency and FL12_1 require subsequent tests.

## Temporary DX12 registration

`registration.ps1` plans and applies a reversible fourth `UserModeDriverName`
REG_MULTI_SZ entry. It requires an exact three-entry baseline, preserves its
ordering and values, and refuses foreign modifications during restoration.
Persist the plan before installation; hold the caller's mutation lock throughout
all phases. A failed install can have written the value and still needs restoration.
Confirm every writer's process tree is closed before restoring or verifying.

Run `powershell -NoProfile -File registration-test.ps1` before packaging a trial.
The mock-key tests cover JSON plan round-trip, normal and repeated restoration,
post-write failure, foreign mutation, wrong registry kind and tampered plans.
They do not establish that dxgkrnl refreshes its effective DX12 driver name.
The trial must query that name through KMT after installation and after restoration;
a registry readback alone is insufficient. No restart or persistent deployment is
implicit in this helper.

## Interactive experiments

`--interactive <directory> --deadline <seconds>` uses the BC-250 through the
system runtime. Use `--interactive-warp` instead for the same sequence through the WARP software
control. Neither selection falls back to the other adapter. The directory must
already exist and must be new for each run.
The deadline is at most150 seconds; a separate bounded Job must cover the whole
process, driver calls and cleanup. The surrounding lab supervisor reserves time
for restoration within its180-second limit.

Commands are immutable ASCII files, published by `controller.ps1` through a
flushed temporary file and rename. For example:

```powershell
.\controller.ps1 -Directory <directory> -Sequence 1 -Command create-device
```

The file contains `1 create-device` and a newline. The probe writes
`result-000001.json` with the operation result and current state before accepting
the next sequence. The controller refuses overwrites and requires the previous
receipt before publishing the next command.

| Command | Operation |
| --- | --- |
| `create-device` | Load System32 D3D12, select the adapter and create an FL11_0 device |
| `create-queue` | Create one DIRECT queue on that device |
| `copy` | Upload a deterministic4096-byte pattern, execute a buffer copy, wait for the runtime fence and compare every readback byte |
| `status` | Read the current device removal reason |
| `exit` | Finish the session and release objects whose GPU work has retired |
| `abort` | Request cancellation at an operation boundary or inside the bounded fence wait |

`build.ps1 -Sparse` replaces the `copy` operation: a reserved buffer of four tiles is mapped to tiles
2 to 5 of a heap with the queue's `UpdateTileMappings`, a pattern goes UPLOAD, reserved buffer,
READBACK, and all 262144 bytes are compared. Only mapped tiles are written and read. The device must
report tiled resources. The software control passes this variant (2026-09-29, exit 0).

`build.ps1 -RayQuery` replaces the `copy` operation with the scene of engine-ddi's `test-raytracing.cpp`
through `ID3D12Device5` and `ID3D12GraphicsCommandList4`: a bottom level of one triangle and a top level of
one instance are built on one DIRECT list, then the cs_6_5 inline ray query of `rayquery.hlsl`
(`rayquery-program.h`, the same DXIL bytes as the harness fixture) writes 64 words, 1 for a hit and 2 for a
miss, through a root SRV and a root UAV by address. The READBACK buffer starts at a value that is neither;
every word is compared with the pattern computed on the CPU, which must hold both hits and misses. The
trace records the reported raytracing tier, the prebuild sizes and the bottom level's GPU address. Below
raytracing tier 1.1 the operation fails before creating anything. The software control passes this variant
with `-FeatureLevel12_1` (2026-09-29, exit 0, 12 hits).

`build.ps1 -RayPipeline` traces the same scene with `DispatchRays` instead of an inline query. One
`CreateStateObject` call makes a raytracing pipeline from the DXIL library of `raypipeline.hlsl`
(`raypipeline-program.h`, lib_6_3, exports `raygen`, `miss` and `closest`), a triangle hit group, a shader
config (4-byte payload, 8-byte attributes), a pipeline config with recursion depth 1 and the global root
signature of the ray query variant; there is no local root signature, collection or `AddToStateObject`. The
identifiers of raygen, miss and the hit group from `ID3D12StateObjectProperties` fill a shader table in
UPLOAD memory at 64-byte steps; `SetPipelineState1` and `DispatchRays` 8x8 on the DIRECT list write the
same 64 words and the same comparison decides. The closest hit shader writes 1, the miss shader 2, and the
payload starts at 0. Below raytracing tier 1.0 the operation fails before creating anything. The software
control passes this variant with `-FeatureLevel12_1` (2026-09-29, exit 0, 12 hits).

`build.ps1 -RayState` is the create/destroy control of that state object. The `copy` operation reads the
raytracing tier, creates the same global root signature, then twice in turn creates the state object from
`raypipeline-program.h`, queries `ID3D12StateObjectProperties`, takes the raygen, miss and hit group
identifiers (32 bytes each, none all zero, no two equal) and releases the properties and the state object,
tracing the reference counts the runtime returns. It builds no acceleration structure, records no command
list and submits nothing, so `gpu_pending` stays false. Success is traced as `Ray state 2 of 2 state
objects created and released`. The code shares its root signature, state object and identifier steps with
`-RayPipeline` in `interactive-raypipeline.h`. The software control passes this variant with
`-FeatureLevel12_1` (2026-09-29, exit 0).

`build.ps1 -RayCollection` traces the scene of `-RayPipeline` through a pipeline built from a collection.
A `COLLECTION` state object holds the library, the hit group, both configs and the global root signature,
so it is self-contained. The executable `RAYTRACING_PIPELINE` has no library of its own: it holds only
that collection (`NumExports` 0, all of its exports), the global root signature and the pipeline config.
The application's reference to the collection is released, with the count the runtime returns traced,
once the pipeline exists. Identifiers, shader table, `DispatchRays` and the 64-word comparison are those of
`-RayPipeline`; success is traced as `Ray collection 64 of 64 words equal, ...`. The software control
passes this variant with `-FeatureLevel12_1` (2026-09-29, exit 0, 12 hits).

`build.ps1 -RayGrow` traces the scene through a pipeline grown by `AddToStateObject`, which needs raytracing
tier 1.1. The parent state object allows additions (`STATE_OBJECT_CONFIG`) and holds the library of
`raygrow.hlsl`, a triangle hit group whose local root signature (one 32-bit constant, b0 in space 1) is
associated with it by name, both configs and the global root signature. The client takes the parent's
raygen, miss and hit group identifiers, reads the parent's default pipeline stack size and sets it 1024
bytes higher, and reads it back. Through `ID3D12Device7`, the addition then brings the miss shader
`miss_new` of `raygrow-miss.hlsl`; it also allows additions and is valid on its own, with the same configs
and global root signature. The DXR specification says the child starts with the parent's stack size, so
the child's `GetPipelineStackSize`, read before any set on it, must equal the parent's value or the
operation fails. `GetShaderStackSize` of raygen is traced for both. The four identifiers must be nonzero
and pairwise different. The parent's identifiers are copied while it is alive; then the parent loses every
application reference, properties first, and no session slot ever holds it. The trace shows the boundary
in order: `Parent identifiers copied`, `Parent properties released`, `Parent state object released`,
then `SetPipelineState1 child` and `DispatchRays` on a list that only ever bound the child. The reference
counts are traced as observations. The shader table is built from the copied bytes and holds the parent's raygen, two miss records (the parent's miss at index 0, the child's `miss_new` at
index 1) and the parent's hit group record, whose identifier is followed by the local constant 0x00C0FFEE.
The ray generation shader picks the miss index from the column's parity, so the expected words are that
constant for a hit, 2 for a miss in an even column and 3 in an odd one. Success is traced as `Ray grow 64
of 64 words equal, H hits, M2 old misses, M3 new misses, ...`.

The stack size check is strict only on the BC-250 (`--interactive`). The DXR specification says, in
`d3d/Raytracing.md` of microsoft/DirectX-Specs at `5a4139be`, line 3777: "The new state object starts off
with the same [pipeline stack size](#pipeline-stack) setting as the previous." The software adapter was
observed on 2026-09-29 to start the child at 0 against that rule: every stack size it reports is 0 except
the parent's value after the set, so the child answers 0 where the parent reads back 1024. Under
`--interactive-warp` the mismatch is therefore traced with both values and `software adapter: not
decisive`, with S_OK, and the operation continues; under `--interactive` it fails the operation. Nothing
else differs between the two modes. With that, the software control passes this variant with
`-FeatureLevel12_1` (2026-09-29, exit 0, 12 hits, 28 old misses, 24 new misses).

`build.ps1 -GameLoad` replaces the `copy` operation with a game-like load in steps, so that the step at which
a machine stops is known (a game stopped unit A within a second of loading the UMD, during a burst of kernel
paging submissions). The steps have fixed totals of 256 MB, 512 MB, 1 GB and 2 GB (one table in
`interactive-gameload.h`). Each step has two arms with the same total: SMALL, committed DEFAULT buffers of
64 KB created back to back, and LARGE, committed resources of 64 MB, buffers and RGBA8 4096x3072 textures
with all 13 mips in turn. `-GameLoadArm Small|Large|Both` (default `Both`, SMALL first) selects the arms. A
step is skipped with a trace line when `IDXGIAdapter3::QueryVideoMemoryInfo` says the step does not fit the
local budget beside the current usage; an unknown budget is traced and not checked. Every resource is written
through the GPU: a pattern of the run's random seed, the resource's id and the word index goes into an UPLOAD
staging buffer and on with `CopyBufferRegion` or `CopyTextureRegion`, in command lists of at most 1024
copies, each waited with the bounded fence wait. Then every LARGE resource and every 64th SMALL one, plus the
last, is read back and compared row by row. The trace counts created, written, fenced, verified and
mismatched resources apart, as well as bytes touched and bytes verified, allocation failures and
submissions, with the time of each part. An arm's resources stay alive until its verification is done;
then they are released and a fence round trip follows before the next arm. The last phase runs four threads
on the SMALL arm of the 256 MB step, each with its own allocator, list, fence, staging, readback and seed,
all submitting to the one queue; their trace records carry a `thread` field, and the trace writer takes a
lock. Single-thread steps may start until 40 s after the operation began and the threads until 50 s (10 s
and 15 s on the software adapter), never in the last 10 s before the client's deadline; a cut is traced as
`Game load time budget reached at step N`, and what was created is still written and verified. A step, an
arm or the thread phase counts as exercised only once it created a resource. The operation succeeds only if
load actually ran (resources created and a sample verified), every exercised arm verified all its samples
with no mismatch, and no allocation failed. A run in which the time bound or the budget skipped every arm
fails with `800700e8` (`Game load incomplete: no load ran`), whatever the screen did. It ends with `Game load:
steps S of 4, created N, verified M, mismatched X, bytes B, threads T, ..., coverage complete|partial|none,
arms A of P, steps skipped K, ...`; `partial` means some arms or threads were cut or skipped, and SMALL
coverage is always the sampled resources only.
Every milestone (load, step and arm begin and end, every LARGE and every 256th SMALL creation, each
submission before it executes, each fence, each verification, thread begin and end) is appended to
`milestones.log` in the session directory with QPC time, step, arm, thread and counts, through a
write-through handle flushed after each line, so a machine-wide stop leaves the last one on disk.

While the load runs, a render thread shows it as a game would. On the BC-250 it creates a borderless window
over the primary output (the size of adapter output 0) and a flip-model swap chain on the same queue, in the
configuration `-Present` proved (`FLIP_DISCARD`, two buffers, the admitted 8-bit format, `Present(1, 0)`),
and presents one frame per vsync from its own allocators, list and fence. The frame is made of clears only,
so the back buffer format is one constant (`back_buffer_format`) and nothing else depends on it. The top band
shows the step in its colour on the left (blue, teal, orange, violet for 256 MB to 2 GB, white for the
threads) and the arm on the right (light blue SMALL, brown LARGE), and turns green or red for a second at
the end. Below it a white bar moves with every frame, so a frozen picture shows a still bar. Below that is
one tile per written batch: yellow when its fence completed, green when its arm verified clean, red for a
mismatch or a failed allocation. The window, the swap chain and the first Present are milestones (with a
`_begin` line before each), then every 60th Present; every milestone line carries the Present count and the
longest interval between two Presents. A failed Present fails the operation and keeps its error. A frame
executed without a following successful `Signal` (a failed Present or Signal) is never taken as retired on
the strength of an earlier fence value: the render thread tries one covering `Signal` and a bounded wait,
and if that does not prove completion its objects stay for process teardown and the session stays pending. Under `--interactive-warp` no
window is made: the same frames go to an offscreen render target of the same size, paced to 60 per second
and reported as offscreen frames. The load starts after the first frame (or after 10 s without one); the
render thread gets 10 s to end after the load, or the session is left pending for process teardown. That
timeout ends the session: the copy receipt is published, then `session.json` with reason `detached-thread`
(hr `WAIT_TIMEOUT`), and no further command runs (the controller refuses one on a terminal session). The
trace is never closed under the detached thread: the main thread records the end, takes the trace lock and
ends the process with `TerminateProcess` (exit code 3), so no DLL detach code runs after a thread that may be
stuck in a driver call. `build.ps1 -GameLoadRenderHoldMs <ms>` makes a test build whose render thread keeps
tracing that long after its last frame; with 15000 on WARP (SMALL arm, 2026-09-29) the copy failed pending,
the session ended `detached-thread` with exit code 3, the status command was refused and all 253 trace lines
were complete, the last one the session end; the same source without the hold passed its five commands with
exit code 0.

The software control passes this variant with `-FeatureLevel12_1` (2026-09-29, exit 0, coverage complete,
9 of 9 arms, 65596 resources, 8.3 GB written, no mismatch, 369 offscreen frames at 3440x1440, longest
interval 44 ms, 11.5 s). With a 10 s client deadline the time bound skips every arm and the operation fails
with `800700e8`, coverage none.

`build.ps1 -ResetChurn` replaces the `copy` operation with multithreaded command list churn
(`interactive-resetchurn.h`). It makes, without a game, the traffic under which trial 172 lost the device in a
RADV callback during `ResetCommandList` (a buffer object's address freed on one thread and mapped on another), and
checks on the way that every draw reads the constants the CPU gave it. `-ResetChurnThreads` (default 4, 1 to 8)
recording threads each own one allocator and one list. Per batch each thread resets its allocator and its list
and records `-ResetChurnDraws` (default 1024) draws of one pixel, each after `SetGraphicsRoot32BitConstants`
with 32 values and `SetGraphicsRootConstantBufferView` into its UPLOAD ring, so that RADV's upload buffer object
and command stream grow by doubling within the list and free what they grew out of at the next reset. RADV keeps
its largest objects across resets, so a thread replaces its allocator after every `-ResetChurnRenew` lists
(default 2, 0 never) and releases the old one in the middle of the next recording, after a draw chosen from the
run's seed, so that one thread's frees land among the others' resets and maps. The main thread executes each
batch's lists in one `ExecuteCommandLists`, signals a fence and waits for it, then reads
`GetDeviceRemovedReason`; a removal ends the run with `Reset churn device removed at batch B: reason R`. Each
draw's pixel program copies the 32 root constants and the 16 CBV words into its own 192-byte slot of a UAV
buffer (slot index = root constant 0), which the list copies to READBACK; after the fence the main thread
compares every word with what the recording thread wrote. Every word is unique to seed, thread, batch, draw and
word; the UPLOAD ring is mapped once and has two regions used in turn, a region being written only after the
fence of the batch that last read it. The first 16 mismatches are traced as `Reset churn mismatch thread T
batch B draw D root|cbv word W, readback offset O, upload offset U|none, expected E, actual A, actual is ...`,
naming where the actual value came from when this run wrote it in the last four batches. Batches start until
`-ResetChurnSeconds` (default 20, 1 to 140) after the operation began, never in the last 10 s before the
client's deadline, and at most `-ResetChurnBatches` (default 2000) of them; progress is traced every 50
batches. The run ends with `Reset churn: batches N of P, threads T, draws per list D, lists L, draws X, words W,
mismatches M, mismatched draws K, removals R, record failures F, renewals A, resets S, time budget
reached|not reached, E ms, batch ms mean ... max ..., record ..., submit ..., verify ...` and succeeds only when
a batch ran, every word compared equal, no thread failed and the device was not removed. The default of 20 s
fits the lab runner as it is (client deadline 70 s, Drive 65 s, copy command about 17 s after the client
starts); a longer run needs a longer client deadline, watcher budget and Drive first. `resetchurn-test`
checks the value oracle with the same parameters. The software control passes this variant with
`-FeatureLevel12_1` (2026-09-30, exit 0, 333 batches in 20 s, 1332 lists, 1363968 draws, 65470464 words
compared, no mismatch, 664 allocator renewals, batch mean 59 ms).

`controller.ps1 -Abort` can publish `abort.request` while an operation is active.
It does not interrupt a driver callback. The independent Job deadline remains
necessary if a DDI call does not return. Unretired GPU resources are retained
through process termination instead of being released as though execution had
completed.

`trace.jsonl` records flushed API before/after events; each receipt records the
command sequence, HRESULT, elapsed time, device/queue state and copy result.
`session.json` is the terminal command summary. Its creation precedes COM
teardown: it is not proof that the process exited or the Job is empty. Acceptance
requires the copy oracle, process result, independent Job closure and restoration
checks together. A successful `exit` or normal void DDI return alone is insufficient.

A session can be replayed against another exact artifact by submitting the same
commands to a new directory, checking each receipt before proceeding. Preserve
the original files rather than restarting an attempt in place. Commands steer
public API operations; driver callback scopes are never suspended for input.

## Compact trace analysis

Enable `AMDGPU_WDDM_DDI_TRACE=1` in the diagnostic UMD process to add paired named
DDI events and typed format/MSAA observations to stderr. Summarize explicit local
files with:

```powershell
python -B summarize_trace.py --runtime-err runtime.err --trace trace.jsonl
```

The analyzer reports call pairing, returned statuses, durations when a consistent
QPC frequency is available, and the last allowlisted format/MSAA values. Edge
counts are explicitly named; a begin and an end contribute two edges. Duplicate,
unmatched, malformed and untracked records remain visible. Hosted callback IDs
are device-local, so the analyzer reports their counts without assuming a global
pairing. Raw text, handles and resource contents are omitted from its bounded
output. The original trace remains the source of evidence.

Trace lines of the `-ResetChurn` variant count under one `Reset churn` name. Its typed lines (start, progress,
thread end, mismatch, removal, summary) are parsed into named fields under `api.reset_churn`, with at most
`MAX_EXAMPLES` mismatch and failure examples, and a `criterion`: `pass` needs the summary line with status 0,
at least one batch, no mismatch, removal, record failure or other failing line, and no malformed typed line;
`fail` lists what went wrong; anything else is `incomplete` with its reasons.

## Receipt-gated command planner

`drive_planner.py` contains a transport-independent planner for an already started
interactive attempt. It waits for the seeded `create-device` receipt, then plans
`create-queue`, `copy`, `status`, and `exit`. A failed operation or missing positive
state witness selects `exit`. Any receipt reporting pending GPU resources permits
only `exit` or `abort`; their retirement is never assumed.

Import `DriveState` for manual integration, or `drive_loop` for bounded polling:

```python
import time
from drive_planner import drive_loop

result = drive_loop(poll, issue, emit, seconds=60,
                    clock=time.monotonic, sleep=time.sleep,
                    diagnostic=record_local_error)
```

`poll(timeout)` returns a complete snapshot: strict boolean `started`,
`runtime_ready`, `stop`, `cancel`, and `terminal`; a `commands` list of
`{sequence, command}` objects; and a `receipts` list containing the probe's
immutable result objects. Receipts include `schema`, `sequence`, `command`,
`success`, eight-digit hex `hr`, `elapsed_ms`, boolean `state.device` and
`state.queue`, `copy_success`, and `gpu_pending`. Startup may have no commands or
receipts while `runtime_ready` is false. Every previously observed receipt must
remain present and unchanged. Extra receipt fields are discarded.

`issue(sequence, command, timeout)` must enforce its timeout, recheck STOP, invoke
the immutable controller, and validate its acknowledgement. It raises on refusal
or uncertain delivery. The planner consumes each action before calling `issue`
and never retries it. For manual integration, call `state.observe(snapshot)`,
then `state.issuing(*action)` immediately before delivery. Stop on any exception;
a refused or terminal planner must not be reused for a new attempt.

The loop accepts a one-to-65-second host budget. Startup and polling consume that
same budget. In its final two seconds it may replace a planned command with a
typed `abort`, only after a completed receipt proves the next sequence. STOP or
cancellation prevents further positive commands. A callback must honor its supplied
timeout, and logging callbacks must return promptly; this Python loop cannot
interrupt a stalled transport implementation.

The optional keyword `retry_poll_timeout=True` asserts that `poll` is read-only
and may safely finish remotely after its local transport times out. It permits
at most one retry of `subprocess.TimeoutExpired` raised directly by `poll`, over
the entire Drive. It defaults to `False`; existing consumers retain fail-stop
behavior. The same planner, issued-command history and absolute deadline remain
in use. Only a fresh, validated snapshot can authorize the next command; STOP
and cancellation still take precedence. Commands, malformed snapshots, unknown
errors and failures in observation or logging are never retried. A timed-out
command delivery stays consumed.

A sanitized `poll_timeout` event and diagnostic record contain the selected
timeout, measured duration, elapsed and remaining Drive budget, explicit `retry`
boolean and `retry_decision`. Exception messages, commands, stdout and stderr are
not logged. Retry does not extend the client or independent supervisor deadline,
and cannot guarantee normal closure when transport remains unavailable.

Transport, artifact verification, deployment, STOP acquisition, logs, and process
supervision stay with the caller. `interactive_terminal_observed` means only that
a terminal receipt or session marker was observed. It does not establish GPU
success, process closure, an empty Job, or restoration. Keep the independent
supervisor and its cleanup checks active.

Run the pure host gate with
`python -B -m unittest discover -s tools/win/d3d12queue -p test_drive_planner.py`.
The tests do not load a driver, connect to a target, or launch a process.
