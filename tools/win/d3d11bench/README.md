# d3d11bench

The workload behind the M14 performance bound: on the same workload, settings, clocks and builds, the system
D3D11 path (Microsoft runtime, the project's UMD, the DXVK engine behind it) may be at most 5 % slower than
per-application DXVK. One executable measures one path per run. Which path it takes depends only on what sits
next to it: DXVK's `d3d11.dll` and `dxgi.dll` in the executable's directory give the per-application path,
nothing there gives the system one. `compare.py` holds the two sides against the bound.

## Build

```
pwsh tools\win\d3d11bench\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
```

The build runs `test_compare.py` first and fails on a failing case, keeps the previous executable under
`retained\` by its hash, compiles with `/W4 /WX`, checks `--help` and prints the SHA-256 that a lab runner pins.
Output: `<BC250_ROOT>\scratch\build\d3d11bench\d3d11bench.exe`.

## Scenes

| Scene | Bound by | Per frame | Gated metric |
|---|---|---|---|
| `draws` | application thread | 2000 draws (`--draws`), each with a `Map(WRITE_DISCARD)` of a 32-byte constant buffer, four texture switches per frame | `frame_ms` |
| `fill` | GPU | 8 blended full-screen layers (`--layers`), each sampling a 512x512 mipmapped texture four times, plus an ALU loop | `frame_ms` |
| `shaders` | shader and pipeline creation | 64 pixel shader variants (`--shaders`) created and drawn once each onto a 256x256 target | `total_ms` |

Frame scenes render 30 warm-up frames (`--warmup`), then 300 measured ones (`--frames`), and read back the last
frame for an FNV-1a checksum. Every scene's output is deterministic, so a checksum mismatch between the paths is a
correctness failure, not noise. Reference values from the development PC (RTX 4090), the same on native D3D11 and
on DXVK over NVIDIA's Vulkan driver, default settings, offscreen: draws `5da11354d23bdd01`, fill
`cd34afa3290da370`, shaders `9ce05937b797b3d3`. They are not yet measured on RADV, and texture filtering may
legitimately differ between vendors: a mismatch across GPUs is a lead, a mismatch between the two paths on one GPU
is a failure.

To follow a lead, run both sides with `--dump DIR`: each scene's checksummed image lands in `DIR\<scene>.pam`
(PAM, RGBA, top row first). Then

```
python tools\win\d3d11bench\imgdiff.py DIR_A DIR_B [--tolerance N]
```

prints, per scene, how many pixels differ, the largest difference per channel, the first pixel beyond the
tolerance and both images' checksums (equal to the `checksum` in the run's JSON, which ties a dump to its run);
`--diff OUT.pam` on two files writes the difference image. A CPU rasterizer and a GPU may round filtering
weights and `sin` differently, so the `fill` and `shaders` scenes can differ by a step or two with both
right, while a wrong draw shows as large differences in whole regions. A tolerance is a judgement recorded with
the result; it never replaces the same-GPU checksum match of the bound.

Modes:

- `--mode window` renders into a 1280x720 (`--size`) window through a two-buffer `FLIP_DISCARD` swap chain with
  `Present(0, 0)`. This is the mode for the bound, because it includes the present path. It needs an interactive
  session with DWM, so on the lab it runs as an interactive scheduled task, never over SSH in session 0.
- `--mode offscreen` renders into a texture and paces itself like a swap chain: at most three frames in flight,
  tracked with event queries. It runs anywhere, including session 0, and is the mode for tool checks.

## Output

A summary on standard output (`SCENE ...` lines, then `PASS` or `FAIL`) and one JSON object, format 1, on
standard output or in `--out FILE` (written atomically):

- `result` (`measured` or `failed`) and `exit`;
- the settings: `mode`, `width`, `height`, `feature_level`, `frame_latency` and every scene's parameters;
- `adapter`: vendor, device and description as DXGI reports them;
- `d3d11`: `app-local` or `system`, from the path of the `d3d11.dll` the process actually loaded;
- `modules`: path and size of every loaded graphics module (D3D runtime, DXGI, Vulkan loader, the project's
  `bc250*` modules, anything from the driver store, every Vulkan driver);
- `icds`: path and SHA-256 of every loaded Vulkan driver, i.e. every module that exports
  `vk_icdGetInstanceProcAddr`;
- `environment`: every `DXVK_*`, `VK_*`, `MESA_*`, `RADV_*`, `ACO_*` and `BC250_*` variable;
- `dxvk_conf`: whether a `dxvk.conf` exists next to the executable or in the working directory;
- per scene: statistics (count, median, p5, p95, mean, min, max) of `frame_ms` (start of one frame to the start
  of the next), `record_ms` (the application's D3D11 calls), `present_ms` (`Present`, or `End` and `Flush` of
  the throttle query offscreen) and `gpu_ms` (timestamp queries, read without flushing), plus the checksum.
  The shaders scene reports `create_ms` and `draw_ms` per variant and `total_ms`, which includes the wait for the
  GPU.

Exit codes: 0 measured, 1 API failure, 2 bad arguments, 3 deadline, 4 device removed. `--deadline` (default 120 s,
at most 170 s) keeps every run inside the lab's three-minute limit.

## Protocol for the bound

1. Same unit, same clocks (the lab operating point), same build of the engine and of per-application DXVK from
   the same DXVK revision, and the same ICD. Record the artifact hashes next to the results. The two paths find
   the ICD differently: per-application DXVK through the Vulkan loader, which uses the registered driver and, in
   an elevated process, ignores `VK_DRIVER_FILES` and `VK_ICD_FILENAMES`; the system path through the UMD, which
   loads its own `bc250radv.dll`. Either register the same file for the loader or run the per-application side
   unelevated with `VK_DRIVER_FILES`; `compare.py` compares the `icds` hashes and refuses runs that differ.
2. Same configuration on both paths: identical `DXVK_*`, Vulkan loader and Mesa variables, no `dxvk.conf` unless
   both sides have the same one. `compare.py` refuses runs whose recorded environment differs.
3. Caches: the engine runs without DXVK's shader cache (it writes no cache file), while per-application DXVK
   keeps one by default. Set `DXVK_SHADER_CACHE=0` on both sides for the bound. The shaders scene is too small
   to price the missing cache. On the development PC, 64 variants showed no `total_ms` difference beyond a 30 %
   run-to-run spread between cache off and a warm cache. The warm cache moved work into creation instead: DXVK
   looks the shader up in the cache synchronously (`DxvkDevice::createCachedShader`), about 0.09 ms per
   `CreatePixelShader` against 0.008 ms without the cache, where conversion waits until first use. Price the cache
   with a title that has many large shaders. Both paths also share the ICD's pipeline cache
   where it has one: either disable it on both sides (`MESA_SHADER_CACHE_DISABLE=true`) or start with one
   discarded run per side. Say which.
4. At least three runs per side, alternating (per-app, system, per-app, ...), window mode, with the same
   arguments. Each run takes a few seconds with the defaults; the whole series stays well under three minutes.
5. Compare:

   ```
   python tools\win\d3d11bench\compare.py --base per-app-*.json --candidate system-*.json
   ```

   Per scene, the median of the per-run medians of the gated metric on each side, and the larger run-to-run
   spread ((max - min) / median) of the two sides as the margin:

   | Verdict | Condition |
   |---|---|
   | PASS | change <= bound - margin |
   | FAIL | change > bound + margin, or the checksums differ |
   | INCONCLUSIVE | anything in between: more or quieter runs are needed |

   Exit 0 when every scene passes, 1 on a failure, 2 on unusable input (different settings, adapter, path,
   configuration or Vulkan drivers, an incomplete run), 3 when inconclusive. `--any-path` and `--ignore-configuration` exist for
   tool checks and never for a bound verdict. Recording, present and GPU times are printed for diagnosis only.

On a development PC with an NVIDIA GPU, DXVK reports the adapter as AMD unless `DXVK_CONFIG` sets
`dxgi.hideNvidiaGpu = False`; set it on both sides, or `compare.py` rightly refuses the runs as different adapters.
A native driver on the system side is a tool check only: its costs sit in different places (NVIDIA's `Flush` does
the command building that DXVK does while recording), and it says nothing about the bound.

Bez pracy nie ma kołaczy. (No work, no cake: the bound is measured, not assumed.)
