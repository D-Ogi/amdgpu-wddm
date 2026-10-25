# Third-party components in this release package

Each file of the package below and the licence texts that apply to it. The texts are in `licenses\`, taken from the
named repository at the commit given (each text's repository, path, commit and SHA256: `manifest.json`, list
"licenses"). Commits are full SHA-1 hashes in the named repository. The commit of each file is also in
`manifest.json`, with the recipe and the arguments of its build.

## Our own code

PolyForm Noncommercial 1.0.0, licensor D-Ogi: `amdgpu-wddm-LICENSE.md`, `amdgpu-wddm-NOTICE.txt` (the same as LICENSE.md
and NOTICE in the package root).

| File (`payload\`) | Source |
|---|---|
| `adl\atiadlxx.dll`, `wow64\adl\atiadlxy.dll` | amdgpu-wddm `c906d8b401eca954987c8acd3b7fc3250ca8595d` (`driver/umd/adl`), public ADL subset. The x86 DriverStore member is renamed `atiadlxy32.dll` |
| `kmd\bc250kmd.sys`, `kmd\bc250kmd.inf` | amdgpu-wddm `b0ad6a30a74408f2dd7a49cfbde62963fd5bbc6f` (branch `train/b35`); the .sys also contains the Linux amdgpu code in the next table |
| `d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `b0ad6a30a74408f2dd7a49cfbde62963fd5bbc6f` (branch `train/b35`, `driver/umd/d3d12`), against the `src/util` host contract of Mesa `66986af442c97653cd6046b01d27bb555ff39fd4`, the commit of `d3d12\amdgpu_wddm_radv.dll` |
| `desktop\bc250d3d_router.dll` | amdgpu-wddm `5c8c15cc6d7c1f625900a65f960f90a219f7fd90` (branch `train/b35`, `driver/umd/router`) |
| `d3d11\amdgpu_wddm_d3d11.dll`, `d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `b0ad6a30a74408f2dd7a49cfbde62963fd5bbc6f` (branch `train/b35`, `driver/umd/dxvk`, the record from `tools/build/write-umd-config.py`) |
| `wow64\desktop\bc250d3d_router.dll` | amdgpu-wddm `5c8c15cc6d7c1f625900a65f960f90a219f7fd90` (branch `train/b35`, `driver/umd/router`), x86 build |
| `wow64\d3d11\amdgpu_wddm_d3d11.dll`, `wow64\d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `b0ad6a30a74408f2dd7a49cfbde62963fd5bbc6f` (branch `train/b35`, `driver/umd/dxvk`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit |
| `wow64\d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `b0ad6a30a74408f2dd7a49cfbde62963fd5bbc6f` (branch `train/b35`, `driver/umd/d3d12`), x86 build with the `-Arch x86` recipe and the `.def` export of the same commit, against the `src/util` host contract of Mesa `66986af442c97653cd6046b01d27bb555ff39fd4`. The x86 ICD below has the same commit |
| `tools\bc250kmd_cli.exe`, `control\bc250control.dll`, `tools\bc250control.dll` | amdgpu-wddm `5c8c15cc6d7c1f625900a65f960f90a219f7fd90` (branch `train/b35`: the CLI and the control DLL from one build of `tools/win/bc250kmd_cli`) |
| `mft\amdgpu_wddm_mft_h264.dll` | amdgpu-wddm `1750f50535a79e49878298c6078df8745ce1068f` (branch `main`, `driver/umd/mft-h264`) |
| `tools\amdgpu_wddm_d3d12caps.exe` | amdgpu-wddm `0d82a0597e3180ead2ab7519fef7e26be1f30ffd` (branch `main`, `tools/win/d3d12caps`) |
| `kmd\amdgpu-state.bin`, `tools\amdgpu-state-installer.exe`, `tools\state-reader.ps1` | amdgpu-wddm `driver/state` and `tools/release/installer`, at the source revision recorded in this package's `manifest.json` |
| `tools\d3d11bench.exe` | amdgpu-wddm `1884b33d8bb7ff98d2864d9b5fd5c6de1ffceb5b` (branch `main`, `tools/win/d3d11bench`) |
| `tools\hip\amdhip64.dll`, `tools\hip\vadd.exe`, `tools\hip\bc250hipblas.dll` | amdgpu-wddm `1750f50535a79e49878298c6078df8745ce1068f` (branch `main`, `compute/hip`: the runtime and the sample) and `3737b075a607147c9937d0504ed6b8ec1cbd5865` (branch `main`, `compute/hipblas`), experimental |
| `control\amdgpu_wddm_control.exe` | amdgpu-wddm `5c8c15cc6d7c1f625900a65f960f90a219f7fd90` (branch `train/b35`, `tools/win/amdgpu_wddm_control`) |
| `tools\start-confirm.ps1`, `tools\start-confirm-core.ps1`, `tools\dwm-session.ps1`, `tools\release-witness.ps1`, `installer\*`, `*.cmd`, `cert\amdgpu-wddm-release.cer`, `kmd\bc250kmd.cat` | amdgpu-wddm (this release) |

## Third-party code in the package

| File (`payload\`) | Component @ commit | Licence texts (`licenses\`) |
|---|---|---|
| `kmd\bc250kmd.sys` | Linux kernel amdgpu sources and register headers, torvalds/linux @ `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` (v6.18) | MIT: `Linux-amdgpu-MIT.txt` |
| `d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `b69df5fb2d44858a7d5aa9e322d6a4698a8b4536` (branch `amdgpu-wddm/tester31-engine`, upstream HansKristian-Work/vkd3d-proton). Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/b69df5fb2d44858a7d5aa9e322d6a4698a8b4536 with the submodules pinned there | LGPL-2.1-or-later: `vkd3d-proton-LGPL-2.1.txt`, `vkd3d-proton-COPYING.txt` |
| | with dxil-spirv, D-Ogi/dxil-spirv @ `064a1aa499b3cf21a40943f7dc4fe4bcb0eab476` (upstream HansKristian-Work/dxil-spirv), and its dxbc-spirv, doitsujin/dxbc-spirv @ `8dae2d8573af08378c6059aca3d5987da99d20b5` | MIT: `dxil-spirv-MIT.txt` (with the bc-decoder and glslang-spirv notices), `dxbc-spirv-MIT.txt` |
| | with SPIRV-Headers, KhronosGroup/SPIRV-Headers @ `f88a2d766840fc825af1fc065977953ba1fa4a91` and `c63848ecf2200425511319fd8bf2c17b751e501e`, and Vulkan-Headers, KhronosGroup/Vulkan-Headers @ `ee2ec5fd83dafce291024683b50dc89219333076` | `SPIRV-Headers-LICENSE.txt`; Apache-2.0 OR MIT: `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `c271e0e005936223683d7ecb7200816b6a2405e1` (branch `amdgpu-wddm/bd214-query-getdata`, upstream doitsujin/dxvk) | zlib: `DXVK-zlib.txt` |
| | with dxbc-spirv, D-Ogi/dxbc-spirv @ `28fda04a8a1ce00b1d2edbbfe5ccc5dec2b7c07f` and its SPIRV-Headers @ `c8ad050fcb29e42a2f57d9f59e97488f465c436d`, libdisplay-info, doitsujin/libdisplay-info @ `275e6459c7ab1ddd4b125f28d0440716e4888078` (branch `windows`), the OpenVR API header (ValveSoftware/openvr, copy in DXVK `include/openvr`), SPIRV-Headers @ `04f10f650d514df88b76d25e83db360142c7b174` and Vulkan-Headers @ `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` | MIT: `dxbc-spirv-MIT.txt`, `libdisplay-info-MIT.txt`; BSD-3-Clause: `OpenVR-BSD-3-Clause.txt`; `SPIRV-Headers-LICENSE.txt`, `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `vulkan\vulkan_radeon.dll`, `vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `29d95fd74bce06a764d536b0ad8a0bc60b1dc608` (branch `train/b35-radv-system`, upstream gitlab.freedesktop.org/mesa/mesa), with zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa: `Mesa-license.rst`, `Mesa-MIT.txt`, `Mesa-BSL-1.0.txt`, `Mesa-SGI-B-2.0.txt`, `Mesa-Apache-2.0.txt`, and Zlib `zlib-LICENSE.txt` |
| `d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `66986af442c97653cd6046b01d27bb555ff39fd4` (branch `fix/bd201-destroy-fail-hosted`), with zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa texts as above, and Zlib `zlib-LICENSE.txt` |
| `d3d11\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `66986af442c97653cd6046b01d27bb555ff39fd4`, the same build as `d3d12\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `desktop\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `a115b714ee6806fa866b7170ae68e8700cd0d3c6` (branch `fix/bd201-destroy-fail-desktop`), with zlib 1.3.1 (Meson wrap) | Mesa texts as above, and Zlib `zlib-LICENSE.txt` |
| `desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `fa5f386e8c93039e4cdbca607b2541e102e4e0f0` (branch `fix/bd167-release-cpu`), with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `desktop\bc250d3d_zink.dll` | Mesa gallium zink and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `471a7487ee7f0404d03b7fe00834d154e2675a48` (branch `fix/bd167-release-zink`) | Mesa texts as above |
| `opengl\amdgpu_wddm_gl.dll` | Mesa gallium zink and the WGL target, D-Ogi/mesa-amdgpu-wddm @ `71b34d2c336264684e03ae9ca68d16def4875c05` (branch `fix/bd167-release-gl`), the system OpenGL ICD of BD-076, for 64-bit applications. This release registers no 32-bit OpenGL driver. The package therefore carries no x86 build of it. With zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa texts as above, and Zlib `zlib-LICENSE.txt` |
| `wow64\d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `c271e0e005936223683d7ecb7200816b6a2405e1` (branch `amdgpu-wddm/bd214-query-getdata`), x86 build of the commit of `d3d11\amdgpu_wddm_dxvk.dll`, with the dxbc-spirv, libdisplay-info, OpenVR, SPIRV-Headers and Vulkan-Headers commits of `d3d11\amdgpu_wddm_dxvk.dll` | the texts of `d3d11\amdgpu_wddm_dxvk.dll` above |
| `wow64\d3d11\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `66986af442c97653cd6046b01d27bb555ff39fd4` (branch `fix/bd201-destroy-fail-hosted`), x86 build of the commit of `d3d12\amdgpu_wddm_radv.dll`, with zlib 1.3.1 (Meson wrap) | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `wow64\vulkan\vulkan_radeon.dll`, `wow64\vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `771f84136ef41d7403ff05360e27773db6e77d8d` (branch `fix/bd201-destroy-fail-wow64`), x86 build, with zlib 1.3.1 (Meson wrap) | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `wow64\d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `b69df5fb2d44858a7d5aa9e322d6a4698a8b4536` (branch `amdgpu-wddm/tester31-engine`), x86 build of the commit of `d3d12\amdgpu_wddm_vkd3d.dll`, with the dxil-spirv, dxbc-spirv, SPIRV-Headers and Vulkan-Headers commits of `d3d12\amdgpu_wddm_vkd3d.dll`. Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/b69df5fb2d44858a7d5aa9e322d6a4698a8b4536 with the submodules pinned there | the texts of `d3d12\amdgpu_wddm_vkd3d.dll` above |
| `wow64\d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `66986af442c97653cd6046b01d27bb555ff39fd4`, the same x86 build as `wow64\d3d11\amdgpu_wddm_radv.dll` | Mesa texts as above |
| `wow64\desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `fa5f386e8c93039e4cdbca607b2541e102e4e0f0`, x86 build, with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`, x86) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `tools\vulkaninfo.exe` | vulkaninfo 1.4.335, KhronosGroup/Vulkan-Tools @ `vulkan-sdk-1.4.335.0` (`8542e6dcfc6daef20d561220f1d91a02c25d95b2`), the unmodified LunarG Vulkan SDK binary. Upstream has no NOTICE file | Apache-2.0: `Vulkan-Tools-LICENSE.txt` |
| `redist\vc_redist.x64.exe` | Microsoft Visual C++ 2015-2022 Redistributable (x64) 14.44.35211, Microsoft Corporation, downloaded unchanged from https://aka.ms/vs/17/release/vc_redist.x64.exe and signed by Microsoft. The installer runs it when the runtime is missing or older than 14.44, and it installs Microsoft's own files in System32 | Microsoft redistributable, under the Visual Studio licence terms that the installer itself shows. No text in `licenses\` |

The Mesa licence texts are the same files at all seven Mesa commits above.

The Mesa files of this release come from worktrees with no local change at the commits named: the hosted RADV of
the D3D drivers, x64 and x86 (`66986af4`), the 32-bit Vulkan ICD (`771f8413`), the system Vulkan ICD (`29d95fd7`),
the desktop RADV (`a115b714`), the zink UMD (`471a7487`), the OpenGL ICD (`71b34d2c`) and the two llvmpipe UMD
builds (`fa5f386e`). No file of this release needs a patch on top of its commit.

## Not part of this package

| What | Source | Licence text (`licenses\`) |
|---|---|---|
| GPU firmware `cyan_skillfish2_*.bin` (8 files), installed to `C:\BC250\firmware` | downloaded at install time from linux-firmware `2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`, under LICENSE.amdgpu; not part of this package | AMD redistributable binary licence: `LICENSE.amdgpu` |
