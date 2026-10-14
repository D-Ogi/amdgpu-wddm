# gdi-cpuwrite: what the processor pays for a CPU-visible surface

The GDI hardware acceleration page of the WDDM documentation asks the driver to report that support
"only if the cache-coherent GPU aperture segment exists and there's no significant performance
penalty when the CPU accesses GPU memory". The first half is a fact of our segment description. The
second half is a measurement, and this is the client that takes it. It is trial 5 of
[docs/testing/gdi-hw-accel-lab.md](../../../docs/testing/gdi-hw-accel-lab.md).

## What it measures

Three memory classes, one process, one run.

| Class | What it is | The placement our driver gives it |
|---|---|---|
| `sysmem` | committed pages from `VirtualAlloc` | ordinary write-back memory, the baseline |
| `type0` | a 32-byte LB7A block with no GDI trailer | CPU-visible, not in the aperture, not cached |
| `type2` | a 48-byte LB7A block with the GDI1 trailer and type 2 | CPU-visible, in the aperture, cached |

Three kinds of access to each class, and each one is a kind the kernel GDI fallback performs: a loop
of 8-byte stores, a copy from system memory, and a loop of 8-byte loads. Each kind runs `--passes`
times and the median is reported. The `PENALTY` lines at the end give the ratio against the
baseline.

## Why it does not use a standard GDI surface

`D3DKMT_CREATESTANDARDALLOCATION` of WDK 10.0.26100 holds one union member, `ExistingHeapData`. No
user-mode process can ask dxgkrnl for a `D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE` surface. The tool
therefore asks our own driver, with the private data the driver reads in `CreateAllocation`. The
placement is then the same `WddmGdiAllocationPolicy` answer the CDD's staging surface gets, which is
what the question is about.

## Build and run

    pwsh tools\win\gdi-cpuwrite\build.ps1 -Kits <workspace>\toolchain\nuget
    gdi-cpuwrite.exe --mib 16 --passes 3
    gdi-cpuwrite.exe --classes sys --mib 4 --passes 1     # no adapter needed

The build compares both private-data magic numbers and the four lines of
`WddmGdiAllocationPolicy` against `driver/kmd/gdi_private.h`. A tool that reports a placement the
driver no longer makes does not build. The build then runs the baseline class once, because a client
that cannot measure ordinary memory is broken before it reaches the lab.

The run is bounded twice: a watchdog thread inside the process ends it at `--budget-ms`, and the lab
kit puts its own bound around the process. Nothing is written to disk and no window is opened. Every
allocation is destroyed on the way out, and the baseline is a 16 MiB region which stays partly in the
processor's cache. That is the comparison this question needs, because a GDI operation touches a
small rectangle and the fallback gets the same cache.
