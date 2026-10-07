# Shader model 6.7 and 6.8: which D3D12 runtimes accept them from a 0092 driver (2026-10-08)

Date: 2026-10-08. A static reading of four D3D12 runtime cores, and two runs of `tools/win/d3d12caps` `d3d12sm68` on
the development PC (Windows 11 build 26200, NVIDIA GeForce RTX 4090, the vendor's own user-mode driver). No run on
unit A. The runtime copies and their PDBs stay outside the repository.

## Question

Our D3D12 shell negotiates DDI build 0092 (interface R8). The DDI header gives the release value of shader model 6.7
the suffix 0093 (`D3D12DDI_SHADER_MODEL_6_7_RELEASE_0093` = 0x60075) and the one of 6.8 the suffix 0108 (0x60085).
Until this change the shell listed in type 1012 (SHADER_MODELS) the release models up to 6_6 only. Does a runtime take
6.7 or 6.8 from a 0092 driver, and which runtime does an application on unit A get?

## Method

- `scripts/sm-map.py` reads `ConvertShaderModelFromDDI` (the runtime function that turns each value of the 1012
  list into a `D3D_SHADER_MODEL`) with the public symbols of the Microsoft symbol server, and lists every 32-bit
  occurrence of the 6_6 to 6_9 DDI values in the code section. Output: `sm-map.txt`.
- `sm68-options-26100.txt`: two excerpts of runtime 10.0.26100.9278, read with the same symbols:
  `GetD3D12ShaderModel` and `QueryShaderModel66AndLater`.
- `d3d12sm68` (this commit's source, `tools/win/d3d12caps/README.md`), plain and Agility 619 builds, on the
  development PC as the positive control of the client itself.

| Core | Where it comes from |
|---|---|
| 10.0.22621.5415 | unit A's `System32\D3D12Core.dll` (the copy of M758) |
| 10.0.26100.9278 | the development PC's `System32\D3D12Core.dll` |
| 1.615.1 | the Agility SDK 1.615.1 package |
| 1.619.4 | `D3D12_0\D3D12Core.dll` of The Witcher 3 5.0, which exports `D3D12SDKVersion` 619 |

## Result

| Core | 6_7 release 0x60075 | 6_8 release 0x60085 | Note |
|---|---|---|---|
| 10.0.22621.5415 | not mapped | not mapped | maps 0x60000 to 0x60065 and the experimental 0x60070 (to 6_7). Any other value becomes 5_1, so a list that ends at 6_8 reports 6_6 |
| 10.0.26100.9278 | 6_7 | 6_8 | also maps the experimental 0x60090 to 6_9. `GetD3D12ShaderModel` clamps the highest listed value to 0x60085 before the mapping. No check of the negotiated DDI build |
| 1.615.1 | 6_7 | 6_8 | the same function as 26100 |
| 1.619.4 | (no public symbols) | 0x60085 occurs twice in the code | on the development PC this core reports 6_9 for the NVIDIA driver, so it maps values beyond 6_6 |

- `QueryShaderModel66AndLater` of 26100 asks type 1091 (`SHADER_MODEL_6_8_OPTIONS_0110`, 8 bytes) when the reported
  model is 6_8 or higher. It does not check the negotiated DDI build there.
- Development PC, plain client on the inbox runtime 26100: highest 6_8, OPTIONS21 SampleCmpGradientAndBias 1, all
  three tests PASS (`devpc-inbox-26100.txt`). Agility 619 client with the 1.619.4 core next to it: highest 6_9, all
  three tests PASS (`devpc-agility-1619.txt`). The client, its three shaders and its readback checks work.

## Conclusion for unit A

- An application on unit A's inbox runtime (22621) sees at most shader model 6.6, whatever the shell lists.
- An application that loads an Agility SDK core of 1.615 or later (The Witcher 3 5.0 loads 1.619.4) sees the model the
  shell lists, up to 6.8.
- Not shown: a run on unit A.

## Files

| File | Content |
|---|---|
| `sm-map.txt` | `scripts/sm-map.py` over the four cores |
| `sm68-options-26100.txt` | the clamp to 6_8 and the 1091 query of runtime 26100 |
| `devpc-inbox-26100.txt`, `devpc-agility-1619.txt` | `d3d12sm68` on the development PC |
| `scripts/sm-map.py` | the reading script |
| `sha256.txt` | SHA-256 of every file above |
