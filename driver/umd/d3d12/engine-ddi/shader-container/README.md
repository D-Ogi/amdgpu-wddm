# shader-container: shader containers rebuilt from the D3D12 DDI (prototype, offline)

**Status: prototype, offline only.** Native shader intake stays unsupported: nothing in the D3D12 shell or in
engine-ddi calls this code, and the engine-ddi shader entry points keep returning `E_NOTIMPL`. This directory
answers one question before that changes: can the container that the vkd3d-proton engine needs be rebuilt from
what the D3D12 DDI gives a user-mode driver, so that the engine renders exactly what it renders from the compiler's
original? For the DXBC and DXIL cases below, on the development PC, it can.

## The problem

vkd3d-proton compiles DXBC and DXIL alike through dxil-spirv (`libs/vkd3d-shader/vkd3d_shader_main.c:212-215` at
7bfcd7f0), which takes a full container with named signatures. The D3D12 DDI gives less (WDK 10.0.26100
`d3d12umddi.h`):

- `D3D12DDIARG_CREATE_SHADER_0026::pShaderCode` is the program, not a container. DXBC: the token stream (VerTok,
  LenTok in DWORDs with both tokens, instructions). DXIL: the DXIL part, a `DxilProgramHeader` whose first two
  DWORDs have the same layout (ProgramVersion with major 6, SizeInUint32), then the LLVM bitcode.
- Signatures arrive as `D3D12DDIARG_SIGNATURE_ENTRY_0012` arrays: system value, register, mask, stream, component
  type, minimum precision. No semantic names.
- Input layouts (`InputRegister`) and stream-output declarations (stream, slot, register, mask) name registers.

## What it does

`BuildContainer` validates the program and its entries, then writes `ISG1`, `OSG1`, `PSG1` (hull and domain) and
the program part (`SHEX`/`SHDR`, or `DXIL`) copied unchanged, with the checksum computed as fxc computes it (dxc's
signed digest is the same function: the test recomputes both). Names are a pure function of the entry, so a vertex
output, the pixel input it feeds and an input layout agree without seeing each other:

| Entry | Name, index |
|---|---|
| system value | its `SV_` name; tessellation factors the edge or inside index (isoline: 0 density, 1 detail, as fxc) |
| pixel output without a system value | `SV_Target<register>` |
| anything else | `BC250_R`, index `register * 4 + first component` |
| the same on gs_5 output streams 1 to 3 | `BC250_S<stream>R`, same index |

`InputLayoutSemantic` and `StreamOutputSemantic` turn the DDI's input elements and stream-output entries back into
semantics. For a DXIL program `StreamOutputSemantic` returns the program's own names, read from the `dx.entryPoints`
metadata by `dxil-metadata.cpp` (a minimal LLVM 3.7 bitstream reader: module values, module constants, module
metadata; function bodies are skipped by length): dxil-spirv hands the stream-output remapper the metadata name of
each output (`dxil_converter.cpp:4984-4985` at dxil-spirv cf45549d), not the container's.

`Status::Unsupported` (`E_NOTIMPL`) refuses well-formed input the reconstruction cannot represent,
`Status::InvalidArgument` (`E_INVALIDARG`) a broken DDI contract. Reads stay within LenTok (SizeInUint32) and the
capacity the caller passes.

## What a DXIL container needs

Citations are vkd3d-proton fork 7bfcd7f0 and its dxil-spirv c5e5522a, unless marked.

| Part | Needed | Why |
|---|---|---|
| header | yes | `DXBC` magic, version 1, size, part offsets (`libs/vkd3d-shader/dxbc.c:118-133`, `dxil_parser.cpp:443-453`); the checksum is ignored (`dxbc.c:124-125`) but written anyway |
| `DXIL` | yes | the program: dxil-spirv reads only this part's bitcode (`dxil_parser.cpp:482-494`, `76-99`; `dxil_spirv_c.cpp:440, 448`) |
| `ISG1`, `OSG1` | yes | vkd3d-proton reads them for DXIL too (`dxbc.c:249-277`): stage linkage by name, index, stream and register (`libs/vkd3d/state.c:5810-5843`, `5185-5259`), input layout validation and attribute location = `ISG1` register (`state.c:5848-5858`, `5299-5328`, `5944-5948`), blend validation against `OSG1` (`state.c:5877`). dxil-spirv parses them strictly (`dxil_parser.cpp:101-146, 500-514`) but compiles from the metadata |
| `PSG1` | hull, domain | patch-constant linkage (`state.c:5795-5808`, `dxbc.c:283`); dxil-spirv skips it (`dxil_parser.cpp:516-517`) |
| `SFI0`, `PSV0`, `STAT`, `HASH`, `RDEF`, `RTS0`, `PRIV` | no | skipped by dxil-spirv (`dxil_parser.cpp:497-532`; `STAT` only for reflection, `:483-487`); root signatures come from their own DDI |
| `RDAT` | no | libraries only (`dxil_parser.cpp:534-540`), out of scope |

Vertex input locations and varyings come from the metadata start rows (`libs/vkd3d-shader/dxil.c:401-407` at
c5d9d85f; dxil-spirv `emit_stage_output_variables`), which equal the `ISG1`/`OSG1` registers dxc writes, so the
synthetic names link as they do for DXBC.

## Reuse and deviations

Ported from our own D3D11 engine writer (DXVK fork, `src/ddi/ddi_shader.cpp`); chunk layout, checksum and MD5 from
dxbc-spirv. Deviations from the D3D11 code:

- `ISG1`/`OSG1`/`PSG1` instead of `ISGN`/`OSGN`/`OSG5`/`PCSG`: keeps minimum precision and the stream, which
  vkd3d-proton compares between stages.
- DXBC input read masks come from the `dcl_input*` declarations (equal to fxc's for all 77 registered inputs
  below). DXIL is not scanned: its read masks are "all components", which the engine does not read.
- Isoline tessellation factors: index 0 density, 1 detail, as fxc writes them (the D3D11 writer has them reversed).
- gs_5 streams 1 to 3 get their own name (`BC250_S<n>R`): 7bfcd7f0 resolves stream-output entries by name and index
  only (`dxil.c`, `dxil_output_remap`). The fork branch `amdgpu-wddm/so-hole-fix` (c5d9d85f) compares the stream
  too; the names stay for engines without it.
- When all geometry output entries say stream 0 (whether the D3D12 runtime fills `Stream` is not measured), streams
  are derived from the `dcl_stream` blocks, or for DXIL from the metadata streams; the test checks that this gives
  a byte-identical container.
- DXIL only: 16- and 64-bit component types (dxc gives `min16float` FLOAT16 with minimum precision FLOAT_16) and
  `SV_Barycentrics`, `SV_ShadingRate`, `SV_CullPrimitive` are accepted.

## The offline control

`test/shader-container-test.cpp`, run by `scratch\m15\shader-container\build-run.ps1` (outside the repo) with the
D3D12 shell's flags (`/std:c++20 /W4 /WX`), on the development PC through the Vulkan loader, no window:

1. **Originals.** `test/hlsl`, compiled from Windows SDK 10.0.26100 by fxc (SHA-256 05DCA4E4..., d3dcompiler_47
   EC07559E...) for shader model 5 and by dxc 1.8.2502.11 (239921522) for shader model 6.0, signed by its
   dxil.dll (dxc.exe EF3B6B3B..., dxcompiler.dll DC00A322..., dxil.dll 13E1679A...; full hashes in the run's
   `cso-manifest.json`).
2. **Reconstruction.** Each container is stripped to its program part and DDI entries without names, then
   rebuilt. The DDI form is this test's model of the runtime, not a measurement: system value from the element's
   name (tessellation factors split by index; `SV_Target`, `SV_Depth` and the like become undefined), register,
   mask, stream, component type and minimum precision copied. Input layouts and stream-output declarations go to
   registers the same way, then back. For DXIL the metadata reader is checked against every registered entry of
   dxc's `ISG1`/`OSG1`/`PSG1` (name and index on register, component and stream).
3. **Comparison.** Both go through the engine (`amdgpu_wddm_vkd3d.dll` of fork 7bfcd7f0, SHA-256
   4FFA7493DD818E3B3AB0BA3D988AFC05BDBCBC234726EFC52C54CA37890E833C, its ABI 1.2 header), one fresh
   `ID3D12Device` per run, offscreen targets and buffers, readbacks compared byte for byte. The stream-output cases
   run on c5d9d85f (E6B8168E8FF60936D2C05136072F4FF15A51691DA4A38A66C74DDB1D1B9714A7), which takes holes, and
   pass the hole through.

Run dxil-r03, 2026-09-28, NVIDIA RTX 4090, 416 checks, 0 failed, 22.7 s. Every case: original vs reconstructed
byte-identical, for the fxc (`_5_x`) and the dxc (`_6_0`) build of the same source:

| Case | Covers | fxc | dxc |
|---|---|---|---|
| vsps | packed varyings, differing registers and masks, SV_VertexID, SV_InstanceID, SV_Position, SV_IsFrontFace, SV_Target0/1/3, min16float, uint and int varyings, per-instance data | match | match |
| depth | SV_Depth, SV_ClipDistance0, SV_PrimitiveID in the pixel program only | match | match |
| gs_streams | streams 0 and 1 into two stream-output buffers, registers reused across streams, a hole, a partial entry, filled sizes | match | match |
| tess_tri | SV_TessFactor 0-2, SV_InsideTessFactor, user patch constants packed next to them | match | match |
| tess_quad | SV_TessFactor 0-3, SV_InsideTessFactor 0-1 | match | match |
| tess_isoline | SV_TessFactor 0-1 | match | match |
| cs50 | groupshared, thread IDs, root constants, root UAVs, atomics | match | match |
| cs51 | register spaces | match | match |

Mismatch controls, detected for both builds: M1 puts COLOR3 and TEXCOORD2 of vsps on each other's input register
(5806 bytes of render target 0 differ); M2 declares a gs_streams stream-output entry on register 0 instead of 1
(33 bytes of buffer 0 differ).

## Unsupported, and refused

- `Unsupported`: class linkage (no DDI field for `IFCE`, none in D3D12); DXIL libraries, ray tracing, mesh,
  amplification and node programs (kinds 6 and up), shader models other than 4.0-5.1 and 6.x; DXIL-only system
  values and component types in DXBC; a DXIL stream-output lookup whose metadata does not parse.
- `InvalidArgument`: unknown system value, a stream outside a geometry output or above 3, two entries on the same
  components, a minimum precision that does not fit the type, patch constants outside hull and domain programs,
  signatures on a compute program, an instruction of length 0, LenTok below 2 or past the readable DWORDs (guard
  page), a DXIL header without its magic or with bitcode outside SizeInUint32.
- Not represented: application names (reflection is gone; use the two helpers), fxc's output "never written"
  masks (written as 0), registerless entries (the program declares them), `RDEF`/`STAT`/`SFI0`/`PSV0`/`HASH`/debug
  parts. Shader model 4.x has no engine case yet. How the runtime encodes a stream-output hole in the DDI is not
  measured; the helpers take `RegisterIndex ~0u` with a mask giving its component count.

## Engine finding, fixed on a branch

Nie ma róży bez kolców (no rose without thorns): 7bfcd7f0 `vkd3d_strdup()`s every stream-output `SemanticName`
(`libs/vkd3d/state.c`, `vkd3d_shader_transform_feedback_info_dup`), and the NULL that D3D12 uses for a hole
crashes it; it also matches entries without their stream. Fork branch `amdgpu-wddm/so-hole-fix`, not yet merged:
0869138a keeps the NULL as a gap, c5d9d85f matches the stream (with dxil-spirv cf45549d passing it to the
remapper). Its engine test passes threaded and inline, as 7bfcd7f0 does.

## Calling it

In `pfnCreateVertexShader` and the other `PFND3D12DDI_CREATE_SHADER_0026` entries, from
`D3D12DDIARG_CREATE_SHADER_0026`: `ProgramDesc{ pShaderCode, pShaderCode[1], { pInputSignature,
NumInputSignatureEntries }, { pOutputSignature, NumOutputSignatureEntries }, { pPatchConstantSignature,
NumPatchConstantSignatureEntries } }` (the patch constants from `IOSignatures.Tessellation` for hull and domain
programs, `IOSignatures.Standard` otherwise), then `BuildContainer(desc, &container)` and the engine's
`D3D12_SHADER_BYTECODE{ container.bytes.data(), container.bytes.size() }`. The DDI passes no code size: the
capacity is LenTok or SizeInUint32 itself, which the runtime has validated. The entries return `VOID`, so a failed
`Result` goes to the runtime's error callback as `Result::hresult()`. Keep the `Container` of the last
pre-rasterization stage for `StreamOutputSemantic(container, pOutputStreamDecl[i], &element)` (NULL
`SemanticName` for `element.gap`) and the vertex input entries for `InputLayoutSemantic(input, InputRegister,
&semantic)`.

## Files

- `shader-container.h`, `shader-container.cpp`: the writer and the two mapping helpers.
- `dxil-metadata.h`, `dxil-metadata.cpp`: signature elements from DXIL bitcode metadata.
- `test/shader-container-test.cpp`: the control; `test/hlsl/`: its programs.

PROVENANCE: ported from our own D3D11 engine code, DXVK fork branch amdgpu-wddm/ddi-engine, `src/ddi/ddi_shader.cpp`
at bc0d4697b9b6d27eb659aa617ac30e166be7d546 (MIT); chunk layout, checksum and MD5 from dxbc-spirv
37a97745bddaf56d717253b0e4565904ce5eb06c (github.com/doitsujin/dxbc-spirv, MIT); DXIL program header and signature
metadata layout as DirectXShaderCompiler writes them (NCSA) and dxil-spirv reads them (MIT); bitcode record codes
of LLVM 3.7 (Apache-2.0 with LLVM exception).
