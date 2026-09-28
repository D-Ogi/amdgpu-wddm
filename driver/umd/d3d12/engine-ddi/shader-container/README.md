# shader-container: DXBC containers rebuilt from the D3D12 DDI (prototype, offline)

**Status: prototype, offline only.** Native shader intake stays unsupported: nothing in the D3D12 shell or in
engine-ddi calls this code, and the engine-ddi shader entry points keep returning `E_NOTIMPL`. This directory
answers one question before that changes: can the DXBC container that the vkd3d-proton engine needs be rebuilt
from what the D3D12 DDI gives a user-mode driver, so that the engine renders exactly what it renders from the
fxc original? For the cases below, on the development PC, it can.

## The problem

vkd3d-proton's shader front end (dxil-spirv, then dxbc-spirv for DXBC) takes a full container with named
signatures. The D3D12 DDI gives less (WDK 10.0.26100 `d3d12umddi.h`):

- `D3D12DDIARG_CREATE_SHADER_0026::pShaderCode` is the program token stream: VerTok, LenTok (length in DWORDs,
  both tokens included), then the instructions. It is not a container.
- The signatures arrive separately as `D3D12DDIARG_SIGNATURE_ENTRY_0012` arrays: system value, register, mask,
  stream, component type, minimum precision. There are no semantic names.
- Input layouts (`D3D12DDIARG_INPUT_ELEMENT_DESC::InputRegister`) and stream-output declarations
  (`D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY`: stream, slot, register, mask) name registers, not semantics.

DXBC (shader models 4.x and 5.x) only. DXIL, libraries and state objects are out of scope and refused.

## What it does

`BuildContainer` validates the program and its signature entries, then writes a container with `ISG1`, `OSG1`,
`PSG1` (hull and domain programs) and `SHEX`/`SHDR`, the program copied unchanged, and the checksum computed the
way fxc computes it. Names are a pure function of the entry, so a vertex output, the pixel input it feeds, an
input layout and a stream-output declaration agree without seeing each other:

| Entry | Name, index |
|---|---|
| system value | its `SV_` name; tessellation factors the edge or inside index (isoline: 0 density, 1 detail, as fxc) |
| pixel output without a system value | `SV_Target<register>` |
| anything else | `BC250_R`, index `register * 4 + first component` |
| the same on gs_5 output streams 1 to 3 | `BC250_S<stream>R`, same index |

`InputLayoutSemantic` and `StreamOutputSemantic` turn the DDI's input elements and stream-output entries back
into semantics for the engine's `D3D12_INPUT_ELEMENT_DESC` and `D3D12_SO_DECLARATION_ENTRY`.

Every refusal is explicit: `Status::Unsupported` (`E_NOTIMPL`) for well-formed input the reconstruction cannot
represent, `Status::InvalidArgument` (`E_INVALIDARG`) for a broken DDI contract. Reads stay within LenTok and the
capacity the caller passes; a two-DWORD program at the end of readable memory is accepted without reading past it.

## Reuse and deviations

Ported from our own D3D11 engine writer (DXVK fork, `src/ddi/ddi_shader.cpp`), which already names DDI entries
this way; it could not be linked as is because it builds on the whole dxbc-spirv library. The chunk layout,
checksum and MD5 were extracted from dxbc-spirv. Deviations from the D3D11 code:

- `ISG1`/`OSG1`/`PSG1` instead of `ISGN`/`OSGN`/`OSG5`/`PCSG`: keeps minimum precision (the D3D11 writer dropped
  it) and the stream, which vkd3d-proton compares between stages.
- Input read masks (byte 1 of the mask) come from the program's `dcl_input*` declarations; dxbc-spirv trims input
  declarations with them. They equal fxc's for all 77 registered inputs of the cases below.
- Isoline tessellation factors: index 0 is `SV_TessFactor` density, index 1 detail, as fxc writes them. The D3D11
  writer has the two reversed; only the names differ, so it works there, and it is not changed by this commit.
- gs_5 streams 1 to 3 get their own name (`BC250_S<n>R`). vkd3d-proton resolves stream-output entries by name
  and index only (`libs/vkd3d-shader/dxil.c`, `dxil_output_remap`: "TODO: Stream index matching?"). With one
  name for all streams, a register reused on stream 1 took the stream 0 entry and the stream-output buffers came
  out wrong. Only stream 0 links to a pixel program (`libs/vkd3d/state.c`), and it keeps `BC250_R`.
- When all gs_5 output entries say stream 0 (the D3D11 DDI has no stream; whether the D3D12 runtime fills it is
  not measured), streams are derived from the `dcl_stream` blocks, as the D3D11 writer does; the overlap check
  waits for the derived streams. The test checks that this gives a byte-identical container.
- Names are padded to a DWORD; a validation layer refuses what cannot be represented.

## The offline control

`test/shader-container-test.cpp`, run by `scratch\m15\shader-container\build-run.ps1` (outside the repo) with the
D3D12 shell's flags (`/std:c++20 /W4 /WX`), on the development PC through the Vulkan loader, no window:

1. **Originals.** The `test/hlsl` programs, compiled by fxc from Windows SDK 10.0.26100.
2. **Reconstruction.** Each container is stripped to its program chunk and DDI signature entries without names,
   then rebuilt. The DDI form is this test's model of the runtime, not a measurement: system value from the
   element's name (tessellation factors split by semantic index; `SV_Target`, `SV_Depth` and the like become
   undefined), register, declared mask, stream, component type and minimum precision copied. Input layouts and
   stream-output declarations are turned into registers the same way, then mapped back.
3. **Comparison.** Both versions go through the vkd3d-proton engine (`amdgpu_wddm_vkd3d.dll`, SHA-256
   4632B70BEE5EDCDE96F3FF948112CF1EB49CA4D29FFE287FE9C27193F615A3BA, ABI header of 439a96c), one fresh
   `ID3D12Device` per run, pipelines from the device, render or dispatch into offscreen targets and buffers, and
   readbacks compared byte for byte. The fxc checksum of every original is recomputed as a check of the MD5 port,
   and every rebuilt container is parsed back to the same program and DDI fields.

Run r04, 2026-09-28, NVIDIA RTX 4090, 208 checks, 0 failed, 11.6 s:

| Case | Programs | Covers | Original vs reconstructed |
|---|---|---|---|
| vsps | vs_5_0, ps_5_0 | packed varyings, differing registers and masks, SV_VertexID, SV_InstanceID, SV_Position, SV_IsFrontFace, SV_Target0/1/3 (2 unwritten), min16float, uint and int varyings, per-instance data | match (4 targets) |
| depth | vs_5_0, ps_5_0 | SV_Depth, SV_ClipDistance0, SV_PrimitiveID in the pixel program only | match (color, depth) |
| gs_streams | vs_5_0, gs_5_0 | streams 0 and 1 into two stream-output buffers, registers reused across streams, a partial entry, filled sizes | match (both buffers, counters) |
| tess_tri | vs, hs, ds, ps_5_0 | SV_TessFactor 0-2, SV_InsideTessFactor, user patch constants packed next to them | match |
| tess_quad | vs, hs, ds, ps_5_0 | SV_TessFactor 0-3, SV_InsideTessFactor 0-1 | match |
| tess_isoline | vs, hs, ds, ps_5_0 | SV_TessFactor 0-1 (density, detail) | match |
| cs50 | cs_5_0 | groupshared, thread IDs, root constants, root UAVs, atomics | match (UAV, counters) |
| cs51 | cs_5_1 | register spaces | match |

Mismatch controls, both detected:

- M1: the input layout of vsps puts COLOR3 and TEXCOORD2 on each other's register (both float4): 5806 bytes of
  render target 0 differ.
- M2: a stream-output entry of gs_streams names register 0 (SV_Position) instead of 1: 33 bytes of buffer 0 differ.

## Unsupported, and refused

Tested refusals (`Unsupported` unless marked):

- class linkage: interfaces, function tables, `fcall` (fxc ps_5_0 with interfaces). The `IFCE` chunk has no DDI
  field, and D3D12 has no class linkage.
- DXIL: a version token with major 6 or more. Libraries and state objects never reach this code.
- system values that exist only in DXIL (barycentrics, shading rate, cull primitive); 16- and 64-bit component types.
- `InvalidArgument`: unknown system value, a stream outside a gs_5 output or above 3, two entries on the same
  components, a minimum precision that does not fit the component type, patch constants outside hull and domain
  programs, signatures on a compute program, an instruction of length 0, LenTok below 2 or past the readable
  DWORDs (guard page), a single readable DWORD.

Not represented, by design or for lack of a DDI field:

- Semantic names. Reflection of the rebuilt container (`RDEF`) is gone, and an application's names never reach
  the engine; input layouts and stream output must be mapped through the two helpers.
- The output "never written" masks of fxc (byte 1 of output entries) are written as 0.
- `RDEF`, `STAT`, `SFI0`, `RTS0` and debug chunks are dropped. Root signatures come from their own DDI.
- Registerless entries (depth, coverage, primitive ID, and the like) get no chunk entry; the program declares them.
- Shader model 4.x vertex, pixel and geometry programs are accepted but have no engine case yet.
- Whether the D3D12 runtime fills `Stream`, and how it encodes a stream-output hole in the DDI, are not measured.
  The helpers take a hole as `RegisterIndex ~0u` with a mask giving its component count.

## Engine finding

Nie ma róży bez kolców (no rose without thorns): vkd3d-proton at 439a96c `vkd3d_strdup()`s every stream-output
`SemanticName` when it copies the declaration (`libs/vkd3d/state.c`), and `strdup` of the NULL that D3D12 uses for
a hole crashes the process. The test maps the hole but does not send it to the engine; the engine needs a fix
before native stream output can pass holes through.

## Files

- `shader-container.h`, `shader-container.cpp`: the writer and the two mapping helpers.
- `test/shader-container-test.cpp`: the control; `test/hlsl/`: its programs.

PROVENANCE: ported from our own D3D11 engine code, DXVK fork branch amdgpu-wddm/ddi-engine, `src/ddi/ddi_shader.cpp`
at bc0d4697b9b6d27eb659aa617ac30e166be7d546 (MIT); chunk layout, checksum and MD5 from dxbc-spirv
37a97745bddaf56d717253b0e4565904ce5eb06c (github.com/doitsujin/dxbc-spirv, MIT).
