# The game arm of `EnableRingVmFlush`: The Witcher 3 Remaster at gate 0 and at gate 1

Date: 2026-10-11, unit A, Windows 11 Pro. Two interactive game sessions, one at each gate of
`EnableRingVmFlush`, on the installed package `0.7.216.100-tester.28`. This is the open game arm of
`docs/design/gfx-submit-root-serialization.md` section 3.

The result: at the same GPU clock the frame time is the same at both gates. At 1300 MHz the mean
frame time is 70.33 ms at gate 0 and 70.23 ms at gate 1. At 1200 MHz it is 75.12 ms and 74.92 ms.
At gate 1 the driver reported no GPU fault, no fence timeout, no `ResetEngine` call, no
`wddm: timeout` line, no TDR and no `Display` 4101 event. This is one pair. It cannot resolve a
difference smaller than about 0.5 ms per frame (0.7 % of the frame time).

## The stack under test

| Part | Identity (SHA-256) |
|---|---|
| Kernel driver `bc250kmd.sys`, KMD 0.7.216.32, ABI 0x000700D8 | `DC68A504154DBA4F2180F75C2A0A6E203238961363DEE1E1873F487AB82AF424` |
| D3D12 shell `amdgpu_wddm_d3d12.dll` | `DDA920E7A6E2F8047E30A68B20A343BE4D624A85449BC2AF194144FC8E92274F` |
| D3D12 engine `amdgpu_wddm_vkd3d.dll` | `80217882D8BEB3BB0115E006034A380D12ED03BD10398EBC372A78027178CBA6` |
| D3D12 ICD `amdgpu_wddm_radv.dll` | `F43E4392148FCD4823BA928B644E02FA528B9B29ED8E548EF091D2878A2820F5` |

The package installer installed all four files. The sessions swapped nothing in. Other settings:
`EnableVmidPool` 1, `HangRecoveryMode` 1, DPM 1 with a 2000 MHz ceiling and a 500 MHz idle state,
40 compute units, `TdrDelay` 10. The KMD drove the fan on its curve. On this KMD the default of
`EnableRingVmFlush` is off: gate 0 is the value absent, and gate 1 is the value 1. Each gate was
set before a Windows restart, because `GfxStart` reads the value once.

## Method

- **Game.** The Witcher 3 Remaster, D3D12, 1920x1080 exclusive fullscreen, HIGH with ray tracing.
  VSync off, frame limit 240. No upscaler, no frame generation, no dynamic resolution.
- **Scene.** CONTINUE loaded the same save in Kaer Morhen in both sessions. Nothing moved the
  camera or the character after the load. The shots `sessions/shot-*.jpg` (half scale) show the
  same view before and after each window.
- **Measurement window.** One `world` command opened a 120 s DxgKrnl capture (ETW window B).
  The operator sent no command during the window.
- **Clock.** The KMD DPM sampler read the clock, the temperature and the power once a second
  (`sessions/dpm-game-N.txt`). It writes the lab's local time, which is UTC+2 on this date.
- **Safety.** A thermal guard stops the game at Tctl 87 C held for 10 s, or at once at 89 C
  (`sessions/tempguard-N.out`). The smart plug was read every 12-15 s (`sessions/plug-N.log`).

| Trial | Gate | Boot (UTC) | Window B (UTC) |
|---|---|---|---|
| native-caps559 | 0 (absent) | 18:25:59 | 20:33:42-20:35:43, 120.5 s |
| native-caps560 | 1 | 20:44:04 | none: the trial was refused before the game started |
| native-caps561 | 1 | 20:44:04 | 20:58:59-21:00:59, 120.0 s |

The trial preflight refused trial 560 with `Capture: Competing test task`
(`sessions/trial-560.log`). Nothing was changed on the lab, and the game did not start. The cause
was in the session harness: the trial's own ETW capture task started while the trial's Capture
phase was still listing the running tasks (`sessions/tasks-560-admission.txt`: `BC250-Etw560`
last ran at 20:47:47). The harness now registers the ETW task only after the Capture phase is done.
Trial 561 repeated the session on the same boot.

## Results

| | Gate 0, trial 559 | Gate 1, trial 561 |
|---|---|---|
| Presents in the window | 1752 | 1615 |
| Presents/s | 14.60 | 13.60 |
| Frame time p50 / p95 / p99 / max | 68.05 / 76.97 / 78.23 / 125.58 ms | 72.47 / 83.57 / 86.88 / 89.28 ms |
| Mean GPU clock (range) | 1344 MHz (1200-1500) | 1225 MHz (1100-1300) |
| Presents per GHz (estimated: model, rate follows the clock) | 10.87 | 11.10 |
| Presents per GHz (estimated), four 30 s quarters | 10.71 / 10.85 / 10.91 / 11.02 | 11.25 / 10.92 / 10.92 / 11.26 |
| GPU busy (DMA union) | 99.5 % | 99.6 % |
| Tctl mean / max in the window | 83.7 / 84.8 C | 83.8 / 85.0 C |
| Plug mean / max in the window | 166.6 / 181.3 W | 158.2 / 170.0 W |
| Plug maximum of the session | 224.4 W | 218.5 W |

The frame time at matched clock (`analysis/clockmatch-559-561.txt`):

| Clock | Gate 0 | Gate 1 | Difference |
|---|---|---|---|
| 1300 MHz | 70.33 ms, 273 frames in 19 s, SE 0.21 ms | 70.23 ms, 952 frames in 67 s, SE 0.14 ms | -0.10 ms |
| 1200 MHz | 75.12 ms, 390 frames in 30 s, SE 0.24 ms | 74.92 ms, 54 frames in 4 s, SE 0.86 ms | -0.20 ms |

`clockmatch.py` assigns each frame interval of the game to the 1 s DPM sample in force at its end.
It counts only stretches where the samples before and after show the same clock. SE is the
standard error of the 1 s means at that clock.

**Why the raw rates differ.** Both windows ran at the thermal limit, where the KMD thermal policy
sets the clock. The window of trial 559 opened about 40 s after the world loaded. The window of
trial 561 opened an estimated 2.5 to 3 minutes after it, because an sshd accept stall at the game
start (BD-051, 20:55:23-20:57:54) delayed the `world` command. The board was hotter by then, so the
clock was lower, and the rate followed the clock.
The model of presents per GHz does not remove this difference in this scene. The estimated rate per GHz
rises as the clock falls, from 10.71 at 1457 MHz to 11.02 at 1243 MHz inside trial
559. The matched-clock table holds the clock constant.

## The driver reads

| | Gate 0 after 559 | Gate 1 at boot | Gate 1 after 561 | After the rollback |
|---|---|---|---|---|
| `EnableRingVmFlush` | absent | 1 | 1 | absent |
| Start line | `gfx: VM flush by MMIO before the frame (EnableRingVmFlush 0), engine 17` | `gfx: VM flush on the ring (EnableRingVmFlush), engine 1` | the same | `... engine 17` |
| `wddm summary: ring VM flush` | `off: 0 frames` | `on: 1037 frames, 1030 ...` | `on: 64417 frames, 64405 of them on a root the VMID held` | `off: 0 frames` |
| Node 0 hardware submissions | 79229 | 1037 | 64417 | 992 |
| VMID pool claims / reuses / busy / rule refusals | 17 / 79212 / 0 / 0 | 7 / 1030 / 0 / 0 | 12 / 64405 / 0 / 0 | 7 / 985 / 0 / 0 |
| GPU faults, fence timeouts, `ResetEngine`, `wddm: timeout` | 0, 0, 0, 0 | 0, 0, 0, 0 | 0, 0, 0, 0 | 0, 0, 0, 0 |
| TDR | none | none | none | none |
| `Display` events since the boot (4101) | 0 (0) | 0 (0) | 0 (0) | 0 (0) |
| Health flags | 15 | 15 | 15 | 15 |

The ring-flush frame count equals the node-0 hardware submissions at gate 1, which is the check of
the design's step 4. The per-frame MMIO flush line counted 0 at both gates. Section 3 of the design
notes that this check does not discriminate, so the summary line carries the answer. No bugcheck or
unexpected shutdown event was logged before or since either boot.

After each game session the node-0 summary shows fewer completed than submitted jobs (559: 79229
submitted and 77864 completed, 561: 64417 and 63431), with 0 timeouts. At the boot reads the two are
equal. Both gates show this, so it does not depend on the gate. This directory does not explain the
counter.

## Files

- `reads/`: the gate read (`ringflush.ps1 -Step read`) before and after each session and after the
  rollback, and the set, clear and restart steps.
- `state/`: the lab state before, between and after the sessions (`state.ps1`).
- `sessions/`: plans, harness trial logs, ETW notes, the presents of the game process
  (`presents-N.csv`, microseconds from the trace zero), the DPM sampler, the thermal guard, the plug
  samples, the task list of the refused trial 560, and four half-scale shots.
- `analysis/`: `analyze.py` (window, quarters, clock, the estimated presents per GHz), `clockmatch.py` (matched
  clock), `plugsum.py`, their outputs, and the two lab scripts `ringflush.ps1` and `state.ps1`.

The ETW dumps are not in the directory, because of their size. The analysis read
`gpu-B-dump.txt` of trial 559 (364769789 bytes, SHA-256
`8432611f01e251f4d0b9e651ddf7c52310d91c9883766a0dedd0d93c11184a7b`) and of trial 561 (304230640
bytes, SHA-256 `9d557deafeef5329d64330f036328d79dd46a0bd96a8b89d62939dcc95b0174d`). `presents-N.csv`
holds the present times of the game process that `clockmatch.py` reads from them. The scripts name
the paths of the workspace in which they ran. `sha256.txt` lists every file of the directory.
