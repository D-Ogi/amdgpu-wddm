# Third-party components in this release package

Each file of the package below and the licence texts that apply to it. The texts are in `licenses\`, taken from the
named repository at the commit given (each text's repository, path, commit and SHA256: `manifest.json`, list
"licenses"). Commits are full SHA-1 hashes in the named repository. The commit of each file is also in
`manifest.json`, with the recipe and the arguments of its build. `tools/release/third_party_doc.py` compares every
commit of this list with the release source pins and refuses the package on a difference.

## Our own code

PolyForm Noncommercial 1.0.0, licensor D-Ogi: `amdgpu-wddm-LICENSE.md`, `amdgpu-wddm-NOTICE.txt` (the same as LICENSE.md
and NOTICE in the package root).

| File (`payload\`) | Source |
|---|---|
| `adl\atiadlxx.dll`, `wow64\adl\atiadlxy.dll` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/adl`), public ADL subset. Both DriverStore members retain their unique ADL load names |
| `kmd\bc250kmd.sys`, `kmd\bc250kmd.inf` | amdgpu-wddm `b1afec48d2acf32c6b650954aaf0f33a54c54bb9`; the .sys also contains the Linux amdgpu code in the next table |
| `d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/d3d12`), against the `src/util` host contract of Mesa `4968dff327a9dacc39cb9627a4b1a20eb7d21f76`, the commit of `d3d12\amdgpu_wddm_radv.dll` |
| `desktop\bc250d3d_router.dll` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/router`) |
| `d3d11\amdgpu_wddm_d3d11.dll`, `d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/dxvk`). The config is the record from `tools/build/write-umd-config.py` at `97b0f39ebc04f8c23c1b2110b9ee0d15034713f6` |
| `wow64\desktop\bc250d3d_router.dll` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/router`), x86 build |
| `wow64\d3d11\amdgpu_wddm_d3d11.dll`, `wow64\d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/dxvk`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit. The config is the record from `tools/build/write-umd-config.py` at `97b0f39ebc04f8c23c1b2110b9ee0d15034713f6` |
| `wow64\d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/d3d12`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit, against the `src/util` host contract of Mesa `4968dff327a9dacc39cb9627a4b1a20eb7d21f76`. The x86 ICD below has the same commit |
| `tools\bc250kmd_cli.exe`, `control\bc250control.dll`, `tools\bc250control.dll` | amdgpu-wddm `b1afec48d2acf32c6b650954aaf0f33a54c54bb9` (the CLI and the control DLL from one build of `tools/win/bc250kmd_cli`) |
| `mft\amdgpu_wddm_mft_h264.dll` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`driver/umd/mft-h264`) |
| `tools\amdgpu_wddm_d3d12caps.exe` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`tools/win/d3d12caps`) |
| `kmd\amdgpu-state.bin`, `tools\amdgpu-state-installer.exe`, `tools\state-reader.ps1` | amdgpu-wddm `driver/state` and `tools/release/installer`: the state file from the kernel driver build at `b1afec48d2acf32c6b650954aaf0f33a54c54bb9`, the state installer at `c281df214cde2c008a8a6e26827b494a76d22e29` |
| `tools\d3d11bench.exe` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`tools/win/d3d11bench`) |
| `tools\hip\amdhip64.dll`, `tools\hip\vadd.exe`, `tools\hip\bc250hipblas.dll` | amdgpu-wddm `c281df214cde2c008a8a6e26827b494a76d22e29` (`compute/hip`: the runtime and the sample, and `compute/hipblas`), experimental |
| `control\amdgpu_wddm_control.exe` | amdgpu-wddm `b1afec48d2acf32c6b650954aaf0f33a54c54bb9` (`tools/win/amdgpu_wddm_control`) |
| `tools\start-confirm.ps1`, `tools\start-confirm-core.ps1`, `tools\dwm-session.ps1`, `tools\release-witness.ps1`, `installer\*`, `*.cmd`, `cert\amdgpu-wddm-release.cer`, `kmd\bc250kmd.cat` | amdgpu-wddm (this release) |

## Third-party code in the package

| File (`payload\`) | Component @ commit | Licence texts (`licenses\`) |
|---|---|---|
| `kmd\bc250kmd.sys` | Linux kernel amdgpu sources and register headers, torvalds/linux @ `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` (v6.18) | MIT: `Linux-amdgpu-MIT.txt` |
| `d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `98e7a56a39166d8ef4e1711b1b34d6942e32c7da` (upstream HansKristian-Work/vkd3d-proton). Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/98e7a56a39166d8ef4e1711b1b34d6942e32c7da with the submodules pinned there | LGPL-2.1-or-later: `vkd3d-proton-LGPL-2.1.txt`, `vkd3d-proton-COPYING.txt` |
| | with dxil-spirv, D-Ogi/dxil-spirv @ `527c237d79d14aac906163cc90886548abdbeeff` (upstream HansKristian-Work/dxil-spirv), and its dxbc-spirv, doitsujin/dxbc-spirv @ `8dae2d8573af08378c6059aca3d5987da99d20b5` with its SPIRV-Headers @ `c8ad050fcb29e42a2f57d9f59e97488f465c436d` | MIT: `dxil-spirv-MIT.txt` (with the bc-decoder and glslang-spirv notices), `dxbc-spirv-MIT.txt` |
| | with SPIRV-Headers, KhronosGroup/SPIRV-Headers @ `f88a2d766840fc825af1fc065977953ba1fa4a91` and `c63848ecf2200425511319fd8bf2c17b751e501e`, and Vulkan-Headers, KhronosGroup/Vulkan-Headers @ `ee2ec5fd83dafce291024683b50dc89219333076` | `SPIRV-Headers-LICENSE.txt`; Apache-2.0 OR MIT: `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `86ca97b2eedaf4554a6c8db11b598ce58d04e643` (upstream doitsujin/dxvk) | zlib: `DXVK-zlib.txt` |
| | with dxbc-spirv, D-Ogi/dxbc-spirv @ `28fda04a8a1ce00b1d2edbbfe5ccc5dec2b7c07f` and its SPIRV-Headers @ `c8ad050fcb29e42a2f57d9f59e97488f465c436d`, libdisplay-info, doitsujin/libdisplay-info @ `275e6459c7ab1ddd4b125f28d0440716e4888078`, the OpenVR API header (ValveSoftware/openvr, copy in DXVK `include/openvr`), SPIRV-Headers @ `04f10f650d514df88b76d25e83db360142c7b174` and Vulkan-Headers @ `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` | MIT: `dxbc-spirv-MIT.txt`, `libdisplay-info-MIT.txt`; BSD-3-Clause: `OpenVR-BSD-3-Clause.txt`; `SPIRV-Headers-LICENSE.txt`, `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `vulkan\vulkan_radeon.dll`, `vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `d12fb4fd8c1a9736b259d5f355a6393b6f585635` (upstream gitlab.freedesktop.org/mesa/mesa), with zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap). The ICD manifest is from amdgpu-wddm `df9be61439d03a667e8874e82e0ca67554589eee` | Mesa: `Mesa-license.rst`, `Mesa-MIT.txt`, `Mesa-BSL-1.0.txt`, `Mesa-SGI-B-2.0.txt`, `Mesa-Apache-2.0.txt`, and Zlib `zlib-LICENSE.txt` |
| `d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `4968dff327a9dacc39cb9627a4b1a20eb7d21f76`, with zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa texts as above, and Zlib `zlib-LICENSE.txt` |
| `d3d11\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `4968dff327a9dacc39cb9627a4b1a20eb7d21f76`, the same build as `d3d12\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `desktop\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `db0834b83075c0908166635e261761cda423edbf`, with zlib 1.3.1 (Meson wrap) | Mesa texts as above, and Zlib `zlib-LICENSE.txt` |
| `desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `6b3e9b300f8c5ee952b4358dc1416aa583c1fcb9`, with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `desktop\bc250d3d_zink.dll` | Mesa gallium zink and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `49962ef736bc4ff8d577e046a6549a633da5fe59` | Mesa texts as above |
| `opengl\amdgpu_wddm_gl.dll` | Mesa gallium zink and the WGL target, D-Ogi/mesa-amdgpu-wddm @ `1226ec984d45b17f6730de84b2b7e98073b679cd`, the system OpenGL ICD of BD-076, for 64-bit applications. This release registers no 32-bit OpenGL driver. The package therefore carries no x86 build of it. With zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa texts as above, and Zlib `zlib-LICENSE.txt` |
| `wow64\d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `86ca97b2eedaf4554a6c8db11b598ce58d04e643`, x86 build of the commit of `d3d11\amdgpu_wddm_dxvk.dll`, with the dxbc-spirv, libdisplay-info, OpenVR, SPIRV-Headers and Vulkan-Headers commits of `d3d11\amdgpu_wddm_dxvk.dll` | the texts of `d3d11\amdgpu_wddm_dxvk.dll` above |
| `wow64\d3d11\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `4968dff327a9dacc39cb9627a4b1a20eb7d21f76`, x86 build of the commit of `d3d12\amdgpu_wddm_radv.dll`, with zlib 1.3.1 (Meson wrap) | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `wow64\vulkan\vulkan_radeon.dll`, `wow64\vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `53ccb05f2045c9f1be014c0d6e8263b01f00ec27`, x86 build, with zlib 1.3.1 (Meson wrap). The ICD manifest is from amdgpu-wddm `df9be61439d03a667e8874e82e0ca67554589eee` | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `wow64\d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `98e7a56a39166d8ef4e1711b1b34d6942e32c7da`, x86 build of the commit of `d3d12\amdgpu_wddm_vkd3d.dll`, with the dxil-spirv, dxbc-spirv, SPIRV-Headers and Vulkan-Headers commits of `d3d12\amdgpu_wddm_vkd3d.dll`. Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/98e7a56a39166d8ef4e1711b1b34d6942e32c7da with the submodules pinned there | the texts of `d3d12\amdgpu_wddm_vkd3d.dll` above |
| `wow64\d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `4968dff327a9dacc39cb9627a4b1a20eb7d21f76`, the same x86 build as `wow64\d3d11\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `wow64\desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `6b3e9b300f8c5ee952b4358dc1416aa583c1fcb9`, x86 build, with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`, x86) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `tools\vulkaninfo.exe` | vulkaninfo 1.4.335, KhronosGroup/Vulkan-Tools @ `vulkan-sdk-1.4.335.0`, the unmodified LunarG Vulkan SDK binary. Upstream has no NOTICE file | Apache-2.0: `Vulkan-Tools-LICENSE.txt` |
| `redist\vc_redist.x64.exe` | Microsoft Visual C++ 2015-2022 Redistributable (x64) 14.44.35211, Microsoft Corporation, downloaded unchanged from https://aka.ms/vs/17/release/vc_redist.x64.exe and signed by Microsoft. The installer runs it when the runtime is missing or older than 14.44, and it installs Microsoft's own files in System32 | Microsoft redistributable, under the Visual Studio licence terms that the installer itself shows. No text in `licenses\` |

The Mesa, DXVK and vkd3d-proton licence texts are the same files at every commit of that repository in this release.

The Mesa files of this release come from worktrees with no local change at the commits named above. No file of this
release needs a patch on top of its commit.

## Not part of this package

| What | Source | Licence text (`licenses\`) |
|---|---|---|
| GPU firmware `cyan_skillfish2_*.bin` (8 files), installed to `C:\BC250\firmware` | downloaded at install time from linux-firmware `2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`, under LICENSE.amdgpu; not part of this package | AMD redistributable binary licence: `LICENSE.amdgpu` |
