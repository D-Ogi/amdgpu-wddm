# E49: The Witcher 3 started through the Steam client on the registered driver, high with RT (M15.7)

Date: 2026-10-01, 14:31-14:41Z (session 255; 254 at 14:20-14:27Z did not start the game). Unit A, Windows 11 Pro.

- Kernel driver 0.7.193.1 (SYS E3F75D80, ABI 0x000700C1), DPM on with the 2000 MHz ceiling, thermal limits unchanged.
- D3D12 triplet registered since 14:12Z: shell 0CE9D4F8 (adapter109 = bc250-win daa465f5 on
  m15/adapter109-app-profile: adapter107's release gate plus the application profile of the experiment list),
  engine 15E3E24E, ICD 2A13235D. Nothing was swapped for the session.
- Application profile: `HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\witcher3.exe`, REG_SZ `Experiment` =
  `present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff`. The shell reads it once per
  process, only when the process has no `AMDGPU_WDDM_D3D12_EXPERIMENT` variable. A game started by Steam has none.
- Desktop composed on the GPU. Game build 25646871 (witcher3.exe 9406ECCC, 5.0.0.1044392, installed by Steam at
  14:25:59Z; every earlier session ran 5.0.0.1041720). Native 1920x1080 fullscreen, FXAA (no upscaler), frame
  generation and dynamic resolution off, LimitFPS 60, preset high with RT.

Method: the native-caps kit's game session with the launch mode `steam`. The runtime in the user's session asks
the running Steam client for `-applaunch 292030 --launcher-skip` and records the process chain. It refuses to
launch unless the app manifest reports StateFlags 4 (installed, no update pending). The trial sets no experiment
switch. The rest is E47's method: the Kaer Morhen route with the remote input channel, window B (40 s of
DxgKrnl present intervals), Task Manager's GPU engine counters once a second, and the HTTP temperature guard.

## Launch (launch-255.txt)

| t | event |
|---|---|
| 14:32:58.84Z | experiment none (the trial's variable), application profile found with the five switches |
| 14:32:58.95Z | Steam state flags 4, build 25646871 |
| 14:32:59.06Z | `steam.exe -applaunch 292030 --launcher-skip` |
| +2 s | REDprelauncher.exe (Steam's only launch entry for the app) |
| +4 s | witcher3.exe in session 1; no REDlauncher, no installer |
| +41 s | amdgpu_wddm_d3d12.dll 0CE9D4F8, amdgpu_wddm_vkd3d.dll 15E3E24E, amdgpu_wddm_radv.dll 2A13235D loaded from the registration directory; game window |
| +87 s | main menu |

## Result

| session | preset | window B frames/s | median / p99 / max ms | 3D engine in window B | clock in window B | Tctl peak |
|---|---|---|---|---|---|---|
| 255 (Steam start) | high + RT | 9.1 | 109.24 / 197.55 / 231.6 | 98.3 % (min 90.2 %) | 2000 MHz | 86.8 C |
| 250 (E47, direct start) | high + RT | 9.3 | 107.02 / 195.34 / 220 | 97.8 % | 2000 MHz | 86.5 C |

- Closed functional-restored: the operator's quit ended the game at 303 s, the tree closed, baseline and game
  settings were restored.
- The image was correct, with RT on: reflections on the floor of the starting room, the valley through the
  balcony door, shots 001 and 003-011.
  - The trial set no switch, so the game's RT came from the application profile alone. Without
    `raytracing-tier` the shell reports no ray tracing support.
- No bugcheck, no fence timeout, no TDR. The KMD summary (`kmd-log-after-255.txt`) reads 0 timeouts, 0 refused,
  "no TDR", and 9940 hardware flips.
- Dedicated GPU memory peaked at 984 MB and shared memory at 550 MB (250 read the same way: 962 and 554 MB).
  Machine CPU was 48 % in window B, DWM 2.8 % of one CPU.
- Reading: the Steam client path reaches the same rendering and frame rate as the direct start (-2 %, within the
  spread between sessions), on a newer game build. High with RT stays GPU-bound.
- Before the gate, session 254 (`result-254.md`) showed two failure modes of a Steam start:
  - `-applaunch` with an update pending runs the update first and launches the game later, outside the trial.
  - REDprelauncher installs or runs REDlauncher before the game unless it is given `--launcher-skip`.

Facts: M777.
