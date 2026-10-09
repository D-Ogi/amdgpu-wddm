# Third-party components in this release package

Each file of the package below and the licence texts that apply to it. The texts are in `licenses\`, taken from the
named repository at the commit given (each text's repository, path, commit and SHA256: `manifest.json`, list
"licenses"). Commits are full SHA-1 hashes in the named repository.

## Our own code

PolyForm Noncommercial 1.0.0, licensor D-Ogi: `amdgpu-wddm-LICENSE.md`, `amdgpu-wddm-NOTICE.txt` (the same as LICENSE.md
and NOTICE in the package root).

| File (`payload\`) | Source |
|---|---|
| `kmd\bc250kmd.sys`, `kmd\bc250kmd.inf` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`); the .sys also contains the Linux amdgpu code in the next table |
| `d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `driver/umd/d3d12`) |
| `desktop\bc250d3d_router.dll` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `driver/umd/router`) |
| `d3d11\amdgpu_wddm_d3d11.dll`, `d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `driver/umd/dxvk`, the record from `tools/build/write-umd-config.py`) |
| `wow64\desktop\bc250d3d_router.dll` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `driver/umd/router`), x86 build. The x86 behaviour of BD-064 is in the commit: the Wow path values, the 64-bit policy view and plain export names |
| `wow64\d3d11\amdgpu_wddm_d3d11.dll`, `wow64\d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `driver/umd/dxvk`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit |
| `wow64\d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `driver/umd/d3d12`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit, against the `src/util` host contract of Mesa `31844893334cd3debb9771105a0612ababe9d0ee`. The x86 ICD below (`aca2c81b`) has the same `src/util` |
| `tools\bc250kmd_cli.exe`, `control\bc250control.dll`, `tools\bc250control.dll` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`: the CLI and the control DLL from one build of `tools/win/bc250kmd_cli`) |
| `mft\amdgpu_wddm_mft_h264.dll` | amdgpu-wddm `70b6f8388309888690a854ddf4cdf5ab35ea594f` (branch `m15/video-encode-mft`, `driver/umd/mft-h264`) |
| `tools\amdgpu_wddm_d3d12caps.exe` | amdgpu-wddm `tools/win/d3d12caps` |
| `tools\d3d11bench.exe` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `tools/win/d3d11bench`) |
| `control\amdgpu_wddm_control.exe` | amdgpu-wddm `0d6b3299dc7b4ed3d8b057755577493a85903853` (branch `train/b23`, `tools/win/amdgpu_wddm_control`) |
| `tools\start-confirm.ps1`, `tools\start-confirm-core.ps1`, `tools\dwm-session.ps1`, `tools\release-witness.ps1`, `installer\*`, `*.cmd`, `cert\amdgpu-wddm-release.cer`, `kmd\bc250kmd.cat` | amdgpu-wddm (this release) |

## Third-party code in the package

| File (`payload\`) | Component @ commit | Licence texts (`licenses\`) |
|---|---|---|
| `kmd\bc250kmd.sys` | Linux kernel amdgpu sources and register headers, torvalds/linux @ `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` (v6.18) | MIT: `Linux-amdgpu-MIT.txt` |
| `d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `41bd52c810dedf32515cd7751dfee8ae8a6065a2` (branch `amdgpu-wddm/per-app-graphics-settings`: `4e9a98e9f882dbab1d474b5debb84f67c46dfa33` of `amdgpu-wddm/upstream-2026-10-05` plus the sampler anisotropy setting; upstream HansKristian-Work/vkd3d-proton). Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/41bd52c810dedf32515cd7751dfee8ae8a6065a2 with the submodules pinned there | LGPL-2.1-or-later: `vkd3d-proton-LGPL-2.1.txt`, `vkd3d-proton-COPYING.txt` |
| | with dxil-spirv, D-Ogi/dxil-spirv @ `d2d82332687d7d8777c9c01a7eda0e4089a9074d` (upstream HansKristian-Work/dxil-spirv), and its dxbc-spirv, doitsujin/dxbc-spirv @ `8dae2d8573af08378c6059aca3d5987da99d20b5` | MIT: `dxil-spirv-MIT.txt` (with the bc-decoder and glslang-spirv notices), `dxbc-spirv-MIT.txt` |
| | with SPIRV-Headers, KhronosGroup/SPIRV-Headers @ `f88a2d766840fc825af1fc065977953ba1fa4a91` and `c63848ecf2200425511319fd8bf2c17b751e501e`, and Vulkan-Headers, KhronosGroup/Vulkan-Headers @ `ee2ec5fd83dafce291024683b50dc89219333076` | `SPIRV-Headers-LICENSE.txt`; Apache-2.0 OR MIT: `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `0898891fccbed3d0ccbc1cbf1499954c36a06002` (branch `amdgpu-wddm/per-app-graphics-settings`: `5611118ef0d29d3b3b4bce83bb452694b3975bfb` of `amdgpu-wddm/upstream-2026-10-05` plus the HUD in the presented surface; upstream doitsujin/dxvk) | zlib: `DXVK-zlib.txt` |
| | with dxbc-spirv, D-Ogi/dxbc-spirv @ `f241996e050d4c04091dd5c03cb84fc5523611c8`; the OpenVR API header (ValveSoftware/openvr, copy in DXVK `include/openvr`); SPIRV-Headers @ `04f10f650d514df88b76d25e83db360142c7b174` and Vulkan-Headers @ `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` | MIT: `dxbc-spirv-MIT.txt`; BSD-3-Clause: `OpenVR-BSD-3-Clause.txt`; `SPIRV-Headers-LICENSE.txt`, `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `vulkan\vulkan_radeon.dll`, `vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `d3da6d0ac57230e8b6fccb1d64e3bd4d993549e4` (branch `amdgpu-wddm/b25-system-icd`: `amdgpu-wddm/b25-system` `aa1bd395048dcb8f61da99e2db236091b612078b` over `amdgpu-wddm/bd102-system` `31d3f39008ad8d140a2f64f05f4cf14664af3fb6`, with the dispatch-table generator fix `amdgpu-wddm/b25-gen-x86` `eca7adc94843500835a957939a11dfb9ace60b16` merged, upstream gitlab.freedesktop.org/mesa/mesa) | Mesa: `Mesa-license.rst`, `Mesa-MIT.txt`, `Mesa-BSL-1.0.txt`, `Mesa-SGI-B-2.0.txt`, `Mesa-Apache-2.0.txt` |
| `d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `aca2c81b3c8a52c4f9fd00a2765a8ea7b76e57b3` (branch `amdgpu-wddm/rottr486-null-vs`: `8c110468f37eeaafecace964bd83488903a08487` of branch `amdgpu-wddm/b23-icd` plus the BD-100 pipeline fix), with zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `d3d11\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `aca2c81b3c8a52c4f9fd00a2765a8ea7b76e57b3`, the same build as `d3d12\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `desktop\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `9ddfcbe2fc657bfaa6794a5e47f5749e012a5de4` (branch `amdgpu-wddm/radv-wddm2-hosted-main`) | Mesa texts as above |
| `desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `76ab2c270a40fe65def27dd86fe897c55b266ee9` (branch `amdgpu-wddm/b19-desktop-umd`), with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `desktop\bc250d3d_zink.dll` | Mesa gallium zink and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `ea8765008e4c26e8f8627e3214861a102ec92471` (branch `amdgpu-wddm/b19-hosted-umd`) | Mesa texts as above |
| `wow64\d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `0898891fccbed3d0ccbc1cbf1499954c36a06002` (branch `amdgpu-wddm/per-app-graphics-settings`), x86 build of the commit of `d3d11\amdgpu_wddm_dxvk.dll`, with the dxbc-spirv, OpenVR, SPIRV-Headers and Vulkan-Headers commits of `d3d11\amdgpu_wddm_dxvk.dll` | the texts of `d3d11\amdgpu_wddm_dxvk.dll` above |
| `wow64\d3d11\amdgpu_wddm_radv.dll`, `wow64\vulkan\vulkan_radeon.dll`, `wow64\vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `aca2c81b3c8a52c4f9fd00a2765a8ea7b76e57b3` (branch `amdgpu-wddm/rottr486-null-vs`), x86 build, with zlib 1.3.1 (Meson wrap). The x86 behaviour of BD-064 is in the commit: the `bc250_host_import` x86 layout and the `APIENTRY` fallback stubs | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `wow64\d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `b477495e9719cb2090b4056caf98c57bed3e69a0` (branch `amdgpu-wddm/per-app-graphics-settings-x86`: `d3d12\amdgpu_wddm_vkd3d.dll`'s commit `41bd52c8` plus the x86 fix `bdec00f3`), x86 build, with the dxil-spirv, dxbc-spirv, SPIRV-Headers and Vulkan-Headers commits of `d3d12\amdgpu_wddm_vkd3d.dll`. Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/b477495e9719cb2090b4056caf98c57bed3e69a0 with the submodules pinned there | the texts of `d3d12\amdgpu_wddm_vkd3d.dll` above |
| `wow64\d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `aca2c81b3c8a52c4f9fd00a2765a8ea7b76e57b3`, the same x86 build as `wow64\d3d11\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `wow64\desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `76ab2c270a40fe65def27dd86fe897c55b266ee9`, x86 build, with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`, x86) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `tools\vulkaninfo.exe` | vulkaninfo 1.4.335, KhronosGroup/Vulkan-Tools @ `vulkan-sdk-1.4.335.0` (`8542e6dcfc6daef20d561220f1d91a02c25d95b2`), the unmodified LunarG Vulkan SDK binary. Upstream has no NOTICE file | Apache-2.0: `Vulkan-Tools-LICENSE.txt` |

The Mesa licence texts are the same files at all five Mesa commits above.

The Mesa files of this release come from worktrees with no local change at the commits named: the two RADV
builds of the D3D drivers (`aca2c81b`), the system Vulkan ICD (`d3da6d0a`), the hosted RADV (`9ddfcbe2`), the zink
UMD (`ea876500`) and the two llvmpipe UMD builds (`76ab2c27`, kept from the earlier releases). The two RADV builds
of the D3D drivers give `git-aca2c81b3c` as their Mesa version, and the system Vulkan ICD gives `git-d3da6d0ac5`.
No file of this release needs a patch on top of its commit.

## Not part of this package

| What | Source | Licence text (`licenses\`) |
|---|---|---|
| GPU firmware `cyan_skillfish2_*.bin` (8 files), installed to `C:\BC250\firmware` | downloaded at install time from linux-firmware `2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`, under LICENSE.amdgpu; not part of this package | AMD redistributable binary licence: `LICENSE.amdgpu` |
