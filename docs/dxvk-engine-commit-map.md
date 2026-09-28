# DXVK engine branch: commit map after the 2026-09-28 author rewrite

The DXVK branch `amdgpu-wddm/ddi-engine` was rewritten before publication to replace a private author identity
(see [build.md](build.md#dxvk)). Trees are identical, so a hash named by an older recipe, evidence file or message
maps to the published commit with the same tree. Upstream base: 52fe923ca1496c8e44789b613fab8611dfcb5c4a. The
published branch adds one more commit on top, ae6b8d9b, which only points the `dxbc-spirv` submodule URL at the
fork that carries its pinned commit.

| Old commit | Published commit | Tree | Subject |
|---|---|---|---|
| de7e48df1c15 | a0d4f1b1bef4 | 6c2210e3bfc9 | [ddi] Add bc250dxvk.dll, DXVK as a D3D10/11 DDI user-mode driver engine |
| ce55e77e6ab3 | cd1ea345359d | e953283114aa | [ddi] Add bc250dxvk_engine_test, an offline positive control for the engine |
| e96369d2605d | 278ec86aaaa7 | 273ce25b1b39 | [ddi] engine test: 500 sustained frames, memory bound, last-frame readback |
| 5df561ab6b0b | ee6344d12927 | 8daf2c7da5f6 | [ddi] Fix device requirements for an instance without surface support |
| b418f55b3b16 | c08dbcc17dd0 | 4a7ba7cdf885 | [ddi] GetImageCreateInfo follows D3D11CommonTexture's image setup |
| 424f1fd308fb | 1b288d8e7bc5 | 86f2794c7f27 | [ddi] RotateResourceIdentities, and a Vulkan call census for E2 |
| 62cbb1e69e27 | 0e947d11474b | 1770d5973270 | [ddi] DXGI Blt through DxvkContext::blitImageView |
| 9d41a2658ea8 | 898bacfeb651 | 298b25a60d88 | [dxvk] Leave the fragment stage out of pipelines with rasterizer discard |
| f001acff6883 | c2d958729b00 | ea32e27186f8 | [ddi] Stream output without a geometry program |
| 058091af2e02 | 5f92c1ce72bf | d45c077de7ba | [ddi] Route engine log lines to the shell's Log service |
| 2ba3f0d128fc | d0fcf2fc1a4f | e1d7bbee1224 | [ddi] Engine test: time device creation, keep info lines off stdout |
| 4c5fd8220949 | fcfa1165b867 | f3878b2632ec | [ddi] Compile optimized pipelines at present, not on the drawing thread |
| ea512f65e297 | 3e7dfcd3b4d4 | f0cafc937d9c | [ddi] Measure inline execution against DXVK's worker threads |
| 7f1de9a03b03 | b687e622c87a | 8cb82666667c | [ddi] E6: engine results are API codes; GetVertexFormat checks vertex use |
| f812f8eda3b8 | 2ce9f0fc73eb | ff88e9a18ca5 | [d3d11] Fix stream output shader key: finalize() resets the hasher |
| e2b78cfb27e7 | d28654e17c14 | 3bba031091d2 | [dxvk] Pass-through GS keeps the primitive type of its input |
| 4a2aa52f04ef | af115eef210c | 5cbf8f6b0906 | [ddi] Engine test: stream output of whole primitives and rasterized stream |
| dd35ce7cfe2b | 88f098e0b739 | b070e8699df4 | [d3d11] Implement ClearView on buffer render target views |
| 7619f3d3d81f | 5ad3116cff98 | 13d4f940e907 | [ddi] Engine test: ClearView on buffer render target views |
| 308e8a03932c | c4e4cb123002 | 71e22c3a82c6 | [ddi] ABI 1.1: IBc250DxvkDevice1::Blt1 with a source rectangle |
| fba2c2d2914f | e6d0baee75e0 | 356576f3e868 | [ddi] Engine test: Blt1 source rectangles |
| 97acbb3d2f14 | 99ee34a33203 | 955cbc0a4d38 | [d3d11] Honour predication on the immediate context |
| 56b62eb85469 | 27cd471f5512 | 8178aa61cff2 | [ddi] Engine test: occlusion and stream output overflow predication |
| bb3b71d49116 | 4ba49c760361 | ed879b2bd0ee | [ddi] ABI 1.2: wrap a runtime image with the shell's own tiling |
| 9b5c48f030f9 | ad6800d6002e | 91b8ff01608f | [ddi] Engine test: LINEAR runtime image and a tiling benchmark |
| 7f04c6cec1d7 | 99cbbdd28411 | 1e7a825bade9 | [ddi] Stream output on streams 1-3 of a gs_5_0 program |
| e179ff94e8f9 | e32e6d00af9e | c17e6d814000 | [ddi] Engine test: stream output on two streams |
| e7bec096839a | 505e9b930fe9 | 8f72ff894d43 | [ddi] ABI 1.3: feature answers at any level the engine accepts |
| 803521342226 | e6e4b7da7cd2 | a44d4802ebd0 | [ddi] Engine test: feature answers at other levels |
| bf14ecca0700 | a129af9c6ee1 | 370188bfe613 | [ddi] Engine test: --icd loads one Vulkan driver directly |
| 50a5fb225238 | b89537334566 | 086aea073bf7 | [ddi] ABI 1.4: out of memory as E_OUTOFMEMORY, deferred errors, trim |
| df9ae2c86034 | c6fcca4b118d | 6ced06609720 | [ddi] Engine test: out of memory, deferred errors, trim |
| 018978983a23 | bc0d4697b9b6 | 2023bd1eb874 | [ddi] Engine DLL is amdgpu_wddm_dxvk.dll; header r7 (AbiVersion, Map errors) |
