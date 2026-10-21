# 3DMark Time Spy: device loss and GPU timeout, rounds A5 to A28 (BD-180, GitHub issue #38)

Unit A, Windows 11 22631, desktop on the GPU route. Every round is one benchmark run of 3DMark Time Spy
on the lab, with the driver files that its own section names. The analysis of these rounds is
[`docs/research/bd180-time-spy-device-loss-and-timeout.md`](../../../docs/research/bd180-time-spy-device-loss-and-timeout.md).
The facts are M911 to M915 in [`docs/facts/d3d.md`](../../../docs/facts/d3d.md) and
[`docs/facts/kmd.md`](../../../docs/facts/kmd.md).

## What is here, and what is not

The files in this directory are transcriptions of counters, hashes and status values out of the round
records of the lab. They are not raw tool output. The raw material of these rounds stays outside this
repository, because it is large or proprietary:

- the kernel memory dumps of each bugcheck, 737 MB to 877 MB each,
- the user-mode driver logs of each run, 27 thousand to 49 thousand lines each,
- the retained command streams, shader bytes and wave snapshots of the diagnostic builds,
- the benchmark's own executable and resource packages.

Each transcription names the round it comes from. A number that no round record states is absent here,
and the research page says so in its own words.

| File | Rounds | What it holds |
|---|---|---|
| `a5-error-codes.txt` | A1, A3, A5 | The error codes that the user-mode driver reported, and the refused pipeline state objects |
| `a6-a8-timeout.txt` | A6, A7, A8 | The bugcheck signature, the fence state of both nodes, the recovery steps and the timeout settings of the operating system |
| `a7-tessellation-controls.txt` | A7 | The twelve probe cases of the native D3D12 client, with their results |
| `a9-a28-diagnostics.txt` | A9 to A28 | What each diagnostic round retained, and the result of each arm |
| `vkroute-2026-10-20.txt` | Vulkan route | The arm with upstream vkd3d-proton, its file hashes and its result |

## Conventions

- A hash of eight characters is the first four bytes of the SHA-256 of that file, as the round record
  gives it.
- A time is UTC.
- "CU" is a compute unit of the GPU. The kernel driver enables them in pairs.
- A round that did not record a value leaves its cell empty.
