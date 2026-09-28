# E49: registered ICD baseline promoted from 93B1D1FD to CF3948D6 (M665)

M665, unit A, 2026-09-27 (lab clock 14:05-14:10 local, UTC+2; boot 2026-09-27T12:58:57.5+02:00 unchanged, no
reset, KMD 164 exact source unchanged, CPU UMD 8279AC7F untouched, DWM 3152 untouched, no registry change,
GPU Present and CDD interop gates 0 throughout). Owner's decision after E47/E48: the candidate that carries the
present-mode work becomes the registered file instead of living as a swap-in candidate.

## Why

Since E38 the registered Vulkan ICD `C:\BC250\m10\wsi-final\vulkan_radeon.dll` was 93B1D1FD (fork c34ab7cd,
D3DKMT adapter enumeration). All present-path measurements since then (E43, E46, E47, E48) ran on candidates
copied over that path and restored afterwards, so every runner carried its own swap and restore and the
measured code was never the code an application picks up by default. The IMMEDIATE present mode on the GDI
path (E47 run 007) and the per-present timing log (E43 onward) had been exercised for hours on the same KMD
generation without a present failure, a smoke failure or a device loss attributable to the ICD.

## Candidate identity and lineage

CF3948D692CAF56FFCFFEA6EBCA5F0E5FDB517FDC55062EFAE389515A373D157 (24258560 bytes) is the fork branch
`amdgpu-wddm/radv-wddm2-gdi-immediate` at 2732f9c8, three commits on the E38 baseline c34ab7cd (93B1D1FD):

| commit | subject | measured in |
|---|---|---|
| 59f53088 | vk_wddm2: create exportable monitored fences with NtSecuritySharing | E43 (present log runs) |
| 0f9811a5 | wsi/win32: optional per-present timing log (`BC250_WSI_PRESENT_LOG`) | E43, E46, E47, E48 |
| 2732f9c8 | wsi/win32: IMMEDIATE present mode on the GDI path (no DwmFlush wait) | E47 runs 006-008 |

The timing log is inert without the environment variable. IMMEDIATE only changes behaviour for a chain that
asks for it: the header of every present log names the mode, and vkcube's default chain stays FIFO (below).
Build recipe: E47 `scripts/vulkan_radeon.CF3948D6.recipe.json` (`tools/build/build-mesa.ps1 -Config radv`).
The later commits of the same branch (host-cached memory preference, memory-type witness, copy cycles,
63AF86CB) are witnesses for E48 and were deliberately not promoted.

## Preflight (promote002/preflight.json)

`scripts/preflight-icd002.ps1`, 12:05:24Z, read only: STOP clear; registered file 93B1D1FD (written 11:57:27Z
by the DWM029 rollback), keep copies 93B1D1FD and 9C40083C present with matching hashes, 29 DWM-held copies
untouched; manifest `radeon_icd.json` 234175BD, `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` one entry, value 0;
eight candidates in `C:\BC250\m12\icd-candidates` hash checked, CF3948D6 among them; no BC250 test task
running (overlay and net watchdog only), no Vulkan process, DWM 3152, Tctl 66.6, health 15, guard 0, gates 0.

## Promotion (promote002/promote002.log)

`scripts/promote-icd002.ps1`, 12:07:02-12:07:20Z, the E38 procedure with the hashes changed:

1. preconditions as in the preflight, re-checked; candidate `vulkan_radeon.CF3948D6.dll` hash checked;
2. rollback: the M569 keep copy `vulkan_radeon.93B1D1FD.dll` next to the active file was accepted after a hash
   check instead of writing a second copy (rollback is `Copy-Item` of that file over the active one);
3. candidate copied over the active file, hash CF3948D6 confirmed; a keep copy `vulkan_radeon.CF3948D6.dll`
   written next to it (restore source for candidate swaps);
4. registry and manifest unchanged (listed before and after);
5. positive control on the promoted file in place: E14 compute smoke through the recorded runner
   (`promote002/smoke-config.json`, `smoke/receipt.json`, `smoke/stdout.txt`): PASS, 8 tests, 0 mismatches,
   live module witness CF3948D6 at the registered path, boot unchanged; DWM 3152 and Tctl 66.6 before and after.

## Control 2: vkcube on the changed code path (gdipromo002/)

`scripts/run-gdipromo002.ps1` (interactive scheduled task `BC250-M13-gdipromo002`, `launch-`, `worker-`,
`status-gdipromo002.ps1`; the cpucopy-003 mechanics of E48), 12:09:32-12:10:13Z: vkcube 87A96AF3, 900 frames,
1920x1200 window, registered ICD in place, no swap, no restore, `BC250_WSI_PRESENT_LOG` set, default present
mode, live module witness, STOP flag and Tctl polled every 2 s, 60 s bound, screenshot at 4 s.

| item | result |
|---|---|
| exit, elapsed | 0, 41 s (bound not reached) |
| modules at 2 s | `vulkan-1.dll` 5C42CA8E, `vulkan_radeon.dll` CF3948D6 (registered path, `modules-promoted-1920x1200.json`) |
| present log header | `1920x1181 format 44 images 3 path gdi mode 2` (FIFO: the default chain does not pick IMMEDIATE) |
| presents | 900, all VK_SUCCESS, span 38.7 s, 23.2 fps |
| copy_ms (host buffer into the DIB) | median 1.06, p5 1.05, p95 1.09, max 3.90 |
| blit_ms | median 1.44, p95 1.58 |
| dwmflush_ms | median 39.45, p95 50.51 (FIFO wait, as in every FIFO run) |
| KMD | health 15 before and after, completed 968 -> 1890, 1000 MHz / VID 116, no log growth beyond the run |
| DWM, Tctl | 3152 before and after; 66.6 -> 67.1 |
| screenshot | `promoted-1920x1200.png` SHA256 a28097ee24b636f1686fe2d7a256bd4cd9fcbdbc9b96ac7c0dedce3d72bb9c08, outside Git (desktop content) |

The copy median equals cpucopy-003 (1.07 ms on 63AF86CB, E48) and the E47 run-006 FIFO shape, so the
promoted file behaves like the candidates measured before it. `scripts/analyze-present-log.py` is the E48
copy. Task unregistered, no vkcube process left, present CSV and stderr in `gdipromo002/`.

## State after

Registered ICD CF3948D6. Rollback file `vulkan_radeon.93B1D1FD.dll`, older `vulkan_radeon.9C40083C.dll` and
the keep copy `vulkan_radeon.CF3948D6.dll` next to it. `STATE.md` (local) rewritten. Scripts that assert
the registered hash must expect CF3948D6: the DWM029 runner (`run.ps1`, `cleanup.ps1`, `archive.ps1` under
`experiments/E34-native-d3d-zink/hosted-runtime/dwm029-gpu-present/`) still names 93B1D1FD and is immutable
as run; its successor was bound to CF3948D6. The hosted trials' mechanism (7A9970CA
copied over the registered path, restore from their own baseline copy) is unaffected. Nothing else changed.

## Limits

Qualification is the E14 smoke and one FIFO vkcube run on the same boot, plus the E43/E47/E48 runs of the same
file as a swap-in candidate. IMMEDIATE itself was not re-run after promotion (E47 run 007 is its measurement).
No DWM probe, no hosted D3D, no game after promotion. Rollback is a file copy, untested in anger.

PROVENANCE: Mesa MIT; procedure and scripts from E38 and E48; scripts original.
