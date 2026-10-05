# E43: candidate ICD 6661C2D2, shared-fence export and the WSI present log (M580, M581)

Unit A, Windows 11, 2026-09-27 01:56-02:01 UTC (03:56-04:01 local). Registered ICD 93B1D1FD swapped for the
candidate for the duration of the run and restored from the run-directory copy (hash verified); UMD 8279AC7F,
KMD 152 (generation 575721647), CPU DWM 4400, clock 1000 MHz / VID 116 before and after. No promotion.

## Candidate

`vulkan_radeon.dll` 6661C2D2FE1DBAAAAA007D1686A0C42F33E7A9F5204DC0681EF98AA404F952AD, built from fork branch
`amdgpu-wddm/radv-wddm2-present-log` 0f9811a5 (on `amdgpu-wddm/radv-wddm2-fence-share` 59f53088, on the
baseline's c34ab7cd) with the recorded radv configuration (`tools/build/build-mesa.ps1 -Config radv`, log
`build-radv-presentlog.log` in the fork scratch). The two commits are in `patches/`:

1. `vk_wddm2_monitored_fence.c`: an exportable semaphore reaches the sync-type init with
   `VK_SYNC_IS_SHAREABLE` only (the runtime sets `VK_SYNC_IS_SHARED` after the first export), but the
   monitored fence's `Shared`/`NtSecuritySharing` flags were derived from `IS_SHARED`. `D3DKMTShareObjects`
   requires `NtSecuritySharing` at creation (d3dkmthk), so it failed, its status was not checked,
   `shared_handle` stayed NULL and `vkGetSemaphoreWin32HandleKHR` returned `VK_ERROR_UNKNOWN` from
   `DuplicateHandle(NULL)`. Now the flags come from `IS_SHAREABLE`, the `ShareObjects` status is checked (init
   fails and destroys the object on error) and a missing NT handle at export time is reported as
   `VK_ERROR_FEATURE_NOT_PRESENT`.
2. `wsi_common_win32.cpp`: with `BC250_WSI_PRESENT_LOG=<file>` each swapchain appends a header and one CSV
   line per queued present (chain, path, present id, enter time in us, CPU copy, BitBlt or Present1, DwmFlush,
   result). Nothing changes when the variable is unset.

## Procedure (`scripts/run-w3dx12-005.ps1`, interactive task)

Preconditions (registered ICD 93B1D1FD, candidate hash, package hashes, STOP flag, Tctl), baseline copy of the
registered file into the run directory, candidate copied over `C:\BC250\m10\wsi-final\vulkan_radeon.dll`, E14
compute smoke through the recorded runner with the candidate in place (`smoke/receipt.json`: PASS, loaded
artifact 6661C2D2), then the E41/E42 steered run (Escape, E for CONTINUE, latest save at Kaer Morhen, scene left
running, stop file at 279 s), finally block: game killed, package removed with hash checks, registered ICD
restored and hash-verified, settings and save count checked, KMD health and clock read.

## Results

- Shared-fence export (M581): `vkd3d.log` contains zero `d3d12_shared_fence_open_export_kmt` errors (E40, E41
  and E42 run 003 each had four on 93B1D1FD). Every other log entry kind is as before (DXR state objects, D24S8
  to D32S8 mapping, OpenXR/OpenVR probes). The game rendered the menu and the scene as in E41/E42 (nine auto
  screenshots and one manual, hashes in `capture-hashes.json`), responsive, working set 3.2-3.9 GB, no device
  loss, KMD `completed` advancing (26104 at the end).
- Present log (M580), `present.csv`, summary `present-summary.json` (`scripts/analyze-present-log.py`): two
  swapchains, both 1920x1200, `VK_FORMAT_B8G8R8A8_UNORM` (37), three images, path `gdi`; the first presented
  once, the game's chain 9280 times over 266.5 s (34.8 presents/s overall). Per 30 s bucket, interval between
  presents (mean / median / p95, ms) and the parts of the present call (mean, ms):

  | bucket (UTC start) | phase (screenshots) | n | interval mean | median | p95 | copy | BitBlt | DwmFlush |
  | --- | --- | --- | --- | --- | --- | --- | --- | --- |
  | 0 s (01:56:39) | start panel, menu | 1666 | 18.0 | 16.7 | 24.7 | 3.0 | 1.5 | 12.0 |
  | 30 s | menu, CONTINUE | 1526 | 19.7 | 16.8 | 33.7 | 3.1 | 1.4 | 13.7 |
  | 60 s | loading, scene | 848 | 35.4 | 33.4 | 36.2 | 3.3 | 1.4 | 15.2 |
  | 90 s to 240 s | scene, idle | 884-893 each | 33.6-33.9 | 33.4-33.6 | 34.4-35.2 | 3.4 | 1.3 | 10.9-11.4 |

  Whole run, game chain: interval median 32.7 ms (p5 16.3, p95 34.7, p99 48.3, max 1159.7 during loading);
  present call median 15.8 ms (p95 29.2) = CPU copy 3.2 ms median (p99 4.3) + BitBlt 1.3 ms (p99 1.8) +
  DwmFlush 11.3 ms median (p95 24.5, p99 26.5). All 9281 results are VK_SUCCESS.

## Reading

- The menu runs at about 55-60 presents/s and the loaded scene locks at 29.7 presents/s with a very narrow
  distribution (p5 32.3, p95 34.5 ms): the cadence is quantised to two refresh periods of the 60 Hz desktop.
  DwmFlush, which waits for the next composition, takes a third of the frame in the scene. Whether the game's
  own vsync setting, the FIFO present mode of the swapchain or DwmFlush after a 16.7 ms miss is what locks the
  rate is not established here; the same save with vsync off in the game settings and/or `VKD3D_SWAPCHAIN_PRESENT_MODE`
  would separate them (not run, would change settings).
- The present call itself costs about 16 ms of the 33 ms frame on the CPU thread that presents: a 9 MB copy
  (3.3 ms, single-threaded), the GDI blit (1.3 ms) and the composition wait. A present path that hands the
  rendered image to the compositor without the CPU copy is the obvious next performance step for the D3D12 and
  Vulkan paths alike; this run gives the baseline to measure it against.
- The fence fix removes the export failure; the CTS cases `dEQP-VK.api.external.semaphore.opaque_win32.*` and
  `d3d12_fence.*` (44 cases in the full list) have not been run on either ICD and are the next check before
  promotion.

## Files

- `run005/`: run log (UTF-8 copy), `present.csv`, `present-summary.json`, `smoke-config.json`,
  `smoke/receipt.json` and stdout/stderr, `modules.json`, `result.json`, `launch.json`, `vkd3d.log`,
  `witcher3_dxgi.log`, KMD health and clock before/after, `dxvk.conf`, done file, input log (all three input
  runs of the night), post-check.
- `patches/`: the two fork commits (`git format-patch`).
- `scripts/`: run, worker, launch, stop, status, pull, analysis and finish scripts.
- `capture-hashes.json`: SHA-256 of the private screenshots; `manifest.json`: artifacts and file hashes.
