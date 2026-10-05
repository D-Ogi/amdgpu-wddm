# wsi-dxgi - the two harnesses behind the DLL shadowing and present-route evidence

A Vulkan WSI on Windows loads `DXGI.DLL`, `DComp.DLL` and `D3D11.DLL` by full System32 path. A game that
ships DXVK or vkd3d-proton has its own `dxgi.dll` next to the executable. The Windows loader resolves a
module's static imports by module name against the modules that are already loaded. A System32 module that
we load by full path can therefore bind to the game's `dxgi.dll` instead.

These two harnesses measure how far that reaches, and whether the composition present route survives it.
They produced the evidence of E56 and the facts M792, M793 and M794. Both are console processes. They open
no window, they start nothing resident, and they run on the development PC. The lab is not involved.

| File | What it is |
|---|---|
| `shadowtest.cpp` | The loader experiment: what a shadowed `dxgi.dll` reaches, and three candidate mitigations |
| `presenttest.cpp` | The present route the WSI needs, run inside three simulated game directories |
| `build.ps1` | Builds one or both harnesses from the NuGet kits |
| `run-shadowtest.ps1` | Runs each `shadowtest` mode in its own process and writes a manifest |
| `run-presenttest.ps1` | Runs each `presenttest` mode in its own process and writes a manifest |

Both sources resolve every D3D, DXGI and DirectComposition entry point with `GetProcAddress`, so the
executables import none of those modules themselves. That is what lets them decide which copy of a module
a call reaches.

The source files keep their exact bytes in this repository. `.gitattributes` holds them out of line-ending
normalization, because the E56 manifests record their SHA-256 sums. `build.ps1` prints those sums.

## The shadowtest modes

| Mode | What it sets up |
|---|---|
| `control` | No application-local `dxgi.dll`. The baseline of the loader behaviour |
| `shadow` | The application-local `dxgi.dll` by name first, then System32 `dxgi` and `d3d11` by full path |
| `shadow-rebind` | `shadow` plus a rewrite of the `d3d11` import table and the `dcomp` delay import table to the System32 modules |
| `shadow-actctx` | `shadow` plus an activation context that holds a `loadFrom` to System32 |
| `shadow-search` | `shadow` plus `LOAD_LIBRARY_SEARCH_SYSTEM32` on the load of `d3d11` |
| `sysfirst` | System32 `dxgi` by full path first, then the same name again by name |

## The presenttest modes

A mode name is a scenario and a policy. The scenario selects the application-local DLLs. The policy selects
the loader behaviour.

| Scenario | Application-local DLLs |
|---|---|
| `control` | None. A pure Vulkan game |
| `dxvk11` | DXVK `d3d11.dll` and `dxgi.dll`. A DXVK D3D11 game |
| `vkd3d` | DXVK `dxgi.dll` and vkd3d-proton `d3d12.dll` with `d3d12core.dll`. A D3D12 game |

| Policy | What it does |
|---|---|
| `plain` | The route as the DXGI WSI would run it. System32 modules by full path and an explicit adapter |
| `seal` | `plain` plus the proposed seal. It rebinds the import tables of the System32 graphics modules to System32 modules that are already loaded, and it redirects bare-name loader calls made from those modules. It loads nothing itself |

The route is one System32 `CreateDXGIFactory2`, `EnumAdapterByLuid` on the wanted LUID, a presenter and a
producer D3D11 device on that adapter, an NT shared texture, a cross-device shared fence, a DirectComposition
device and visual with no target, `CreateSwapChainForComposition`, four frames of `CopyResource` and
`Present1` against the fence, `ResizeBuffers`, and one `ALLOW_TEARING` swap chain. The harness traces every
`LoadLibrary` and `GetModuleHandle` call that a System32 graphics module makes.

## Build and run

```
pwsh -NoProfile -File tools\win\wsi-dxgi\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
pwsh -NoProfile -File tools\win\wsi-dxgi\run-shadowtest.ps1 -Exe <build>\shadowtest.exe `
     -Out <BC250_ROOT>\scratch\m16\wsi-dxgi\logs -AppLocalDxgi <package>\dxgi.dll
```

`-Out` of `build.ps1` defaults to `<BC250_ROOT>\scratch\build\wsi-dxgi`. The workspace root comes from
`BC250_ROOT`, and else from `-Kits`, which points at `<BC250_ROOT>\toolchain\nuget`. Neither script guesses
the root from its own place in the file system, because a build from a git worktree sits somewhere else.
The flags are `/std:c++20 /EHsc /W4 /WX /MD /O2`, which are the flags of the E56 builds.

The application-local DLLs are copies of builds that live in the workspace, never in this repository. Each
runner pins the copy it was given by SHA-256 and stops when the hash does not match, so a result cannot
claim a build it did not use. `run-presenttest.ps1` skips a scenario whose DLLs it did not get, so one run
can cover a subset.

A run writes one log per mode, one directory of DXVK logs per mode, and a `manifest.json` with the harness
hash, the application-local hashes, the System32 module versions, and the exit code and wall time of each
mode. Keep that manifest with the logs. It is the record of what the numbers came from.

The two runners take their paths as parameters. The E56 runs of 2026-09-28 used the same logic from the
workspace design directory with the paths written into the script.

## Limits of this instrument

The measurements of E56 ran against the development PC's own user-mode driver, with no window and therefore
no `CreateTargetForHwnd`, in synthetic application directories and not in a real game. A present interval
measured here is the vsync pace of that machine. None of it is evidence about our driver or about unit A.

The import and export offsets that M794 records come from Microsoft binaries and can move with a Windows
update. The WSI gate must read the bindings at run time. The listings behind those offsets stay outside this
repository.
