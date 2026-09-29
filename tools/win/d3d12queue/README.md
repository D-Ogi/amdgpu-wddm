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

Transport, artifact verification, deployment, STOP acquisition, logs, and process
supervision stay with the caller. `interactive_terminal_observed` means only that
a terminal receipt or session marker was observed. It does not establish GPU
success, process closure, an empty Job, or restoration. Keep the independent
supervisor and its cleanup checks active.

Run the pure host gate with
`python -B -m unittest discover -s tools/win/d3d12queue -p test_drive_planner.py`.
The tests do not load a driver, connect to a target, or launch a process.
