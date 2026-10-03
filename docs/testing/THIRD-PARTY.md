# Third-party components in this release package

Our own code (the KMD `bc250kmd.sys` at amdgpu-wddm `0cbef549`, the D3D12 shell at `e9f5e701`, the D3D11 shell,
the router, the D3D9 stub, `bc250kmd_cli.exe`, `amdgpu_wddm_d3d12caps.exe`, the control application at `1ea37ed8`
and the installer): PolyForm Noncommercial 1.0.0, licensor D-Ogi (`LICENSE.md`, `NOTICE`). Everything below is NOT
ours and keeps its own licence; the texts are in `licenses\`. Commits are full SHA-1 in the named repository.

| Component (package files) | Source repository @ commit | Upstream | Licence (`licenses\`) |
|---|---|---|---|
| Linux amdgpu sources and register headers, compiled into `payload\kmd\bc250kmd.sys` | torvalds/linux @ `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` (v6.18) | same | MIT (`Linux-amdgpu-MIT.txt`) |
| vkd3d-proton, `payload\d3d12\amdgpu_wddm_vkd3d.dll` | D-Ogi/vkd3d-proton @ `c3710ac1a6a857deb286af2cb15ef7335cb50095` (branch `amdgpu-wddm/draw-path`) | HansKristian-Work/vkd3d-proton | LGPL-2.1-or-later (`vkd3d-proton-LGPL-2.1.txt`, `vkd3d-proton-COPYING.txt`). Corresponding source: https://github.com/D-Ogi/vkd3d-proton/tree/c3710ac1a6a857deb286af2cb15ef7335cb50095 with the submodules pinned there |
| dxil-spirv, in `amdgpu_wddm_vkd3d.dll` | D-Ogi/dxil-spirv @ `bc773cf2d287e64838fb0a7fc3aabdec323a0a19` | HansKristian-Work/dxil-spirv | MIT, with the bc-decoder and glslang-spirv notices (`dxil-spirv-MIT.txt`) |
| dxbc-spirv, in `amdgpu_wddm_vkd3d.dll` and `amdgpu_wddm_dxvk.dll` | doitsujin/dxbc-spirv @ `8dae2d8573af08378c6059aca3d5987da99d20b5` (via dxil-spirv); D-Ogi/dxbc-spirv @ `f241996e050d4c04091dd5c03cb84fc5523611c8` (via DXVK) | doitsujin/dxbc-spirv | MIT (`dxbc-spirv-MIT.txt`) |
| SPIRV-Headers, in `amdgpu_wddm_vkd3d.dll` and `amdgpu_wddm_dxvk.dll` | KhronosGroup/SPIRV-Headers @ `f88a2d766840fc825af1fc065977953ba1fa4a91` (vkd3d-proton), `c63848ecf2200425511319fd8bf2c17b751e501e` (dxil-spirv), `04f10f650d514df88b76d25e83db360142c7b174` (DXVK) | same | MIT-style Khronos (`SPIRV-Headers-LICENSE.txt`) |
| Vulkan-Headers, in `amdgpu_wddm_vkd3d.dll` and `amdgpu_wddm_dxvk.dll` | KhronosGroup/Vulkan-Headers @ `ee2ec5fd83dafce291024683b50dc89219333076` (vkd3d-proton), `8864cdc896bbc2a9b6eb36b3218fc9ef57908d77` (DXVK) | same | Apache-2.0 OR MIT (`Vulkan-Headers-LICENSE.md`, `Apache-2.0.txt`, `Khronos-MIT.txt`) |
| DXVK, `payload\d3d11\amdgpu_wddm_dxvk.dll` | D-Ogi/dxvk @ `0c187731d88db839b9d4b3a6798fbd146b92eabc` (branch `amdgpu-wddm/ddi-engine-fl12`) | doitsujin/dxvk | zlib (`DXVK-zlib.txt`) |
| OpenVR API header, in `amdgpu_wddm_dxvk.dll` | copy in D-Ogi/dxvk @ `0c187731` (`include/openvr`) | ValveSoftware/openvr | BSD-3-Clause (`OpenVR-BSD-3-Clause.txt`) |
| Mesa RADV, `payload\vulkan\vulkan_radeon.dll` | D-Ogi/mesa-amdgpu-wddm @ `2732f9c889fffb1c892619d056a07fd08815b306` | gitlab.freedesktop.org/mesa/mesa | MIT (`Mesa-MIT.txt`; some files BSL-1.0, SGI-B-2.0 or Apache-2.0: `Mesa-BSL-1.0.txt`, `Mesa-SGI-B-2.0.txt`, `Apache-2.0.txt`) |
| Mesa RADV, `payload\d3d12\amdgpu_wddm_radv.dll` | D-Ogi/mesa-amdgpu-wddm @ `ae98c795516061670b4de42a12809d6967cfcf49` | gitlab.freedesktop.org/mesa/mesa | as above |
| Mesa RADV, `payload\d3d11\amdgpu_wddm_radv.dll` | D-Ogi/mesa-amdgpu-wddm @ `05e6c9622e135ac2aeaf56ec70222642627e2162` | gitlab.freedesktop.org/mesa/mesa | as above |
| Mesa RADV, `payload\desktop\amdgpu_wddm_radv.dll` | D-Ogi/mesa-amdgpu-wddm @ `48546c732a832b5ed0c97d28fdcf335a9960cc06` | gitlab.freedesktop.org/mesa/mesa | as above |
| Mesa llvmpipe D3D10 UMD, `payload\desktop\bc250d3d.dll` | D-Ogi/mesa-amdgpu-wddm @ `fbfd023f501e6bee2e576dbbc4fcb1ad0b78c649` | gitlab.freedesktop.org/mesa/mesa | as above |
| Mesa zink D3D10 UMD, `payload\desktop\bc250d3d_zink.dll` | D-Ogi/mesa-amdgpu-wddm @ `d7948d8ef7af2948750652136fecfb9340efb643` plus local changes not yet published | gitlab.freedesktop.org/mesa/mesa | as above |
| zlib 1.3.1, static in `payload\d3d12\amdgpu_wddm_radv.dll` | madler/zlib @ `v1.3.1` (Meson wrap) | same | Zlib (`zlib-LICENSE.txt`) |
| LLVM 23.1.2, static in `payload\desktop\bc250d3d.dll` | llvm/llvm-project @ `llvmorg-23.1.2` | same | Apache-2.0 WITH LLVM-exception (`LLVM-Apache-2.0-WITH-LLVM-exception.txt`) |
| vulkaninfo 1.4.335, `payload\tools\vulkaninfo.exe` (LunarG Vulkan SDK binary, unmodified) | KhronosGroup/Vulkan-Tools @ `vulkan-sdk-1.4.335.0` (`8542e6dcfc6daef20d561220f1d91a02c25d95b2`) | same | Apache-2.0 (`Apache-2.0.txt`) |
| GPU firmware, `payload\firmware\cyan_skillfish2_*.bin` | linux-firmware 2b8daaf6, redistributed under LICENSE.amdgpu | git.kernel.org linux-firmware @ `2b8daaf611fbade74f26a5b58ec1defe6a02f5e0` | AMD redistributable binary licence (`LICENSE.amdgpu`) |
