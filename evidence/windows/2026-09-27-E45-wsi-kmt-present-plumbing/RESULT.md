# E45: KMT present path of the Vulkan WSI, plumbing control (wsi-kmt-001..003)

Unit A, Windows 11, 2026-09-27 03:49-03:59 UTC (05:49-05:59 local). Registered ICD 93B1D1FD swapped for the
candidate for each run's duration and restored from the run-directory copy (hash verified, exit 0); UMD 8279AC7F,
KMD 152 (generation 575721647, epoch 157, flags 7 before and after every run), CPU DWM 8116, clock 1000 MHz / VID 116,
Tctl 67.6-69.2 C. The lab BlitGate (`EnablePresentBlit`) was open throughout (KMD summary "blit gate open"). No
promotion, no registry change, no reboot, no DWM restart. Lab window granted by the KMD/DWM agent for this control
only and handed back with the state above.

## What was tested

ADR 0018 work item 2 (a Vulkan present without the steady-state CPU copy), design note
`docs/design/wsi-engine-present.md`, path B: swapchain images are dedicated, linear, device-local allocations whose
private driver data is the KMD's 32-byte LB7A block; the winsys creates one non-UMD present context per device
(`D3DKMTCreateContextVirtual`, node 0, no private data) and per present queues a GPU wait on the WSI blit timeline,
`D3DKMTPresent` with `hWindow`, `hSource` = the image's allocation, `Flags.Blt | DstRectValid | SrcRectValid`, then a
GPU signal on the same timeline; the CPU DIB, BitBlt and DwmFlush of the software path are not used. The two
candidates (`patches/`, fork branch `amdgpu-wddm/radv-wddm2-wsi-kmt` on `radv-wddm2-present-log` 0f9811a5):

| candidate | fork commit | difference |
| --- | --- | --- |
| B9F4C95B22AB80F671FC94A678A70F3A960DB05F12D6615C0A8422FB8ED693F9 | 1ae7b6af | KMT present path; `SubRectCnt = 0` when the app passes no damage rectangles |
| 11BA214350632A0DA665521D7132318DA62043C5D7EAFE7BFF7EF1BDB48FEFF2 | aa8296c0 | plus `SubRectCnt >= 1` always (the whole image is the one sub-rectangle) |

Procedure (`scripts/run-kmt-003.ps1`, interactive scheduled task at RunLevel Highest, 8 min limit): preconditions
(registered ICD hash, candidate hash, vkcube 87A96AF3, PresentMon B2A706BC, STOP flag, Tctl), KMD health, clock and
log summary before; stage A: `vkcube --c 900 --width 640 --height 480 --suppress_popups` with
`BC250_WSI_PRESENT_LOG` and `BC250_TRACE_SUBMITS=1`, screenshot at 4 s, KMD log summary after; stage B: the same with
`BC250_WSI_CPU_PRESENT=1` (the software present path as the control), screenshots at 4 s and 12 s; PresentMon ETW
session `BC250KMT` around both; finally block: strays killed, `logman stop`, ICD restored and hash-checked, health,
clock and log summary after, overlay status. `scripts/cleanup-kmt-003.ps1` removed the task and confirmed the
registered ICD and the running tasks (overlay and watchdog only).

## Results

- wsi-kmt-001 (null run): the candidate was offered through `VK_DRIVER_FILES` and both stages loaded the registered
  ICD 93B1D1FD instead (`run001/modules-*.json`): the Vulkan loader ignores the variable in an elevated process, and
  the interactive task runs elevated for PresentMon. Nothing about the KMT path was measured. From 002 on the
  candidate is copied over the registered file for the run (the E43 method).
- wsi-kmt-002 (B9F4C95B): stage A loaded the candidate; every one of the 900 presents returned
  `VK_ERROR_SURFACE_LOST_KHR` (`run002/present-A-kmt.csv`, result -1000000000) because `D3DKMTPresent` returned
  `0xC000000D STATUS_INVALID_PARAMETER` (`run002/vkcube-A-kmt.stderr.wddm2-lines.txt`, first 8 failures printed, 8
  distinct source handles). `SubRectCnt == 0` or `pSrcSubRects == NULL` with `Flags.Blt` is a documented
  INVALID_PARAMETER condition of the thunk (`ref/ddi-display/d3dkmthk.md`, D3DKMTPresent remarks). No message about
  the present context or the wait, so `CreateContextVirtual` and `WaitForSynchronizationObjectFromGpu` succeeded;
  the LB7A allocations were created (the KMD ring shows their vidmm traffic and no allocation failure).
- wsi-kmt-003 (11BA2143): parameter validation passed; every present returned
  `0xC01E0342 STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE` (SDK 10.0.26100 `shared/ntstatus.h` line 18150), again
  `VK_ERROR_SURFACE_LOST_KHR` for all 900 presents. The KMD's `DxgkDdiPresent` was never reached: the blit counters
  read `blit gate open, 711 blits, 0 skips, 711 sources translated contiguous` before, between the stages and after
  both runs (`run00{2,3}/*log-summary.blit-lines.txt`), so dxgkrnl or win32k rejects the windowed Blt before the
  miniport. The status is the one the documentation attaches to VidPn-source ownership (`D3DKMTSetVidPnSourceOwner`,
  `D3DKMTCheckVidPnExclusiveOwnership`): the DWM composes the desktop and owns the source, and a windowed Blt without
  a redirection token is evidently routed to the primary.
- The signal after a failed present kept the render queue's wait satisfied: vkcube ran its 900 frames in 4.1 s
  (present interval median 4.3 ms) and exited 0. vkcube recreates the swapchain on `SURFACE_LOST`, which is why
  the 900 presents are spread over 4 to 5 chains and the failures name 8 different source allocations. The window
  stayed blank (`capture-hashes.json`, `run00{2,3}/A-kmt-0.png`: an empty client area over the desktop).
- Stage B (software path, the control) rendered normally in both runs: 900 presents `VK_SUCCESS`, one chain,
  present interval median 16.8 ms, p95 47.0-47.4 ms; per present CPU copy 0.15 ms, BitBlt 0.36-0.38 ms, DwmFlush
  median 15.8 ms, p95 45.8-46.2 ms (`present-stats.json`). The p95 is three refresh periods: the DwmFlush after a
  missed composition is the "slowing down" seen at the monitor.
- PresentMon produced no CSV in any run: neither the GDI presents of the software path nor the rejected KMT
  presents emit DXGI or dxgkrnl present events (as M577).

## Reading

- What works: the candidate loads through the in-place swap, the LB7A-described linear device-local swapchain
  images allocate and get opened by the KMD, the non-UMD present context exists, the GPU-side wait/signal pair
  on the WSI timeline is accepted on that context and keeps the frame loop alive without a CPU wait.
- What does not: dxgkrnl rejects a windowed `D3DKMTPresent(Blt)` from a non-D3D-runtime device while the DWM
  composes. The documented OpenGL-ICD example (`hDevice`, `hWindow`, `Blt`, one sub-rectangle) is the
  pre-composition contract; under composition a present has to name a redirection surface (a
  `D3DKMT_PRESENTHISTORYTOKEN` of the redirected Blt, GDI or composition models) that the DWM can open. Which of
  these models applies, and what the DWM's user-mode driver must be able to open, is the next desk step
  (`scratch/wsi-kmt-2026-09-27/research-windowed-present-under-dwm.md`); no further lab time before a candidate
  has a concrete reason to behave differently.
- Not established by this evidence: the KMD engine Blt (never entered), any present timing of the KMT path, any
  composition of the image.

## Files

- `patches/`: the two fork commits (MIT, Mesa).
- `scripts/`: run, launch, worker, status and cleanup scripts of 003 (002 differs by candidate hash and header only,
  `make002.py`/`make003.py` generated them), `labcheck.ps1`, `pull-003.py`, `finish-e45.py`.
- `run001/`, `run002/`, `run003/`: run logs (UTF-8 copies), done records, loaded-module records, KMD health and
  clock before and after, the blit lines of the KMD log summaries, present logs of both stages, the winsys lines of
  vkcube's stderr, `logman stop` output. `present-stats.json` summarises the present logs.
- `capture-hashes.json`: SHA-256 of the screenshots (desktop content, private, kept in the scratch run directories).
