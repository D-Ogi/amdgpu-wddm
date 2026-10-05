# E44: Win32 external-semaphore CTS cases, baseline against the fence-share candidate (M583)

Unit A, Windows 11, 2026-09-27 02:14:57-02:15:23 UTC (04:14-04:15 local). Registered ICD 93B1D1FD (E38 baseline)
before and after, candidate 6661C2D2 (E43: fence-share 59f53088 plus present log 0f9811a5 on c34ab7cd) copied over
the registered file for phase 2 only. UMD 8279AC7F, KMD 152 (generation 575721647), CPU DWM 4400, lab operating
point unchanged, Tctl 68.2 to 68.4 C. No promotion, no UMD/DWM/KMD change.

## Setup

- Case list `scripts/cts-external-semaphore-win32.txt`: the 44 `dEQP-VK.api.external.semaphore.opaque_win32.*` and
  `.opaque_win32_kmt.*` cases of the full CTS case list. The full list has no `d3d12_fence` semaphore group; the
  slot request that named one was wrong on that point.
- `deqp-vk` from cts-release-tools (AE7BEFDD, hash checked), one process per case with `--deqp-case`,
  `--deqp-log-filename`, `--deqp-watchdog=enable`, a 45 s bound, `VK_LOADER_DEBUG=all` kept in `NNN.err`, status
  parsed from `<Result StatusCode>` in the `.qpa`. STOP flag and Tctl checked before every case.
- Phase `baseline` on 93B1D1FD, then the candidate copied in (`icd_swapped=6661C2D2`), phase `candidate`, then the
  registered file restored from the run-directory copy and hash-verified (`registered ICD restored to 93B1D1FD`).
- Run 001 (`cts001/`) is the void first attempt: this `deqp-vk` build rejects `--deqp-watchdog-total-time-limit`
  and `--deqp-watchdog-interval-time-limit` ("Unrecognized command line option", exit -1 on all 44 cases). Run 002
  differs only by dropping those two options.

## Result

| phase | ICD | Pass | NotSupported | Fail / Crash / timeout | wall time |
| --- | --- | --- | --- | --- | --- |
| baseline | 93B1D1FD | 4 | 40 | 0 / 0 / 0 | 11.7 s |
| candidate | 6661C2D2 | 4 | 40 | 0 / 0 / 0 | 11.6 s |

No per-case difference between the phases (`cases.jsonl`). The four passes are `opaque_win32.info_binary`,
`opaque_win32.info_timeline`, `opaque_win32_kmt.info_binary`, `opaque_win32_kmt.info_timeline`. Every process
loaded `C:\BC250\m10\wsi-final\vulkan_radeon.dll` (loader log and module check per case). KMD health `completed`
27480 before, 27521 after, no reset; no `deqp-vk` process left; the scheduled task was unregistered.

## Reading

All 40 NotSupported results carry the same reason, `Semaphore doesn't support exporting in external type`
(`vktApiExternalMemoryTests.cpp:398`: `externalSemaphoreFeatures` lacks the EXPORTABLE bit for the handle type).
The info logs show what the ICD advertises (candidate `011.qpa`, `012.qpa`, `033.qpa`, `034.qpa`, identical on
the baseline):

| handle type | semaphore type | exportFromImported / compatible | features |
| --- | --- | --- | --- |
| OPAQUE_WIN32 | binary | none | none |
| OPAQUE_WIN32 | timeline | D3D12_FENCE and OPAQUE_WIN32 | EXPORTABLE and IMPORTABLE |
| OPAQUE_WIN32_KMT | binary | none | none |
| OPAQUE_WIN32_KMT | timeline | none | none |

Source of that shape (fork c34ab7cd, unchanged by the candidate): `src/vulkan/runtime/vk_semaphore.c` derives the
handle types from the selected `vk_sync_type`; `vk_wddm2_monitored_fence_type` (timeline, exports and imports NT
handles) is the only type with `export_win32_handle`/`import_win32_handle`, binary semaphores are served by the
`vk_sync_binary` wrapper around it, which has no import or export entry points, and `vk_semaphore.c` never maps
`OPAQUE_WIN32_KMT` to a sync type. The 40 functional cases in this group create binary semaphores, so the CTS
reaches the fence-share change (`vk_wddm2_monitored_fence_init`, sharing flags at creation) only through the info
query. The runtime witness for that change remains M581 (vkd3d-proton's D3D12_FENCE timeline export, 4 errors to 0).

Consequences: the candidate introduces no regression in this group; binary-semaphore export (OPAQUE_WIN32 and
KMT) is a feature gap of the port shared with the baseline, to be weighed against what DXVK and vkd3d-proton
actually request before any work is spent on it.

## Files

- `cts002/`: `cases.jsonl` (one row per case and phase: status, exit code, duration, ICD seen in the module list
  and in the loader log), `baseline-summary.json`, `candidate-summary.json`, KMD health before and after, the run log
  (UTF-8 copy), the done file, and per phase `NNN.qpa`, `NNN.out`, `NNN.err` for all 44 cases.
- `cts001/`: the void run 001 (rejected options), its log and summaries.
- `scripts/`: run scripts for 001 and 002, worker, launch and poll scripts, the case list, the pull scripts and the
  finishing script that wrote `manifest.json` and the facts row.
