# E46: the windowed KMT present is refused by dxgkrnl at admission (wsi-kmt-004, DxgKrnl trace)

Unit A, Windows 11, 2026-09-27 04:18-04:19 UTC (06:18-06:19 local). Same procedure as E45 (`scripts/run-kmt-004.ps1`:
registered ICD 93B1D1FD swapped for the candidate and restored with a hash check, exit 0; vkcube 900 frames on the
KMT path, then 900 on the software path as the control; KMD health, clock and log summary around each stage). UMD
8279AC7F, KMD 152 (generation 575721647, epoch 165 before and after), CPU DWM 4596, Tctl 67.6-69.5 C, BlitGate open.
Two additions: the candidate logs `D3DKMTCheckVidPnExclusiveOwnership` on source 0 once, and a raw
`Microsoft-Windows-DxgKrnl` trace (own session `BC250WSIKmt004`, all keywords, level 5, 1 MB buffers, 128 to 512 of
them, sequential file) ran around both stages. Task removed, no ETW session left, only the overlay and watchdog tasks
running afterwards (`cleanup-kmt-004.ps1`).

## Candidate

`vulkan_radeon.dll` 0CD4A98DD80AE1248CFA1E6D4C1EB650DCF217FA9A840C002EDC4BB7EB0ADB47 = E45's 11BA2143 plus fork commit
1d7ac9ca (`patches/`): the ownership query next to the present-context creation, printed once per process.

## Results

- Ownership query: `wddm2: present context 0x40001840 created; VidPn source 0 exclusive-ownership check 0x0`
  (`run004/vkcube-A-kmt.stderr.wddm2-lines.txt`). Within the query's documented scope (a display mode manager
  client holding the source) nobody holds source 0 exclusively.
- The present still fails: all 900 KMT presents `VK_ERROR_SURFACE_LOST_KHR` after `D3DKMTPresent` returned
  `0xC01E0342` (`present-A-kmt.csv`, the eight printed failures name the same source handles as E45's run 003);
  vkcube completed in 7 s and exited 0; the KMD blit counter read 721 before, between and after the stages
  (`run004/*.blit-lines.txt`); the software-path control rendered normally, 900 `VK_SUCCESS`
  (`present-stats.json`).
- Trace (`run004/dxgkrnl-present-events.csv`, decoded with `scripts/decode-004.ps1` and the export step recorded in
  `decode.log`; raw trace private, hash in `capture-hashes.json`): dxgkrnl's Present event (task 107, event 184)
  for the vkcube process (pid 13088) occurs 900 times between 06:18:09.253 and 06:18:14.026 with
  `hWindow = 0x4904d0`, `VidPnSourceId = 0`, `FlipInterval = 0`, `Flags = 193` (0xC1 = Blt | DstRectValid |
  SrcRectValid), `hSrcAllocHandle` = the ICD's allocation handle, `hDstAllocHandle = 0` and
  `ReturnStatus = 3223192386` (0xC01E0342), every time. There is no `Blit_Info` (event 166) and no `BlitCancel`
  (event 44) for any process in the trace, and no PresentHistory event (171, 172, 215) for the vkcube process.
  In the same trace the DWM (pid 4596) presents 982 times with `Flags = 0x401004` (Flip, PresentCountValid) and 9
  times with `Flags = 0x8000` (RedirectedFlip), all status 0, with PresentHistory tokens of model 7
  (`D3DKMT_PM_REDIRECTED_COMPOSITION`); one other process (pid 4964) presents 10 times with `Flags = 0x8000`,
  `hWindow = 0`, status 0, with tokens (`dxgkrnl-presenthistory-events.csv`).

## Reading

- dxgkrnl receives the present with exactly the parameters the ICD sets and refuses it in the admission stage,
  before any blit routing: a refused blt still leaves the Present event with its return status, but no blit event,
  no cancel and no token. `DxgkDdiPresent` is never involved (the KMD counter agrees).
- The refusal is not the documented exclusive-ownership case: the ownership query from the same process says the
  source is free while the present is refused 900 times in the same seconds. What dxgkrnl checks before a windowed
  Blt from a device that is not the source's owner is not documented. The reading of E45 ("no GPU redirection
  surface, so the blt is a legacy copy to the front buffer, which the DWM's ownership forbids") remains the
  candidate explanation, now narrowed to an ownership that the exclusive-ownership query does not report (the
  DWM's shared or composition ownership), and it stays a hypothesis until a KMD that advertises CDD-DWM interop
  and implements its obligations is measured with the same trace (facts M601; the design note's plan items 3-5).
- The ICD side of ADR 0018 item 2 is complete up to this point (allocation, context, GPU-side wait and signal,
  present parameters as documented and as traced); no further ICD-side lab run is planned before that KMD exists.

## Files

- `patches/0001-*.patch`: fork commit 1d7ac9ca (MIT, Mesa).
- `scripts/`: run, launch, worker, status and cleanup of 004 (generated from 003 by `makenext.py`), `pull-004.py`,
  `decode-004.ps1` (Blit_Info, PresentHistory and Present event decode on the development PC), `finish-e46.py`.
- `run004/`: run log, done record, loaded modules, KMD health and clock before and after, blit lines of the KMD log
  summaries, present logs of both stages, winsys lines of vkcube's stderr, `logman` start/query/stop outputs, the
  decoded Present events (all processes) and PresentHistory events as CSV. `present-stats.json` summarises the
  present logs.
- `capture-hashes.json`: SHA-256 of the screenshots and of the raw 367 MB trace (both private, kept in the scratch
  run directory and on the target).
