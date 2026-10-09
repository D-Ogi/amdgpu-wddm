# bc250d3d_router.dll - the D3D10/D3D11 UMD router

The registered D3D10/D3D11 user-mode driver of unit A (`UserModeDriverName` entries 1 and 2) is this router, not a
driver of its own. At every `OpenAdapter10` / `OpenAdapter10_2` call it picks one of three UMDs and forwards the call:

- `dwm.exe` (and any `HostedClients` entry) goes to the hosted GPU UMD `bc250d3d_zink.dll`, or to the CPU UMD by
  the kill switch `DwmForceCpu` or when the KMD's GPU DWM interop switches are off;
- every other process goes to the application GPU UMD (the DXVK-based shell `amdgpu_wddm_d3d11.dll`,
  `driver/umd/dxvk`) or to the CPU UMD, by the `AppRouter` policy (`cpu`, `allowlist`, `gpu-default`, with `Allow`
  and `Deny` lists). The sign-in and consent processes always stay on the CPU UMD. In `gpu-default` mode (the
  installer's default since 2026-10-04) Windows components - images below the Windows directory and packaged apps
  under `WindowsApps\Microsoft*` - take the GPU UMD like any other image since train b20. Until then they stayed on
  the CPU UMD, at FL 10_0 (BD-088), because Notepad, Calculator and Task Manager failed on the GPU UMD (BD-061, fixed
  by the shared A8, FP16 and RGB10A2 surfaces of the D3D11 shell and verified on b20). Image and Windows directory
  are compared as final resolved paths (`router-identity.h`), so `\\?\` prefixes, 8.3 names and junctions read as
  one form; an image or directory that cannot be resolved stays on the CPU UMD unless `Allow` names it (reason
  `app-component-unknown`);
- a Direct3D 10.0 application takes the same application decision as a D3D11 one. The 10.0, 10.1 and 11 runtimes
  of Windows 11 all open the router through `OpenAdapter10_2` and create a device at a D3D11-family interface
  (BD-081, measured on x64 and x86 on 2026-10-07: `evidence/windows/2026-10-07-BD081-d3d10-entry-devpc`). The
  router's `OpenAdapter10` export stays, and still sends every application to the CPU UMD (reason
  `app-d3d10-entry`), because the application GPU UMD has no `OpenAdapter10`; no measured runtime calls it.
  `tools/win/d3d10probe --trace-entry` shows the entry and the interface for one process;
- a failed GPU load or GPU `OpenAdapter` falls back to the CPU UMD with the caller's arguments restored.

In `dwm.exe`, the router also writes each desktop decision into the session's desktop-route record
(`driver/contract/bc250_desktop_route.h`, M15.14): `gpu`, `cpu-kill-switch`, `cpu-switches-off` or `cpu-fallback`.
The record is a named section, `Local\bc250-desktop-route`. Its owner is the compositor's account, and the record
ends when `dwm.exe` ends. The D3D12 and D3D11 shells read the record before they ask for a scan-out primary: the
CPU compositor cannot read a scan-out primary, and the fallback after a failed hosted open is visible nowhere
else. The desktop route line ends with `record=published`, `record=failed-<Win32 error>` (no record, so the shells
keep the composed primary) or `record=none` (a `HostedClients` entry, which writes nothing).

The registry interface (`HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter`, `...\AppRouter` and the KMD's
`InteropLastState`) is documented value by value at the top of `router.cpp`. The decisions themselves are free of
I/O in `router-policy.h`, so the host tests drive them directly; the path resolution in `router-identity.h` is
tested on real files (the `identity` scenario: extended prefix, 8.3 alias, junction, `Windows2`/`Windows.old`).

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
them, the application-policy ones. The desktop-route scenarios run the harness as `<layout>\dwm\dwm.exe`. They
read the record with the harness's own account as the owner, and the shells' rule over it: `route-dwm-name` (GPU,
admitted), `route-dwm-name-kill` and `route-dwm-name-fallback` (the rule stands down on the record alone, with
its `DwmForceCpu` argument 0) and `desktop-dwm-unchanged` (the last of three decisions).

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

The recipe now uses `/Brepro` on the compiler and the linker and `/FC /d1trimfile:<repo>` on the compiler
(`docs/design/reproducible-builds.md`). Two builds of one commit in two directories give the same bytes. The
comparison above stays the method for `674AD261` and for the other routers that came before this change.
