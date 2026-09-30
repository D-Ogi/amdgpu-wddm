# Building the driver stack

How to build every binary this project deploys from this repository plus public upstream sources. The KMD
builds from this repository alone; LLVM and the Mesa components build from pinned upstream commits plus this
project's patches. The executable half of this page is [`tools/build/`](../tools/build/README.md).

## Scope

A fresh Windows x64 development machine needs Visual Studio 2022 or its Build Tools, the WDK and SDK as
unpacked NuGet packages, Python with meson and mako, CMake, Ninja, win_flex_bison and glslang. No WDK
installer is needed, and nothing here touches a lab machine. Outside the repository stay the upstream
sources (large, so pinned by commit instead of vendored), build output and temporary files (`scratch\`),
portable toolchains (`toolchain\`), AMD firmware (redistributable but never committed; `tools/firmware`
fetches and verifies it) and secrets (lab addresses, SSH keys, the signing certificate's private key).

## Workspace layout and BC250_ROOT

```
<BC250_ROOT>\
  bc250-win\    this repository
  toolchain\    portable tools, no installers: nuget\, winflexbison-2.5.25\, llvm2312\ (LLVM install prefix)
  ref\          upstream sources at pinned revisions: llvm-project-23.1.2\, linux-firmware__WARN-...\
  scratch\      builds, temporary files, py\ (meson and friends), glslang\
  secrets\      never in the repository
```

Tools read `BC250_ROOT`; when it is unset they use the directory that holds the repository. The recipe
scripts default every path below to this layout, and each can be overridden by a parameter.

## Prerequisites

| Tool | Recorded version | Location | Notes |
|---|---|---|---|
| Visual Studio 2022 (Community or Build Tools), C++ x64 tools | MSVC 14.44.35207, `cl` 19.44.35221 | installed | Found through `vswhere`; `vcvars64.bat` also brings the installed Windows SDK 10.0.26100.0 |
| WDK and SDK NuGet packages | `Microsoft.Windows.WDK.x64`, `Microsoft.Windows.SDK.CPP`, `Microsoft.Windows.SDK.CPP.x64`, each 10.0.26100.6584 | `toolchain\nuget\<lower-case id>\` | KMD build (`-Kits`); the Mesa builds put the WDK `um` and `shared` headers in front of `INCLUDE`, since `d3d10umddi.h` exists only in the WDK |
| Python | 3.14.0 | installed | Runs meson, Mesa's generators and this repository's tools |
| meson, mako, MarkupSafe, PyYAML | 1.12.0, 1.4.1, 3.0.3, 6.0.3 | `scratch\py` | `pip install --target`. Mesa also imports `packaging` (25.0 came from the Python user site; install it into `scratch\py` too) |
| CMake | 3.25.2 | MSYS2 `mingw64\bin` | LLVM needs 3.20.0 or newer |
| Ninja | 1.11.1 | MSYS2 `mingw64\bin` | LLVM and Mesa |
| win_flex_bison | 2.5.25 (flex 2.6.4, bison 3.8.2) | `toolchain\winflexbison-2.5.25` | `win_flex_bison-2.5.25.zip`, SHA-256 `8d324b62be33604b2c45ad1dd34ab93d722534448f55a16ca7292de32b6ac135` |
| glslang | 16.6.0 | `scratch\glslang\bin` | `glslang-16.6.0-windows-x86_64-release.zip`, SHA-256 `82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b`; required for RADV |
| PowerShell 7, Git | | on PATH | The recipe scripts need `pwsh` 7 |

```
python -m pip install --target <BC250_ROOT>\scratch\py meson==1.12.0 mako==1.4.1 pyyaml==6.0.3 packaging==25.0
```

How the WDK and SDK packages were downloaded is not recorded. The lab directories match each `.nupkg` from
the NuGet v3 flat container unzipped into a directory named after the lower-case package id, which this does
(not run in this form; compare the hashes after download):

```
$kits = "<BC250_ROOT>\toolchain\nuget"; $v = '10.0.26100.6584'
foreach ($id in 'microsoft.windows.wdk.x64', 'microsoft.windows.sdk.cpp', 'microsoft.windows.sdk.cpp.x64') {
    Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/$id/$v/$id.$v.nupkg" -OutFile "$kits\$id.$v.nupkg"
    New-Item -ItemType Directory -Force "$kits\$id" | Out-Null; tar -xf "$kits\$id.$v.nupkg" -C "$kits\$id"
}
```

| Package file | SHA-256 |
|---|---|
| `microsoft.windows.wdk.x64.10.0.26100.6584.nupkg` | `c393d03dfb640b5c92f546b32f6770ef68cd3aaf691956e7d66d8e2c28a1b55e` |
| `microsoft.windows.sdk.cpp.10.0.26100.6584.nupkg` | `5d31b38205bdd9ac761b4cb39fbbc6b7209b01c11194324afc674d7d119483a0` |
| `microsoft.windows.sdk.cpp.x64.10.0.26100.6584.nupkg` | `c29ce7a4641cb37ee32ebb8078cc65cfbabc7025076bcfba869039204b1e960d` |

PROVENANCE: WDK and SDK NuGet packages from nuget.org, under Microsoft's package licence.

## LLVM 23.1.2

PROVENANCE: LLVM, Apache-2.0 with LLVM exception. Tag `llvmorg-23.1.2`, commit `85ac560262434c9ccfc0c183ec22d4138ed647fb`.

```
git clone --depth 1 --branch llvmorg-23.1.2 https://github.com/llvm/llvm-project.git <BC250_ROOT>\ref\llvm-project-23.1.2
pwsh tools\build\build-llvm.ps1 -Build <BC250_ROOT>\scratch\llvm2312-build -Prefix <BC250_ROOT>\toolchain\llvm2312 -Jobs 12
```

The script runs the recorded
[build-llvm23.cmd](../evidence/windows/2026-09-24-E26-desktop-resume/llvm23/build-llvm23.cmd) line:

```
cmake -S <source>\llvm -B <build> -G Ninja -DCMAKE_MAKE_PROGRAM=<ninja> -DCMAKE_INSTALL_PREFIX=<prefix>
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
  -DCMAKE_CXX_FLAGS=/utf-8 -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_INCLUDE_UTILS=OFF
  -DLLVM_INCLUDE_RUNTIMES=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF
  -DLLVM_BUILD_TOOLS=OFF -DLLVM_ENABLE_DIA_SDK=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF
  -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_PARALLEL_LINK_JOBS=2
ninja -C <build> llvm-config install
```

- **Configure gate.** The script reads `CMakeCache.txt` back and stops if a passed value differs, or one of
  the defaults the Mesa link relies on: `BUILD_SHARED_LIBS`, `LLVM_ENABLE_RTTI`, `LLVM_ENABLE_EH`,
  `LLVM_OPTIMIZED_TABLEGEN` all `OFF`, `LLVM_ENABLE_PROJECTS` empty, host triple `x86_64-pc-windows-msvc`.
- **No `llvm-config.exe` in the prefix.** The prefix holds static libraries and headers. `LLVM_BUILD_TOOLS=OFF`
  keeps `llvm-config.exe` out of the install, so the recipe builds it explicitly and it stays in `<build>\bin`.
- **Mesa uses the build tree.** `LLVM_CONFIG` points at that `llvm-config.exe`, and meson's config-tool lookup
  succeeds before CMake is asked. It reports the source's `llvm\include` plus the build's `include` and `lib`,
  so the source and build directories must stay in place ([m13-present-baseline.md](research/m13-present-baseline.md)).
- **Existing prefix refused.** The script will not install over a prefix that holds files unless given
  `-AllowExistingPrefix`: the working toolchain is kept for rollback until its replacement is validated.

## Mesa

PROVENANCE: Mesa, MIT.

`tools/build/build-mesa.ps1 -Config <name> -Source <tree> -Build <dir>` configures and, unless
`-ConfigureOnly` is given, builds one component. The option sets live in
[`tools/build/mesa-configs.json`](../tools/build/mesa-configs.json); the script stops if the new
`meson-logs\meson-log.txt` records a different `Build Options:` line. It does not patch: the tree must already
carry the component's patches.

Sources come from the companion Mesa repository: one branch per component (RADV WDDM2 ICD, llvmpipe d3d10umd
desktop UMD, Zink d3d10umd UMD), each on the upstream commit recorded in the corresponding
`experiments/*/*-source.json` or README, with this project's patches as commits. The companion repository is
https://github.com/D-Ogi/mesa-amdgpu-wddm (a fork of a GitHub mirror of upstream Mesa); its
`README-amdgpu-wddm.md` on branch `amdgpu-wddm/readme` lists every branch with its verification result.

| Component | Branch | Upstream base | Patches recorded in |
|---|---|---|---|
| RADV WDDM2 ICD | `amdgpu-wddm/radv-wddm2` | `05e6c9622e135ac2aeaf56ec70222642627e2162` | [mesa05-source.json](../experiments/E33-m12-applications/mesa05-source.json), `mesa05-wddm2.patch` |
| RADV WDDM2 ICD, deployed baseline 9C40083C | `amdgpu-wddm/radv-wddm2-baseline-9c40083c` | `f333dd6d1c85297ac41773eaeb9b02f16acf1919` | E27 `radv-main/mesa-main-wddm2-bc250.patch`, E31 `wsi-cpu-fifo.patch` |
| llvmpipe d3d10umd desktop UMD | `amdgpu-wddm/d3d10umd-llvmpipe` | `f9a2d34a19c496e552b6cd603e7807f1025a2e98` | [E26 README](../experiments/E26-wddm-desktop/README.md), `mesa-main-bc250-gallium.patch` |
| Zink d3d10umd UMD | `amdgpu-wddm/d3d10umd-zink` | `05e6c9622e135ac2aeaf56ec70222642627e2162` | [E34 README](../experiments/E34-native-d3d-zink/README.md) |

The script sets up the environment of the recorded cmd scripts:

- `vcvars64.bat`, then the WDK `um` and `shared` include directories in front of `INCLUDE`.
- win_flex_bison on PATH, and glslang where the recorded build of that configuration had it.
- `PYTHONPATH` set to `scratch\py`, `TEMP` and `TMP` set to `scratch\tmp`.
- For llvmpipe, `LLVM_CONFIG=<LLVM build>\bin\llvm-config.exe`, with that directory on PATH.

The recorded scripts also had MSYS2 `mingw64\bin` on PATH, so meson probed pkg-config 1.8.0 and CMake 3.25.2
for optional dependencies; nothing was found through either. A clean tree's first configure downloads
subprojects into its `subprojects\` directory: zlib 1.3.1 for every configuration (wrap-file with a pinned
SHA-256, forced by `force_fallback_for=zlib`) and DirectX-Headers v1.619.1 for RADV (wrap-git from GitHub).
Meson also leaves a `.wraplock` file there. An offline machine needs those subprojects populated in advance.

All four configurations are `debugoptimized` with meson's default `b_ndebug=if-release`, so `NDEBUG` is not
defined and Mesa's assertions and NIR validation are active. A performance comparison with a Linux build must
use the same setting or change it on both sides.

### RADV Vulkan ICD (`-Config radv`)

```
-Dbuildtype=debugoptimized -Dforce_fallback_for=zlib -Dllvm=disabled -Damd-use-llvm=false -Dvulkan-drivers=amd
-Dgallium-drivers=[] -Ddefault_library=static -Dplatforms=windows -Dvideo-codecs=[] -Degl=disabled -Dglx=disabled
-Dzstd=disabled
```

Target `src/amd/vulkan/vulkan_radeon.dll`: ACO only, no LLVM, and glslangValidator is required. It links the
DLL C runtime (`MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`). Candidate patches on top of the base
are listed in the [E33 README](../experiments/E33-m12-applications/README.md).

### Desktop D3D10 UMD on llvmpipe (`-Config llvmpipe-umd -Llvm <LLVM build>`)

```
-Dbuildtype=debugoptimized -Dforce_fallback_for=zlib -Dllvm=enabled -Dshared-llvm=disabled -Damd-use-llvm=false
-Dvulkan-drivers=[] -Dgallium-drivers=llvmpipe,softpipe -Dgallium-d3d10umd=true -Dopengl=false -Db_vscrt=mt
-Ddefault_library=static -Dplatforms=windows -Dvideo-codecs=[] -Degl=disabled -Dglx=disabled -Dzstd=disabled
-Dgallium-d3d10-dll-name=bc250d3d
```

Target `src/gallium/targets/d3d10umd/bc250d3d.dll`, plus the patch's test programs `bc250_ttn_control.exe`
and `bc250_lp_test_arit.exe`. `b_vscrt=mt` matches LLVM's `/MT`, so the DLL imports no C runtime DLL.

### Native D3D10 UMD on Zink (`-Config zink-umd`)

```
-Dbuildtype=debugoptimized -Dforce_fallback_for=zlib -Dllvm=disabled -Damd-use-llvm=false -Dvulkan-drivers=[]
-Dgallium-drivers=zink -Dgallium-d3d10umd=true -Dgallium-d3d10-dll-name=bc250d3d_zink -Ddefault_library=static
-Dplatforms=windows -Dvideo-codecs=[] -Degl=disabled -Dglx=disabled -Dzstd=disabled
```

Target `src/gallium/targets/d3d10umd/bc250d3d_zink.dll`, linked against the DLL C runtime. Upstream
`meson.build` refuses d3d10umd without softpipe or llvmpipe; the patched tree accepts Zink.

### OpenGL on Zink (`-Config zink-gl`)

```
-Dbuildtype=debugoptimized -Dforce_fallback_for=zlib -Dllvm=disabled -Damd-use-llvm=false -Dvulkan-drivers=[]
-Dgallium-drivers=zink -Ddefault_library=static -Dplatforms=windows -Dvideo-codecs=[] -Degl=disabled -Dglx=disabled
-Dzstd=disabled
```

Targets `src/gallium/targets/wgl/libgallium_wgl.dll` and `src/gallium/targets/libgl-gdi/opengl32.dll`, both on
the DLL C runtime. The recorded build used the RADV ICD's tree; its Zink/WGL patches are the `zink-*.patch`
files in `experiments/E33-m12-applications/`.

## DXVK

PROVENANCE: DXVK, zlib.

`tools/build/build-dxvk.ps1 -Config <name> -Source <tree> -Build <dir>` works like the Mesa script. The option
sets are in [`tools/build/dxvk-configs.json`](../tools/build/dxvk-configs.json), and the same `Build Options:`
gate applies. `recipe.json` also records the submodule commits: DXVK's shader compiler lives in the
`dxbc-spirv` submodule. `-Reconfigure` re-runs meson on a configured build directory. DXVK needs no WDK headers,
only `vcvars64.bat`, meson, ninja and glslangValidator. The tree must have its submodules initialized
(`git submodule update --init --recursive`).

| Config | Source | Targets |
|---|---|---|
| `per-app` | upstream DXVK | `d3d11.dll`, `dxgi.dll`, `d3d10core.dll`, `d3d9.dll`: application-local DLLs, the comparison path of the M14 5 % bound (D004) |
| `ddi-engine` | DXVK branch `amdgpu-wddm/ddi-engine` | `amdgpu_wddm_dxvk.dll`, the engine behind the M14 system D3D10/11 DDI UMD (ADR 0017 item 4), and `amdgpu_wddm_dxvk_engine_test.exe` |

Both are `-Dbuildtype=release` without D3D8. `ddi-engine` also drops D3D9 and D3D10 and sets
`-Denable_ddi_engine=true`, an option that exists only on that branch. The branch is upstream DXVK plus
this project's commits under `src/ddi/` and small host-mode hooks in `src/dxvk/` and `src/d3d11/`. It is
published at <https://github.com/D-Ogi/dxvk/tree/amdgpu-wddm/ddi-engine>; each `recipe.json` names its commit.
Branch commits before engine header r7 name the DLL `bc250dxvk.dll` and the test `bc250dxvk_engine_test.exe`.

The branch's `dxbc-spirv` submodule points at <https://github.com/D-Ogi/dxbc-spirv> (branch
`amdgpu-wddm/ddi-engine`), which carries the pinned commit 253c08ce: geometry shader output stream decoration and
the pass-through GS fix, not upstream yet.

Before publication the branch was rewritten on 2026-09-28 to replace a private author identity. The trees are
unchanged, but every commit on top of upstream 52fe923c has a new hash. Recipes and evidence written before that
date name the old hashes; [dxvk-engine-commit-map.md](dxvk-engine-commit-map.md) maps each one to its published
commit through the identical tree hash.

`amdgpu_wddm_dxvk_engine_test.exe <amdgpu_wddm_dxvk.dll> [adapter substring]` is the engine's offline positive
control. It runs on any Vulkan 1.3 GPU, opens no window and exits by itself; exit code 0 means every check
passed. The test plays the UMD shell: it owns the Vulkan instance and device, allocates the images and feeds
shaders in DDI form. The checks are listed under Validation in
[the engine design note](design/d3d11-ddi-engine.md#validation).

## vkd3d-proton

PROVENANCE: vkd3d-proton, LGPL-2.1.

`tools/build/build-vkd3d.ps1 -Config <name> -Source <tree> -Build <dir> [-Widl <widl.exe>]` works like the DXVK
script, with the same `Build Options:` gate and the submodule commits in `recipe.json`. The option sets are in
[`tools/build/vkd3d-configs.json`](../tools/build/vkd3d-configs.json).

What differs from the DXVK script:
- vkd3d-proton also needs the IDL compiler `widl`. MSYS2 mingw64 ships one. The script appends its directory to
  PATH, so nothing else in it shadows the MSVC tools.
- The script sets CC and CXX to `cl`.
- The tree must have its submodules initialized: `dxil-spirv` with its own nested submodules, and the Khronos
  headers.

| Config | Source | Targets |
|---|---|---|
| `per-app` | upstream vkd3d-proton | `d3d12.dll`, `d3d12core.dll`: application-local DLLs, the M12 per-application D3D12 path |
| `ddi-engine` | vkd3d-proton branch `amdgpu-wddm/ddi-engine` | `amdgpu_wddm_vkd3d.dll`, the engine behind the proposed M15 native D3D12 UMD (ADR 0017 item 5), and `amdgpu_wddm_vkd3d_engine_test.exe` |

Both configs build with `-Dbuildtype=release -Denable_tests=false`.

`ddi-engine` adds two options:
- `-Denable_ddi_engine=true`, an option that exists only on that branch;
- `-Db_vscrt=mt`: the engine links the C runtime statically, and the branch's meson refuses anything else for it.

The branch is upstream vkd3d-proton plus `libs/ddi/` (MIT) and two small libvkd3d changes. It is published at
<https://github.com/D-Ogi/vkd3d-proton/tree/amdgpu-wddm/ddi-engine>, together with the draft inline queue mode
branch `amdgpu-wddm/ddi-engine-inline-wip`; each `recipe.json` names its commit. Branch commits before a582668d name the DLL `bc250vkd3d.dll` and the test
`bc250vkd3d_engine_test.exe`. Engine ABI 1.2 and 1.3 are on `amdgpu-wddm/ddi-engine-1.3-rtcfg`, whose `dxil-spirv`
submodule points at <https://github.com/D-Ogi/dxil-spirv> (branch `amdgpu-wddm/rtcfg-loop-merge`); clone it with
`--recurse-submodules`, since the pinned dxil-spirv commit is not upstream.

`amdgpu_wddm_vkd3d_engine_test.exe <amdgpu_wddm_vkd3d.dll> [adapter substring] [--icd <driver DLL>]` is the
engine's offline positive control. It runs on any Vulkan 1.3 GPU, opens no window, writes no files and exits by
itself; exit code 0 means every check passed. The checks are listed under Validation in
[the engine design note](design/d3d12-ddi-engine.md#validation).

## KMD and host tests

```
pwsh driver\kmd\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Out <BC250_ROOT>\scratch\build\bc250kmd
```

- **Build.** `cl` and `link` run directly, without project files: MSVC through `vswhere`, headers and
  libraries from the NuGet kits (`-KitVersion 10.0.26100.0`). The fast quality gates in `tools/quality` run
  first and a kernel stack-budget check (`tools/win/stackbudget.py`) runs last.
- **Output.** `<Out>\package\{bc250kmd.sys, bc250kmd.inf, bc250kmd.cat, bc250-lab-test.cer}`. `-UmdStub` adds
  a second package with the stub UMD from `driver/umd-stub/build.ps1`; the script header explains both.
- **Signing.** A self-signed code-signing certificate `CN=BC-250 lab test signing` in `Cert:\CurrentUser\My`,
  created on first use by `tools\win\bc250rd\build.ps1`. Run [`tools/packagecheck`](../tools/packagecheck/README.md)
  over the exact package directory before any install.
- **Target.** Test signing on (which needs Secure Boot off) and the exported `bc250-lab-test.cer` in the
  machine's Root and TrustedPublisher stores. `tools/wininstall` sets test signing during an unattended install.

Host tests need no lab. Several `run*.ps1` scripts still default their parameters to the lab's own paths;
on another layout, pass them explicitly.

```
python -m unittest discover -s tools/regcalc
python -m unittest discover -s tools/diagusb
python -m unittest discover -s tools/packagecheck
pwsh driver\shim\test\run.ps1 -Out <dir> -Kits <BC250_ROOT>\toolchain\nuget    (and the other run_*.ps1 there)
pwsh driver\kmd\test\run_<name>.ps1 ...                                          (-Root/-Out or -Out/-Kits)
```

## Firmware

The KMD loads eight PSP firmware files at runtime from `C:\BC250\firmware\` on the target
(`BC250_PSP_FIRMWARE_DIR`, `driver/kmd/psp.c`): `amdgpu/cyan_skillfish2_{ce,me,mec,mec2,pfp,rlc,sdma,sdma1}.bin`
from linux-firmware commit `2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`. `tools/firmware/fetch_firmware.py fetch`
downloads them over HTTPS from the linux-firmware repository and checks size and SHA-256 against
`tools/firmware/cyan_skillfish2.json`; `push` copies verified files to the target
([tools/firmware](../tools/firmware/README.md)).

PROVENANCE: linux-firmware, redistributable per LICENSE.amdgpu.

## Deployment overview

Everything reaches the target through [`tools/win/target.py`](../tools/win/README.md), which pipes tar over
ssh. The target's working directory is `C:\BC250\`, scratch `C:\BC250\tmp\`.

| What | On the target | How (reference) |
|---|---|---|
| KMD package | driver store | `pnputil /add-driver bc250kmd.inf /install`; every install closes the registry gates ([driver/kmd/README.md](../driver/kmd/README.md), `experiments/E16-full-wddm-stage-a/e16_target.ps1`) |
| PSP firmware | `C:\BC250\firmware\` | `tools/firmware/fetch_firmware.py push` |
| D3D UMD (`bc250d3d.dll`, `bc250d3d_zink.dll`) | a directory under `C:\BC250\` | the adapter's class key `UserModeDriverName` names the DLL by full path (`experiments/E26-wddm-desktop/mesa-probe.ps1`) |
| Vulkan ICD (`vulkan_radeon.dll` + JSON manifest) | a directory under `C:\BC250\` | manifest registered under `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` and in the adapter's `VulkanDriverName` (`experiments/E33-m12-applications/install-system-icd.ps1`) |
| Zink OpenGL (`opengl32.dll`, `libgallium_wgl.dll`) | next to the application | app-local, no system registration |
| MSVC runtime for RADV and Zink | System32 | Microsoft Visual C++ x64 redistributable (the lab has 14.51.36247.0) |
| Lab tools | `C:\BC250\kmd\`, `C:\BC250\mon\`, `C:\BC250\bc250rd\` | the READMEs under `tools/win/` |
| KMD log files | `C:\BC250\kmdlog\` | written by the driver (`driver/kmd/guard.c`) |

What is deployed on the lab right now is recorded outside this repository.

## Reproducibility notes

- **Tool versions and options** are pinned in this page and `tools/build/`. Each recipe run writes
  `recipe.json` into its build directory: source commit plus SHA-256 of `git diff --binary HEAD` for patched
  trees, exact arguments, tool paths and versions, gate result and, after a build, artifact hashes.
- **Mesa sources** are identified per experiment, e.g.
  [mesa05-source.json](../experiments/E33-m12-applications/mesa05-source.json) (upstream commit, patch SHA-256,
  candidate DLL SHA-256). The companion repository replaces patch chains with one branch per component.
- **Firmware** is pinned by commit and hash in `tools/firmware/cyan_skillfish2.json`.
- **The workspace's `ref\README.md`**, outside this repository, lists the local upstream checkouts with
  revisions and licences.
- **Pins are the current baseline, not permanent targets.** Newer LLVM and Mesa are preferred; change a pin
  together with this page, `tools/build/` and a validated build (`CLAUDE.md`, "Toolchain and Mesa freshness").
- **Validation (2026-09-26).** Configure-only runs of `build-llvm.ps1` and of `build-mesa.ps1` for `radv`,
  `llvmpipe-umd` and `zink-gl` reproduced the recorded builds. The Mesa `Build Options:` lines were identical
  and meson found the same programs and dependencies. Every non-internal LLVM cache entry matched except the
  form of the compiler entry: bare `cl` after a re-run, the resolved path after a first configure.

## Known gaps

- **Configure-only validation.** No full build has run through these scripts; artifact hashes from them have
  not been compared with deployed binaries.
- **`zink-umd` has not been configured through the script:** its source tree was in active use.
- **WDK/SDK download command not recorded,** and `toolchain\nuget\versions.txt` names SDK 10.0.26100.9169
  while the unpacked SDK packages are 10.0.26100.6584.
- **Download origins not recorded** for the win_flex_bison and glslang zips and the MSYS2 CMake and Ninja
  packages; only versions and hashes are known.
- **Not produced by these recipes:** the Vulkan loader (`vulkan-1.dll`) the lab installs, and the ICD
  manifest JSON.
- **Standalone operation** is not yet packaged: the KMD's boot-loop guard (`driver/kmd/guard.c`, ADR 0006)
  needs a user-mode confirmation after each start (`bc250kmd_cli health confirm` or the lab monitor's automatic
  confirmation) or it refuses to start after two unconfirmed boots; the feature gates in the service key
  (`EnableFullWddm` and the other `Enable*` values) are set by the lab's install scripts, not by an INF; the
  Vulkan ICD and D3D UMD registrations are manual. A self-confirming release profile, an INF with default gates
  and an on-target install script are open work.
