# C62 thermal runs on unit A under Windows (2026-10-07): grid sweep and open-loop identification

Unit A, Windows 11, BIOS fan setting unchanged (the driver takes the fan only for the time of a lease). Every
trial ran over SSH as one bounded script (`python tools/win/target.py ps ...`). Each trial has a thermal stop:
Tctl at or above 87 C for 10 s, or 89 C once. The dev PC sampled the smart plug during each trial
(`*.plug.jsonl`, `plug.py telemetry`, TinyTuya scales, model calibration unverified, measurement age unknown).

Telemetry is one `bc250kmd_cli telemetry` call about every 1.7 to 2 s. `dpm ... temperature_c` is the GPU sensor,
`power_w` is the SMU socket power as the KMD reports it (`SocketPowerMw`), `fan ... tsi_c` is Tctl through the
board's hardware monitor, `rpm` and `duty_pct` are the fan tachometer and the duty read-back.

## Part 1: grid sweep (`grid/`)

`grid/sweep.py` (dev PC) ran `grid/c62-trial.ps1` once per cell, with `grid/c62-cool.ps1` between cells (fan
100 % until Tctl < 62 C and GPU < 60 C). Kernel driver escape ABI `0x000700D7`, `DpmMaxMHz` 1500. Load: `d3d11bench`
offscreen fill, 1920x1080, 32 layers, plus 2 busy CPU threads, 75 s. Cells: fan `f0` = the driver's Standard curve,
GPU V/F offset `g0` or `g25` (25 mV, `dpm curve offset`), CPU undervolt `c0` or `c4` (4 curve steps,
`cpu set uv`). The time box ended the sweep after 4 of the 12 cells (`grid/sweep.log`, 03:11:56Z to 03:23:14Z).
`grid/fit.py` wrote `grid/fit.md` and `grid/fit.json`.

| Cell | MHz mean | MHz last 30 s | GPU C max | Tctl max | SMU W mean | SMU W last 30 s | wall W mean | stop |
|---|---|---|---|---|---|---|---|---|
| f0-g0-c0 | 988 | 800 | 87.8 | 87.0 | 105.2 | 89.5 | 163.1 | Tctl >= 87 for 10 s, 71 s |
| f0-g25-c0 | 963 | 800 | 86.9 | 87.0 | 98.3 | 84.7 | 163.2 | time, 75 s |
| f0-g0-c4 | 984 | 800 | 86.0 | 86.0 | 101.4 | 84.3 | 159.9 | time, 75 s |
| f0-g25-c4 | 1030 | 800 | 87.6 | 87.0 | 102.4 | 82.5 | 171.4 | time, 75 s |

What the grid shows:

1. Every cell starts at 1500 MHz and ends at 800 MHz, the lowest point of the table. The thermal policy reaches
   800 MHz 29 to 49 s after the first sample (the 10 s samples of the `slow` CSV in each run file). The first
   clock step comes after 10.5 to 12.3 s.
2. The fan ran at full duty in every cell under the Standard curve: `duty_pct` 100 in 40 to 43 samples per cell
   (97 or 98 in at most 2), 1612 to 1741 rpm. A fixed fan lease of 100 % cannot add air flow here.
3. The GPU offset lowers the voltage at the high points only. `g25` reads 1500 MHz at 894 mV against 919 mV, and
   1100 MHz at 820 mV against 840 mV. At 800 MHz all four cells read 820 mV (VID 116).
4. In the last 30 s the SMU socket power is 89.5 W in the baseline cell and 82.5 to 84.7 W in the three offset
   cells (4.8 to 7.0 W lower). The clock is 800 MHz in all of them. There is one trial per cell, so these files
   do not separate the difference from the spread between trials.
5. The thermal stop fired in the baseline cell only. No WHEA event and no event 4101 in any cell.
6. The per-cell fit of `grid/fit.md` (first-order model over the samples before the first clock step) gives R
   values from -0.0107 to 0.0132 C/W and tau 5 to 6 s. These values do not describe a thermal path (negative R).
   The grid runs therefore do not identify the thermal model. Part 2 does that with fixed clocks.

## Part 2: open-loop identification (`ident/`)

`ident/c62-ident.ps1` holds the GPU clock fixed by the start's ceiling (`DpmMaxMHz` 1000, the governor's only
level, 500 MHz at idle), fixes the fan with a lease, and applies a power schedule: 0-20 s idle, 20-70 s GPU fill
(`d3d11bench` 1920x1080, 32 layers), 70-100 s GPU plus 4 busy CPU threads, 100-125 s idle, 125-150 s 4 CPU
threads only. Kernel driver escape ABI `0x000700D8` (release r19). Two trials:

| Trial | Start (UTC) | Fan lease | Stop |
|---|---|---|---|
| `ident/fan100.txt` | 04:06:15 | 100 % | Tctl >= 87 C for 10 s, at 98 s |
| `ident/fan50.txt` | 04:09:59 | 50 % | Tctl 89 C, at 79 s |

Both trials stopped in the GPU + CPU phase. The idle and CPU-only phases after 100 s did not run.

`ident/ident-fit.py` fits one node per sensor over all samples (least squares, `numpy`):
`dT/dt = a * (P - P_idle) + c * cpu_on - b * (T - T_idle)`. `P` is `power_w`, `cpu_on` is 1 in the GPU + CPU
phase. It reports `tau = 1/b`, `R = a/b` and the CPU equivalence `c/a`: the change of `power_w` that heats the
sensor as fast as the 4 CPU threads. `ident/ident-fit.json` is its output. A new run on the dev PC gives the same
file byte for byte.

| Trial, sensor | tau s | R C/W | CPU equivalence W | R2 of dT/dt | samples |
|---|---|---|---|---|---|
| fan 100 %, GPU sensor | 14.4 | 0.259 | 68.4 | 0.466 | 56 |
| fan 100 %, Tctl | 15.1 | 0.252 | 75.1 | 0.577 | 56 |
| fan 50 %, GPU sensor | 19.0 | 0.414 | 63.6 | 0.566 | 45 |
| fan 50 %, Tctl | 27.6 | 0.459 | 79.4 | 0.675 | 45 |

Phase data (`ident-fit.json`): the GPU phase draws `power_w` 90.4 W (fan 100 %) and 90.6 W (fan 50 %) at
1000 MHz. In 48 s the GPU sensor rises from 59.4 to 73.4 C (fan 100 %) and from 60.9 to 79.4 C (fan 50 %). In the
GPU + CPU phase `power_w` is 92.3 W and 103.9 W, and the clock falls to 800 or 900 MHz.

The wall power (`*.plug.jsonl`, one sample about every 12.5 s): in the fan 100 % trial the three GPU-phase samples
at full load read 165.8, 166.4 and 169.6 W. The GPU + CPU phase has two samples: 167.6 W at 6.6 s into the phase
and 179.4 W at 19 s. The second one is 9.8 to 13.6 W above the GPU-phase samples. The fan 50 % trial has one
GPU + CPU sample (168.4 W), taken at the moment of the thermal stop.

Limits of the model:

- R2 of 0.47 to 0.68 means that the model explains about half to two thirds of the variance of dT/dt. It is a
  coarse model.
- The CPU-only phase did not run. The CPU term rests on 15 samples (fan 100 %) and 4 samples (fan 50 %) of the
  GPU + CPU phase. In these samples the clock and `power_w` also changed.
- In the fan 50 % trial the duty read-back rose from 50 to 94 % with Tctl. The fan stayed at 920 to 963 rpm.
  In the last two samples the fan emergency set 100 % and the fan turned at 1623 and 1673 rpm. These two
  samples are in the fit. The fan 100 % trial read duty 100 and 1664 to 1744 rpm in all samples.

## Files

| File | Content |
|---|---|
| `grid/f*-g*-c*.txt` | the lab output of each grid cell: settings, fan state, stop, WHEA count, telemetry, `slow` CSV (CPU mV, core MHz, GPU V/F point every 10 s) |
| `grid/f*-g*-c*.plug.jsonl` | the plug samples of each cell |
| `grid/sweep.py`, `grid/sweep.log`, `grid/c62-trial.ps1`, `grid/c62-cool.ps1` | the sweep driver, its log, the trial and cool-down scripts |
| `grid/fit.py`, `grid/fit.md`, `grid/fit.json` | the per-cell summary and fit |
| `ident/fan100.txt`, `ident/fan50.txt` | the lab output of the two identification trials |
| `ident/fan100.plug.jsonl`, `ident/fan50.plug.jsonl` | the plug samples of the two trials |
| `ident/c62-ident.ps1`, `ident/ident-fit.py`, `ident/ident-fit.json` | the trial script, the fit and its output |

No file needed a change for privacy. Checked and found clean: no MAC address, serial number, UUID, IPv4 address, host
name or user name. The plug samples carry no device identity.
