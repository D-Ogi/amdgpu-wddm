# Direct3D 9 through D3D9On12 as x64 and x86 (2026-10-08)

Unit A, Windows 11 Pro, boot 2026-10-08T02:46:03Z. Run 03:24:14Z to 03:24:29Z. Every number in this file comes from
the files next to it. Fact: M843.

## Question

A D3D9 application on our driver uses the D3D9On12 mapping layer of Windows. Does this route render correctly as a
64-bit process and as a 32-bit process? Which modules does each process load? Does the run change DWM or the start
confirmation of the kernel driver?

## Stack

The lab ran the installed tester.20 package with one hand deviation (kernel driver 0.7.216.20 and the deployed
D3D12 triplet). No install happened between the run and the hash read (`stack-hashes.out`, same boot, same DWM).

| Part | Identity (SHA-256, first 8 hex digits) |
|---|---|
| Kernel driver | 0.7.216 (`health` version 0x000700D8), `bc250kmd.sys` `7580A8F7` |
| x64 D3D12 shell, engine, ICD | `BBB5803E`, `348117F1`, `822134D0` |
| x86 D3D12 shell, engine, ICD | `077104FE`, `C00AC538`, `F6392B51` |
| Application router, x64 and x86 | `93F707BB`, `9A91CA91` |
| `d3d9on12.dll`, System32 and SysWOW64 | `B10DE0CA`, `64E72A28`, both file version 10.0.22621.2506 |
| Probe, x64 and x86 | `C0CED039`, `DA42A183` (`sha256-probe.txt`) |

## Route

The display class key has an empty D3D9 entry (entry 0) in `UserModeDriverName` and in `UserModeDriverNameWow`
(`lab-state.out`). With an empty D3D9 entry, `d3d9.dll` uses `d3d9on12.dll`. That layer runs on `d3d12.dll`, and
the D3D12 runtime loads our D3D12 shell from entry 3 of the same list. For a 32-bit process, entry 3 of
`UserModeDriverNameWow` names the x86 D3D12 shell under `wow64\d3d12\`.

## Method

1. Build `tools/win/d3d9probe` at commit 9c03c35c with `build.ps1` (x64) and `build.ps1 -Arch x86`.
2. Copy both binaries to unit A. `d3d9-run.ps1` refuses a binary with another hash.
3. Run each binary in two modes. `default` calls `Direct3DCreate9Ex`, as every D3D9 application does. `on12` calls
   `Direct3DCreate9On12Ex` with the mapping layer forced on.
4. Each mode runs in the interactive session as a one-shot scheduled task of the logged-on user. A D3D9 device
   needs that session. The task has a 45 s limit and the script removes the task after each mode.
5. Record DWM, the start confirmation (`bc250kmd_cli health read`) and the count of GPU fault and fence timeout
   lines in the kernel driver log before and after the four modes.

The probe renders with the fixed-function pipeline into a 256x256 render target. It clears the target, draws one
pre-transformed triangle and reads the target back with `GetRenderTargetData`. The top-left fill rule gives exactly
32 896 triangle pixels. Then the probe draws 200 frames of 100 triangles each with `DrawPrimitiveUP`. An event query
closes each frame, and the probe waits for it. The probe opens no visible window and calls no Present.

## Results

| Process | Mode | Device | 9On12 layer | Triangle pixels | Wrong samples | Draws/s | Verdict |
|---|---|---|---|---|---|---|---|
| x64 | default | S_OK | yes | 32 896 of 32 896 | 0 | 182 450 | PASS |
| x64 | on12 | S_OK | yes | 32 896 of 32 896 | 0 | 189 379 | PASS |
| x86 | default | S_OK | yes | 32 896 of 32 896 | 0 | 181 738 | PASS |
| x86 | on12 | S_OK | yes | 32 896 of 32 896 | 0 | 189 690 | PASS |

All four modes report one adapter, "BC-250 GPU (amdgpu-wddm)", vendor 0x1002, device 0x13FE, with the driver name
`bc250d3d_router.dll`. All four report vertex and pixel shader model 3.0, a 16384x16384 texture limit and four
simultaneous render targets. No pixel has a colour other than the clear colour or the triangle colour. Each mode
took 2.3 s to 3.6 s, including the start of the task.

Modules that name a graphics stack:

- x64: `d3d9.dll`, `d3d9on12.dll`, `dxgi.dll`, `d3d12.dll`, `D3D12Core.dll`, and
  `amdgpu_wddm_d3d12.dll`, `amdgpu_wddm_vkd3d.dll` and `amdgpu_wddm_radv.dll` from `amdgpu-wddm\d3d12\`.
- x86: the same system modules from the 32-bit view, and the same three files from `amdgpu-wddm\wow64\d3d12\`.

Neither process loads `bc250d3d_router.dll`, although the adapter identifier names it. The probe lists every module
whose name contains `bc250`.

Before and after the run:

| Item | Before (03:24:14Z) | After (03:24:29Z) |
|---|---|---|
| DWM process | 1912 | 1912 |
| Start confirmation | flags=15, generation 27357403, epoch 17 | flags=15, generation 27357403, epoch 17 |
| Completed fence | 12766 | 13108 |
| GPU fault lines, fence timeout or reset lines | 0, 0 | 0, 0 |

The completed fence moved by 342 because the probes used the GPU. The start confirmation and DWM did not change.

## Conclusions

1. A 32-bit D3D9 application renders correctly on unit A through D3D9On12 and the x86 D3D12 shell. This is the
   first measurement of the 32-bit route on unit A.
2. The 64-bit route still renders correctly. Its rate agrees with the earlier probe run of 2026-10-06 (about
   185 000 draws/s).
3. The x86 route gives the same draw rate as the x64 route, within 1 %.
4. The empty D3D9 entry is sufficient. Windows selects the mapping layer for both modes, and the forced `on12` mode
   gives the same result as the `default` mode.

## Limits

- One run per mode. The draw rate measures the submission path of small `DrawPrimitiveUP` batches with a wait per
  frame, not the GPU throughput of a game.
- The probe uses no shaders of its own, no textures and no Present. A D3D9 game session on an installed package is
  still open.

## Files

| File | Content |
|---|---|
| `lab-state.ps1`, `lab-state.out` | Class key entries, installed shell hashes, x86 directory, DWM, boot, start confirmation |
| `d3d9-run.ps1`, `d3d9-run.out` | The four probe modes with the state before and after |
| `stack-hashes.ps1`, `stack-hashes.out` | Full hashes of the D3D12 route files, `d3d9on12.dll` and the kernel driver image |
| `sha256-probe.txt` | Hashes of the two probe binaries and their source commit |
