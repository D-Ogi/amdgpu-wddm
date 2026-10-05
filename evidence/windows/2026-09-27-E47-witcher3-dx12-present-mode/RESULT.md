# E47: Witcher 3 DX12 present modes on the GDI path (FIFO control, IMMEDIATE, E43 repeat) (M607)

Unit A, Windows 11, 2026-09-27 05:02-05:23 UTC (07:02-07:23 local). Three steered runs of the same save on the
same box within 21 minutes, same procedure as E43 run 005 (registered ICD 93B1D1FD swapped in place for the
candidate for the duration of each run and restored from the run-directory copy, hash verified; E14 compute
smoke as positive control before each game run; vkd3d-proton 472989aa package placed and removed with hash
checks). UMD 8279AC7F, diagnostic KMD 0.7.153.1 (generation 431166497000, loaded in-session on 2026-09-27
without a reboot, M603), CPU DWM 4596, clock 1000 MHz / VID 116 before and after every run, Tctl 66-70 C,
boot 2026-09-26T18:44:31+02:00 (same boot as E43). No promotion, no reboot, no reset, no registry change.

## Question

E43 (M580) measured the loaded scene at 29.7 presents/s with the present call taking 16 ms of the 33 ms frame
(CPU copy 3.3, BitBlt 1.3, DwmFlush 11.3 ms) and left open whether the compositor's refresh quantises the rate
or the game's own frame time does. The port's GDI present path advertised `VK_PRESENT_MODE_FIFO_KHR` only, and
vkd3d-proton ignores a `VKD3D_SWAPCHAIN_PRESENT_MODE` override the surface does not support
(`libs/vkd3d/swapchain.c:1797-1810, 2139-2156`; the check is `vkGetPhysicalDeviceSurfacePresentModesKHR`).

## Candidate CF3948D6

`vulkan_radeon.dll` CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157, fork branch
`amdgpu-wddm/radv-wddm2-gdi-immediate` 2732f9c8 = E43's tree 0f9811a5 plus one commit (`patches/`), radv
configuration of `tools/build/build-mesa.ps1` (recipe in `scripts/vulkan_radeon.CF3948D6.recipe.json`):

- `present_modes_gdi[]` gains `VK_PRESENT_MODE_IMMEDIATE_KHR`.
- In the GDI present, the `DwmFlush()` wait after the `BitBlt` is skipped when the chain's mode is IMMEDIATE
  (the BitBlt with `GdiFlush` has already copied the frame into the window's redirection surface, so the
  swapchain image is free; the DWM still shows at most one frame per composition).
- The present log header names the selected mode (`mode <VkPresentModeKHR>`), so the mode in effect is read
  from the ICD, not inferred from vkd3d's log.

## Runs

| run | ICD | `VKD3D_SWAPCHAIN_PRESENT_MODE` | mode in the log header | vkd3d.log | stop |
| --- | --- | --- | --- | --- | --- |
| 006 | CF3948D6 | `FIFO` | 2 (FIFO) | "Overriding swapchain present mode to FIFO" | stop file, 283 s |
| 007 | CF3948D6 | `IMMEDIATE` | 0 (IMMEDIATE) | "Overriding swapchain present mode to IMMEDIATE" | stop file, 299 s |
| 008 | 6661C2D2 (E43's) | unset | header without mode (E43 build) | no override line | stop file, 314 s |

Each run: Escape at about 60 s and E (CONTINUE, latest save at Kaer Morhen) about 25 s later through the
interactive-session input task, scene left running, screenshots every 30 s (hashes in `capture-hashes.json`),
exit 0, smoke PASS, zero `d3d12_shared_fence_open_export_kmt` errors, settings unchanged, 115 saves before and
after, registered ICD restored to 93B1D1FD, task unregistered.

## Results (game chain, 1920x1200, `VK_FORMAT_B8G8R8A8_UNORM`, three images, path `gdi`)

Whole run (mean ms unless stated; per-run details in `run00x/present-summary.json`):

| run | presents | span s | interval median ms | interval p5 / p95 | copy | BitBlt | DwmFlush | present call |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| E43/005 (KMD 152, DWM 4400) | 9280 | 266 | 32.7 | 16.3 / 34.7 | 3.3 | 1.4 | 12.1 | 16.8 |
| 006 FIFO | 4736 | 253 | 55.7 | 38.8 / 69.2 | 19.7 | 1.7 | 20.0 | 41.4 |
| 007 IMMEDIATE | 5952 | 268 | 44.1 | 27.5 / 61.0 | 23.6 | 1.8 | 0.0 | 25.5 |
| 008 E43 repeat | 5568 | 294 | 55.6 | 38.9 / 67.5 | 19.5 | 1.7 | 19.8 | 41.0 |

Per 30 s bucket, interval between presents (median ms, ~fps) and the parts of the present call (mean ms):

| bucket | 006 FIFO | 007 IMMEDIATE | 008 E43 repeat |
| --- | --- | --- | --- |
| 0-30 s (start panel, menu) | 40.7 (20.9) copy 20.6 dwm 21.1 | 32.5 (28.0) copy 26.9 dwm 0 | 39.4 (22.0) copy 19.9 dwm 19.6 |
| 30-60 s (menu) | 39.4 (23.8) copy 19.8 dwm 19.1 | 32.3 (30.0) copy 28.3 dwm 0 | 40.5 (22.8) copy 20.5 dwm 20.3 |
| 60-90 s (menu, CONTINUE) | 43.0 (21.6) copy 21.0 dwm 21.1 | 35.3 (27.6) copy 26.9 dwm 0 | 40.4 (22.6) copy 20.4 dwm 20.6 |
| 90-120 s (loading) | 44.5 (16.4) copy 20.1 dwm 21.0 | 38.6 (21.4) copy 24.8 dwm 0 | 43.1 (21.8) copy 19.6 dwm 21.0 |
| 120-150 s (scene) | 56.2 (17.0) copy 18.9 dwm 19.3 | 54.0 (18.5) copy 20.0 dwm 0 | 57.0 (14.1, load spike) copy 19.5 dwm 19.8 |
| 150-270 s (scene, idle) | 56.7-57.3 (16.5-17.0) copy 18.7-19.4 dwm 19.2-20.3 | 54.1-54.8 (18.1-18.7) copy 19.6-19.9 dwm 0 | 56.2-56.8 (16.9-17.2) copy 18.8-19.2 dwm 18.9-19.6 |

All presents returned `VK_SUCCESS`. The loading spike (one interval of 4.7-4.9 s) sits in the 90-150 s buckets.

## Reading

- The present-mode override works end to end on the candidate: 006 stays FIFO (mode 2) and 007 runs IMMEDIATE
  (mode 0) with DwmFlush at 0 ms and the present call down from 41 to 25 ms. vkd3d-proton's own selection with
  the game's vsync off would now pick IMMEDIATE by itself (`swap_interval > 0 ? FIFO : IMMEDIATE`, `swapchain.c:2179`),
  so on this candidate DXGI `SyncInterval 0` stops being quantised to the compositor.
- The box is not the box of E43. Run 008, an exact repeat of E43 run 005 with E43's ICD, reproduces 006 and not
  E43: the CPU copy of the 9.2 MB host-visible swapchain image into the DIB takes 19.5 ms (0.47 GB/s) instead of
  3.3 ms (2.8 GB/s), stable per present (p5 17.9, p95 28.1 ms), the DWM's composition period is about 18.5 ms
  instead of 11-12 (the FIFO interval of 55.6 ms is three of them), and the scene's frame interval is 54-56 ms
  instead of 33. What changed between E43 and E47 on the same boot: the in-session transition from KMD 152 to
  the diagnostic 153 (M603) and the DWM instance (4400 then, 4596 since 06:12 local). Not changed: the game, the
  save, the package, the ICD (008), the clock, the temperature.
- Memory is not short: with the game closed, 4.5 GB of the 7.9 GB visible physical memory were available before
  and after 008, commit 5.2 GB of a 40.7 GB limit, nonpaged pool 353 MB, paged pool 300 MB; during the game
  the free figure (0.9-1.1 GB on the overlay) is the game's 3.4 GB working set, as in E43, and pages/sec was 64
  (`run008/state/`). CPU during 008: 45-50 % of the six cores at 3.19 GHz, the CPU DWM at 3.5 cores (10.45
  CPU-s per 3 s) and the game at 2.1; no Defender scan (last quick scan 2026-09-26 13:08), Balanced plan. No
  CPU snapshot exists for E43, so the DWM's share then is unknown.
- The uniform 6x slowdown of a plain `memcpy` from a host-visible allocation, with plenty of free memory and
  the CPU half idle, is the signature of uncached or write-combined reads, not of pressure. Hypothesis (not
  measured here): since the 153 transition the CPU mapping of RADV's host-visible allocations, or their segment,
  differs from 152's. A matched control is KMD-side (map attribute or segment of the swapchain allocation under
  153, or a rollback to 152 and one more repeat); an ICD-side probe is a vkcube CPU-present run with the present
  log against E46's copy times.
- For the owner's question: on today's box the loaded scene is limited by the game's own frame time (about 54 ms
  with the compositor wait removed), not by the refresh quantisation; on E43's box the FIFO quantisation to two
  refresh periods was real (33 ms) and IMMEDIATE would have exposed the frame time underneath. The measurement
  has to be repeated once the box's copy speed is back at E43's level, together with a vkcube control.

## Files

- `run006/`, `run007/`, `run008/`: run log (UTF-8 copy), `present.csv`, `present-summary.json`, smoke config and
  receipt, `modules.json`, `result.json`, `launch.json`, `vkd3d.log`, `witcher3_dxgi.log`, KMD health and clock
  before/after, `dxvk.conf`, done file, input log.
- `run008/state/`: `memstate-before-008.json`, `memstate-after-008.json` (physical/commit/pool counters,
  processes, KMD health/clock/version), `cpustate-before-008.json`, `cpustate-during-008.json` (05:20:42-05:21:03,
  scene running), `cpustate-after-008.json` (processor counters, top CPU consumers over 3 s, Defender, power plan).
- `patches/0001-gdi-immediate-2732f9c8.patch`: the candidate's commit. `scripts/`: run/launch/worker/stop/status
  scripts of the three runs, the generators (`make-w3dx12-mode.py`, `make-w3dx12-repeat.py`), input scripts,
  `precheck-006.ps1`, `memstate.ps1`, `cpustate.ps1`, pull and analysis scripts, the candidate recipe, `finish-e47.py`.
- `capture-hashes.json`: SHA-256 of the auto screenshots and the two half-scale screenshots (images private in
  scratch, desktop content). `manifest.json`: artifacts and file hashes.
