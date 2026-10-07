# BD-081: which OpenAdapter entry the Direct3D 10.0 runtime calls (development PC, 2026-10-07)

Date: 2026-10-07. The development PC, Windows 11 Pro build 26200 (the runtime DLLs report 10.0.26100), one NVIDIA
GeForce RTX 4090 with its own registered user-mode driver. Not unit A: this run shows what the Microsoft runtime
asks of a user-mode driver that exports both entries, which is a property of the runtime and not of the adapter.
Our router (`driver/umd/router`) also exports both entries, so the same question decides where a Direct3D 10.0
application goes on unit A. Every process ran without a visible window and exited by itself.

## Question

BD-081 (defaults audit row 18, 2026-10-05) reads the router's source: `OpenAdapter10` goes to the CPU UMD under every
AppRouter mode, and the router comment calls `OpenAdapter10` "the D3D10.0 runtime". The defect assumes that a
Direct3D 10.0 application reaches the user-mode driver through `OpenAdapter10`. Nothing measured that assumption.

## Method

- `scripts/entryspy.cpp` (built by `scripts/run2.ps1` for x64 and `scripts/run3.ps1` for x86, Visual Studio 2022
  and the WDK 10.0.26100 headers) loads `d3d10.dll`, `d3d10core.dll`, `d3d10_1.dll`, `d3d10_1core.dll`,
  `d3d11.dll` and `dxgi.dll`, replaces the `GetProcAddress` import of each of them in its own process, and logs every
  `OpenAdapter*` name the runtime asks for. It wraps `OpenAdapter10` and `OpenAdapter10_2`, and after a successful
  open it wraps `CalcPrivateDeviceSize` and `CreateDevice` of the adapter table, logs `Interface` and `Version`, and
  calls through. It then creates one device with each of `D3D10CreateDevice` (the 10.0 runtime),
  `D3D10CreateDevice1` at level 10_0 (the 10.1 runtime) and `D3D11CreateDevice` (the 11 runtime).
- `entryspy.exe hide102 d3d10` answers NULL for `OpenAdapter10_2`, as for a driver without that export, and calls
  `D3D10CreateDevice` only.
- `tools/win/d3d10probe` (this commit's source, builds `C7A37E8E` x64 and `0B054C9E` x86) carries the same trace
  as `--trace-entry`, and renders and reads back a triangle through the 10.0 runtime: `d3d10probe.exe
  --trace-entry --frames 10 --out <file>`.

## Files

| File | Content |
|---|---|
| `entryspy-x64.txt` | x64: the three runtimes with both entries exported, then the 10.0 runtime with `OpenAdapter10_2` hidden |
| `entryspy-x86.txt` | x86 (WoW64): the three runtimes with both entries exported |
| `probe-trace-x64.txt`, `probe-trace-x86.txt` | `d3d10probe --trace-entry --frames 10`, x64 and x86 |
| `scripts/` | the spy source and its two build-and-run scripts |
| `sha256.txt` | SHA-256 of every file above |

## Result

- The Direct3D 10.0 runtime asks for `OpenAdapter10_2`, not `OpenAdapter10`, on x64 and on x86. It passes
  `Interface 000B0011` to the open (the field is to be ignored there) and creates the device at `Interface 000B002D`
  (11.45, the newest interface the NVIDIA driver lists), with the 3D pipeline level 12_1. The 10.1 and the 11 runtime
  make the same three calls with the same values.
- The probe renders the triangle exactly through the 10.0 runtime on both builds: 32 640 triangle pixels, 32 896
  clear pixels, no other value, 10 of 10 Present calls succeed. The same probe on WARP (`--driver warp`) gives the
  same counts.
- With `OpenAdapter10_2` hidden, `D3D10CreateDevice` ends the process with `0xC0000005` (exit -1073741819) and does
  not ask for `OpenAdapter10`. This is an artificial state (a loader that exports `OpenAdapter10_2` and then answers
  NULL for it), so it shows only that this runtime does not fall back to `OpenAdapter10` on its own.

## What follows

On Windows 11 the router sees a Direct3D 10.0 application at `OpenAdapter10_2`, where the AppRouter policy decides it
like any other application: under `gpu-default` it gets the application GPU UMD (`amdgpu_wddm_d3d11.dll`), which lists
the D3D11.1 and WDDM 2.0 interfaces and accepts the 10_0 to 12_1 pipeline levels. The router's `OpenAdapter10` path
(reason `app-d3d10-entry`) is not reached by these runtimes. Unit A has not run the probe yet; its route log line
(`entry=OpenAdapter10_2 route=gpu`) and the probe's `--trace-entry` output on unit A are the check that closes BD-081.
