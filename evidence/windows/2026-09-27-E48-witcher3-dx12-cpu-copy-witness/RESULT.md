# E48: the slow CPU present copy of E47 is process-specific (vkcube controls, memory-type and thread-cycle witness) (M610)

Unit A, Windows 11, 2026-09-27 05:43-06:13 UTC (07:43-08:13 local). Four bounded stages on the same box and boot as
E43 and E47 (boot 2026-09-26T18:44:31+02:00), diagnostic KMD 0.7.153.1 (generation 431166497000, M603), UMD 8279AC7F,
CPU DWM 4596, clock 1000 MHz / VID 116 before and after every stage, Tctl 66-70 C. Method as E43/E47: the registered
ICD 93B1D1FD is swapped in place for the candidate for the duration of a stage and restored from the run-directory copy
with a hash check; E14 compute smoke before each game run; the vkd3d-proton 472989aa package placed and removed with
hash checks. No promotion, no reboot, no reset, no KMD or registry change. Lab windows granted by the KMD owner
(agent-discussion, local) for each pair.

## Question

E47 (M607) found the game's CPU copy of the 1920x1200 swapchain image 6x slower than in E43 on the same save (19.5 ms
against 3.3 ms per 9.2 MB) with the same ICD, and left the cause open (hypothesis: an uncached or write-combined CPU
mapping of the host-visible allocation after the in-session KMD 153 transition). This evidence separates the
candidates: mapping attribute requested by the ICD, memory pressure, page faults, thread starvation, and the process
itself.

## What the port's present path is (source facts, Mesa fork)

- `radv_wddm2_wsi_init` sets `wsi->sw = true` and `wsi->blit = NULL`, so the Win32 WSI takes the buffer-blit CPU path:
  the swapchain image lives in device memory, the GPU blits it into a host buffer, the present thread copies that
  buffer into a DIB with `memcpy` per row, `BitBlt`s it into the window and waits for `DwmFlush` (FIFO).
- The host buffer's memory type comes from `wsi_select_host_memory_type`, which with `sw` already requires
  `HOST_CACHED`. On this port the types are [4]/[5] GTT cached (`CPU_ACCESS`, `HOST_CACHED`); the KMD marks such
  allocations `Cached` for the aperture segment when the v2 blob says heap GTT without `NO_CPU_ACCESS`/`USWC`
  (`umd_blob.c`, `wddm.c`). Candidate 50E99A84's unconditional `HOST_CACHED` request (patch 0002) is therefore a no-op
  here, which the witness confirms.
- With `sw` set, `wsi_common_queue_present` waits for the image's fence before `queue_present`, so `copy_us` in the
  present log is memcpy time and never a GPU wait.

## Candidates

| ICD | fork commit | change | used in |
| --- | --- | --- | --- |
| 50E99A84 | 1dd68127 | E47's CF3948D6 + `HOST_CACHED` required for CPU-presented images + memory-type header line (reads an unset field on the buffer-blit path; not a finding) | cpucopy-001 stage A, run 009 |
| 6661C2D2 | 0f9811a5 | E43's ICD (control) | cpucopy-001 stage B |
| 09191AAA | 238c495a | fixes the witness: `memory_type_index` recorded in `wsi_create_buffer_blit_context` | built, never launched |
| 63AF86CB | 6358c3a9 | + `copy_cycles` column: `QueryThreadCycleTime` of the present thread around the copy loop | cpucopy-002, run 010 |

Patches in `patches/`, build recipes in `scripts/vulkan_radeon.*.recipe.json` (radv configuration of
`tools/build/build-mesa.ps1`). The registered ICD stayed 93B1D1FD throughout; every stage restored it and the hash was
checked after each stage and at the end of each window.

## Stages

| stage | UTC | ICD | workload | bound | result |
| --- | --- | --- | --- | --- | --- |
| cpucopy-001 A | 05:43 | 50E99A84 | vkcube 900 frames, 640x480 then 1920x1181, CPU present path, present log | 60 s each | exit 0, all presents VK_SUCCESS |
| cpucopy-001 B | 05:44 | 6661C2D2 | same | 60 s each | exit 0 |
| run 009 | 05:49-05:53 | 50E99A84 | steered Witcher 3 DX12 (Escape, E = latest save at Kaer Morhen), `VKD3D_SWAPCHAIN_PRESENT_MODE=FIFO`, side sampler of page faults | 180 s | bound, smoke PASS, 115 saves before and after |
| cpucopy-002 | 06:04 | 63AF86CB | vkcube 900 frames, 1920x1181 | 60 s | exit 0 |
| run 010 | 06:06-06:09 | 63AF86CB | as 009 | 180 s | bound, smoke PASS, 115 saves |

cpucopy-002's first pass (06:01-06:03) had a collapsed PowerShell size list (`@(@(1920,1200))` is a flat array) and
launched vkcube twice with an empty `--height`; no present log was produced, both hit the bound, the restore ran and
was hash-checked. That pass is preserved on the target as `cpucopy-002-bad-sizes` and is not part of the results.

## Results

vkcube, CPU present path, per-present copy of the host buffer into the DIB (`copy_ms`, 900 presents each):

| stage | ICD | image | bytes | copy median ms | copy p95 | GB/s | thread Mcycles | bytes/cycle |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| cpucopy-001 A | 50E99A84 | 640x480 | 1.23 MB | 0.14 | 0.15 | 8.8 | n/a | n/a |
| cpucopy-001 B | 6661C2D2 | 640x480 | 1.23 MB | 0.15 | 0.15 | 8.2 | n/a | n/a |
| cpucopy-001 A | 50E99A84 | 1920x1181 | 9.07 MB | 1.07 | 1.11 | 8.5 | n/a | n/a |
| cpucopy-001 B | 6661C2D2 | 1920x1181 | 9.07 MB | 1.05 | 1.09 | 8.6 | n/a | n/a |
| cpucopy-002 | 63AF86CB | 1920x1181 | 9.07 MB | 1.08 | 1.12 | 8.4 | 3.45 (CPU share 1.00) | 2.6 |

The game (chain 1920x1200 `VK_FORMAT_B8G8R8A8_UNORM`, three images, path `gdi`, mode 2 = FIFO):

| run | ICD | presents | span s | interval median ms | copy median ms | copy p5 / p95 | DwmFlush mean | present call mean | thread Mcycles median | CPU share |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| E43/005 (KMD 152, DWM 4400) | 6661C2D2 | 9280 | 266 | 32.7 | 3.3 (mean) | | 12.1 | 16.8 | n/a | n/a |
| E47/008 (E43 repeat) | 6661C2D2 | 5568 | 294 | 55.6 | 19.5 (mean) | | 19.8 | 41.0 | n/a | n/a |
| 009 | 50E99A84 | 3201 | 157 | 42.8 | 18.3 | 17.9 / 30.5 | 20.2 | 42.1 | n/a | n/a |
| 010 | 63AF86CB | 2688 | 114 | 38.6 | 17.2 | 16.9 / 27.6 | 19.4 | 40.2 | 54.3 (p5 53.9, p95 79.5) | 1.00 (p5 0.94) |

Per 30 s bucket the game's copy is flat (009: 19.9, 20.3, 20.7, 20.4, 19.4, 19.0 ms mean; 010: 19.1, 18.8, 19.3,
19.0 ms), in the menu as in the scene, and never approaches vkcube's 1.1 ms for the same byte count. Menu and scene
have the same copy time, so the game's rendering load is not the variable either.

Witness header lines (`# chain ... image memory type T properties 0xP`):

| process | chain | memory type | properties |
| --- | --- | --- | --- |
| vkcube (cpucopy-002) | 1920x1181, format 44 | 5 | 0xe = HOST_VISIBLE, HOST_COHERENT, HOST_CACHED |
| witcher3 (run 010), start-panel chain | 1920x1200, format 37 | 5 | 0xe |
| witcher3 (run 010), game chain | 1920x1200, format 37 | 5 | 0xe |

Side sampler (`pfsample-009.txt`, `pfsample-010.txt`, 2 s cadence): the game's page faults are 0-22 per second in the
scene with one burst at the save load (20k/s at the E keypress in 010, 36k/s in 009); DWM shows 3.6-6.8k/s only around
the menu transition; available memory 2.3-2.5 GB; pages input 0 except at the load. Not page faults on the DIB.

KMD log summary counters (`cpucopy-002/*-log-summary.txt`, `run010/log-summary-after-010.txt`): across the game
session, paging operation 9 (virtual transfer) rose 2167 to 2418, 11 (page-table update) 114384 to 127519, 12 (TLB
flush) 26007 to 29074, UMD allocations 5232 to 5842, 0 refused, node 0 no timeouts. No per-allocation placement is
in the summary.

## Reading

1. The mapping requested by the ICD is the same in both processes (type 5, cached, coherent), the KMD's cache rule
   for it is the same, and vkcube reads it at 8.5 GB/s on the same box, minute and KMD. The E47 hypothesis of a
   write-combined or uncached mapping caused by the ICD's choice or by KMD 153 in general is refuted.
2. The present thread is not starved: `QueryThreadCycleTime` charges the copy 54 million cycles per frame, which at
   3.19 GHz is the 17 ms wall time; the thread runs the whole time and does 0.17 bytes per cycle (vkcube 2.6).
3. Not memory pressure (E47 memstate), not page faults (sampler), not the DwmFlush wait (copy is measured before it),
   not the game's frame load (menu equals scene).
4. What remains is the physical placement or the cache attribute of what the game's copy touches: the 9.2 MB blit
   buffer as actually placed by dxgkrnl/KMD and mapped by `Lock2` in this process (segment, PAT), or the DIB. 0.5 GB/s
   is the shape of a CPU read stream from an uncached BAR mapping. Neither is visible from the ICD with the present
   witnesses.

## Next witnesses (not run)

- ICD side: `D3DKMTQueryAllocationResidency` on the blit buffer's allocation after the first present, written into the
  present-log header (resident in GPU memory / shared memory / not resident).
- KMD side (KMD owner): the segment id and the `Lock2` cache attribute of that allocation (9.2 MB, heap GTT,
  `CPU_ACCESS`, created by witcher3.exe) in the KMD log.

## Files

- `cpucopy-001/`, `cpucopy-002/`: present logs per stage (`present-<stage>-<size>.csv`) with `*-summary.json` from
  `scripts/analyze-present-log.py`, vkcube stdout/stderr, module hashes, health/clock/log-summary before and after,
  runner log. Screenshot hashes in `capture-hashes.json` (images private, they show the desktop).
- `run009/`, `run010/`: `present.csv` with `present-summary.json`, `launch.json`, `result.json`, `modules.json`,
  `vkd3d.log`, `witcher3_dxgi.log`, smoke receipts, input log, runner log, sampler text; `run010/log-summary-after-010.txt`.
- `scripts/`: runners, launchers, workers, status/stop/pull helpers, generators, sampler, analyzer, the cycles patch
  script, build recipes. `patches/`: fork commits 1dd68127, 238c495a, 6358c3a9 (E47's 2732f9c8 is in E47).
- `manifest.json`: SHA-256 of every file and the artifact identities.
