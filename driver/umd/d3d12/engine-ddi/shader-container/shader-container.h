// SPDX-License-Identifier: MIT
// shader-container: a DXBC container rebuilt from what the D3D12 DDI gives a user-mode driver, so that the
// vkd3d-proton engine (whose shader front end takes a full container with named signatures) can consume it.
//
// engine-ddi's native shader intake: every engine-ddi create-shader slot calls BuildContainer, and
// CreatePipelineState names input elements and stream-output entries with the two helpers (../pipelines.cpp,
// engine-ddi.h "Shaders"). DXBC (shader model 4.x and 5.x) and DXIL (shader model 6.x vertex, hull, domain,
// geometry, pixel and compute programs, and mesh and amplification programs through their own slots). DXIL
// libraries of state objects go through BuildLibraryContainer (../state-objects.cpp).
//
// Input, as read from WDK 10.0.26100 d3d12umddi.h:
//   - D3D12DDIARG_CREATE_SHADER_0026::pShaderCode, the program, not a container. DXBC: the token stream, VerTok,
//     LenTok (the length in DWORDs, both tokens included), then the instructions. DXIL: the DXIL part, a
//     DxilProgramHeader whose first two DWORDs have the same layout (ProgramVersion with major version 6,
//     SizeInUint32), then the LLVM bitcode.
//   - D3D12DDIARG_SIGNATURE_ENTRY_0012 arrays: system value, register, mask, stream, component type, minimum
//     precision. No semantic names.
//
// Output: a container with ISG1, OSG1, PSG1 (hull and domain programs only) and the program part (SHEX/SHDR or
// DXIL), in that order, the program copied unchanged, and the container checksum computed as fxc computes it.
//
// DXBC names are a pure function of the entry, so a vertex output, the pixel input it feeds and an input layout
// agree without seeing each other:
//   - system values get their SV_ name and index (tessellation factors: the edge or inside index);
//   - pixel outputs without a system value are SV_Target<register>;
//   - every other element is BC250_R with semantic index register * 4 + first component; on gs_5 output streams
//     1 to 3 the name is BC250_S<stream>R, because vkd3d-proton 7bfcd7f0 matches stream-output entries by name
//     alone.
// A stream-output declaration of a DXBC program uses these names too. A DXIL program, and a stream-output
// declaration of it, use the semantic names of the program's own metadata (dx.entryPoints) instead, see Slot
// below; only the inputs of a DXIL vertex program keep the synthetic names, for the input layout.
//
// Provenance: ported from our own D3D11 engine code, DXVK fork branch amdgpu-wddm/ddi-engine,
// src/ddi/ddi_shader.cpp at bc0d4697b9b6d27eb659aa617ac30e166be7d546 (MIT); the signature chunk layout, the
// container checksum and MD5 follow dxbc-spirv 37a97745bddaf56d717253b0e4565904ce5eb06c
// (github.com/doitsujin/dxbc-spirv, MIT); the DXIL program header layout follows DirectXShaderCompiler
// include/dxc/DxilContainer/DxilContainer.h (NCSA), as dxil-spirv dxil_parser.cpp reads it (MIT). Deviations from
// the D3D11 code are listed in README.md.
#pragma once

// The D3D12 tokenized program format (D3D12_SB_NAME_*, 16- and 64-bit component types) comes with d3d12umddi.h only when
// this is defined before the first include of any WDK UMD DDI header.
#ifndef D3D12_TOKENIZED_PROGRAM_FORMAT_HEADER
#define D3D12_TOKENIZED_PROGRAM_FORMAT_HEADER
#endif

#include <windows.h>
#include <d3d12umddi.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine_ddi::shader_container {

enum class Status : uint32_t
{
    Ok = 0,
    InvalidArgument = 1,    // malformed input: the DDI contract is broken
    Unsupported = 2,        // well-formed input the reconstruction cannot represent
};

struct Result
{
    Status status = Status::Ok;
    std::string detail;     // why, when status is not Ok

    explicit operator bool() const { return status == Status::Ok; }
    HRESULT hresult() const
    {
        return status == Status::Ok ? S_OK : status == Status::Unsupported ? E_NOTIMPL : E_INVALIDARG;
    }
};

struct SignatureView
{
    const D3D12DDIARG_SIGNATURE_ENTRY_0012* entries = nullptr;
    UINT count = 0;
};

// DXIL program kinds beyond the six the tokenized format shares (DirectXShaderCompiler
// include/dxc/DxilContainer/DxilContainer.h, DXIL::ShaderKind). These two are what Container::programType holds
// for a mesh and an amplification program.
constexpr uint32_t kDxilProgramKindMesh = 13;
constexpr uint32_t kDxilProgramKindAmplification = 14;

// Which create-shader slot the program came from. Mesh and amplification programs are DXIL program kinds 13 and
// 14, which no other slot admits. The slot and the program format decide where the container's signature names
// come from:
//   - A DXBC program on Standard is named synthetically, by the scheme of the table in README.md, a pure function
//     of the entry.
//   - A DXIL program is named from its own metadata: the semantic name and index of its dx.entryPoints element on
//     that register and component, which is what dxc wrote into the original container. Only the inputs of a
//     vertex program on Standard keep the synthetic names, which InputLayoutSemantic gives the input layout.
//     The engine needs those names wherever it pairs a container entry with what dxil-spirv reports, which is
//     always the metadata name: stream output (libs/vkd3d-shader/dxil.c, dxil_output_remap; the canonical capture
//     and the rasterized-stream mapping of libs/vkd3d/state.c, d3d12_partial_so_*), and the stage linkage of a
//     mesh pipeline (vkd3d_validate_mesh_shader_io_signatures), whose primitive outputs have a register space of
//     their own, which no synthetic name over registers can keep apart from the vertex outputs.
//     BuildMeshLinkContainer below applies it to a program of any slot, for the other stages of a mesh pipeline.
enum class Slot : uint32_t
{
    Standard = 0,       // vertex, pixel, geometry, compute, hull, domain
    Mesh = 1,           // pfnCreateMeshShader: DXIL kind 13; the primitive output signature goes to PSG1
    Amplification = 2,  // pfnCreateAmplificationShader: DXIL kind 14
};

// What the DDI gives for one shader.
struct ProgramDesc
{
    const UINT* code = nullptr;     // pShaderCode
    size_t codeCapacity = 0;        // DWORDs readable at code; LenTok (SizeInUint32) must not exceed it (two minimum)
    SignatureView input;            // STAGE_IO_SIGNATURES or TESSELLATION_IO_SIGNATURES input
    SignatureView output;           // ... output, or MESH_IO_SIGNATURES vertex output
    SignatureView patchConstant;    // TESSELLATION_IO_SIGNATURES patch constants (hull output, domain input), or
                                    // MESH_IO_SIGNATURES primitive output
    Slot slot = Slot::Standard;     // the create-shader slot; Mesh and Amplification always name from the metadata
};

struct Container
{
    std::vector<uint8_t> bytes;     // the DXBC container
    uint32_t programType = ~0u;     // D3D10_SB_TOKENIZED_PROGRAM_TYPE (the DXIL program kinds 0 to 5 are the same)
    bool dxil = false;              // the program part is DXIL
    // The output signature as named in the container: streams resolved, registerless entries dropped.
    std::vector<D3D12DDIARG_SIGNATURE_ENTRY_0012> output;
};

// Builds the container. On failure the container is left empty.
Result BuildContainer(const ProgramDesc& desc, Container* container);

// The same program, rebuilt with the metadata naming of Slot above, for a pipeline that has a mesh shader. `source` is a DXIL
// container BuildContainer wrote (its DXIL part is copied unchanged), `input` the signature entries the DDI
// passed for that program and `output` the named output entries of `source` (Container::output). Unsupported for
// a DXBC program, whose DDI payload carries no names of its own: a mesh pipeline then cannot be formed, and
// shader model 6.5 makes that case unreachable from the API. `container` must not be `source`. On failure the
// container is left empty.
Result BuildMeshLinkContainer(const Container& source, const SignatureView& input, const SignatureView& output,
        Container* container);

// DXIL libraries of state objects (D3D12DDI_DXIL_LIBRARY_DESC_0054::pDXILLibrary, which has no size). The payload is
// taken as a DXIL part (DxilProgramHeader, program kind 6) or as a whole DXBC container; which one the runtime passes
// is not measured. LibraryPayloadDwords is the length the payload claims: SizeInUint32 (DWORD 1) of a DXIL part, or
// the container size (bytes 24 to 27) in DWORDs, rounded up; 0 for a null payload. It reads DWORD 1, or DWORD 6
// behind the DXBC magic, and nothing else. BuildLibraryContainer checks it for internal consistency; nothing in the
// DDI proves the backing extends that far.
size_t LibraryPayloadDwords(const UINT* payload);

// Builds the container the engine parses a library from: the header and the DXIL part alone, copied unchanged (of a
// whole container, its DXIL part; RDAT and the rest are dropped). Reads no further than capacity DWORDs, and checks the
// part offsets and sizes, SizeInUint32 and the bitcode bounds against it. Unsupported
// for a DXIL program that is not a library. On failure the container is left empty.
Result BuildLibraryContainer(const UINT* payload, size_t capacity, Container* container);

namespace dxil { struct Signatures; }
// Non-library SM6+ state-object exports. Rebuilds signature chunks with the names and types in DXIL metadata.
Result BuildGenericContainer(const UINT* payload, size_t capacity, Container* container, dxil::Signatures* metadata);

struct Semantic
{
    std::string name;
    UINT index = 0;
};

// The semantic of the vertex input element on inputRegister (D3D12DDIARG_INPUT_ELEMENT_DESC::InputRegister), for a
// D3D12_INPUT_ELEMENT_DESC of the engine: the input signature entry on that register with the lowest component.
Result InputLayoutSemantic(const SignatureView& vertexInput, UINT inputRegister, Semantic* semantic);

struct StreamOutputElement
{
    bool gap = false;               // a hole: D3D12_SO_DECLARATION_ENTRY with no semantic name
    Semantic semantic;
    BYTE startComponent = 0;
    BYTE componentCount = 0;
};

// One D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY in D3D12_SO_DECLARATION_ENTRY terms, against the container of the
// last stage before rasterization. A hole is RegisterIndex ~0u with RegisterMask giving its component count; the
// engine takes it as an entry with a NULL SemanticName (vkd3d-proton fork branch amdgpu-wddm/so-hole-fix; 7bfcd7f0
// crashes on it). For a DXIL program the semantic comes from its metadata: Unsupported when that does not parse.
Result StreamOutputSemantic(const Container& lastStage, const D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY& entry,
        StreamOutputElement* element);

// The container checksum (bytes 4 to 19) of a container of size bytes, as fxc computes it.
void DxbcChecksum(const uint8_t* data, size_t size, uint8_t digest[16]);

}
