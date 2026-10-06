# Tuner CPU surface: first readback on unit A (2026-10-06)

Date: 2026-10-06, 22:40:24Z. Unit A, Windows 11, tester package 0.7.213.102-tester.17 (KMD 0.7.213.1, escape ABI
`0x000700D5`). The CPU surface of the Tuner is off unless the operator value `CpuTune` is 1; the KMD reads it once
at its start. `scripts/set-cputune.ps1` wrote `CpuTune = 1` before a restart. The step then ran the read-only
`readback` stage of the Tuner order (read, read back, no write). Bound 180 s, it took 13 s.

## Files

| File | Content |
|---|---|
| `0-cpu.txt` | `bc250kmd_cli cpu` before the readback |
| `1-cpu-readback.txt`, `cpu-readback.txt` | the readback command and its result (the second is the step's copy) |
| `2-cpu.txt` | `bc250kmd_cli cpu` after the readback |
| `3-dpm-curve.txt`, `4-dpm.txt`, `5-clock-read.txt` | the GPU V/F curve, the DPM state and the clock read at the same time |
| `temperature-before.txt`, `temperature-after.txt` | Tctl around the step |
| `plug-before.txt` | the smart plug's power telemetry before the step (`plug.py` scale, not calibrated) |
| `verdict.txt` | the step's verdict and duration |

## Result

- SMU queue 3 answered. Reads 20 before, 40 after; writes 0, refusals 0, reverts 0.
- CPU voltage 806 mV in the first read and 1199 mV in the two later reads. GPU voltage 824 mV. Firmware thermal cap
  100 C, feature mask `0xDD602C7D`.
- Core clocks 1400 MHz on six cores and 900 MHz on two. P-state table 3200, 2550, 2325, 1960, 1820, 1600, 1271 and
  800 MHz.
- Core mask `0x77` in force, nothing stored, 12 logical processors visible to Windows.
- Baseline recorded for later stages: clock limit 3200 MHz, undervolt 0 steps, cap 100 C. Nothing was applied.
- The GPU stayed at its idle point (500 MHz, 820 mV, 65.5 C) during the step.

The two voltage values come from the same message at different moments. This run does not tell why they differ.
