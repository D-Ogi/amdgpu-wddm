# E57: the ray-tracing CTS list with and without `RADV_PERFTEST=rtwave64` on unit A (M795)

Unit A, Windows 11, 2026-10-01 16:44-16:46 UTC. The same 140-case ray-tracing list ran twice through the CTS
batch runner, one `deqp-vk` process per run. The first run used the default RADV settings. The second run added
`RADV_PERFTEST=rtwave64`. Nothing else changed between the two runs.

| item | identity |
|---|---|
| CTS binary | `deqp-vk` Release from `vulkan-cts-1.4.6.2` (commit `f6a29701220f34dd1407513bfe80d74ca7b392ce`), SHA-256 `E3DABB44...` |
| case list | `batches/rt-k97.txt`, 140 names, SHA-256 `EEBB4343...` |
| ICD | `amdgpu_wddm_radv.dll` 2A13235D, the registered ICD, the same file as in M773 |
| route | "direct": `--deqp-vk-library-path` points at `cts-direct.dll`, which loads the ICD copy next to it. This route skips the Vulkan loader |
| environment, both runs | `RADV_EXPERIMENTAL=sparse` |
| environment, second run only | `RADV_PERFTEST=rtwave64` |
| device the CTS reports | `AMD BC-250 (RADV GFX1013)` |
| host | BC250-A |
| bound per run | 170 s wall clock, thermal stop at 87 C |

## Result

| run | Pass | Fail | NotRun | `deqp-vk` exit | elapsed |
|---|---|---|---|---|---|
| `rt-k97-base` (default) | 137 | 0 | 3 | 0 | 28.3 s |
| `rt-k97-wave64` | 136 | 1 | 3 | 1 | 31.3 s |

One case changes between the runs: `dEQP-VK.ray_query.misc.dynamic_indexing`. It passes with the default
settings in 11.1 s. Under `rtwave64` it fails in 15.8 s and reports:

```
Unexpected value found at position 0 in the output buffer: expected 1 but found 0 at vktRayQueryMiscTests.cpp:394
```

The other 136 executed cases pass in both runs. The shader of the failing case compiles and links in both runs
(`attempt-01.qpa`, `CompileStatus="OK"` and `LinkStatus="OK"`), so the result is a wrong value in the output
buffer, not a compiler error the CTS caught.

## The three NotRun names

`deqp-vk` executed 137 of the 140 names. The base run printed `Passed: 137/137 (100.0%)` and `DONE!`. The runner
records the three names that `deqp-vk` never began as `NotRun`:

- `dEQP-VK.ray_query.misc.dynamic_indexing_inbounds`
- `dEQP-VK.ray_query.misc.preserve_flip_facing`
- `dEQP-VK.ray_query.misc.shared_memory_consistency`

A string search of the pinned `deqp-vk` build (SHA-256 `E3DABB44...`) finds `dynamic_indexing_use_first` but
none of those three names. They are not cases in this build, so the three rows say nothing about the driver.
Both runs have the same three.

## Thermal state

Tctl was 67.1 C at the start of the base run and 67.4 C at the end of the `rtwave64` run. Each run stayed far
inside the 170 s runner bound and inside the three-minute lab limit.

## What this does not show

- One run per setting. A single wrong result is not excluded by a repeat.
- No cause. This evidence holds no shader disassembly, no ISA dump and no ACO output for the failing case.
- The KMD log ring was not read next to these two runs, so a VM fault is neither shown nor excluded.
- `RADV_PERFTEST` is opt-in. The runner removes it from the child environment and sets it only when it is
  asked for, so the base run proves the default path and the second run proves the opt-in path. The effect of
  `rtwave64` on ray-tracing performance was not measured here, because the base run was already a pass.

## Files

One directory per run, raw runner and `deqp-vk` output, unedited. The two ICD copies (`amdgpu_wddm_radv.dll`
and `cts-direct.dll`, about 22 MB per run) stay outside the repository.

| file | base SHA-256 | wave64 SHA-256 |
|---|---|---|
| `summary.txt` | `389F7EDA...` | `5CCB3BE0...` |
| `summary.json` | `D5DD4216...` | `9232390E...` |
| `config.json` | `54B6610A...` | `3CD35C6C...` |
| `cases.tsv` | `C6794874...` | `C8FABA84...` |
| `attempt-01.json` | `E5272BFB...` | `1A5F482D...` |
| `attempt-01.caselist.txt` | `26BAB47D...` | `26BAB47D...` (same file) |
| `attempt-01.stdout.txt` | `11850343...` | `B1B649C3...` |
| `attempt-01.stderr.txt` | `66926A6E...` | `2067E847...` |
| `attempt-01.qpa` | `6A0E37DB...` | `C7100B8E...` |
