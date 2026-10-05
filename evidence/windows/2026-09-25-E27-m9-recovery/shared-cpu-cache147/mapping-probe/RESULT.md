# Shared CPU mapping diagnostic (source/build only)

Prepared 2026-09-25. No target execution, no production KMD/UMD changes.

## Run on the lab

```
shared-map-probe.exe --run v1
shared-map-probe.exe --run v2
```

Use v1 with KMD146. v2 is reserved for the matching newer KMD implementation:
E26R v1 is 12 bytes `{magic=0x52363245, version=1, shared=1}`; v2 is 16 bytes
`{magic, version=2, shared=1, cpuAccessFlags=2}`. CPU_READ=2, PRIMARY=1.
The probe never requests primary and never submits GPU rendering or compute.
KMD146 does not implement v2 policy, so running v2 there cannot test the new policy.

The process creates one 1024x1024, pitch4096, 4 MiB LB7A v1 surface on the adapter
matching bc250. D3DKMT CreateResource/CreateShared/NtSecuritySharing are set;
no shared NT handle is exported, no global sharing handle is published.
CreateCached/CreateWriteCombined are deliberately unset: SDK marks them kernel-only.
There is no context, command buffer, Present or SubmitCommand. MapGpuVirtualAddress,
MakeResident and their paging fences follow the existing E26/E27 path; these operations
may cause VidMm paging work. Lock2 flags are zero.

It prints each KMT status, resource/allocation identity, and VirtualQuery regions over
the complete mapping with Protect/AllocationProtect/State/Type/WC/NOCACHE fields.
After a complete pattern write and MemoryBarrier, every DWORD is checked against its
index-dependent expected value. Three timed single-thread scalar sequential reads
consume every DWORD using four independent sums, each checked against expected sum.
Timing includes QPC start/end/frequency, milliseconds and MiB/s. A normal PAGE_READWRITE
VirtualAlloc buffer runs the identical procedure before and after the shared mapping.
This is a scalar read diagnostic, not a SIMD bandwidth or realistic shader benchmark.

Normal cleanup checks Unlock2, FreeGpuVirtualAddress, DestroyAllocation2(resource),
DestroyPagingQueue, DestroyDevice and CloseAdapter. Any reported failure makes the
final result FAIL. There is a 5 s paging-fence bound and 30 s process watchdog. The
watchdog terminates a stuck process rather than reporting success; it cannot guarantee
recovery from a kernel or hardware hang. Successful normal cleanup is the acceptance path.

VirtualQuery reports Windows virtual mapping metadata. It does not prove effective
PAT/MTRR, physical residency, CPU/GPU coherence, or attributes of another process's
surface. A speed difference in this owned surface is evidence about this allocation
path only. Comparing v1/v2 requires otherwise matched driver, power state and workload.
This probe does not inspect DWM memory and does not prove which mapping DWM renders to.

## Build and host validation

`build.ps1` uses installed MSVC, portable SDK10.0.26100, /W4 /WX /O2 /MT.
Build passed. Only --help, --selftest and invalid-argument checks ran on the development PC.
Self-test checks the actual 4 MiB fill/verify/read implementations, three injected
corruptions (first/middle/last DWORD), exact v1/v2 descriptor fields/sizes and invalid
version rejection. Five invalid command lines return2 before any KMT call. No adapter
was opened by these tests. `build.log` preserves results.

SHA256:

- shared-map-probe.exe: 55536D5AA735DBB461BAADA07A3DE5F4C798E46CD51A4B9B6DB4E78D54811CC6
- shared-map-probe.c: 817B4E02EFD223A7E06262F42399D6EA9E26C00D2A5A5CA674AEA01DC411FF82
- kmtprobe-snapshot.c: 8340F1498F6FB0CCE941625199D0A7AC655168915FA1747FB08BAFD50077C6B3

The helper snapshot is an unchanged local copy of tools/win/kmtprobe/kmtprobe.c.
Its original main is renamed; only adapter/device/paging/map/lock helpers are invoked.
No original PM4 submission helper is reachable from this probe's main.

## Local references

- `ref/ddi-display/d3dkmthk.md`: D3DKMT_CREATEALLOCATION (WDK line1608), flags (line1552),
  D3DKMT_LOCK2 (line5130); Lock2 can map CPU backing store or GPU framebuffer.
- `ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmthk/ns-d3dkmthk-_d3dkmt_destroyallocation2.md`:
  nonzero hResource requires null allocation list; allocation count is then ignored.
- `bc250-win/experiments/E26-wddm-desktop/mesa-main-bc250-gallium.patch`:
  Bc250EnsureSurface LB7A/E26R descriptors and paging/map/Lock2 ordering.
- `bc250-win/driver/kmd/wddm.c`: Bc250WddmCreateAllocation v1 shared aperture policy.
- `bc250-win/tools/win/kmtprobe/kmtprobe.c`: existing bounded KMT helpers.

The local CreateAllocation page describes the older entry point and calls info2 reserved;
this probe deliberately follows the project's already exercised CreateAllocation2/info2
path, rather than substituting the old allocation record layout.
