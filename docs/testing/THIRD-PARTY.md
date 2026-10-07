# Third-party components in this release package

Each file of the package below and the licence texts that apply to it. The texts are in `licenses\`, taken from the
named repository at the commit given (each text's repository, path, commit and SHA256: `manifest.json`, list
"licenses"). Commits are full SHA-1 hashes in the named repository.

## Our own code

PolyForm Noncommercial 1.0.0, licensor D-Ogi: `amdgpu-wddm-LICENSE.md`, `amdgpu-wddm-NOTICE.txt` (the same as LICENSE.md
and NOTICE in the package root).

| File (`payload\`) | Source |
|---|---|
| `kmd\bc250kmd.sys`, `kmd\bc250kmd.inf` | amdgpu-wddm `85ac47b6c7fe8fb015e1c9c333b82ee94162cfa7` (branch `train/b19-setup`); the .sys also contains the Linux amdgpu code in the next table |
| `d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `85ac47b6c7fe8fb015e1c9c333b82ee94162cfa7` (branch `train/b19-setup`, `driver/umd/d3d12`) |
| `desktop\bc250d3d_router.dll` | amdgpu-wddm `a748e78eedd62745b104dc2ab941ed9b95053c09` (branch `train/b19-wow64-x86`, `driver/umd/router`) |
| `d3d11\amdgpu_wddm_d3d11.dll`, `d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `85ac47b6c7fe8fb015e1c9c333b82ee94162cfa7` (branch `train/b19-setup`, `driver/umd/dxvk`) |
| `wow64\desktop\bc250d3d_router.dll` | amdgpu-wddm `a748e78eedd62745b104dc2ab941ed9b95053c09` (branch `train/b19-wow64-x86`, `driver/umd/router`), x86 build. The x86 behaviour of BD-064 is in the commit: the Wow path values, the 64-bit policy view and plain export names |
| `wow64\d3d11\amdgpu_wddm_d3d11.dll`, `wow64\d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `85ac47b6c7fe8fb015e1c9c333b82ee94162cfa7` (branch `train/b19-setup`, `driver/umd/dxvk`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit |
| `wow64\d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `41919147448aef5696ea2f3335766449844cd613` (branch `umd/wow64-d3d12`, `driver/umd/d3d12`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit, against the `src/util` host contract of Mesa `31844893334cd3debb9771105a0612ababe9d0ee` (the x86 ICD below) |
| `tools\bc250kmd_cli.exe`, `control\bc250control.dll`, `tools\bc250control.dll` | amdgpu-wddm `3ef45a0f92f71980bdaa053be1d5b64c02f1b103` (branch `train/b19-setup`: the CLI and the control DLL from one build of `tools/win/bc250kmd_cli`) |
| `mft\amdgpu_wddm_mft_h264.dll` | amdgpu-wddm `70b6f8388309888690a854ddf4cdf5ab35ea594f` (branch `m15/video-encode-mft`, `driver/umd/mft-h264`) |
| `tools\amdgpu_wddm_d3d12caps.exe` | amdgpu-wddm `tools/win/d3d12caps` |
| `control\amdgpu_wddm_control.exe` | amdgpu-wddm `3ef45a0f92f71980bdaa053be1d5b64c02f1b103` (branch `train/b19-setup`, `tools/win/amdgpu_wddm_control`) |
| `tools\start-confirm.ps1`, `tools\start-confirm-core.ps1`, `tools\dwm-session.ps1`, `tools\release-witness.ps1`, `installer\*`, `*.cmd`, `cert\amdgpu-wddm-release.cer`, `kmd\bc250kmd.cat` | amdgpu-wddm (this release) |

## Third-party code in the package

| File (`payload\`) | Component @ commit | Licence texts (`licenses\`) |
|---|---|---|
| `kmd\bc250kmd.sys` | Linux kernel amdgpu sources and register headers, torvalds/linux @ `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` (v6.18) | MIT: `Linux-amdgpu-MIT.txt` |
| `d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `4e9a98e9f882dbab1d474b5debb84f67c46dfa33` (branch `amdgpu-wddm/upstream-2026-10-05`; upstream HansKristian-Work/vkd3d-proton). Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/4e9a98e9f882dbab1d474b5debb84f67c46dfa33 with the submodules pinned there | LGPL-2.1-or-later: `vkd3d-proton-LGPL-2.1.txt`, `vkd3d-proton-COPYING.txt` |
| | with dxil-spirv, D-Ogi/dxil-spirv @ `d2d82332687d7d8777c9c01a7eda0e4089a9074d` (upstream HansKristian-Work/dxil-spirv), and its dxbc-spirv, doitsujin/dxbc-spirv @ `8dae2d8573af08378c6059aca3d5987da99d20b5` | MIT: `dxil-spirv-MIT.txt` (with the bc-decoder and glslang-spirv notices), `dxbc-spirv-MIT.txt` |
| | with SPIRV-Headers, KhronosGroup/SPIRV-Headers @ `f88a2d766840fc825af1fc065977953ba1fa4a91` and `c63848ecf2200425511319fd8bf2c17b751e501e`, and Vulkan-Headers, KhronosGroup/Vulkan-Headers @ `ee2ec5fd83dafce291024683b50dc89219333076` | `SPIRV-Headers-LICENSE.txt`; Apache-2.0 OR MIT: `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `5611118ef0d29d3b3b4bce83bb452694b3975bfb` (branch `amdgpu-wddm/upstream-2026-10-05`; upstream doitsujin/dxvk) | zlib: `DXVK-zlib.txt` |
| | with dxbc-spirv, D-Ogi/dxbc-spirv @ `f241996e050d4c04091dd5c03cb84fc5523611c8`; the OpenVR API header (ValveSoftware/openvr, copy in DXVK `include/openvr`); SPIRV-Headers @ `04f10f650d514df88b76d25e83db360142c7b174` and Vulkan-Headers @ `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` | MIT: `dxbc-spirv-MIT.txt`; BSD-3-Clause: `OpenVR-BSD-3-Clause.txt`; `SPIRV-Headers-LICENSE.txt`, `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `vulkan\vulkan_radeon.dll`, `vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `2732f9c889fffb1c892619d056a07fd08815b306` (upstream gitlab.freedesktop.org/mesa/mesa) | Mesa: `Mesa-license.rst`, `Mesa-MIT.txt`, `Mesa-BSL-1.0.txt`, `Mesa-SGI-B-2.0.txt`, `Mesa-Apache-2.0.txt` |
| `d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `31844893334cd3debb9771105a0612ababe9d0ee` (branch `amdgpu-wddm/b19-icd`), with zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `d3d11\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `31844893334cd3debb9771105a0612ababe9d0ee`, the same build as `d3d12\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `desktop\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `48546c732a832b5ed0c97d28fdcf335a9960cc06` | Mesa texts as above |
| `desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `76ab2c270a40fe65def27dd86fe897c55b266ee9` (branch `amdgpu-wddm/b19-desktop-umd`), with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `desktop\bc250d3d_zink.dll` | Mesa gallium zink and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `7117b189f17d27039b22b729d9c592a85c3b73b5` (branch `amdgpu-wddm/b19-hosted-umd`) | Mesa texts as above |
| `wow64\d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `5611118ef0d29d3b3b4bce83bb452694b3975bfb` (branch `amdgpu-wddm/upstream-2026-10-05`), x86 build, with the dxbc-spirv, OpenVR, SPIRV-Headers and Vulkan-Headers commits of `d3d11\amdgpu_wddm_dxvk.dll` | the texts of `d3d11\amdgpu_wddm_dxvk.dll` above |
| `wow64\d3d11\amdgpu_wddm_radv.dll`, `wow64\vulkan\vulkan_radeon.dll`, `wow64\vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `31844893334cd3debb9771105a0612ababe9d0ee` (branch `amdgpu-wddm/b19-icd`), x86 build, with zlib 1.3.1 (Meson wrap). The x86 behaviour of BD-064 is in the commit: the `bc250_host_import` x86 layout and the `APIENTRY` fallback stubs | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `wow64\d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `bdec00f356f620959d8b93a0a545f4f11c111b0d` (branch `amdgpu-wddm/wow64-d3d12`: `d3d12\amdgpu_wddm_vkd3d.dll`'s commit `4e9a98e9` plus one x86 calling convention fix), x86 build, with the dxil-spirv, dxbc-spirv, SPIRV-Headers and Vulkan-Headers commits of `d3d12\amdgpu_wddm_vkd3d.dll`. Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/bdec00f356f620959d8b93a0a545f4f11c111b0d with the submodules pinned there | the texts of `d3d12\amdgpu_wddm_vkd3d.dll` above |
| `wow64\d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `31844893334cd3debb9771105a0612ababe9d0ee`, the same x86 build as `wow64\d3d11\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `wow64\desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `76ab2c270a40fe65def27dd86fe897c55b266ee9`, x86 build, with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`, x86) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `tools\vulkaninfo.exe` | vulkaninfo 1.4.335, KhronosGroup/Vulkan-Tools @ `vulkan-sdk-1.4.335.0` (`8542e6dcfc6daef20d561220f1d91a02c25d95b2`), the unmodified LunarG Vulkan SDK binary. Upstream has no NOTICE file | Apache-2.0: `Vulkan-Tools-LICENSE.txt` |

The Mesa licence texts are the same files at all five Mesa commits above.

The Mesa files that this release built come from worktrees with no local change at the commits named: the two
RADV builds (`31844893`), the two llvmpipe UMD builds (`76ab2c27`) and the zink UMD (`7117b189`). Both RADV
builds give `git-3184489333` as their Mesa version, which is that commit. `desktop\amdgpu_wddm_radv.dll` and
`vulkan\vulkan_radeon.dll` keep the builds of the earlier releases. No file of this release needs a patch on
top of its commit.

## Not part of this package

| What | Source | Licence text (`licenses\`) |
|---|---|---|
| GPU firmware `cyan_skillfish2_*.bin` (8 files), installed to `C:\BC250\firmware` | downloaded at install time from linux-firmware `2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`, under LICENSE.amdgpu; not part of this package | AMD redistributable binary licence: `LICENSE.amdgpu` |
