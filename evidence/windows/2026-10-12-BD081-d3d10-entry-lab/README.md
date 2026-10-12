# BD-081: Direct3D 10.0 on unit A goes to the GPU user-mode driver (lab, 2026-10-12)

Date: 2026-10-12, 05:34Z. Unit A, Windows 11 Pro 22631.6199, release package 0.7.216.100-tester.28 (router,
application GPU UMD), KMD 0.7.216.36 (BD-124 part A candidate, SYS 7290210C), display on a DP-to-HDMI adapter.
Probe: `tools/win/d3d10probe` of `evidence/windows/2026-10-07-BD081-d3d10-entry-devpc`, the same builds (`C7A37E8E`
x64, `0B054C9E` x86), run with `--trace-entry` and the default frame count.

## Question

BD-081 asked whether a Direct3D 10.0 application on unit A reaches our router through `OpenAdapter10` (which the
router sends to the CPU UMD) or through `OpenAdapter10_2` (which the AppRouter decision sends to the GPU UMD under
`gpu-default`). The development-PC run of 2026-10-07 showed the runtime side; this run is the lab check named in the
DEFECTS.md comment of that day.

## Method

1. `scripts/bd081-probe.ps1` over SSH (session 0): both builds, 60 s bound each. A swap chain cannot be created in
   session 0, so this run shows the entry and the device creation only.
2. `scripts/bd081-session1.ps1`: the same two builds as a one-shot scheduled task of the interactive user (session 1),
   which creates the swap chain, renders, reads back and presents. The task is removed at the end.

## Result

| Run | Entry the runtime asks for | CreateDevice | Device | Pixels | Present | Served by | Verdict |
|---|---|---|---|---|---|---|---|
| x64, session 1 | `OpenAdapter10_2`, exported, 0x0 | interface 000B0023 (11.35), level 12_1, 0x0 | `D3D10CreateDeviceAndSwapChain(hardware)` 0x0 | triangle 32640 (exact rule 32640), clear 32896, wrong 0 | 60 of 60, 0 failed | application GPU UMD (`amdgpu_wddm_d3d11.dll`) | PASS |
| x86, session 1 | `OpenAdapter10_2` (WoW64 router), 0x0 | same | 0x0 | same | 60 of 60 | WoW64 application GPU UMD | PASS |
| x64 and x86, session 0 | `OpenAdapter10_2`, 0x0 | same, 0x0 | `D3D10CreateDevice(hardware)` 0x0; with a swap chain 0x887A0022 | - | - | application GPU UMD | NO-DEVICE (the probe's swap-chain step; session 0 has no desktop) |

The runtime never asked for `OpenAdapter10`. A Direct3D 10.0 application on unit A is served by the GPU UMD, x64 and
x86, with exact pixels and working presents. The session 0 verdict is a property of the SSH session, not of the
route: the device itself is created there too.

## Files

| File | Content |
|---|---|
| `session1-trace.txt` | output of `scripts/bd081-session1.ps1` (the task result and both probe outputs) |
| `session0-trace.txt` | output of `scripts/bd081-probe.ps1` |
| `scripts/` | the two scripts as run |
