# gpuload: a sustained, self-checking GPU compute load

One C file over core Vulkan 1.1 (`vulkan-1.dll` loaded at run time) and one compute shader. It keeps the GPU busy
for `--seconds` with batches of one dispatch each, `--inflight` deep on one queue, so the GPU does not wait for the
CPU between them. It exists for clock governor trials: the DPM trial kit's load step (DEFECTS BD-050) ran `vkcompute`
back to back, which is mostly process start-up and device creation, and left the governor's GRBM busy at 0-7 %.

- **Kernel** (`shaders/spin.comp`): two dependent uint32 chains of `iters` steps per invocation and one store. ALU
  work only, no memory traffic to speak of.
- **Batch length in time**: each batch is timed by a top-of-pipe and a bottom-of-pipe timestamp. A calibration
  phase (one batch at a time) finds the iteration count for `--batch-ms`, then every retired batch moves it halfway
  toward the count its measured rate asks for. The clock rises under the load, so the count rises with it.
- **Every batch is checked**: 8 of its invocations (one per eighth of the grid, moving with the batch) against the
  CPU reference `GlExpected`, with a new seed per batch, so a slot still holding an older batch's words fails.
  A run that reports load is a run in which the GPU did the work.
- **Busy self-report**: the share of GPU time with a batch in flight, from the timestamps alone (overlaps never
  counted twice, gaps counted as idle), once a second and for the whole run. This is the load's own view; a clock
  governor sampling `GRBM_STATUS.GUI_ACTIVE` also sees every other client.
- **Headless**: no window, no swap chain; runs from an SSH session in session 0. The graphics-and-compute queue
  family is preferred (the ring the games and the desktop use).

```
pwsh -File build.ps1 [-Root <BC250_ROOT>]     -> <BC250_ROOT>\scratch\build\gpuload\gpuload.exe
gpuload [--seconds S] [--batch-ms M] [--inflight K] [--groups G] [--device N] [--stop-file PATH] [--negative-control]
gpuload --format-sample
```

Defaults: S 20 (1..600), M 20 (2..200), K 2 (1..4), G 1024 groups of 256 invocations (1..65535). The build runs the
host test (`gpuload_host_test.c`: reference vectors from an independent implementation, check coverage, busy
accounting, the work controller under a clock that doubles, the report lines), a usage and a range check, and
`--format-sample`. Run the build from the workspace (`BC250_ROOT` or `-Root`) when the repository is a worktree.

## Contract

Exit codes: **0** ran for `--seconds`, every check matched; **1** a check failed (`--negative-control` expects one
iteration more than the GPU ran, so it must end here); **2** usage, or the stop file existed before the start;
**3** Vulkan or system error, including device loss and a fence wait over 5 s; **4** the stop file appeared, the run
drained the queue and ended early, every check so far matched.

Once the device exists, the last stdout line is the result line, also on exit 3. Every line is flushed as written,
so a controlling script can follow the run:

```
gpuload t=3.0 batches=151 iters=48211 batch_ms=19.87 busy_pct=99.4
gpuload result=ok stop=duration seconds=30.0 batches=1502 checked=1502 mismatches=0 busy_pct=99.1 busy_min_1s_pct=97.8 gpu_ms=29811.5 iters=51034
```

`result` is `ok`, `FAILED` (mismatch), `stopped` (stop file) or `error`; `stop` is `duration`, `stop-file`,
`mismatch`, `fence-timeout`, `device-lost` or `vulkan`. `batch_ms` is the mean new GPU time per batch in that second,
`busy_min_1s_pct` the lowest whole-second share. Calibration batches are checked but not counted. The DPM kit parses
these lines with `ConvertFrom-GpuLoadReport` (`scratch\dpm\dpm-trial\dpm-lib.ps1`, outside this repository); its test
reads `--format-sample`, which prints one line of each kind from the code above.

## Positive and negative control

On the development PC (RTX 4090, 2026-10-02): `--seconds 5` calibrated to 562811 iterations for 20 ms, 251 batches,
every check matched, busy 100.0 % in every second, exit 0; `--negative-control` failed all 8 checks of the first
batch, exit 1; a stop file created 3 s into a 30 s run ended it 73 ms later with exit 4; a stop file present at the
start, exit 2.

Unit A: the DPM trial's load step, `scratch\dpm\dpm-trial` (`dpm-step.ps1 -LoadKind sustained`).
