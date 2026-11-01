# clprobe: an OpenCL platform and one exact kernel

`clprobe.exe` answers two questions for defect BD-083. Is there an OpenCL platform on this computer? Does one of its
GPU devices compute a known kernel exactly? The probe loads `OpenCL.dll` at run time, so it also runs on a computer
without an OpenCL loader and reports that.

PROVENANCE: OpenCL-Headers (Khronos, Apache-2.0), read at build time only, revision e6060189.

## What it does

1. It loads `OpenCL.dll` from System32, then through the normal search order, or from `--loader <path>`.
2. It lists every platform and every device: name, vendor, version, type, compute units, global memory.
3. It selects the first GPU device, or the first GPU device whose name contains `--device <text>`.
4. It builds one kernel from source, `c[i] = a[i] * 3 + (b[i] ^ i)`, over 1048576 words (`--size`).
5. It compares every word with the result that the CPU calculates.

The output buffer starts as the complement of the expected result, so a kernel that writes nothing cannot pass.
`--inject skip-kernel` leaves out the dispatch. That run must end in `result=mismatch`, which proves the oracle.
`--list` stops after step 3. A watchdog ends the run after `--bound` seconds (default 60, at most 170).

The last line is the verdict:

```
CLPROBE VERDICT result=pass|mismatch|fail|build-failed|no-opencl|no-platform|no-device|timeout stage=.. cl=.. detail=..
```

| Exit code | Result |
|---|---|
| 0 | `pass` |
| 1 | `mismatch` |
| 2 | `fail` or `build-failed` |
| 3 | `no-opencl`, `no-platform` or `no-device` |
| 4 | usage |
| 5 | `timeout` |

## Build

```powershell
pwsh tools\win\clprobe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
```

The compiler is clang-cl of LLVM 23.1.4 with lld-link (`tools\build\umd-compiler.ps1`). The output goes to
`<workspace>\scratch\build\clprobe`, with `SHA256SUMS.txt`.

## Host controls

On a development PC with another vendor's OpenCL driver, 2026-11-01:

| Run | Result |
|---|---|
| `clprobe` | `pass`, 1048576 words, 0 mismatched |
| `clprobe --inject skip-kernel` | `mismatch`, all words untouched |
| `clprobe --loader <a file that does not exist>` | `no-opencl` |
| `clprobe --list --device "no such device"` | `no-device` |
| `clprobe --size 5` | usage, exit code 4 |

## Lab arm for BD-083

Each step stays inside three minutes. OpenCL needs no desktop, so `target.py run` in session 0 is enough for the
probe. A Microsoft Store package installs for one user, so step 2 runs in the logged-on session.

1. Baseline on the installed release. Expected today: `no-opencl` or `no-platform`.

   ```
   python tools\win\target.py push <workspace>\scratch\build\clprobe\clprobe.exe --to C:\BC250\tmp\clprobe
   python tools\win\target.py run "C:\BC250\tmp\clprobe\clprobe.exe --list"
   ```

2. Install the OpenCL, OpenGL and Vulkan Compatibility Pack (CLon12 over our D3D12 driver) in the logged-on session.
   Find its Store identifier first, then install it:

   ```
   winget search "Compatibility Pack" --source msstore
   winget install --id <identifier> --source msstore --accept-package-agreements --accept-source-agreements
   ```

3. Run the probe in the logged-on session and in session 0, then the negative control:

   ```
   C:\BC250\tmp\clprobe\clprobe.exe
   C:\BC250\tmp\clprobe\clprobe.exe --inject skip-kernel
   ```

   If the result is `no-opencl`, the package did not put a loader on the search path. Find its `OpenCL.dll` under the
   package's install folder (`Get-AppxPackage` gives `InstallLocation`) and run again with `--loader <that path>`.

Pass: a platform of the package lists a GPU device with the board's name, the probe ends in `result=pass` with 0
mismatched words, and the negative control ends in `result=mismatch`. Keep the full output of each run. A pass
shows the CLon12 route only. The clvk route of ADR 0016 needs its own build.
