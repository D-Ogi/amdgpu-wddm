// SPDX-License-Identifier: MIT
// dxil-metadata: the signature elements of a DXIL program, read from the dx.entryPoints metadata of its LLVM 3.7
// bitcode. Part of shader-container (prototype, offline only). The reconstructed container names its signature
// entries synthetically, but dxil-spirv resolves stream-output declarations against the semantic names in this
// metadata, so a D3D12 stream-output declaration for a DXIL program must use them.
//
// Reads only what that needs: the bitstream container, BLOCKINFO abbreviations, module-level GLOBALVAR, FUNCTION
// and ALIAS records (value numbering), the module CONSTANTS block (integer constants) and the module METADATA
// blocks. Function bodies, types and symbol tables are skipped by their block length.
//
// Provenance: record codes and operand encodings as in LLVM 3.7 (include/llvm/Bitcode/LLVMBitCodes.h,
// lib/Bitcode/Reader/BitcodeReader.cpp, docs/BitCodeFormat.rst; Apache-2.0 with LLVM exception, format facts
// only); the metadata numbering matches the reader of dxil-spirv bc/module.cpp (MIT). The element layout is the
// one dxc writes (DirectXShaderCompiler DxilMetadataHelper, MIT; read by dxil-spirv dxil_converter.cpp,
// emit_stage_output_variables).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine_ddi::shader_container::dxil {

// One signature element: rows starting at startRow, cols components starting at startCol. startRow is -1 for an
// element without a register (SV_Depth, SV_Coverage, a generated SV_PrimitiveID).
struct Element
{
    std::string name;
    std::vector<uint32_t> indices;      // semantic index of each row
    uint32_t kind = 0;                  // DXIL semantic kind (0 user, 1 SV_VertexID, ...)
    uint32_t rows = 0;
    uint32_t cols = 0;
    int32_t startRow = -1;
    int32_t startCol = -1;
    uint32_t stream = 0;                // geometry stream (extended property tag 0), 0 outside geometry programs
};

struct Signatures
{
    std::vector<Element> input;
    std::vector<Element> output;
    std::vector<Element> patchConstant;
};

// The bitcode inside a DXIL program part (DxilProgramHeader and what follows, size bytes). False when the
// header is not a DXIL program header or the bitcode lies outside the part.
bool ProgramBitcode(const uint8_t* part, size_t size, const uint8_t** bitcode, size_t* bitcodeSize);

// The signatures of the first entry of dx.entryPoints. On failure, error says what did not parse.
bool ReadSignatures(const uint8_t* bitcode, size_t size, Signatures* signatures, std::string* error);

}
