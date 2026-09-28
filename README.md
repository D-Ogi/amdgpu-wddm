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
  ([M736](docs/facts.md), [M747](docs/facts.md), [M752](docs/facts.md)). In those tests a router in the
  registered driver path hands only the test process to the new driver; the desktop stays on the CPU driver.
- **D3D12:** the system runtime reaches the registered diagnostic driver ([M768](docs/facts.md)), but no device exists yet through it ([M763](docs/facts.md), [M768](docs/facts.md)).
  The Witcher 3 DX12 results ([M571](docs/facts.md), [M573](docs/facts.md)) are per-application:
  vkd3d-proton and DXVK's `dxgi.dll` next to the game, which is exactly what the native design removes.

## Status (as of 2026-09-28)

Every row cites [docs/facts.md](docs/facts.md), where each fact links its evidence. Open means open. Roadmap:
M0-M6, M8 and M10 are closed; M7, M9, M11, M12 and M13 are open; M14 and M15 are proposed numbers (ADR 0017).

| Area | State | Facts | Limits and open items |
|---|---|---|---|
| Kernel driver | Full WDDM lab driver with GPU submission and display scanout; tested CPU baseline KMD 171 | [M727](docs/facts.md) | Development, test-signed stack: not general Windows driver compatibility or certified recovery. TDR recovery is not implemented ([M179](docs/facts.md)). KMD 171 was built from revision `75b8ab6f`, not from `main` ([provenance](evidence/windows/2026-09-28-E34-m14-kmd171/RESULT.md)) |
| GPU desktop | GPU composition demonstrated on the tested hosted Zink/RADV path, with correct-image controls, adapter-attributed DWM GPU work and the agreed bounded no-copy audit; the bounded implementation objective (G0 in the facts) is met for that path | [M723](docs/facts.md), [M724](docs/facts.md) | Full M13 ([gates](docs/m13-accelerated-desktop-roadmap.md)) and a permanent GPU desktop are separate and open; the lab baseline composes on the CPU |
| Vulkan ICD | RADV with a WDDM2 winsys, the lab's registered Vulkan driver. Compute hashes equal CPU and Linux (M8); llama.cpp text equals the Linux GPU text; first picture (M10, CPU presentation) | [M139](docs/facts.md), [M163](docs/facts.md), [M476](docs/facts.md), [M665](docs/facts.md) | Only a basic-compute CTS slice has run: 75 pass, 5 not supported, 0 fail of 80 ([M480](docs/facts.md)). Must-pass list and Linux parity open; a native-sparse control hung ([M483](docs/facts.md)). M9 performance and paging open |
| Native D3D11 (proposed M14) | System-runtime GPU rendering at FL 11_1 demonstrated; native window Present, resize and error-path controls pass under the earlier module names; the renamed pair passes admission and error controls (M755, M756) | [M736](docs/facts.md), [M747](docs/facts.md), [M752](docs/facts.md), [M755](docs/facts.md), [M756](docs/facts.md) | Not full M14, broad game compatibility or FL 12_1. The 5 % bound against per-application DXVK (ADR 0017) is not measured |
| Native D3D12 (proposed M15) | System runtime reaches the registered diagnostic UMD (M768); hosted adapter-only engine policy queries pass (M769). Native device, queue and rendering acceptance remain open | [M763](docs/facts.md)-[M769](docs/facts.md) | M763 is diagnostic admission, not `D3D12CreateDevice` success. After the bounded PnP restart the fourth-slot registration is effective and system D3D12 enters `OpenAdapter12` (M768), but runtime `GetCaps` 1074/1007 return `E_NOTIMPL` and FL 11_0 `CreateDevice` fails. M769 is a separate adapter-only helper (FL 11_1, tiled tier 0, binding tier 3, RT tier 1.1; no device callbacks, queue bindings or GPU work): not runtime `GetCaps` success, `D3D12CreateDevice` or native DXR. Not a functional D3D12 driver |
| D3D12 engine | Standalone vkd3d-proton engine: GPU copy/readback and DXIL compute pass on RADV | [M757](docs/facts.md), [M759](docs/facts.md), [M769](docs/facts.md) | An internal-backend control, not the native Windows DDI path |
| Per-app D3D12 | The Witcher 3 next-gen DX12 build renders its menu and an in-game scene with vkd3d-proton and DXVK's `dxgi.dll` next to the game, on our ICD with an experimental sparse flag | [M571](docs/facts.md), [M573](docs/facts.md), [M580](docs/facts.md) | Not native. Presents by CPU copy ([M577](docs/facts.md)), about 6x slower than a `vkcube` control, cause open ([M610](docs/facts.md)); what locks the scene at 29.7 fps is open (M580). No RT acceptance |
| Ray tracing | Selected Vulkan ray-query and `TraceRays` CTS controls pass on the RT-fixed ICD candidate (not the registered ICD) | [M760](docs/facts.md), [M761](docs/facts.md), [M762](docs/facts.md) | No Witcher 3 RT acceptance, full RT conformance or native DXR acceptance |
| FL 12_1 | Target, not achieved | [M757](docs/facts.md), [M570](docs/facts.md) | The engine reports FL 11_1 with tiled resources tier 0 in the measured configuration (M757). Per-application vkd3d-proton reports 12_1 only with an experimental sparse flag, sparse correctness not shown (M570). Sparse/residency and complete native DDI coverage remain work |
| Linux reference | amdgpu and RADV on the same unit: register baseline, Vulkan compute hashes, llama.cpp and clock/throughput numbers | [M1](docs/facts.md), [M50](docs/facts.md)-[M52](docs/facts.md) | No working GPU reset under Linux either ([M53](docs/facts.md)). M12's Linux-parity comparisons (CTS, performance) are open |

## Layers

```
application: D3D11 / D3D12 / DXGI calls, nothing of ours next to it
  -> Microsoft runtime in System32: d3d11.dll, d3d12.dll + D3D12Core.dll, dxgi.dll
  -> our user-mode driver (the adapter's UserModeDriverName)
       D3D11: shell amdgpu_wddm_d3d11.dll (driver/umd/dxvk)             + engine amdgpu_wddm_dxvk.dll
       D3D12: shell amdgpu_wddm_d3d12.dll (driver/umd/d3d12, engine-ddi/) + engine amdgpu_wddm_vkd3d.dll
       desktop today: bc250d3d.dll (Mesa d3d10umd on llvmpipe, CPU rendering)
  -> hosted RADV amdgpu_wddm_radv.dll, driven through the runtime's callbacks
  -> dxgkrnl (VidMm, VidSch) -> bc250kmd.sys (driver/kmd) -> GPU
Vulkan applications: Vulkan loader -> vulkan_radeon.dll (system ICD) -> dxgkrnl -> bc250kmd.sys -> GPU
```

| File | Role | Built by | State of the name |
|---|---|---|---|
| `bc250kmd.sys`, service `bc250kmd` | kernel-mode driver | `driver/kmd/build.ps1` | Baseline, not renamed |
| `bc250d3d.dll` | desktop D3D10/11 UMD on llvmpipe | `tools/build/mesa-configs.json` `llvmpipe-umd` | Baseline: the lab desktop runs on it |
| `bc250d3d_zink.dll` | the same frontend on Zink over hosted RADV | `mesa-configs.json` `zink-umd` | Candidate: the M723 GPU desktop run |
| `vulkan_radeon.dll` | system Vulkan ICD | `mesa-configs.json` `radv` | Baseline (upstream RADV name) |
| `amdgpu_wddm_radv.dll` | hosted RADV loaded by the D3D11 shell and used by the D3D12 policy query (M769) | no recipe: a `vulkan_radeon.dll` build staged under this name | Candidate, was `bc250radv.dll` |
| `amdgpu_wddm_d3d11.dll` | D3D10/11 DDI shell | `tools/build/build-umd-dxvk.ps1` | Candidate, renamed from `bc250d3d11.dll` (M756); not deployed |
| `amdgpu_wddm_dxvk.dll` | DXVK engine | `tools/build/dxvk-configs.json` `ddi-engine` | Candidate, renamed from `bc250dxvk.dll` (M755) |
| `amdgpu_wddm_d3d12.dll` | D3D12 diagnostic adapter shell | `tools/build/build-umd-d3d12.ps1` | Diagnostic candidate (M763) |
| `amdgpu_wddm_vkd3d.dll` | vkd3d-proton engine | `tools/build/vkd3d-configs.json` `ddi-engine` | Candidate, renamed from `bc250vkd3d.dll`; M757 ran the pre-rename GPU-device test, M769 measured the renamed ABI 1.2 policy query (adapter only, no device) |

## Repositories

| Repository | Branches | What it holds |
|---|---|---|
| [D-Ogi/amdgpu-wddm](https://github.com/D-Ogi/amdgpu-wddm) (this one) | `main` | Kernel-mode driver, UMD shells, contracts, tools, experiments, evidence and facts |
| [D-Ogi/mesa-amdgpu-wddm](https://github.com/D-Ogi/mesa-amdgpu-wddm) | `amdgpu-wddm/*`, listed in `README-amdgpu-wddm.md` on `amdgpu-wddm/readme` | RADV with the WDDM2 winsys and the `d3d10umd` UMD builds (llvmpipe, Zink); not every current lab artifact's source is there yet (e.g. [the RT fix](driver/icd/README-rt-node-address.md)) |
| [D-Ogi/dxvk](https://github.com/D-Ogi/dxvk/tree/amdgpu-wddm/ddi-engine) | `amdgpu-wddm/ddi-engine` | DXVK as the D3D11 engine `amdgpu_wddm_dxvk.dll`, engine ABI 1.4 ([commit map](docs/dxvk-engine-commit-map.md)) |
| [D-Ogi/vkd3d-proton](https://github.com/D-Ogi/vkd3d-proton/tree/amdgpu-wddm/ddi-engine) | `amdgpu-wddm/ddi-engine`, `amdgpu-wddm/ddi-engine-inline-wip` | vkd3d-proton as the D3D12 engine `amdgpu_wddm_vkd3d.dll`: published: frozen ABI 1.0 and a draft ABI 1.1 inline queue mode; a local ABI 1.2 candidate (`7bfcd7f0`) is not published and only measured in M769 |
| [D-Ogi/dxbc-spirv](https://github.com/D-Ogi/dxbc-spirv/tree/amdgpu-wddm/ddi-engine) | `amdgpu-wddm/ddi-engine` | DXVK's shader compiler with two geometry-shader stream fixes, pinned by the DXVK engine branch |

## Repo map

| Path | Contents |
|---|---|
| `docs/` | Goal and roadmap, evidence rules, ADRs (`adr/`), design notes (`design/`), research notes, [build guide](docs/build.md) |
| `docs/facts.md` | The only list of established facts. Each entry has a status and an evidence link |
| `experiments/`, `evidence/` | Experiments `Exx` (hypothesis, procedure, expected result, result); raw results from hardware, immutable once added |
| `journal/`, `regs/` | Lab notebook by day (chronology, not a source of facts); generated register tables (never edited by hand) |
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
