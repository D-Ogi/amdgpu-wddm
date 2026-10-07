# Shader model 6.7 and 6.8 on unit A (d3d12sm68, three runs)

Date: 2026-10-08. Unit A on Windows 11 23H2 (build 22631, System32 D3D12Core 10.0.22621.5415), kernel driver
0.7.216.20, D3D12 shell of branch m15/shader-model-68 (x64 SHA-256 306AF1D2...) swapped in for the runs by
`scratch/sm68/lab/sm68-lab.ps1`, which put the previous shell (BBB5803E) back at the end. Clients:
`amdgpu_wddm_d3d12sm68.exe` (ACEC9813...) and `amdgpu_wddm_d3d12sm68_agility619.exe` (F3A5188F...), the second
with the D3D12Core 1.619.4.0 of The Witcher 3 5.0 next to it.

| Run | Runtime | Expected | Highest reported | Tests | Result |
|---|---|---|---|---|---|
| `sm68-plain.txt` | System32 22621.5415 | 6_6 | 6_6 | 0 run (6.7 and 6.8 tests skipped) | PASS |
| `sm68-agility.txt` | Agility 1.619.4 | 6_8 | 6_8 | QuadAny/QuadAll, WaveSize, SampleCmpGrad: 3 of 3 exact | PASS |
| `sm68-switch68off.txt` | Agility 1.619.4, switch `shader-model-68-off` | 6_7 | 6_7 | QuadAny/QuadAll exact, 6.8 tests skipped | PASS |

In the 6_8 run OPTIONS21 reports SampleCmpGradientAndBias 1 and ExtendedCommandInfo 1; WaveSize ran with 64
lanes (range 32-64). GetDeviceRemovedReason is 0 after every run. The `*-driver-log.txt` files are the shell and
engine logs of each run.
