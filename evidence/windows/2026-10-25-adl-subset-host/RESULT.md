# Open ADL subset with the real AGS 6.3 library, host tests (2026-10-25)

Question: does AMD AGS 6.3 `agsInitialize` succeed with our ADL library (`driver/umd/adl`, design in
[docs/design/adl-subset.md](../../../docs/design/adl-subset.md)), and does it free everything again?

**Result on the development PC (NVIDIA RTX 4090, no AMD GPU, no ADL library in System32 or SysWOW64): yes, on x64
and on x86.** All 13 runs of `tools/build/test-umd-adl-ags.ps1` gave their expected exit code on each architecture.
This is a host result. The lab result on unit A is still open.

Source: commit a82f2aa0 of branch feat/adl-subset (`source.txt`). Compiler clang-cl 23.1.4 (`x64-build.json`,
`x86-build.json`).

| File | SHA256 |
|---|---|
| `atiadlxx.dll` (x64, shipped) | fefb773bd9f109da2abe2b087d1051bca722b3b64467e18ad20e6dcbf4c7e5f3 |
| `atiadlxy.dll` (x86, shipped) | 2353d36b586cd0de81d89c27baf2e8e602ed1c399844ed8675461bbaaa23850e |
| `amd_ags_x64.dll` 6.3.0 | b27b070ca39dc37984fb3dde0187515d36094e72cb881d7a99bd1055befd8da2 |
| `amd_ags_x86.dll` 6.3.0 | d7a0122abd8be157ab838a88368bd3358c8483c6b23083a28832c19dc7698bf6 |

The image bytes are the same for every build into the same output directory. The PDB signature in the image
contains the object paths, so another output directory gives another SHA256 with the same code.

## Runs

| Case | x64 | x86 | What it shows |
|---|---|---|---|
| shipped: info, info without GPU information | 0, 0 | 0, 0 | `agsInitialize` 0, `numDevices 0` (no adapter runs our driver here), `driverVersion ""`, `agsDeInitialize` 0, 0 callback allocations left |
| shipped: 100 rounds | +0 heap bytes | +0 heap bytes | No growth of live heap bytes (HeapWalk) or handles. AGS loads and unloads the library in each round |
| shipped: D3D12 device through AGS on the RTX 4090 | ext 0x0 | ext 0x0 | `agsDriverExtensionsDX12_CreateDevice` 0, extension bits 0, clear and readback 64 127 191 255 on 4096 of 4096 pixels, 0 references left |
| inject: two AMD GPUs and three displays (`tests/test-snapshot.h`) | as expected | as expected | AGS parses the adapter list, the version, the VRAM, the displays, the modes and the clone pair of the snapshot |
| host: the RTX 4090 taken for ours (`-TestService nvlddmkm`) | as expected | as expected | Discovery on real data: 10DE:2684, 25314721792 bytes of VRAM, driver 32.0.15.9597, one display 3440x1440 at 59.973 Hz |
| none: no ADL library | 6 | 6 | Negative control: `AGS_NO_AMD_DRIVER_INSTALLED` |
| no-modes: no `ADL2_Display_Modes_Get` | 5 | 5 | Negative control: `AGS_LEGACY_DRIVER` for a missing mandatory export |

`loop-1000-x64.txt`: 1000 rounds of the shipped x64 library, heap busy bytes +0. Private bytes went up and down
by up to 320 KiB over such runs, without a trend, so the gate uses the live heap bytes.

`x64-unit.txt`, `x86-unit.txt`: 3109 and 3144 checks, 0 failed. On x86 every export removes 0 stack bytes
(`__cdecl`). `x86-callconv.txt`: the repository's x86 calling-convention gate, 69 checks passed.

## Files

`ags-x64/`, `ags-x86/`: one output file for each run, `results.json` (inputs with SHA256, runs, exit codes),
`SHA256SUMS`. `build-*.txt`: the build logs. `*-discover.txt`: the production discovery on this PC (0 GPUs).
`*-discover-host.txt`: the host-service discovery. `SHA256SUMS.txt`: every file of this directory.
