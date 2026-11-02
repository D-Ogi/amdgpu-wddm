# residency-budget - D3D12 residency probe (BD-052)

`residency-budget.exe` is a native D3D12 client. It checks whether the residency calls of an application change the
local VRAM use of its process. It reads the use with `IDXGIAdapter3::QueryVideoMemoryInfo(0, LOCAL)` before
and after each step. The GPU work is a copy from an upload buffer into the heap and back into a readback buffer, so
the probe needs no shader compiler. It opens no window.

BD-052 is the defect that this probe finds: a heap import of the D3D12 shell kept its own residency reference for the
life of the heap. While it held that reference, `Evict` and `D3D12_HEAP_FLAG_CREATE_NOT_RESIDENT` had no effect.

## Cases

One case runs in each process. Each step writes one JSON line to stdout and, with `--log`, to a file. The exit code
is 0 when the pass rule of the case is true.

| Case | Steps | Pass rule |
|---|---|---|
| `a` | A heap of 512 MiB, a copy through it, `Evict`, `MakeResident`, a copy back | The heap counts after its creation and after the copy. It counts 8 MiB or less after `Evict` and again after `MakeResident`. The pattern comes back. |
| `b` | A heap of 512 MiB with `CREATE_NOT_RESIDENT`, `EnqueueMakeResident`, a copy through it | The heap counts 8 MiB or less after its creation and 500 MiB or more after `EnqueueMakeResident`. The pattern comes back. |
| `c` | 20 heaps of 64 MiB, each one written by a copy at once after its creation, with no wait between | No device removal at any iteration, and all 20 patterns come back. |

A sample after a residency change is read again for up to 1000 ms, because the use is not guaranteed to change at
the same time as the call. Every case also fails on a device removal.

## Use

```
residency-budget.exe --case a|b|c [--log <file>] [--adapter-vendor <hex>]
```

`--adapter-vendor` selects the first hardware adapter of that PCI vendor, for example `1002`. Without it, the probe
uses the first hardware adapter.

## Build

```
pwsh tools\win\residency-budget\build.ps1 -Kits P:\BC-250\toolchain\nuget
```

The compiler is clang-cl of `toolchain\llvm-23.1.4-win64` (`-Llvm`), for x86-64-v3. The script does not use a
different compiler if that one is missing. The headers and libraries come from MSVC (vswhere) and from the SDK
10.0.26100 of the NuGet kits. The default output directory is `<BC250_ROOT>\scratch\build\residency-budget`.

## Results

On 2026-11-01 a lab run on unit A used this probe. With the tester.35 D3D12 shell, the probe showed the defect: the
use did not go down by the heap size after `Evict` in case `a`, or the heap counted at its creation in case `b`. With
the shell of the BD-052 fix, cases `a`, `b` and `c` passed. As a control, the per-application profile value
`Experiment=import-residency-hold-on` gives the old hold back, and case `a` showed the defect again with it. BD-052 is
GitHub issue #31. The KMD half of the defect (`DriverManagesResidency`) is a different measurement.
