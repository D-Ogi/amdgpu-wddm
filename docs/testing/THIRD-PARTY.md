# Third-party components in this release package

Each file of the package below and the licence texts that apply to it. The texts are in `licenses\`, taken from the
named repository at the commit given (each text's repository, path, commit and SHA256: `manifest.json`, list
"licenses"). Commits are full SHA-1 hashes in the named repository.

## Our own code

PolyForm Noncommercial 1.0.0, licensor D-Ogi: `amdgpu-wddm-LICENSE.md`, `amdgpu-wddm-NOTICE.txt` (the same as LICENSE.md
and NOTICE in the package root).

| File (`payload\`) | Source |
|---|---|
| `kmd\bc250kmd.sys`, `kmd\bc250kmd.inf` | amdgpu-wddm `0aec4c58839d2e712a1fff3d9937215a5277ed02`; the .sys also contains the Linux amdgpu code in the next table |
| `system32\bc250umd.dll` | amdgpu-wddm `driver/umd-stub` |
| `d3d12\amdgpu_wddm_d3d12.dll` | amdgpu-wddm `e9f5e701ac0412e1cf39e217075d1f8ed313d120` |
| `desktop\bc250d3d_router.dll` | amdgpu-wddm `6bbcf4f6bcba577e5d9d170246128496c7eb087d` (`driver/umd/router`) |
| `d3d11\amdgpu_wddm_d3d11.dll`, `d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `driver/umd/dxvk` |
| `syswow64\bc250umd.dll` | amdgpu-wddm `driver/umd-stub`, x86 build |
| `wow64\desktop\bc250d3d_router.dll` | amdgpu-wddm `631d25da` (`driver/umd/router`) with the BD-064 x86 changes (Wow path values, 64-bit policy view, plain export names), x86 build |
| `wow64\d3d11\amdgpu_wddm_d3d11.dll`, `wow64\d3d11\amdgpu_wddm_d3d11.config` | amdgpu-wddm `7ae6462f` (`driver/umd/dxvk`) with the BD-064 x86 changes (plain export name), x86 build |
| `tools\bc250kmd_cli.exe`, `control\bc250control.dll`, `tools\bc250control.dll` | amdgpu-wddm `d8347cf031d2ee3bc2a79b5b142d2612875d96a3` (branch `release/control-app-t11`: the CLI and the control DLL from one build of `tools/win/bc250kmd_cli`; the build at `dd30a44d` gives the same bytes) |
| `tools\amdgpu_wddm_d3d12caps.exe` | amdgpu-wddm `tools/win/d3d12caps` |
| `control\amdgpu_wddm_control.exe` | amdgpu-wddm `dd30a44d94500e7002b297a3ca5e19c43e1d9ff1` (branch `release/control-app-t11`) |
| `tools\start-confirm.ps1`, `tools\start-confirm-core.ps1`, `tools\dwm-session.ps1`, `installer\*`, `*.cmd`, `cert\amdgpu-wddm-release.cer`, `kmd\bc250kmd.cat` | amdgpu-wddm (this release) |

## Third-party code in the package

| File (`payload\`) | Component @ commit | Licence texts (`licenses\`) |
|---|---|---|
| `kmd\bc250kmd.sys` | Linux kernel amdgpu sources and register headers, torvalds/linux @ `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` (v6.18) | MIT: `Linux-amdgpu-MIT.txt` |
| `d3d12\amdgpu_wddm_vkd3d.dll` | vkd3d-proton, D-Ogi/vkd3d-proton @ `c3710ac1a6a857deb286af2cb15ef7335cb50095` (branch `amdgpu-wddm/draw-path`; upstream HansKristian-Work/vkd3d-proton). Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/c3710ac1a6a857deb286af2cb15ef7335cb50095 with the submodules pinned there | LGPL-2.1-or-later: `vkd3d-proton-LGPL-2.1.txt`, `vkd3d-proton-COPYING.txt` |
| | with dxil-spirv, D-Ogi/dxil-spirv @ `bc773cf2d287e64838fb0a7fc3aabdec323a0a19` (upstream HansKristian-Work/dxil-spirv), and its dxbc-spirv, doitsujin/dxbc-spirv @ `8dae2d8573af08378c6059aca3d5987da99d20b5` | MIT: `dxil-spirv-MIT.txt` (with the bc-decoder and glslang-spirv notices), `dxbc-spirv-MIT.txt` |
| | with SPIRV-Headers, KhronosGroup/SPIRV-Headers @ `f88a2d766840fc825af1fc065977953ba1fa4a91` and `c63848ecf2200425511319fd8bf2c17b751e501e`, and Vulkan-Headers, KhronosGroup/Vulkan-Headers @ `ee2ec5fd83dafce291024683b50dc89219333076` | `SPIRV-Headers-LICENSE.txt`; Apache-2.0 OR MIT: `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `0c187731d88db839b9d4b3a6798fbd146b92eabc` (branch `amdgpu-wddm/ddi-engine-fl12`; upstream doitsujin/dxvk) | zlib: `DXVK-zlib.txt` |
| | with dxbc-spirv, D-Ogi/dxbc-spirv @ `f241996e050d4c04091dd5c03cb84fc5523611c8`; the OpenVR API header (ValveSoftware/openvr, copy in DXVK `include/openvr`); SPIRV-Headers @ `04f10f650d514df88b76d25e83db360142c7b174` and Vulkan-Headers @ `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` | MIT: `dxbc-spirv-MIT.txt`; BSD-3-Clause: `OpenVR-BSD-3-Clause.txt`; `SPIRV-Headers-LICENSE.txt`, `Vulkan-Headers-LICENSE.md`, `Vulkan-Headers-Apache-2.0.txt`, `Vulkan-Headers-MIT.txt` |
| `vulkan\vulkan_radeon.dll`, `vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `2732f9c889fffb1c892619d056a07fd08815b306` (upstream gitlab.freedesktop.org/mesa/mesa) | Mesa: `Mesa-license.rst`, `Mesa-MIT.txt`, `Mesa-BSL-1.0.txt`, `Mesa-SGI-B-2.0.txt`, `Mesa-Apache-2.0.txt` |
| `d3d12\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `ae98c795516061670b4de42a12809d6967cfcf49`, with zlib 1.3.1 (madler/zlib @ `v1.3.1`, Meson wrap) | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `d3d11\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `05e6c9622e135ac2aeaf56ec70222642627e2162` | Mesa texts as above |
| `desktop\amdgpu_wddm_radv.dll` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `48546c732a832b5ed0c97d28fdcf335a9960cc06` | Mesa texts as above |
| `desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `43d0907cba87cbc8d71026ca68ba7fd409fc3514` (branch `amdgpu-wddm/desktop-umd-bd058`), with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `desktop\bc250d3d_zink.dll` | Mesa gallium zink and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `8cb2284409219b0f4ece460623709ac7c8e91756` (branch `amdgpu-wddm/hosted-umd-bd058`) | Mesa texts as above |
| `wow64\d3d11\amdgpu_wddm_dxvk.dll` | DXVK, D-Ogi/dxvk @ `ebeadc49` (branch `amdgpu-wddm/ddi-engine-inline-throttle`), x86 build, with the dxbc-spirv, OpenVR, SPIRV-Headers and Vulkan-Headers commits of `d3d11\amdgpu_wddm_dxvk.dll` | the texts of `d3d11\amdgpu_wddm_dxvk.dll` above |
| `wow64\d3d11\amdgpu_wddm_radv.dll`, `wow64\vulkan\vulkan_radeon.dll`, `wow64\vulkan\radeon_icd.json` | Mesa RADV, D-Ogi/mesa-amdgpu-wddm @ `ae98c795516061670b4de42a12809d6967cfcf49` with the BD-064 x86 changes (`bc250_host_import` x86 layout, `APIENTRY` fallback stubs), x86 build, with zlib 1.3.1 (Meson wrap) | Mesa texts as above; Zlib: `zlib-LICENSE.txt` |
| `wow64\desktop\bc250d3d.dll` | Mesa gallium llvmpipe and D3D10 UMD, D-Ogi/mesa-amdgpu-wddm @ `43d0907cba87cbc8d71026ca68ba7fd409fc3514`, x86 build, with LLVM 23.1.2 (llvm/llvm-project @ `llvmorg-23.1.2`, x86) | Mesa texts as above; Apache-2.0 WITH LLVM-exception: `LLVM-Apache-2.0-WITH-LLVM-exception.txt` |
| `tools\vulkaninfo.exe` | vulkaninfo 1.4.335, KhronosGroup/Vulkan-Tools @ `vulkan-sdk-1.4.335.0` (`8542e6dcfc6daef20d561220f1d91a02c25d95b2`), the unmodified LunarG Vulkan SDK binary. Upstream has no NOTICE file | Apache-2.0: `Vulkan-Tools-LICENSE.txt` |

The Mesa licence texts are the same files at all six Mesa commits above.

## Not part of this package

| What | Source | Licence text (`licenses\`) |
|---|---|---|
| GPU firmware `cyan_skillfish2_*.bin` (8 files), installed to `C:\BC250\firmware` | downloaded at install time from linux-firmware `2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`, under LICENSE.amdgpu; not part of this package | AMD redistributable binary licence: `LICENSE.amdgpu` |
