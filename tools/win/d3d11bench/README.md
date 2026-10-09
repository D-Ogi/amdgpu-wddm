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

The build runs the tests of `compare.py` and `imgdiff.py` first and fails on a failing case, keeps the previous executable under
`retained\` by its hash, compiles with `/W4 /WX`, checks `--help` and the image architecture, and prints the SHA-256 that a lab runner pins.
Output: `<BC250_ROOT>\scratch\build\d3d11bench\d3d11bench.exe`.

`-Arch x86` builds the 32-bit client from the same source, into `scratch\build\d3d11bench\x86`. The D3D11 shell
has an x64 and an x86 image, so a lab matrix that must cover both takes both clients from this one source.

## Scenes

| Scene | Bound by | Per frame | Gated metric |
|---|---|---|---|
| `draws` | application thread | 2000 draws (`--draws`), each with a `Map(WRITE_DISCARD)` of a 32-byte constant buffer and one of four textures, read with `Load` | `frame_ms` |
| `fill` | GPU | 8 blended full-screen layers (`--layers`), each sampling a 512x512 mipmapped texture four times, plus an ALU loop | `frame_ms` |
| `shaders` | shader and pipeline creation | 64 pixel shader variants (`--shaders`) created and drawn once each, each into its own tile of a 256x256 target | `total_ms` |

Frame scenes render 30 warm-up frames (`--warmup`), then 300 measured ones (`--frames`), and read back the last
frame for an FNV-1a checksum. Every scene's output is deterministic, so a checksum mismatch between the two paths on
one GPU is a correctness failure, not noise.

In window mode `--sync-interval N` is the sync interval the program gives `IDXGISwapChain::Present`, 0 to 4.
The default is 0, which is what the program always asked for. The per-application `VSync` setting of the driver
overrides this interval, so the acceptance matrix of BD-099 needs all three of 0, 1 and 2
(`docs/design/per-app-graphics-settings.md`). Offscreen mode has no swap chain and ignores the option.
The result records the interval the run really presented at as `sync_interval`, and `compare.py` reads it as a
setting of the workload: a capped run and an uncapped one are not the same work, so it refuses such a pair.
A result from before the option carries no such key, and it reads as interval 0.

Across implementations (a CPU rasterizer against a GPU, one vendor against another) the scenes are built to be
comparable too; this is scene revision 2, `scene_revision` in the result:

- `draws` is exact on every conforming implementation. Vertices snap to chosen positions, no triangle edge passes
  through a pixel centre, the texel comes from `Load` at the pixel's integer position, and the tint is a 0/1
  channel mask, so no value needs rounding. Checked on the development PC: native D3D11, DXVK and WARP agree
  bit for bit at 64x64 and 1280x720, and so does the exact rational model of the scene in `draws_model.py`
  (`python draws_model.py DIR\draws.pam --draws N` names every pixel that differs from it).
- `fill` and `shaders` use `sin` and trilinear filtering, which implementations may round differently. Their
  loops contract, so such differences shrink instead of growing. WARP and the RTX 4090 differ by at most 1 per
  channel in both. Judge them with `imgdiff.py` and a recorded tolerance.

Revision 1 (results without `scene_revision`) was not comparable across implementations. Its `draws` sampled with
an interpolated coordinate and used tints whose products were exact halves. Its `fill` iterated a chaotic map, so
one step of filter weight came out as full-range noise. `compare.py` refuses to mix revisions.

Reference checksums, revision 2, default settings, offscreen, from the development PC:

| Scene | RTX 4090 native and DXVK | WARP (`--adapter warp`) |
|---|---|---|
| `draws` | `03a5b8ea7dbea991` | `03a5b8ea7dbea991` |
| `fill` | `4f7e3b37e1283a07` | `7198ddabd07f3927` |
| `shaders` | `484089e89be4655d` | `d79df22502a9ef46` |

`--adapter warp` runs Microsoft's software rasterizer. It gives a CPU reference image on any Windows machine,
never a performance number.

To follow a lead, run both sides with `--dump DIR`: each scene's checksummed image lands in `DIR\<scene>.pam`
(PAM, RGBA, top row first). Then

```
python tools\win\d3d11bench\imgdiff.py DIR_A DIR_B [--tolerance N]
```

prints, per scene, how many pixels differ, the largest difference per channel, the first pixel beyond the
tolerance and both images' checksums (equal to the `checksum` in the run's JSON, which ties a dump to its run);
`--diff OUT.pam` on two files writes the difference image. A wrong draw shows as large differences over whole
regions: a displaced or missing triangle, or a black `shaders` tile. A tolerance is a judgement recorded with the
result; it never replaces the same-GPU checksum match of the bound, and never applies to `draws`.

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
- `scene_revision`: the scenes' shaders and constants (2; absent in revision 1 results);
- the settings: `mode`, `width`, `height`, `feature_level`, `frame_latency`, `sync_interval` and every scene's
  parameters;
- `adapter`: vendor, device and description as DXGI reports them;
- `d3d11`: `app-local` or `system`, from the path of the `d3d11.dll` the process actually loaded;
- `modules`: path and size of every loaded graphics module (D3D runtime, DXGI, Vulkan loader, the project's
  `bc250*` and `amdgpu_wddm_*` modules, anything from the driver store, every Vulkan driver);
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
   loads its own sibling `amdgpu_wddm_radv.dll` (`bc250radv.dll` in shells built before the rename). Either register the same file for the loader or run the per-application side
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

## Window resource lifetime control

`--mode window --scenes resize --size 64x64 --deadline 20` is a functional
control, excluded from the three performance scenes. It renders three distinct
exact clear colors and Presents at each of four sizes: requested,65x33,127x79,
requested again. The twelve pre-Present readbacks must match the expected BGRA
bytes, including alpha. Optional PAM dumps preserve every frame. An event query
retires each iteration; before each ResizeBuffers the context state is cleared,
views/back-buffer/staging references released and commands flushed. Reacquired
back-buffer dimensions and format must match. CPU/GPU comparison can therefore
check resizing, identity rotation and old-resource retirement with the same
workload. It does not verify composed screen pixels or copy-free transport.

The resize scene must be selected alone and requires window mode. Successful
JSON has three resizes, twelve Presents, twelve pixel checks and four per-size
records. Failure or deadline never produces a successful scene. Existing scene
revision2 workloads and their performance comparison are unchanged.
