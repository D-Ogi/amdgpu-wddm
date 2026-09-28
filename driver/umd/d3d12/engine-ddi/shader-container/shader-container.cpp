// SPDX-License-Identifier: MIT
// shader-container: DXBC container reconstruction from the D3D12 DDI shader payload. Prototype, offline only;
// see shader-container.h for the contract and the provenance, README.md for the deviations from the D3D11 code.

#include "shader-container.h"

#include <d3dcommon.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace engine_ddi::shader_container {
namespace {

using Entry = D3D12DDIARG_SIGNATURE_ENTRY_0012;

constexpr UINT Registerless = ~0u;

// Enumerators of the WDK and SDK headers as unsigned values, for comparisons without sign warnings.
template <typename T>
constexpr uint32_t U(T value)
{
    return uint32_t(value);
}

void PutTag(std::vector<uint8_t>& out, const char tag[4])
{
    for (int i = 0; i < 4; i++)
        out.push_back(uint8_t(tag[i]));
}

// Name of every element without a system value; the semantic index encodes the register and first component.
constexpr const char* RegisterSemantic = "BC250_R";

Result Fail(Status status, std::string detail)
{
    Result r;
    r.status = status;
    r.detail = std::move(detail);
    return r;
}

uint32_t FirstComponent(uint32_t mask)
{
    for (uint32_t i = 0; i < 4; i++)
        if (mask & (1u << i))
            return i;
    return 0;
}

uint32_t ComponentCount(uint32_t mask)
{
    return uint32_t(std::popcount(mask & 0xfu));
}

// ---- System values ------------------------------------------------------------------------------------------

struct SysvalName
{
    const char* name;       // nullptr: no signature representation
    uint32_t d3dName;       // the D3D_NAME a signature chunk stores
    uint32_t index;         // fixed semantic index, or ~0u to count elements of the same name
};

// D3D10_SB_NAME (the DDI's SystemValue) to the signature representation. The tokenized format names every
// tessellation factor separately; signatures use one name and the semantic index, as fxc assigns it.
SysvalName LookupSysval(uint32_t sbName)
{
    switch (sbName)
    {
    case D3D10_SB_NAME_POSITION:                        return { "SV_Position", D3D_NAME_POSITION, 0 };
    case D3D10_SB_NAME_CLIP_DISTANCE:                   return { "SV_ClipDistance", D3D_NAME_CLIP_DISTANCE, ~0u };
    case D3D10_SB_NAME_CULL_DISTANCE:                   return { "SV_CullDistance", D3D_NAME_CULL_DISTANCE, ~0u };
    case D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX:       return { "SV_RenderTargetArrayIndex", D3D_NAME_RENDER_TARGET_ARRAY_INDEX, 0 };
    case D3D10_SB_NAME_VIEWPORT_ARRAY_INDEX:            return { "SV_ViewportArrayIndex", D3D_NAME_VIEWPORT_ARRAY_INDEX, 0 };
    case D3D10_SB_NAME_VERTEX_ID:                       return { "SV_VertexID", D3D_NAME_VERTEX_ID, 0 };
    case D3D10_SB_NAME_PRIMITIVE_ID:                    return { "SV_PrimitiveID", D3D_NAME_PRIMITIVE_ID, 0 };
    case D3D10_SB_NAME_INSTANCE_ID:                     return { "SV_InstanceID", D3D_NAME_INSTANCE_ID, 0 };
    case D3D10_SB_NAME_IS_FRONT_FACE:                   return { "SV_IsFrontFace", D3D_NAME_IS_FRONT_FACE, 0 };
    case D3D10_SB_NAME_SAMPLE_INDEX:                    return { "SV_SampleIndex", D3D_NAME_SAMPLE_INDEX, 0 };
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR: return { "SV_TessFactor", D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR, 0 };
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_0_EDGE_TESSFACTOR: return { "SV_TessFactor", D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR, 1 };
    case D3D11_SB_NAME_FINAL_QUAD_U_EQ_1_EDGE_TESSFACTOR: return { "SV_TessFactor", D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR, 2 };
    case D3D11_SB_NAME_FINAL_QUAD_V_EQ_1_EDGE_TESSFACTOR: return { "SV_TessFactor", D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR, 3 };
    case D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR:  return { "SV_InsideTessFactor", D3D_NAME_FINAL_QUAD_INSIDE_TESSFACTOR, 0 };
    case D3D11_SB_NAME_FINAL_QUAD_V_INSIDE_TESSFACTOR:  return { "SV_InsideTessFactor", D3D_NAME_FINAL_QUAD_INSIDE_TESSFACTOR, 1 };
    case D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR: return { "SV_TessFactor", D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR, 0 };
    case D3D11_SB_NAME_FINAL_TRI_V_EQ_0_EDGE_TESSFACTOR: return { "SV_TessFactor", D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR, 1 };
    case D3D11_SB_NAME_FINAL_TRI_W_EQ_0_EDGE_TESSFACTOR: return { "SV_TessFactor", D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR, 2 };
    case D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR:     return { "SV_InsideTessFactor", D3D_NAME_FINAL_TRI_INSIDE_TESSFACTOR, 0 };
    // fxc gives an isoline's density index 0 and its detail index 1 (D3D11 engine code: the reverse).
    case D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR:    return { "SV_TessFactor", D3D_NAME_FINAL_LINE_DETAIL_TESSFACTOR, 1 };
    case D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR:   return { "SV_TessFactor", D3D_NAME_FINAL_LINE_DENSITY_TESSFACTOR, 0 };
    default:                                            return { nullptr, D3D_NAME_UNDEFINED, 0 };
    }
}

// ---- Program scan -----------------------------------------------------------------------------------------------

struct OutputDecl
{
    uint32_t reg;
    uint32_t mask;
    uint32_t stream;
    uint32_t claimed;
};

struct ProgramInfo
{
    uint32_t type = 0;
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t length = 0;
    std::vector<OutputDecl> outputs;                // dcl_output* on o#, with the dcl_stream block around them
    std::vector<std::pair<uint32_t, uint32_t>> inputUse;   // (register, mask) of v#, vicp# declarations
    std::vector<std::pair<uint32_t, uint32_t>> patchUse;   // (register, mask) of vpc# declarations
};

// The operand of a declaration: type, 4-component mask (0 when not in mask mode) and the last immediate index.
struct DeclOperand
{
    uint32_t type = ~0u;
    uint32_t mask = 0;
    uint32_t reg = ~0u;
};

bool ParseDeclOperand(const UINT* instr, uint32_t length, DeclOperand* operand)
{
    uint32_t i = 1;

    // Extended opcode tokens
    if (DECODE_IS_D3D10_SB_OPCODE_EXTENDED(instr[0]))
    {
        while (i < length && DECODE_IS_D3D10_SB_OPCODE_EXTENDED(instr[i]))
            i++;
        i++;
    }

    if (i >= length)
        return false;

    UINT token = instr[i++];
    operand->type = uint32_t(DECODE_D3D10_SB_OPERAND_TYPE(token));

    if (DECODE_D3D10_SB_OPERAND_NUM_COMPONENTS(token) == D3D10_SB_OPERAND_4_COMPONENT
            && DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(token) == D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE)
        operand->mask = uint32_t(DECODE_D3D10_SB_OPERAND_4_COMPONENT_MASK(token)) >> D3D10_SB_OPERAND_4_COMPONENT_MASK_SHIFT;

    // Extended operand tokens
    if (DECODE_IS_D3D10_SB_OPERAND_EXTENDED(token))
    {
        while (i < length && DECODE_IS_D3D10_SB_OPERAND_EXTENDED(instr[i]))
            i++;
        i++;
    }

    uint32_t dims = uint32_t(DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token));

    for (uint32_t d = 0; d < dims; d++)
    {
        if (DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(d, token) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
            return false;
        if (i >= length)
            return false;
        operand->reg = instr[i++];
    }

    return true;
}

// Walks the instructions within LenTok. Refuses class linkage and anything that does not parse.
Result ScanProgram(const UINT* code, ProgramInfo* info)
{
    uint32_t stream = 0;

    for (uint32_t pos = 2; pos < info->length;)
    {
        const UINT* instr = &code[pos];
        uint32_t remaining = info->length - pos;
        auto opcode = DECODE_D3D10_SB_OPCODE_TYPE(instr[0]);
        uint32_t length;

        if (opcode == D3D10_SB_OPCODE_CUSTOMDATA)
        {
            if (remaining < 2)
                return Fail(Status::InvalidArgument, "custom data block past the program length");
            length = instr[1];
            if (length < 2)
                return Fail(Status::InvalidArgument, "custom data block shorter than its header");
        }
        else
        {
            length = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(instr[0]);
            if (!length)
                return Fail(Status::InvalidArgument, "instruction of length 0 at token " + std::to_string(pos));
        }

        if (length > remaining)
            return Fail(Status::InvalidArgument, "instruction past the program length at token " + std::to_string(pos));

        switch (opcode)
        {
        // Class linkage (interfaces, function tables, fcall): no DDI field describes it (the IFCE chunk), and
        // D3D12 has no class linkage.
        case D3D11_SB_OPCODE_DCL_FUNCTION_BODY:
        case D3D11_SB_OPCODE_DCL_FUNCTION_TABLE:
        case D3D11_SB_OPCODE_DCL_INTERFACE:
        case D3D11_SB_OPCODE_INTERFACE_CALL:
            return Fail(Status::Unsupported, "class linkage (interfaces) at token " + std::to_string(pos));

        case D3D11_SB_OPCODE_DCL_STREAM:
        {
            DeclOperand op;
            if (!ParseDeclOperand(instr, length, &op) || op.type != U(D3D11_SB_OPERAND_TYPE_STREAM) || op.reg > 3)
                return Fail(Status::InvalidArgument, "malformed dcl_stream at token " + std::to_string(pos));
            stream = op.reg;
            break;
        }

        case D3D10_SB_OPCODE_DCL_OUTPUT:
        case D3D10_SB_OPCODE_DCL_OUTPUT_SGV:
        case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
        {
            DeclOperand op;
            if (!ParseDeclOperand(instr, length, &op))
                return Fail(Status::InvalidArgument, "malformed output declaration at token " + std::to_string(pos));
            if (op.type == U(D3D10_SB_OPERAND_TYPE_OUTPUT) && op.reg != ~0u)
                info->outputs.push_back({ op.reg, op.mask, stream, 0 });
            break;
        }

        case D3D10_SB_OPCODE_DCL_INPUT:
        case D3D10_SB_OPCODE_DCL_INPUT_SGV:
        case D3D10_SB_OPCODE_DCL_INPUT_SIV:
        case D3D10_SB_OPCODE_DCL_INPUT_PS:
        case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV:
        case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:
        {
            DeclOperand op;
            if (!ParseDeclOperand(instr, length, &op))
                return Fail(Status::InvalidArgument, "malformed input declaration at token " + std::to_string(pos));
            if ((op.type == U(D3D10_SB_OPERAND_TYPE_INPUT) || op.type == U(D3D11_SB_OPERAND_TYPE_INPUT_CONTROL_POINT))
                    && op.reg != ~0u)
                info->inputUse.emplace_back(op.reg, op.mask);
            else if (op.type == U(D3D11_SB_OPERAND_TYPE_INPUT_PATCH_CONSTANT) && op.reg != ~0u)
                info->patchUse.emplace_back(op.reg, op.mask);
            break;
        }

        default:
            break;
        }

        pos += length;
    }

    return {};
}

// ---- Signatures -------------------------------------------------------------------------------------------------

enum class SigRole
{
    Input,
    Output,
    PatchConstantOutput,    // hull
    PatchConstantInput,     // domain
};

Result ValidateSignature(const SignatureView& view, SigRole role, const ProgramInfo& program, const char* what,
        bool checkOverlap = true)
{
    if (view.count && !view.entries)
        return Fail(Status::InvalidArgument, std::string(what) + ": entries missing");

    bool gsOutput = role == SigRole::Output && program.type == U(D3D10_SB_GEOMETRY_SHADER) && program.major >= 5;

    for (UINT i = 0; i < view.count; i++)
    {
        const Entry& e = view.entries[i];
        std::string where = std::string(what) + " entry " + std::to_string(i);

        if (e.Stream > 3)
            return Fail(Status::InvalidArgument, where + ": stream " + std::to_string(e.Stream));
        if (e.Stream && !gsOutput)
            return Fail(Status::InvalidArgument, where + ": stream " + std::to_string(e.Stream) + " outside a gs_5 output");

        if (e.Register == Registerless)
            continue;       // declared by the program (depth, coverage, primitive ID); no chunk entry

        if (!(e.Mask & 0xfu) || (e.Mask & ~0xfu))
            return Fail(Status::InvalidArgument, where + ": mask " + std::to_string(e.Mask));

        uint32_t sv = uint32_t(e.SystemValue);
        if (sv >= U(D3D12_SB_NAME_BARYCENTRICS) && sv <= U(D3D12_SB_NAME_CULLPRIMITIVE))
            return Fail(Status::Unsupported, where + ": system value " + std::to_string(sv) + " exists only in DXIL");
        if (sv != U(D3D10_SB_NAME_UNDEFINED) && !LookupSysval(sv).name)
            return Fail(Status::InvalidArgument, where + ": unknown system value " + std::to_string(sv));

        uint32_t type = uint32_t(e.RegisterComponentType);
        if (type >= U(D3D10_SB_REGISTER_COMPONENT_UINT16) && type <= U(D3D10_SB_REGISTER_COMPONENT_FLOAT64))
            return Fail(Status::Unsupported, where + ": component type " + std::to_string(type) + " exists only in DXIL");
        if (type > U(D3D10_SB_REGISTER_COMPONENT_FLOAT64))
            return Fail(Status::InvalidArgument, where + ": unknown component type " + std::to_string(type));

        uint32_t precision = uint32_t(e.MinPrecision);
        bool precisionOk = precision == U(D3D11_SB_OPERAND_MIN_PRECISION_DEFAULT)
            || ((type == U(D3D10_SB_REGISTER_COMPONENT_FLOAT32) || type == U(D3D10_SB_REGISTER_COMPONENT_UNKNOWN))
                    && (precision == U(D3D11_SB_OPERAND_MIN_PRECISION_FLOAT_16) || precision == U(D3D11_SB_OPERAND_MIN_PRECISION_FLOAT_2_8)))
            || (type == U(D3D10_SB_REGISTER_COMPONENT_SINT32) && precision == U(D3D11_SB_OPERAND_MIN_PRECISION_SINT_16))
            || (type == U(D3D10_SB_REGISTER_COMPONENT_UINT32) && precision == U(D3D11_SB_OPERAND_MIN_PRECISION_UINT_16));
        if (!precisionOk)
            return Fail(Status::InvalidArgument, where + ": minimum precision " + std::to_string(precision)
                    + " with component type " + std::to_string(type));

        // Two entries on the same components would get the same name.
        for (UINT j = 0; checkOverlap && j < i; j++)
        {
            const Entry& o = view.entries[j];
            if (o.Register == e.Register && o.Stream == e.Stream && (o.Mask & e.Mask & 0xfu))
                return Fail(Status::InvalidArgument, where + ": overlaps entry " + std::to_string(j));
        }
    }

    return {};
}

struct Named
{
    Entry entry;
    std::string name;
    uint32_t index = 0;
    uint32_t d3dName = D3D_NAME_UNDEFINED;
};

// Names every entry that has a register, in order. Validation has run.
std::vector<Named> NameSignature(const Entry* entries, UINT count, bool pixelOutput)
{
    std::vector<Named> named;
    uint32_t clipCount = 0;
    uint32_t cullCount = 0;

    for (UINT i = 0; i < count; i++)
    {
        const Entry& e = entries[i];

        if (e.Register == Registerless)
            continue;

        Named n;
        n.entry = e;

        if (pixelOutput && e.SystemValue == D3D10_SB_NAME_UNDEFINED)
        {
            n.name = "SV_Target";
            n.index = e.Register;
            n.d3dName = D3D_NAME_TARGET;
        }
        else if (e.SystemValue != D3D10_SB_NAME_UNDEFINED)
        {
            SysvalName sv = LookupSysval(uint32_t(e.SystemValue));
            n.name = sv.name;
            n.d3dName = sv.d3dName;
            n.index = sv.index;

            if (sv.d3dName == U(D3D_NAME_CLIP_DISTANCE))
                n.index = clipCount++;
            else if (sv.d3dName == U(D3D_NAME_CULL_DISTANCE))
                n.index = cullCount++;
        }
        else
        {
            // gs_5 streams 1 to 3 reuse registers of stream 0. vkd3d-proton resolves stream-output entries by name
            // and index alone (libs/vkd3d-shader/dxil.c, dxil_output_remap: "TODO: Stream index matching?"), so
            // those streams get names of their own. Only stream 0 links to a pixel program (libs/vkd3d/state.c,
            // vkd3d_validate_shader_io_signatures), and it keeps BC250_R.
            n.name = e.Stream ? "BC250_S" + std::to_string(e.Stream) + "R" : std::string(RegisterSemantic);
            n.index = e.Register * 4 + FirstComponent(e.Mask);
        }

        named.push_back(std::move(n));
    }

    return named;
}

// The runtime's entries may carry no stream (the D3D11 DDI has none; whether the D3D12 runtime fills Stream is
// not yet measured). When every entry of a gs_5 output signature says 0, each takes the first dcl_stream block
// whose output declarations cover its register and components and have not given them to an earlier entry.
// fxc reuses registers across streams, so the register alone is not enough.
void DeriveOutputStreams(ProgramInfo& program, std::vector<Entry>& entries)
{
    for (Entry& e : entries)
    {
        uint32_t mask = e.Mask & 0xfu;

        if (e.Register == Registerless)
            continue;

        for (OutputDecl& d : program.outputs)
        {
            if (d.reg == e.Register && (d.mask & mask) && !(d.claimed & mask))
            {
                e.Stream = BYTE(d.stream);
                d.claimed |= mask;
                break;
            }
        }
    }
}

void Put32(std::vector<uint8_t>& out, uint32_t value)
{
    uint8_t bytes[4];
    std::memcpy(bytes, &value, 4);
    out.insert(out.end(), bytes, bytes + 4);
}

void Set32(std::vector<uint8_t>& out, size_t offset, uint32_t value)
{
    std::memcpy(&out[offset], &value, 4);
}

uint32_t UsedMask(const std::vector<std::pair<uint32_t, uint32_t>>& uses, uint32_t reg)
{
    uint32_t mask = 0;
    for (const auto& u : uses)
        if (u.first == reg)
            mask |= u.second;
    return mask;
}

// One ISG1/OSG1/PSG1 chunk: header, entry count, 8, eight DWORDs per entry (stream, name offset, semantic index,
// system value, component type, register, mask with the read/write mask in byte 1, minimum precision), then the
// names, padded to a DWORD. Name offsets count from the entry count.
std::vector<uint8_t> WriteSignatureChunk(const char tag[4], const std::vector<Named>& named, bool input,
        const std::vector<std::pair<uint32_t, uint32_t>>* uses)
{
    std::vector<uint8_t> chunk;
    PutTag(chunk, tag);
    Put32(chunk, 0);                        // chunk size, set below

    const size_t data = chunk.size();
    Put32(chunk, uint32_t(named.size()));
    Put32(chunk, 8);

    std::vector<size_t> nameFields;

    for (const Named& n : named)
    {
        const Entry& e = n.entry;
        uint32_t mask = e.Mask & 0xfu;
        // Inputs: the components the program declares (fxc's "used"); without a program scan, all of them.
        // Outputs: no component is marked as never written.
        uint32_t rw = input ? (uses ? (mask & UsedMask(*uses, e.Register)) : mask) : 0u;
        uint32_t type = e.RegisterComponentType == D3D10_SB_REGISTER_COMPONENT_UNKNOWN
            ? uint32_t(D3D10_SB_REGISTER_COMPONENT_FLOAT32) : uint32_t(e.RegisterComponentType);

        Put32(chunk, e.Stream);
        nameFields.push_back(chunk.size());
        Put32(chunk, 0);
        Put32(chunk, n.index);
        Put32(chunk, n.d3dName);
        Put32(chunk, type);
        Put32(chunk, e.Register);
        Put32(chunk, mask | (rw << 8));
        Put32(chunk, uint32_t(e.MinPrecision));
    }

    for (size_t i = 0; i < named.size(); i++)
    {
        Set32(chunk, nameFields[i], uint32_t(chunk.size() - data));
        chunk.insert(chunk.end(), named[i].name.begin(), named[i].name.end());
        chunk.push_back(0);
    }

    while (chunk.size() % 4)
        chunk.push_back(0);

    Set32(chunk, 4, uint32_t(chunk.size() - data));
    return chunk;
}

// ---- MD5 and the container checksum -------------------------------------------------------------------------------

class Md5
{
public:
    void Update(const uint8_t* data, size_t size)
    {
        size_t offset = m_size % 64;
        m_size += size;

        if (offset)
        {
            size_t n = std::min(size, 64 - offset);
            std::memcpy(&m_block[offset], data, n);
            data += n;
            size -= n;
            if (offset + n == 64)
                Block(m_block.data());
        }

        for (; size >= 64; data += 64, size -= 64)
            Block(data);

        if (size)
            std::memcpy(m_block.data(), data, size);
    }

    // The state as a digest, without MD5's own finalisation (the container checksum pads by itself).
    void Digest(uint8_t digest[16]) const
    {
        for (uint32_t i = 0; i < 4; i++)
            std::memcpy(&digest[4 * i], &m_state[i], 4);
    }

private:
    void Block(const uint8_t* data)
    {
        static constexpr uint8_t shifts[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        };
        static constexpr uint32_t constants[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
        };

        uint32_t words[16];
        for (uint32_t i = 0; i < 16; i++)
            words[i] = uint32_t(data[4 * i]) | uint32_t(data[4 * i + 1]) << 8
                    | uint32_t(data[4 * i + 2]) << 16 | uint32_t(data[4 * i + 3]) << 24;

        uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];

        for (uint32_t i = 0; i < 64; i++)
        {
            uint32_t f, g;
            if (i < 16)
            {
                f = (b & c) | (~b & d);
                g = i;
            }
            else if (i < 32)
            {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            }
            else if (i < 48)
            {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            }
            else
            {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }

            f += a + constants[i] + words[g];
            a = d;
            d = c;
            c = b;
            b += std::rotl(f, shifts[i]);
        }

        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
    }

    std::array<uint8_t, 64> m_block = {};
    uint64_t m_size = 0;
    std::array<uint32_t, 4> m_state = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
};

} // namespace

void DxbcChecksum(const uint8_t* data, size_t size, uint8_t digest[16])
{
    // The checksum covers everything after itself: the version field on.
    constexpr size_t Skip = 20;
    static constexpr uint8_t padding[64] = { 0x80 };

    std::memset(digest, 0, 16);
    if (size < Skip)
        return;

    const uint8_t* bytes = data + Skip;
    size -= Skip;

    uint32_t bits = uint32_t(size) * 8u;
    uint32_t magic = (bits >> 2) | 1u;
    uint8_t a[4], b[4];
    std::memcpy(a, &bits, 4);
    std::memcpy(b, &magic, 4);

    size_t remainder = size % 64;
    size_t paddingSize = 64 - remainder;

    Md5 md5;
    md5.Update(bytes, size - remainder);

    // Not MD5's finalisation: the bit count leads the last block, or fills a block of its own.
    if (remainder >= 56)
    {
        md5.Update(&bytes[size - remainder], remainder);
        md5.Update(padding, paddingSize);
        md5.Update(a, 4);
        md5.Update(padding + 4, 64 - 8);
        md5.Update(b, 4);
    }
    else
    {
        md5.Update(a, 4);
        if (remainder)
            md5.Update(&bytes[size - remainder], remainder);
        md5.Update(padding, paddingSize - 8);
        md5.Update(b, 4);
    }

    md5.Digest(digest);
}

Result BuildContainer(const ProgramDesc& desc, Container* container)
{
    container->bytes.clear();
    container->output.clear();
    container->programType = ~0u;

    if (!desc.code || desc.codeCapacity < 2)
        return Fail(Status::InvalidArgument, "no program, or fewer than two readable DWORDs");

    ProgramInfo program;
    UINT version = desc.code[0];
    program.length = desc.code[1];
    program.type = uint32_t(DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(version));
    program.major = uint32_t(DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(version));
    program.minor = uint32_t(DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(version));

    if (program.length < 2)
        return Fail(Status::InvalidArgument, "LenTok " + std::to_string(program.length) + " below 2");
    if (program.length > desc.codeCapacity)
        return Fail(Status::InvalidArgument, "LenTok " + std::to_string(program.length) + " past the "
                + std::to_string(desc.codeCapacity) + " readable DWORDs");

    // DXIL's program header has the same two DWORDs; its major version is 6 or more.
    if (program.major >= 6)
        return Fail(Status::Unsupported, "shader model " + std::to_string(program.major) + "."
                + std::to_string(program.minor) + ": DXIL is out of scope");
    if (program.type > U(D3D11_SB_COMPUTE_SHADER))
        return Fail(Status::InvalidArgument, "program type " + std::to_string(program.type));

    bool sm4 = program.major == 4 && program.minor <= 1;
    bool sm5 = program.major == 5 && program.minor <= 1;
    bool tessellation = program.type == U(D3D11_SB_HULL_SHADER) || program.type == U(D3D11_SB_DOMAIN_SHADER);
    if (!(sm5 || (sm4 && !tessellation)))
        return Fail(Status::Unsupported, "program type " + std::to_string(program.type) + " version "
                + std::to_string(program.major) + "." + std::to_string(program.minor));

    if (program.type == U(D3D11_SB_COMPUTE_SHADER) && (desc.input.count || desc.output.count || desc.patchConstant.count))
        return Fail(Status::InvalidArgument, "signatures on a compute program");
    if (!tessellation && desc.patchConstant.count)
        return Fail(Status::InvalidArgument, "patch-constant signature outside a hull or domain program");

    if (Result r = ScanProgram(desc.code, &program); !r)
        return r;

    if (Result r = ValidateSignature(desc.input, SigRole::Input, program, "input"); !r)
        return r;
    bool isPixel = program.type == U(D3D10_SB_PIXEL_SHADER);
    bool hasStreams = program.type == U(D3D10_SB_GEOMETRY_SHADER) && program.major >= 5;
    bool deriveStreams = hasStreams && std::none_of(desc.output.entries, desc.output.entries + desc.output.count,
            [](const Entry& e) { return e.Stream != 0; });

    // Without streams, entries of different streams may share components; the overlap check waits for them.
    if (Result r = ValidateSignature(desc.output, SigRole::Output, program, "output", !deriveStreams); !r)
        return r;
    SigRole pcRole = program.type == U(D3D11_SB_HULL_SHADER) ? SigRole::PatchConstantOutput : SigRole::PatchConstantInput;
    if (Result r = ValidateSignature(desc.patchConstant, pcRole, program, "patch constant"); !r)
        return r;

    // Streams of a gs_5 output signature, unless the runtime passed them.
    std::vector<Entry> output(desc.output.entries, desc.output.entries + desc.output.count);
    if (deriveStreams)
    {
        DeriveOutputStreams(program, output);

        // Derived streams must still name each component once.
        SignatureView derived = { output.data(), UINT(output.size()) };
        if (Result r = ValidateSignature(derived, SigRole::Output, program, "output (derived streams)"); !r)
            return r;
    }

    std::vector<Named> inputNames = NameSignature(desc.input.entries, desc.input.count, false);
    std::vector<Named> outputNames = NameSignature(output.data(), UINT(output.size()), isPixel);
    std::vector<Named> patchNames = NameSignature(desc.patchConstant.entries, desc.patchConstant.count, false);

    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(WriteSignatureChunk("ISG1", inputNames, true, &program.inputUse));
    chunks.push_back(WriteSignatureChunk("OSG1", outputNames, false, nullptr));
    if (tessellation)
        chunks.push_back(WriteSignatureChunk("PSG1", patchNames, program.type == U(D3D11_SB_DOMAIN_SHADER), &program.patchUse));

    // Program chunk: tag, byte size, the tokens unchanged
    std::vector<uint8_t> code;
    PutTag(code, program.major >= 5 ? "SHEX" : "SHDR");
    uint32_t codeSize = program.length * uint32_t(sizeof(UINT));
    Put32(code, codeSize);
    const uint8_t* tokens = reinterpret_cast<const uint8_t*>(desc.code);
    code.insert(code.end(), tokens, tokens + codeSize);
    chunks.push_back(std::move(code));

    // Header: magic, checksum, version 1, file size, chunk count, chunk offsets
    uint32_t chunkCount = uint32_t(chunks.size());
    uint32_t fileSize = 32 + 4 * chunkCount;
    for (const auto& c : chunks)
        fileSize += uint32_t(c.size());

    std::vector<uint8_t>& out = container->bytes;
    out.reserve(fileSize);
    PutTag(out, "DXBC");
    out.resize(20, 0);
    Put32(out, 1);
    Put32(out, fileSize);
    Put32(out, chunkCount);

    uint32_t offset = 32 + 4 * chunkCount;
    for (const auto& c : chunks)
    {
        Put32(out, offset);
        offset += uint32_t(c.size());
    }
    for (const auto& c : chunks)
        out.insert(out.end(), c.begin(), c.end());

    DxbcChecksum(out.data(), out.size(), &out[4]);

    container->programType = program.type;
    for (const Named& n : outputNames)
        container->output.push_back(n.entry);

    return {};
}

Result InputLayoutSemantic(const SignatureView& vertexInput, UINT inputRegister, Semantic* semantic)
{
    // Vertex inputs carry no stream and no pixel-output rule; validation of the full signature is the
    // container build's job, this only needs the entries to be present.
    if (vertexInput.count && !vertexInput.entries)
        return Fail(Status::InvalidArgument, "input entries missing");

    std::vector<Named> named = NameSignature(vertexInput.entries, vertexInput.count, false);
    const Named* best = nullptr;

    for (const Named& n : named)
    {
        if (n.entry.Register != inputRegister || n.entry.SystemValue != D3D10_SB_NAME_UNDEFINED)
            continue;
        if (!best || FirstComponent(n.entry.Mask) < FirstComponent(best->entry.Mask))
            best = &n;
    }

    if (!best)
        return Fail(Status::InvalidArgument, "no input signature entry on register " + std::to_string(inputRegister));

    semantic->name = best->name;
    semantic->index = best->index;
    return {};
}

Result StreamOutputSemantic(const Container& lastStage, const D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY& entry,
        StreamOutputElement* element)
{
    *element = {};

    uint32_t mask = entry.RegisterMask & 0xfu;
    if (!mask || (entry.RegisterMask & ~0xfu) || entry.Stream > 3)
        return Fail(Status::InvalidArgument, "stream-output entry mask or stream out of range");

    uint32_t first = FirstComponent(mask);
    uint32_t count = ComponentCount(mask);
    if ((mask >> first) != (1u << count) - 1u)
        return Fail(Status::InvalidArgument, "stream-output entry mask not contiguous");

    element->componentCount = BYTE(count);

    // A hole: no semantic, skips its components in the buffer.
    if (entry.RegisterIndex == ~0u)
    {
        element->gap = true;
        return {};
    }

    bool isPixel = lastStage.programType == U(D3D10_SB_PIXEL_SHADER);
    bool hasStreams = lastStage.programType == U(D3D10_SB_GEOMETRY_SHADER);
    std::vector<Named> named = NameSignature(lastStage.output.data(), UINT(lastStage.output.size()), isPixel);

    for (const Named& n : named)
    {
        uint32_t entryMask = n.entry.Mask & 0xfu;
        if (n.entry.Register != entry.RegisterIndex || n.entry.Stream != (hasStreams ? entry.Stream : 0u)
                || !(entryMask & (1u << first)))
            continue;
        if (mask & ~entryMask)
            return Fail(Status::InvalidArgument, "stream-output entry spans more than one signature element");

        element->semantic.name = n.name;
        element->semantic.index = n.index;
        element->startComponent = BYTE(first - FirstComponent(entryMask));
        return {};
    }

    return Fail(Status::InvalidArgument, "stream-output register " + std::to_string(entry.RegisterIndex)
            + " stream " + std::to_string(entry.Stream) + " not in the output signature");
}

}
