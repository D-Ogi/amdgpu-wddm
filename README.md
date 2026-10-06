# amdgpu-wddm

An open Windows (WDDM) driver stack for AMD GPUs, developed first on the ASRock BC-250 (AMD Cyan Skillfish APU,
GFX 10.1.3, PCI `1002:13FE`, VRAM carve-out, no resizable BAR): a WDDM kernel-mode driver built around AMD's own
amdgpu code, and user-mode drivers that put Mesa's RADV, DXVK and vkd3d-proton behind the standard Windows
graphics runtimes. It is a test-signed development stack measured on one lab unit ("unit A"), not a driver for
end users. The working name inside the workspace is `bc250-win`. Not affiliated with or endorsed by AMD, ASRock
or Microsoft.

## What "native" means here

Applications call the standard D3D11, D3D12 and DXGI runtimes in System32, which load the installed driver of
the adapter; nothing of ours is placed next to the application (the M13 criterion of the
[roadmap](docs/00-goal-and-roadmap.md)). Inside that driver, DXVK (D3D11), vkd3d-proton (D3D12) and RADV
(Vulkan) are internal backends, loaded by our user-mode driver, not by the application: the owner's decision,
recorded in [ADR 0017](docs/adr/0017-graphics-stack-direction-after-m12.md) items 1, 2, 4 and 5. That is the
intended architecture. What is measured today is less:

- **D3D11:** the system runtime renders on the GPU through our user-mode driver in bounded test clients
  ([M736](docs/facts/d3d.md#m736), [M747](docs/facts/d3d.md#m747), [M752](docs/facts/d3d.md#m752)) and creates a device at FL 12_1
  ([M770](docs/facts/d3d.md#m770)). An application router, registered in the
  D3D10 and D3D11 slots, sends ordinary D3D11 applications to that driver, and retail titles ran on it in
  the lab. Windows components keep the CPU desktop driver, which offers FL 10_0
  ([M780](docs/facts/d3d.md#m780)). DWM itself composes on the GPU
  through a separate hosted driver ([M772](docs/facts/games.md#m772)).
- **D3D12:** the system runtime loads our registered D3D12 driver and renders through it. Measured so far:
  - copy, draw, scene and Present exact ([M778](docs/facts/d3d.md#m778), [M779](docs/facts/d3d.md#m779));
  - a device at FL 12_1 under the driver's defaults ([M780](docs/facts/d3d.md#m780));
  - ROV and conservative rasterization tier 3 exact ([M781](docs/facts/d3d.md#m781));
  - DXR exact ([M782](docs/facts/d3d.md#m782)). Ray-tracing tier 1.1 became the driver's default report
    after that fact, in commit `7373ae6c`, which no fact states yet;
  - The Witcher 3 next-gen DX12 edition with RT, every screenshot correct ([M775](docs/facts/games.md#m775),
    [M777](docs/facts/games.md#m777)).

  These are functional tests on one unit, not a conformance run. The earlier Witcher 3 results
  ([M571](docs/facts/games.md#m571), [M573](docs/facts/games.md#m573)) were per-application: vkd3d-proton and DXVK's `dxgi.dll` sat next
  to the game, which is exactly what the native design removes.

## Status (as of 2026-10-06)

Every row cites [docs/facts.md](docs/facts.md), where each fact links its evidence. Open means open. Roadmap:
M0-M6, M8 and M10 are closed; M7, M9, M11, M12 and M13 are open. M14 and M15 are proposed numbers (ADR 0017);
their criteria and the state of each are in [docs/m15-reconciliation.md](docs/m15-reconciliation.md). The unit
runs the release package 0.7.208.100-tester.13 (train b19), installed on 2026-10-06, whose every binary is in
[Registered on unit A](#registered-on-unit-a).

Two throughput workloads compare Windows with Linux on speed on this unit. At an equal shader clock the installed
release is faster than Linux on llama.cpp ([M816](docs/facts/games.md#m816)). VRAM writes measured 16 to
17 % slower, on kernel driver 0.7.193.1 and at a shader clock that differs between the two sides
([M807](docs/facts/hardware.md#m807), [M776](docs/facts/games.md#m776)). No game ran on both systems.

| Area | State | Facts | Limits and open items |
|---|---|---|---|
| Kernel driver | Full WDDM lab driver with GPU submission and display scanout. CPU-desktop baseline KMD 171; the sessions of M775, M777 and M781 ran 0.7.185.1 to 0.7.193.1, the game sessions with DPM up to 2000 MHz. The installed release b19 runs 0.7.208.1 (escape ABI `0x000700D0`) with the driver's own DPM governor under a 1500 MHz ceiling, at 40 compute units | [M727](docs/facts/kmd.md#m727), [M775](docs/facts/games.md#m775), [M781](docs/facts/d3d.md#m781), [M801](docs/facts/d3d.md#m801), [M816](docs/facts/games.md#m816) | Development, test-signed stack: not general Windows driver compatibility or certified recovery. TDR recovery is not implemented ([M179](docs/facts/kmd.md#m179)): a GPU hang ends in a bugcheck (M15.12 in the [reconciliation](docs/m15-reconciliation.md)). KMD 171 was built from revision `75b8ab6f`, not from `main` ([provenance](evidence/windows/2026-09-28-E34-m14-kmd171/RESULT.md)). The driver registered today, 0.7.208.1, comes from `85ac47b6` on `train/b19-setup`, which is in this repository's history. [Registered on unit A](#registered-on-unit-a) names the source of every binary |
| GPU desktop | DWM composes on the GPU through a router, hosted Zink and RADV. This has been the lab's default desktop since 2026-10-01, after the ladder T1-T7: correct images; the 8-bit Present client exact and the 10-bit one within 1 LSB; The Witcher 3 (with the game's FSR 2 upscaling) at 35.5 frames/s against 21.1 on the CPU-composed desktop. G0 is met for the DWM050 path | [M723](docs/facts/display.md#m723), [M724](docs/facts/display.md#m724), [M771](docs/facts/display.md#m771), [M772](docs/facts/games.md#m772) | Full M13 ([gates](docs/m13-accelerated-desktop-roadmap.md)) is open, among them the lifecycle transitions of M13.6 |
| Vulkan ICD | RADV with a WDDM2 winsys, the lab's registered Vulkan driver. Compute hashes equal CPU and Linux (M8); llama.cpp text equals the Linux GPU text; first picture (M10, CPU presentation). On the installed release, llama.cpp is faster than Linux on the same unit at the same shader clock. At 1000 MHz TinyLlama gives prompt 1813 against 1120 t/s and generation 203 against 155 t/s. The whole CTS sparse-resources list: 10778 pass, 8299 not supported, 0 fail of 19078 | [M139](docs/facts/icd.md#m139), [M163](docs/facts/icd.md#m163), [M476](docs/facts/display.md#m476), [M665](docs/facts/icd.md#m665), [M773](docs/facts/icd.md#m773), [M816](docs/facts/games.md#m816) | Only two CTS groups have run: basic compute (75 pass, 5 not supported, 0 fail of 80, [M480](docs/facts/icd.md#m480)) and sparse resources (M773). The must-pass list and Linux parity are open; a native-sparse control hung on an earlier kernel driver ([M483](docs/facts/icd.md#m483)). M9 performance and paging open |
| Native D3D11 (proposed M14) | System-runtime GPU rendering at FL 11_1, images byte-equal to per-application DXVK on the same GPU; native window Present and resize; error propagation and recovery; a device at FL 12_1 with tiled resources tier 3, ROVs and conservative rasterization tier 3 reported, tiled operations exact | [M736](docs/facts/d3d.md#m736), [M739](docs/facts/d3d.md#m739), [M747](docs/facts/d3d.md#m747), [M752](docs/facts/d3d.md#m752), [M756](docs/facts/d3d.md#m756), [M770](docs/facts/d3d.md#m770) | The release installer registers the application router in the D3D10 and D3D11 slots, and its default policy sends ordinary D3D11 applications to the GPU driver (M14.1). Windows components and D3D10.0 applications stay on the CPU desktop driver, which offers FL 10_0 ([M780](docs/facts/d3d.md#m780)). Not measured: the 5 % bound against per-application DXVK (ADR 0017). Partly done: threading (M14.6), where our own multithreaded client passes but the engine stays inline, and residency, offer and reclaim (M14.8), where the shell forwards runtime-backed surfaces to the kernel and answers engine-private ones with hints. FL 12_1 is a functional witness, not a conformance run. No broad game compatibility |
| Native D3D12 (proposed M15) | The system `d3d12.dll` and `dxgi.dll` load our D3D12 driver (shell, vkd3d-proton engine, hosted RADV), which is registered between sessions. Measured through it: buffer copy, draw and indexed scene exact; 8-bit swap-chain Present and ResizeBuffers with exact frames, and 10-bit within 1 LSB under GPU composition; a device at FL 12_1; ROV and conservative rasterization exact; DXR, with tier 1.1 as the driver's default report. That default came in commit `7373ae6c`. The Witcher 3 next-gen DX12 edition runs at low, high and high with RT (45.3, 31.3 and 9.3 frames/s at native 1080p, every shot correct), also started from Steam. Those frame rates come from kernel driver 0.7.193.1 on 2026-10-01, not from the installed release | [M771](docs/facts/display.md#m771), [M775](docs/facts/games.md#m775), [M777](docs/facts/games.md#m777), [M778](docs/facts/d3d.md#m778)-[M782](docs/facts/d3d.md#m782) | Functional tests on one unit, not full D3D12 compatibility. No run of a D3D12 conformance suite or of a DXR subset. Stability is open (M15.8): game sessions have ended in 0x116 bugchecks, and a hang cannot be recovered (M15.12). Also open: the streaming-noise acceptance of M15.7 and the per-application comparison of M15.9. Every binary of the installed release comes from a published commit, each in [Registered on unit A](#registered-on-unit-a). A rebuild gives the same bytes only for the files that this repository's own `cl` and `link` recipes build (M15.10). Criteria and status: [reconciliation](docs/m15-reconciliation.md) |
| D3D12 engine | The vkd3d-proton fork, built as `amdgpu_wddm_vkd3d.dll` and loaded by the D3D12 shell behind the runtime's DDI in every native trial. Standalone controls: GPU copy/readback and DXIL compute on RADV | [M757](docs/facts/d3d.md#m757), [M759](docs/facts/d3d.md#m759), [M778](docs/facts/d3d.md#m778)-[M782](docs/facts/d3d.md#m782) | The engine of the installed release is `4e9a98e9` on `amdgpu-wddm/upstream-2026-10-05` of the fork, as [Registered on unit A](#registered-on-unit-a) records |
| Per-app D3D12 | vkd3d-proton and DXVK's `dxgi.dll` next to the game: The Witcher 3 next-gen DX12 menu and in-game scene on our ICD, with an experimental sparse flag | [M571](docs/facts/games.md#m571), [M573](docs/facts/games.md#m573), [M580](docs/facts/games.md#m580) | No longer the M15 route (owner, 2026-09-28; native D3D12 above). It stays the reference for the per-application comparison of M15.9. Its frame rates came from the retired CPU present path ([M577](docs/facts/games.md#m577), [M610](docs/facts/games.md#m610)) and do not compare with the native ones |
| Ray tracing | Vulkan: ray-query and `TraceRays` CTS controls, and hardware BVH intersection in the compiled program. Native DXR through the system runtime: inline ray query, state objects, DispatchRays and indirect DispatchRays exact. The Witcher 3 renders with RT on through it | [M760](docs/facts/icd.md#m760)-[M762](docs/facts/icd.md#m762), [M775](docs/facts/games.md#m775), [M777](docs/facts/games.md#m777), [M782](docs/facts/d3d.md#m782) | RaytracingTier 1.1 is the driver's default report since commit `7373ae6c` (`driver/umd/d3d12/adapter-caps.cpp`, `engine-ddi/caps.cpp`), and `raytracing-tier` is a no-op positive name. M782 and M780 still read the shells before that commit: tier 1.1 with the switch and tier 0 without it. The witness on the registered build is lab check 422, which is in no fact row and in no `evidence/` directory yet. No DXR conformance subset has run, and collections, `AddToStateObject` and local root arguments are not measured. With RT the game runs at 9.1-9.3 frames/s at native 1080p, the 3D engine 98 % busy |
| FL 12_1 | Reached through the system runtime. D3D12: a device at FL 12_1 with no switch, 12_2 refused, tiled resources tier 3 under the driver's default sparse policy, and a reserved-resource round trip exact. ROV and conservative rasterization tier 3, inner coverage included, exact. D3D11: a device at FL 12_1, tiled operations exact | [M770](docs/facts/d3d.md#m770), [M773](docs/facts/icd.md#m773), [M780](docs/facts/d3d.md#m780), [M781](docs/facts/d3d.md#m781) | Functional witnesses, not conformance: no D3D12 or D3D11 conformance-suite run at this level, and no D3D12-level tiled-resources subset. Vulkan sparse CTS has 0 fails, but 8299 of 19078 cases are not supported, 4715 of them because they need a second device (M773). The ROV and conservative client has not been rerun on the triplets registered after 2026-10-01 (M781). FL 12_2 is not offered: no mesh shaders, sampler feedback or VRS |
| Linux reference | amdgpu and RADV on the same unit: register baseline, Vulkan compute hashes, llama.cpp and clock/throughput numbers | [M1](docs/facts/hardware.md#m1), [M50](docs/facts/linux.md#m50)-[M52](docs/facts/hardware.md#m52), [M816](docs/facts/games.md#m816) | No working GPU reset under Linux either ([M53](docs/facts/linux.md#m53)). The first comparison at an equal clock is [M816](docs/facts/games.md#m816), where the Windows stack leads on llama.cpp. Mesa differs between the two sides there, so that comparison is between two stacks, not between two operating systems with one Mesa. VRAM bandwidth still goes the other way: Linux writes 445 GB/s against 370 under Windows ([M807](docs/facts/hardware.md#m807)). The pre-driver register baseline now covers recorded starts. The power-up state differs at each cold start ([M812](docs/facts/hardware.md#m812)). A warm restart keeps the register contents that no reset touches ([M813](docs/facts/hardware.md#m813)). `MP0_SMN_C2PMSG_81` tells a cold start from a warm restart ([M814](docs/facts/hardware.md#m814)). No game ran on both systems. Both halves of M12's Linux parity are open. M12 (2) asks for a written benchmark set with rendering, at one Mesa version, within 10 % of Linux. M816 is one compute benchmark with two Mesa versions |

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
| `bc250d3d.dll` | desktop D3D10/11 UMD on llvmpipe | `tools/build/mesa-configs.json` `llvmpipe-umd` | Baseline for D3D10/11 processes other than DWM, and DWM's CPU fallback route ([M771](docs/facts/display.md#m771)) |
| `bc250d3d_zink.dll` | the same frontend on Zink over hosted RADV | `mesa-configs.json` `zink-umd` | Deployed: DWM composes on it since 2026-10-01 ([M772](docs/facts/games.md#m772)) |
| `vulkan_radeon.dll` | system Vulkan ICD | `mesa-configs.json` `radv` | Baseline (upstream RADV name) |
| `amdgpu_wddm_radv.dll` | hosted RADV loaded by the D3D11 and D3D12 shells | no recipe: a `vulkan_radeon.dll` build staged under this name | Registered beside the D3D12 shell ([M780](docs/facts/d3d.md#m780)); was `bc250radv.dll` |
| `bc250d3d_router.dll` | the registered D3D10/11 UMD: routes DWM and applications to one of the UMDs here | `tools/build/build-umd-router.ps1` | Registered since 2026-10-01. Its shipped policy is `gpu-default`, which sends ordinary applications to the GPU UMD (M14.1) |
| `amdgpu_wddm_d3d11.dll` | D3D10/11 DDI shell | `tools/build/build-umd-dxvk.ps1` | Renamed from `bc250d3d11.dll` (M756). The application router reaches it, and the router's shipped default policy sends ordinary D3D11 applications there (M14.1, [M770](docs/facts/d3d.md#m770)) |
| `amdgpu_wddm_dxvk.dll` | DXVK engine | `tools/build/dxvk-configs.json` `ddi-engine` | Shipped in the release as `C242BA6A` ([Registered on unit A](#registered-on-unit-a)). Renamed from `bc250dxvk.dll` (M755) |
| `amdgpu_wddm_d3d12.dll` | D3D12 DDI shell over the vkd3d-proton engine (`engine-ddi/`) | `tools/build/build-umd-d3d12.ps1` | Registered as the adapter's D3D12 user-mode driver on unit A ([M780](docs/facts/d3d.md#m780)); it began as the diagnostic shell of M763 |
| `amdgpu_wddm_vkd3d.dll` | vkd3d-proton engine | `tools/build/vkd3d-configs.json` `ddi-engine` | Registered beside the D3D12 shell ([M780](docs/facts/d3d.md#m780)); renamed from `bc250vkd3d.dll`. Standalone controls M757, M759 |
| `amdgpu_wddm_mft_h264.dll` | H.264 encoder Media Foundation transform, eight stages on D3D11 compute | `driver/umd/mft-h264/build.ps1` | Shipped in the release and registered machine wide. The cases of 2026-10-06 loaded their own copy of the transform and encoded on unit A's GPU ([M801](docs/facts/d3d.md#m801)). No case activated the machine-wide copy yet |

## Registered on unit A

The lab unit runs the release package 0.7.208.100-tester.13 (train b19), installed on 2026-10-06T06:44Z. The
table below is that package, with the revision each binary was built from, as
`tools/release/release-sources.json` records it. The SHA-256 column is the first four bytes of the file's hash,
which is how the lab's own records name a build.

| Binary | SHA-256 | Repository, branch | Commit |
|---|---|---|---|
| `bc250kmd.sys` 0.7.208.1 (escape ABI `0x000700D0`) | `7FF11DE1` | this repo, `train/b19-setup` | `85ac47b6` |
| `bc250kmd_cli.exe`, `bc250control.dll` and the control application 0.5.1.0 | `0AD206EC`, `BB04D72D`, `BD047F75` | this repo, `train/b19-setup` | `3ef45a0f` |
| `amdgpu_wddm_d3d12.dll`, the D3D12 shell | `C82938C9` | this repo, `train/b19-setup` | `62aa9bc0` |
| `amdgpu_wddm_vkd3d.dll`, the D3D12 engine | `348117F1` | [vkd3d-proton fork](https://github.com/D-Ogi/vkd3d-proton), `amdgpu-wddm/upstream-2026-10-05` | `4e9a98e9` |
| `amdgpu_wddm_radv.dll`, hosted RADV for the D3D12 and the GPU D3D11 route (one build for both) | `822134D0` | [Mesa fork](https://github.com/D-Ogi/mesa-amdgpu-wddm), `amdgpu-wddm/b19-icd` | `31844893` |
| `amdgpu_wddm_d3d11.dll`, the GPU D3D11 shell | `C15FE971` | this repo, `train/b19-setup` | `85ac47b6` |
| `amdgpu_wddm_dxvk.dll`, the D3D11 engine | `C242BA6A` | [DXVK fork](https://github.com/D-Ogi/dxvk), `amdgpu-wddm/upstream-2026-10-05` | `5611118e` |
| `bc250d3d_router.dll`, the application router in the D3D10 and D3D11 slots | `11385954` | this repo, `train/b19-wow64-x86` | `a748e78e` |
| `bc250d3d.dll`, the CPU D3D10/11 UMD, the router's fallback route | `A57F7376` | Mesa fork, `amdgpu-wddm/b19-desktop-umd` | `76ab2c27` |
| `bc250d3d_zink.dll`, the compositor's UMD | `17D034D7` | Mesa fork, `amdgpu-wddm/b19-hosted-umd` | `7117b189` |
| hosted RADV under that UMD | `66FE8F31` | Mesa fork, `amdgpu-wddm/radv-wddm2-hosted-main` | `48546c73` |
| `vulkan_radeon.dll`, the system Vulkan ICD | `CF3948D6` | Mesa fork, `amdgpu-wddm/gdi-immediate-quiet` | `2732f9c8` |
| `amdgpu_wddm_mft_h264.dll`, the H.264 encoder transform | `141969A3` | this repo, `m15/video-encode-mft` | `70b6f838` |
| the 32-bit set for WoW64 processes: router, GPU D3D11 shell, engine, ICD, CPU UMD | `CD03C5ED`, `387F25DD`, `A42ADB6E`, `F6392B51`, `FF33F4B1` | this repo `train/b19-wow64-x86` and `train/b19-setup`, DXVK fork `upstream-2026-10-05`, Mesa fork `b19-icd` and `b19-desktop-umd` | `a748e78e`, `85ac47b6`, `5611118e`, `31844893`, `76ab2c27` |

Every binary of that release comes from a commit published at its own hash, and no file takes a patch on top of
its commit. That closes the provenance half of M15.10 in the
[reconciliation](docs/m15-reconciliation.md), and it replaces the two working trees the earlier text named. A
rebuild gives the same hash for the files that this repository's own `cl` and `link` recipes build. Those recipes
pass `/Brepro`, and two builds of one tree into two directories gave one hash. The list is the D3D12 shell with
the engine-ddi libraries, the x64 and the x86 D3D11 shell, the H.264 transform, the control application, the
control DLL and the CLI.

Three groups of release files stay outside `/Brepro`. The kernel driver is the open recipe. `driver/kmd/build.ps1`
has no `/Brepro` yet, so its PE timestamp, PDB signature and Authenticode signature differ on every build. The
per-file source manifest beside the driver is what the release compares. The change that makes the driver
reproduce is measured on branch `train/b19-repro-kmd` and rides the next package. The second group has no
`/Brepro` in its recipe. For the x64 and the x86 router that is a stated choice: the recipe is the one that built
the registered router `674AD261`, and a `/Brepro` link would no longer be that recipe. A rebuild of the router
differs only in its timestamps and PDB GUID, which `tools/build/pe_compare.py` compares field by field. The
router's own record shows that comparison for `674AD261`
([driver/umd/router](driver/umd/router/README.md#reproducing-674ad261)), and
`tools/release/release-sources.json` records it for the shipped router `11385954`. The x64 and the x86 D3D9 stub
and `amdgpu_wddm_d3d12caps.exe` carry no `/Brepro` and no note about it. The third group is every file
that Meson builds, which [Reproducible builds](docs/design/reproducible-builds.md) leaves outside: the D3D12
engine, both D3D11 engines, the three RADV builds, the Zink UMD, both CPU UMDs, the system Vulkan ICD and
`vulkaninfo.exe`.

## Repositories

| Repository | Branches | What it holds |
|---|---|---|
| [D-Ogi/amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm) (this one) | `main` | Kernel-mode driver, UMD shells, contracts, tools, experiments, evidence and facts |
| [D-Ogi/mesa-amdgpu-wddm](https://github.com/D-Ogi/mesa-amdgpu-wddm) | `amdgpu-wddm/*`, listed in `README-amdgpu-wddm.md` on `amdgpu-wddm/readme` | RADV with the WDDM2 winsys and the `d3d10umd` UMD builds (llvmpipe, Zink). The ICD of the release is `amdgpu-wddm/b19-icd`, one build for the D3D12 and the GPU D3D11 route. The compositor's hosted UMD is `amdgpu-wddm/b19-hosted-umd`, its hosted ICD `amdgpu-wddm/radv-wddm2-hosted-main` and the CPU desktop UMD `amdgpu-wddm/b19-desktop-umd` |
| [D-Ogi/dxvk](https://github.com/D-Ogi/dxvk/tree/amdgpu-wddm/ddi-engine) | `amdgpu-wddm/ddi-engine`, `amdgpu-wddm/ddi-engine-fl12` | DXVK as the D3D11 engine `amdgpu_wddm_dxvk.dll`, engine ABI 1.4 ([commit map](docs/dxvk-engine-commit-map.md)); the engine of the release is `5611118e` on `amdgpu-wddm/upstream-2026-10-05` |
| [D-Ogi/vkd3d-proton](https://github.com/D-Ogi/vkd3d-proton/tree/amdgpu-wddm/ddi-engine-1.3-rtcfg) | `amdgpu-wddm/registered-2026-10-02`, `amdgpu-wddm/ddi-engine`, `amdgpu-wddm/ddi-engine-1.3-rtcfg`, `amdgpu-wddm/quiet-stdio-2026-10-02`, `amdgpu-wddm/draw-path`, and two more listed in the fork's README | vkd3d-proton as the D3D12 engine `amdgpu_wddm_vkd3d.dll`: frozen ABI 1.0 on `ddi-engine`, then ABI 1.2 and 1.3 (imported memory, private instances, linear images) with the dxil-spirv structurizer fix on `ddi-engine-1.3-rtcfg`, which has since merged upstream (2026-09-30). The engine of the release is `4e9a98e9` on `amdgpu-wddm/upstream-2026-10-05` |
| [D-Ogi/dxil-spirv](https://github.com/D-Ogi/dxil-spirv/tree/amdgpu-wddm/rtcfg-loop-merge) | `amdgpu-wddm/rtcfg-loop-merge` | dxil-spirv with the geometry stream passed to stream output, upstream's `f2d1b55` loop fixup, and a structurizer fix for frozen loops that left two loops sharing one merge block; pinned by the vkd3d-proton branch above |
| [D-Ogi/dxbc-spirv](https://github.com/D-Ogi/dxbc-spirv/tree/amdgpu-wddm/ddi-engine) | `amdgpu-wddm/ddi-engine` | DXVK's shader compiler with two geometry-shader stream fixes, pinned by the DXVK engine branch |

## Repo map

| Path | Contents |
|---|---|
| `docs/` | Goal and roadmap, evidence rules, ADRs (`adr/`), design notes (`design/`), research notes, [build guide](docs/build.md) |
| `docs/facts.md`, `docs/facts/` | The only list of established facts, generated from the facts graph in `docs/facts/data/`, one page per area. Each entry has a status, an evidence link and its relations to other facts |
| `experiments/`, `evidence/` | Experiments `Exx` (hypothesis, procedure, expected result, result); raw results from hardware, immutable once added |
| `regs/` | Generated register tables (never edited by hand) |
| `driver/kmd/`, `driver/contract/` | The WDDM kernel-mode driver `bc250kmd`; the private KMD/UMD contract (caps, allocation, context, submission) |
| `driver/shim/`, `driver/amdgpu-import/` | AMD's imported, unmodified amdgpu code and the slice of the Linux API it needs |
| `driver/icd/` | RADV WDDM2 winsys patches; the component branches live in the Mesa fork |
| `driver/umd/`, `driver/umd-stub/` | The D3D10/11 shell over DXVK (`dxvk/`), the diagnostic D3D12 shell (`d3d12/`, with `engine-ddi/`); the stub UMD of M7 stage A |
| `tools/regcalc/`, `tools/diagusb/` | Register address calculator with tests; bootable diagnostic USB for Linux probes, results as QR codes |
| `tools/build/`, `tools/quality/` | Build recipes (LLVM, Mesa, DXVK, vkd3d-proton, UMD shells); build quality gates |
| `tools/win/`, other `tools/` | Windows measurement and lab tools; firmware fetch, package check, run comparison, Windows install |
| `third_party/` | Foreign code kept in the repo, with provenance and license |
| `LICENSE.md`, `NOTICE`, `THIRD-PARTY.md`, `SECURITY.md`, `CONTRIBUTING.md` | License, required notice, third-party licenses, authenticity, inbound license for contributions |

## Starting point

Linux (`amdgpu` + Mesa RADV) fully drives this hardware. The earlier Windows attempt (`Keshas-dev/AMD-BC-250-Windows-Driver`)
stalled on "registers are firmware-locked", which our [analysis](docs/predecessor-analysis.md) traces to a register addressing bug. The method:

1. **Linux on the same physical unit is the reference.** Every Windows measurement is compared with a Linux one.
2. **Addresses and sequences come from AMD's MIT-licensed code, never from hand calculation** (`tools/regcalc`, [addressing](docs/02-register-addressing.md)).
3. **Every hardware claim carries a status and evidence** ([evidence rules](docs/01-evidence-rules.md)).

## Build

[docs/build.md](docs/build.md) builds the kernel-mode driver, LLVM, the Mesa components, DXVK and vkd3d-proton
from pinned sources. Register lookups need only Python:

```
python tools/regcalc/regcalc.py lookup mmGRBM_STATUS mmSPI_PG_ENABLE_STATIC_WGP_MASK
python tools/regcalc/regcalc.py reverse 0x5C3C
python -m unittest discover -s tools/regcalc
```

## License

Copyright (c) 2026 D-Ogi. Source-available under the **PolyForm Noncommercial License 1.0.0** (`LICENSE.md`): free for noncommercial use, modification and sharing, provided the `Required Notice` lines in `NOTICE` stay attached. No commercial use, including bundling with hardware or OS images for sale. Reasons: `docs/adr/0004-polyform-noncommercial.md`.

Third-party code keeps its own license (`THIRD-PARTY.md`). AMD firmware blobs are not part of this repo.

**Beware of fakes.** This project publishes source and signed checksums only, never Windows images, installers from file hosts or BIOS files. See `SECURITY.md`.
