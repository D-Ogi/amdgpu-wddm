# Architecture

## What "native" means here

Applications call the standard D3D11, D3D12 and DXGI runtimes in System32, which load the installed driver of
the adapter. Nothing of ours is placed next to the application (the M13 criterion of the
[roadmap](00-goal-and-roadmap.md)). Inside that driver, DXVK (D3D11), vkd3d-proton (D3D12) and RADV
(Vulkan) are internal backends, loaded by our user-mode driver, not by the application: the owner's decision,
recorded in [ADR 0017](adr/0017-graphics-stack-direction-after-m12.md) items 1, 2, 4 and 5. That is the
intended architecture. What is measured today is less:

- **D3D11:** the system runtime renders on the GPU through our user-mode driver in bounded test clients
  ([M736](facts/d3d.md#m736), [M747](facts/d3d.md#m747), [M752](facts/d3d.md#m752)) and creates a device at FL 12_1
  ([M770](facts/d3d.md#m770)). An application router, registered in the
  D3D10 and D3D11 slots, sends ordinary D3D11 applications to that driver, and retail titles ran on it in
  the lab. Windows components keep the CPU desktop driver, which offers FL 10_0
  ([M780](facts/d3d.md#m780)). DWM itself composes on the GPU
  through a separate hosted driver ([M772](facts/games.md#m772)).
- **D3D12:** the system runtime loads our registered D3D12 driver and renders through it. Measured so far:
  - copy, draw, scene and Present exact ([M778](facts/d3d.md#m778), [M779](facts/d3d.md#m779)).
  - a device at FL 12_1 under the driver's defaults ([M780](facts/d3d.md#m780)).
  - ROV and conservative rasterization tier 3 exact ([M781](facts/d3d.md#m781)).
  - DXR exact ([M782](facts/d3d.md#m782)). Ray-tracing tier 1.1 became the driver's default report
    after that fact, in commit `7373ae6c`, which no fact states yet.
  - The Witcher 3 Remaster DX12 edition with RT, every screenshot correct ([M775](facts/games.md#m775),
    [M777](facts/games.md#m777)).

  These are functional tests on one unit, not a conformance run. The earlier Witcher 3 results
  ([M571](facts/games.md#m571), [M573](facts/games.md#m573)) were per-application: vkd3d-proton and DXVK's `dxgi.dll` sat next
  to the game, which is exactly what the native design removes.

## Layers

```
application: D3D11 / D3D12 / DXGI calls, nothing of ours next to it
  -> Microsoft runtime in System32: d3d11.dll, d3d12.dll + D3D12Core.dll, dxgi.dll
  -> our user-mode driver (the adapter's UserModeDriverName)
       D3D11: shell amdgpu_wddm_d3d11.dll (driver/umd/dxvk)             + engine amdgpu_wddm_dxvk.dll
       D3D12: shell amdgpu_wddm_d3d12.dll (driver/umd/d3d12, engine-ddi/) + engine amdgpu_wddm_vkd3d.dll
       desktop today: a router sends DWM to bc250d3d_zink.dll (Mesa d3d10umd on Zink over hosted RADV)
                      and other D3D10/11 processes to bc250d3d.dll (the same frontend on llvmpipe, CPU)
  -> hosted RADV amdgpu_wddm_radv.dll, driven through the runtime's callbacks
  -> dxgkrnl (VidMm, VidSch) -> bc250kmd.sys (driver/kmd) -> GPU
Vulkan applications: Vulkan loader -> vulkan_radeon.dll (system ICD) -> dxgkrnl -> bc250kmd.sys -> GPU
```

| File | Role | Built by | State of the name |
|---|---|---|---|
| `bc250kmd.sys`, service `bc250kmd` | kernel-mode driver | `driver/kmd/build.ps1` | Baseline, not renamed |
| `bc250d3d.dll` | desktop D3D10/11 UMD on llvmpipe | `tools/build/mesa-configs.json` `llvmpipe-umd` | Baseline for D3D10/11 processes other than DWM, and DWM's CPU fallback route ([M771](facts/display.md#m771)) |
| `bc250d3d_zink.dll` | the same frontend on Zink over hosted RADV | `mesa-configs.json` `zink-umd` | Deployed: DWM composes on it since 2026-10-01 ([M772](facts/games.md#m772)) |
| `vulkan_radeon.dll` | system Vulkan ICD | `mesa-configs.json` `radv` | Baseline (upstream RADV name) |
| `amdgpu_wddm_radv.dll` | hosted RADV loaded by the D3D11 and D3D12 shells | no recipe: a `vulkan_radeon.dll` build staged under this name | Registered beside the D3D12 shell ([M780](facts/d3d.md#m780)). Was `bc250radv.dll` |
| `bc250d3d_router.dll` | the registered D3D10/11 UMD: routes DWM and applications to one of the UMDs here | `tools/build/build-umd-router.ps1` | Registered since 2026-10-01. Its shipped policy is `gpu-default`, which sends ordinary applications to the GPU UMD (M14.1) |
| `amdgpu_wddm_d3d11.dll` | D3D10/11 DDI shell | `tools/build/build-umd-dxvk.ps1` | Renamed from `bc250d3d11.dll` (M756). The application router reaches it, and the router's shipped default policy sends ordinary D3D11 applications there (M14.1, [M770](facts/d3d.md#m770)) |
| `amdgpu_wddm_dxvk.dll` | DXVK engine | `tools/build/dxvk-configs.json` `ddi-engine` | Shipped in the release as `D4C4BE86` ([Registered on unit A](#registered-on-unit-a)). Renamed from `bc250dxvk.dll` (M755) |
| `amdgpu_wddm_d3d12.dll` | D3D12 DDI shell over the vkd3d-proton engine (`engine-ddi/`) | `tools/build/build-umd-d3d12.ps1` | Registered as the adapter's D3D12 user-mode driver on unit A ([M780](facts/d3d.md#m780)). It began as the diagnostic shell of M763 |
| `amdgpu_wddm_vkd3d.dll` | vkd3d-proton engine | `tools/build/vkd3d-configs.json` `ddi-engine` | Registered beside the D3D12 shell ([M780](facts/d3d.md#m780)). Renamed from `bc250vkd3d.dll`. Standalone controls M757, M759 |
| `amdgpu_wddm_mft_h264.dll` | H.264 encoder Media Foundation transform, eight stages on D3D11 compute | `driver/umd/mft-h264/build.ps1` | Shipped in the release and registered machine wide. The cases of 2026-10-06 loaded their own copy of the transform and encoded on unit A's GPU ([M801](facts/d3d.md#m801)). No case activated the machine-wide copy yet |

## Registered on unit A

The lab unit runs the release package 0.7.216.100-tester.23 (train b25), installed on 2026-10-09. The
table below is that package, with the revision each binary was built from, as
`tools/release/release-sources.json` records it. The SHA-256 column is the first four bytes of the file's hash,
which is how the lab's own records name a build. The package signs the kernel driver and its catalog again, so the
hash of `bc250kmd.sys` here is not the hash of the build directory it comes from.

| Binary | SHA-256 | Repository, branch | Commit |
|---|---|---|---|
| `bc250kmd.sys` 0.7.216.24 (escape ABI `0x000700D8`) | `D2675C6E` | this repo, `train/b25` | `00f09bfa` |
| `bc250kmd_cli.exe`, `bc250control.dll` and the control application 0.7.0.0 | `9DEDF3A3`, `7B291BCF`, `E2E5578E` | this repo, `train/b25` | branch head `2a7f6dfa` |
| `amdgpu_wddm_d3d12.dll`, the D3D12 shell | `C228AFFE` | this repo, `train/b24` | `1884b33d` |
| `amdgpu_wddm_vkd3d.dll`, the D3D12 engine | `DEF9A60B` | [vkd3d-proton fork](https://github.com/D-Ogi/vkd3d-proton), `amdgpu-wddm/per-app-graphics-settings` | `41bd52c8` |
| `amdgpu_wddm_radv.dll`, hosted RADV for the D3D12 and the GPU D3D11 route (one build for both) | `20A593DC` | [Mesa fork](https://github.com/D-Ogi/mesa-amdgpu-wddm), `amdgpu-wddm/rottr486-null-vs` | `aca2c81b` |
| `amdgpu_wddm_d3d11.dll`, the GPU D3D11 shell | `2E8D6BD6` | this repo, `train/b24` | `1884b33d` |
| `amdgpu_wddm_dxvk.dll`, the D3D11 engine | `D4C4BE86` | [DXVK fork](https://github.com/D-Ogi/dxvk), `amdgpu-wddm/per-app-graphics-settings` | `0898891f` |
| `bc250d3d_router.dll`, the application router in the D3D10 and D3D11 slots | `71D14725` | this repo, `train/b24` | `1884b33d` |
| `bc250d3d.dll`, the CPU D3D10/11 UMD, the router's fallback route | `A57F7376` | Mesa fork, `amdgpu-wddm/b19-desktop-umd` | `76ab2c27` |
| `bc250d3d_zink.dll`, the compositor's UMD | `CA812652` | Mesa fork, `amdgpu-wddm/b19-hosted-umd` | `ea876500` |
| hosted RADV under that UMD | `CD360941` | Mesa fork, `amdgpu-wddm/radv-wddm2-hosted-main` | `9ddfcbe2` |
| `vulkan_radeon.dll`, the system Vulkan ICD | `4E3F393F` | Mesa fork, `amdgpu-wddm/b25-system-icd` | `d3da6d0a` |
| `amdgpu_wddm_mft_h264.dll`, the H.264 encoder transform | `D3E29E6E` | this repo, in `main` | `689852a8` |
| the 32-bit set for WoW64 processes: router, GPU D3D11 shell and engine, ICD, CPU UMD, D3D12 shell and engine | `0CD45DF1`, `044A4B25`, `BD1900C7`, `887A08E5`, `FF33F4B1`, `4D74021E`, `76426250` | this repo `train/b24`, DXVK fork `per-app-graphics-settings`, Mesa fork `rottr486-null-vs` and `b19-desktop-umd`, vkd3d-proton fork `per-app-graphics-settings-x86` | `1884b33d`, `0898891f`, `aca2c81b`, `76ab2c27`, `b477495e` |

Every binary of that release comes from a commit published at its own hash, and no file takes a patch on top of
its commit. The one exception is the control triple, where the manifest names the train branch and not one
commit. Its head `2a7f6dfa` is in `main` as the merge `5fc83d67`. That closes the provenance half of M15.10 in
the [reconciliation](m15-reconciliation.md). A rebuild gives the same hash for the files that this
repository's own `cl` and `link` recipes build. Those recipes pass `/Brepro`, and two builds of one tree into two
directories gave one hash. The list is the kernel driver, the D3D12 shell with the engine-ddi libraries, the x64
and the x86 D3D11 shell, the H.264 transform, the control application, the control DLL and the CLI.

The kernel driver joined that list with this release family. `driver/kmd/build.ps1` passes `/Brepro` and sorts
its object list, and two builds of this driver into two empty directories gave the same unsigned and the same
signed image. Only the catalog differed between them, because each build signs a new one
(`tools/release/release-sources.json`). Two groups of release files stay outside `/Brepro`. The first has no
`/Brepro` in its recipe. For the x64 and the x86 router that is a stated choice: the recipe is the one that built
the registered router `674AD261`, and a `/Brepro` link would no longer be that recipe. A rebuild of the router
differs only in its timestamps and PDB GUID, which `tools/build/pe_compare.py` compares field by field. The
router's own record gives that comparison for `674AD261`
([driver/umd/router](../driver/umd/router/README.md#reproducing-674ad261)), and
`tools/release/release-sources.json` records it for the shipped router. `amdgpu_wddm_d3d12caps.exe` carries no
`/Brepro` and no note about it. The second group is every file
that Meson builds, which [Reproducible builds](design/reproducible-builds.md) leaves outside: the D3D12
engine, both D3D11 engines, the three RADV builds, the Zink UMD, both CPU UMDs, the system Vulkan ICD and
`vulkaninfo.exe`.

