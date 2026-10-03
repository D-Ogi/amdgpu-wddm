# bc250d3d_router.dll - the D3D10/D3D11 UMD router

The registered D3D10/D3D11 user-mode driver of unit A (`UserModeDriverName` entries 1 and 2) is this router, not a
driver of its own. At every `OpenAdapter10` / `OpenAdapter10_2` call it picks one of three UMDs and forwards the call:

- `dwm.exe` (and any `HostedClients` entry) goes to the hosted GPU UMD `bc250d3d_zink.dll`, or to the CPU UMD by
  the kill switch `DwmForceCpu` or when the KMD's GPU DWM interop switches are off;
- every other process goes to the application GPU UMD (the DXVK-based shell `amdgpu_wddm_d3d11.dll`,
  `driver/umd/dxvk`) or to the CPU UMD, by the `AppRouter` policy (`cpu`, `allowlist`, `gpu-default`, with `Allow`
  and `Deny` lists). The sign-in and consent processes always stay on the CPU UMD;
- a failed GPU load or GPU `OpenAdapter` falls back to the CPU UMD with the caller's arguments restored.

The registry interface (`HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter`, `...\AppRouter` and the KMD's
`InteropLastState`) is documented value by value at the top of `router.cpp`. The decisions themselves are free of
I/O in `router-policy.h`, so the host tests drive them directly.

The compiled default CPU UMD path, `C:\BC250\m15\desktop-umd173-007\bc250d3d.dll`, is the lab's deployment path
on unit A; the `DesktopRouter` value `CpuUmdPath` overrides it. It stays in the source because it is part of the
registered binary.

## Registered build

| Binary | SHA-256 | Built | Registered on unit A |
|---|---|---|---|
| `bc250d3d_router.dll` | `674AD261640B1A3A79150E41F39C2B8962F3AB7747E3E725C8C2F3E42FFCAED0` | 2026-10-01, from this source | since 2026-10-01T04:26Z |

It replaced the desktop-only router `5BBEB783`; the desktop decision and its route log line are unchanged from it.

## Build and host gate

```powershell
pwsh tools\build\build-umd-router.ps1 [-OutputDir <dir>]
pwsh tools\build\test-umd-router.ps1 -HostedUmd <bc250d3d_zink.dll> -CpuUmd <bc250d3d.dll> -AppPackage <dir> [-Build <dir>]
```

`build-umd-router.ps1` is the recipe of `674AD261`: Visual Studio 2022 `vcvars64` (MSVC 14.44.35207, cl
19.44.35221) with the WDK 10.0.26100.0 `um` and `shared` headers from `<BC250_ROOT>\toolchain\nuget` in front of
`INCLUDE`; `/O2 /MD /W4 /WX /Zi /LD`, linked `/DEBUG /OPT:REF /OPT:ICF /PDBALTPATH:%_PDB%`. The same run builds
five UMD doubles from `tests/fake-umd.cpp` and the harness `tests/test-router.cpp`. Output goes to
`<BC250_ROOT>\scratch\build\umd-router` unless `-OutputDir` says otherwise.

`test-umd-router.ps1` lays out the router, the doubles and three real binaries (hosted UMD, CPU UMD, the DXVK shell
with its config) and runs `test-router.exe`: each scenario in its own process on a private application hive, so no
machine state is touched and no device is opened. The gate of `674AD261` ran 68 scenarios with 0 failures (hosted
UMD `E6B944CF`, CPU UMD `4176D1DF`, shell `E748418C` with config `A9B498ED`); the old router `5BBEB783` fails 26 of
them, the application-policy ones.

## Reproducing 674AD261

The link does not use `/Brepro`, so a rebuild differs from `674AD261` in exactly five places: the COFF header
`TimeDateStamp`, the `TimeDateStamp` of the three debug directory entries and the GUID of the CodeView (RSDS)
record. The DLL holds no build path (`/PDBALTPATH:%_PDB%` leaves the PDB's file name only).
`tools/build/pe_compare.py` checks that and nothing else differs:

```
python tools/build/pe_compare.py <674AD261 bc250d3d_router.dll> <rebuilt bc250d3d_router.dll>
```

Measured on the development PC on 2026-10-03: same size (21504 bytes), five differing ranges, all in those
fields, and the rebuilt image with the reference's timestamps and GUID copied in hashes to `674AD261...` again.
The doubles and `test-router.exe` (`CCB5EBE8`) compare the same way. A build with `/Brepro` would make the hash
itself reproducible, but it would not be `674AD261`.
