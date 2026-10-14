# amdgpu_wddm_d3d12pso - our own client for the pipeline-state path

This client does what a game does when it loads, and nothing else: it creates pipeline states in one loop, then
draws once with each of them and checks every pixel against arithmetic the CPU repeats. It exists for the owner's
rule that multithreading is tested with our own clients before a game is asked to show it, and it is the measurement
of the asynchronous pipeline compilation of the D3D12 driver (BD-158).

## What it prints, and what each number means

| column | what it holds |
| --- | --- |
| `create` | the wall time of `CreateGraphicsPipelineState` on the calling thread |
| `set` | the wall time of the first `SetPipelineState` of that pipeline, where the driver waits for a compile |
| `gpu` | submit to fence of the draw, for scale |

With the asynchronous path off, `create` holds the whole engine create (the DXIL to SPIR-V conversion and the
pipeline compile) and `set` is zero. With it on, `create` collapses to the copy of the description and whatever the
caller still pays, and the remaining wait appears in `set`. A game's load loop pays both, so the number that must go
down is `caller_us` in the last line, the sum of the two columns, not either column by itself.

The split between the conversion in `amdgpu_wddm_vkd3d.dll` and the shader compile in `amdgpu_wddm_radv.dll` is not
visible from an application: one engine call covers both. Read that split from the driver's own counters, which the
D3D12 shell writes at device destruction, or from ETW.

## The programs

`shaders\d3d12pso.hlsl` is one vertex program and one pixel program. `build.ps1` compiles the pixel program 16 times
with its own `-D ROUNDS=`, 4 to 64 unrolled rounds of exact 32-bit arithmetic, so every variant is a different
program of a different length and the engine has to convert and compile each of them. `--selftest` compares the 16
programs byte for byte and refuses two that are the same: the measurement rests on them being different work.

Pipeline `i` uses variant `i % 16` and state permutation `i / 16` (four rasterizer bits that change the pipeline the
engine makes and cannot change the picture: there is no depth target and culling is off). So:

- every pipeline is a different D3D12 object and a different engine pipeline,
- the conversion of a program happens 16 times at most, the pipeline compile `--count` times. Above `--count 16` the
  client measures the pipeline compiles of programs the engine has already converted, which is also the shape of a
  game's load: many pipelines over fewer programs.

The pixels of a draw depend on the variant alone. The CPU computes the expected word of every pixel with the same
arithmetic (`mix_round`, the mirror of the shader's), the destination holds the complement of every word before the
draw, and all 4096 words are compared. A wrong picture is therefore a failure of this driver, never a difference
from another renderer.

## Build

```
pwsh -NoProfile -File tools\win\d3d12pso\build.ps1 -Kits <workspace>\toolchain\nuget            # x64
pwsh -NoProfile -File tools\win\d3d12pso\build.ps1 -Kits <workspace>\toolchain\nuget -Arch x86  # the WoW64 shell
```

The build runs the CPU-only gates itself: `--help` 0, an unknown argument 2, `--count 0` 2, `--selftest` 0. It
creates no device, so it is a host build like the other clients here. The previous binary stays under `retained\`
by its hash, and `gen\dxc.txt` records the dxc that made the headers.

## Run

```
amdgpu_wddm_d3d12pso [--adapter warp|bc250|hw|index:N] [--count N] [--threads N] [--deadline SECONDS]
                     [--label TEXT] [--quiet] | --selftest | --help
```

`--count` is 1 to 256 (default 32), `--threads` 1 to 64 (default 1, which is what a game's load loop does), and
`--deadline` bounds the whole run (default 150 s, inside the three-minute bound of a lab trial). Exit codes: 0 pass,
2 a bad command line, 3 the environment or the device (no adapter, a refused create, a fence that did not signal,
a removed device), and 4 a check that failed (different pixels, two identical programs).

A pair of runs measures the switch. The per-application switch of the asynchronous path is the `Experiment` value of
the D3D12 shell (`docs\design\per-app-graphics-settings.md`). The token `async-pso-off` turns it off for the named
executable, and the default is on:

```
reg add "HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\amdgpu_wddm_d3d12pso.exe" /v Experiment /t REG_SZ /d async-pso-off /f
amdgpu_wddm_d3d12pso.exe --adapter bc250 --count 64 --label off
reg delete "HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\amdgpu_wddm_d3d12pso.exe" /v Experiment /f
amdgpu_wddm_d3d12pso.exe --adapter bc250 --count 64 --label on
```

`--threads 4` after those two is the second half of the owner's rule: the same creates from several application
threads, where the driver's pool and the callers compete for the same cores.
