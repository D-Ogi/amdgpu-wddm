// SPDX-License-Identifier: MIT
// dxil-metadata: signature elements from DXIL bitcode metadata. See dxil-metadata.h for scope and provenance.

#include "dxil-metadata.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace engine_ddi::shader_container::dxil {
namespace {

// ---- Bitstream -------------------------------------------------------------------------------------------------

// Bits are read least significant first from little-endian bytes.
class Bits
{
public:
    Bits(const uint8_t* data, size_t size) : m_data(data), m_end(uint64_t(size) * 8u) {}

    bool Fixed(uint32_t width, uint64_t* value)
    {
        if (width > 64 || m_end - m_pos < width)
            return false;
        uint64_t v = 0;
        for (uint32_t done = 0; done < width;)
        {
            uint32_t bit = uint32_t(m_pos & 7u);
            uint32_t take = std::min(8u - bit, width - done);
            v |= uint64_t((m_data[m_pos >> 3] >> bit) & ((1u << take) - 1u)) << done;
            done += take;
            m_pos += take;
        }
        *value = v;
        return true;
    }

    bool Vbr(uint32_t width, uint64_t* value)
    {
        if (width < 2 || width > 32)
            return false;
        const uint64_t more = 1ull << (width - 1);
        uint64_t v = 0;
        for (uint32_t shift = 0;; shift += width - 1)
        {
            uint64_t piece;
            if (shift > 63 || !Fixed(width, &piece))
                return false;
            v |= (piece & (more - 1)) << shift;
            if (!(piece & more))
                break;
        }
        *value = v;
        return true;
    }

    bool Align32()
    {
        uint64_t pos = (m_pos + 31u) & ~uint64_t(31u);
        if (pos > m_end)
            return false;
        m_pos = pos;
        return true;
    }

    bool Seek(uint64_t pos)
    {
        if (pos > m_end)
            return false;
        m_pos = pos;
        return true;
    }

    uint64_t Pos() const { return m_pos; }
    uint64_t Remaining() const { return m_end - m_pos; }

private:
    const uint8_t* m_data;
    uint64_t m_end;
    uint64_t m_pos = 0;
};

struct AbbrevOp
{
    enum Kind : uint8_t { Literal, Fixed, Vbr, Array, Char6, Blob } kind;
    uint64_t value;     // the literal, or the width of Fixed and Vbr
};

using Abbrev = std::vector<AbbrevOp>;

struct Record
{
    uint64_t code = 0;
    std::vector<uint64_t> ops;
};

// Block and record identifiers of LLVM 3.7.
constexpr uint64_t BlockInfoBlock = 0;
constexpr uint64_t ModuleBlock = 8;
constexpr uint64_t ConstantsBlock = 11;
constexpr uint64_t MetadataBlock = 15;

constexpr uint64_t EndBlock = 0;
constexpr uint64_t EnterSubblock = 1;
constexpr uint64_t DefineAbbrev = 2;
constexpr uint64_t UnabbrevRecord = 3;

constexpr uint64_t BlockInfoSetBid = 1;

constexpr uint64_t ModuleGlobalVar = 7;
constexpr uint64_t ModuleFunction = 8;
constexpr uint64_t ModuleAliasOld = 9;
constexpr uint64_t ModuleAlias = 14;

constexpr uint64_t ConstSetType = 1;
constexpr uint64_t ConstNull = 2;
constexpr uint64_t ConstInteger = 4;

constexpr uint64_t MdString = 1;
constexpr uint64_t MdValue = 2;
constexpr uint64_t MdNode = 3;
constexpr uint64_t MdName = 4;
constexpr uint64_t MdDistinctNode = 5;
constexpr uint64_t MdNamedNode = 10;

// Records of the metadata block that define the next metadata ID (BitcodeReader::parseMetadata of LLVM 3.7):
// strings, values, nodes, locations, old nodes and the debug-info records. NAME, NAMED_NODE and KIND do not.
bool DefinesMetadata(uint64_t code)
{
    return code == MdString || code == MdValue || code == MdNode || code == MdDistinctNode
        || code == 7 || code == 8 || code == 9 || (code >= 12 && code <= 31);
}

// Operands of a string record: one character each.
std::string Chars(const std::vector<uint64_t>& ops)
{
    std::string s;
    s.reserve(ops.size());
    for (uint64_t c : ops)
        s.push_back(char(uint8_t(c)));
    return s;
}

struct Md
{
    enum Kind : uint8_t { Other, String, Value, Node } kind = Other;
    std::string string;
    uint64_t value = 0;                 // Value: the module value ID
    std::vector<int64_t> operands;      // Node: metadata IDs, -1 for null
};

struct Constant
{
    bool integer = false;
    int64_t value = 0;
};

class Reader
{
public:
    Reader(const uint8_t* data, size_t size) : m_bits(data, size) {}

    bool Parse()
    {
        // Top level: abbreviation width 2, the module block.
        while (m_bits.Remaining() >= 32)
        {
            uint64_t id;
            if (!m_bits.Fixed(2, &id) || id != EnterSubblock)
                return Fail("top level: expected a block");
            uint64_t block;
            if (!Enter(&block))
                return false;
            if (block == ModuleBlock)
                return ReadBlock(ModuleBlock, m_width, 0);
            if (!m_bits.Seek(m_end))
                return Fail("top level: block past the bitcode");
        }
        return Fail("no module block");
    }

    bool ReadEntry(Signatures* out)
    {
        if (m_entryPoints.empty())
            return Fail("no dx.entryPoints");
        const Md* entry = NodeAt(m_entryPoints[0]);
        if (!entry || entry->operands.size() < 3)
            return Fail("dx.entryPoints: first entry is not an entry point node");
        if (entry->operands[2] < 0)
            return true;    // no signatures (compute)
        const Md* sigs = NodeAt(uint64_t(entry->operands[2]));
        if (!sigs || sigs->operands.size() < 3)
            return Fail("entry point: signatures are not a node of three lists");
        std::vector<Element>* lists[3] = { &out->input, &out->output, &out->patchConstant };
        for (size_t i = 0; i < 3; i++)
        {
            if (sigs->operands[i] < 0)
                continue;
            const Md* list = NodeAt(uint64_t(sigs->operands[i]));
            if (!list)
                return Fail("signature list is not a node");
            for (int64_t id : list->operands)
            {
                Element e;
                if (id < 0 || !ReadElement(uint64_t(id), &e))
                    return Fail("signature element " + std::to_string(lists[i]->size()) + " of list "
                            + std::to_string(i) + " does not parse");
                lists[i]->push_back(std::move(e));
            }
        }
        return true;
    }

    std::string error;

private:
    bool Fail(std::string what)
    {
        if (error.empty())
            error = std::move(what);
        return false;
    }

    // After ENTER_SUBBLOCK: block ID, abbreviation width, alignment, length in words.
    bool Enter(uint64_t* block)
    {
        uint64_t width, words;
        if (!m_bits.Vbr(8, block) || !m_bits.Vbr(4, &width) || !m_bits.Align32() || !m_bits.Fixed(32, &words))
            return Fail("block header past the bitcode");
        if (width < 1 || width > 32 || words * 32u > m_bits.Remaining())
            return Fail("block " + std::to_string(*block) + ": width or length out of range");
        m_width = uint32_t(width);
        m_end = m_bits.Pos() + words * 32u;
        return true;
    }

    bool ReadAbbrev(Abbrev* abbrev)
    {
        uint64_t count;
        if (!m_bits.Vbr(5, &count) || count > 256)
            return Fail("abbreviation: operand count");
        for (uint64_t i = 0; i < count; i++)
        {
            uint64_t literal, encoding, width;
            if (!m_bits.Fixed(1, &literal))
                return Fail("abbreviation past the bitcode");
            if (literal)
            {
                if (!m_bits.Vbr(8, &width))
                    return Fail("abbreviation past the bitcode");
                abbrev->push_back({ AbbrevOp::Literal, width });
                continue;
            }
            if (!m_bits.Fixed(3, &encoding))
                return Fail("abbreviation past the bitcode");
            switch (encoding)
            {
            case 1:
            case 2:
                if (!m_bits.Vbr(5, &width))
                    return Fail("abbreviation past the bitcode");
                // A Fixed or VBR operand of width 0 is the literal 0 (BitstreamReader::ReadAbbrevRecord).
                if (!width)
                    abbrev->push_back({ AbbrevOp::Literal, 0 });
                else if (width > (encoding == 1 ? 64u : 32u) || (encoding == 2 && width < 2))
                    return Fail("abbreviation: operand width " + std::to_string(width));
                else
                    abbrev->push_back({ encoding == 1 ? AbbrevOp::Fixed : AbbrevOp::Vbr, width });
                break;
            case 3: abbrev->push_back({ AbbrevOp::Array, 0 }); break;
            case 4: abbrev->push_back({ AbbrevOp::Char6, 0 }); break;
            case 5: abbrev->push_back({ AbbrevOp::Blob, 0 }); break;
            default: return Fail("abbreviation: encoding " + std::to_string(encoding));
            }
        }
        return true;
    }

    bool Scalar(const AbbrevOp& op, uint64_t* value)
    {
        switch (op.kind)
        {
        case AbbrevOp::Literal: *value = op.value; return true;
        case AbbrevOp::Fixed: return m_bits.Fixed(uint32_t(op.value), value);
        case AbbrevOp::Vbr: return m_bits.Vbr(uint32_t(op.value), value);
        case AbbrevOp::Char6:
        {
            static constexpr char table[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
            uint64_t c;
            if (!m_bits.Fixed(6, &c))
                return false;
            *value = uint8_t(table[c]);
            return true;
        }
        default: return false;
        }
    }

    bool ReadRecord(uint64_t id, const std::vector<Abbrev>& abbrevs, Record* record)
    {
        std::vector<uint64_t> values;

        if (id == UnabbrevRecord)
        {
            uint64_t count, v;
            if (!m_bits.Vbr(6, &record->code) || !m_bits.Vbr(6, &count) || count > m_bits.Remaining())
                return Fail("record past the bitcode");
            record->ops.resize(size_t(count));
            for (uint64_t& op : record->ops)
            {
                if (!m_bits.Vbr(6, &v))
                    return Fail("record past the bitcode");
                op = v;
            }
            return true;
        }

        if (id - 4 >= abbrevs.size())
            return Fail("record with undefined abbreviation " + std::to_string(id));
        const Abbrev& a = abbrevs[size_t(id - 4)];

        for (size_t i = 0; i < a.size(); i++)
        {
            uint64_t v;
            if (a[i].kind == AbbrevOp::Array)
            {
                // The element operand follows the array and ends the abbreviation.
                uint64_t count;
                if (i + 2 != a.size() || !m_bits.Vbr(6, &count) || count > m_bits.Remaining())
                    return Fail("abbreviated array");
                for (uint64_t k = 0; k < count; k++)
                {
                    if (!Scalar(a[i + 1], &v))
                        return Fail("abbreviated array past the bitcode");
                    values.push_back(v);
                }
                break;
            }
            if (a[i].kind == AbbrevOp::Blob)
            {
                uint64_t count;
                if (i + 1 != a.size() || !m_bits.Vbr(6, &count) || !m_bits.Align32() || count > m_bits.Remaining() / 8u)
                    return Fail("abbreviated blob");
                for (uint64_t k = 0; k < count; k++)
                {
                    m_bits.Fixed(8, &v);
                    values.push_back(v);
                }
                if (!m_bits.Align32())
                    return Fail("abbreviated blob past the bitcode");
                break;
            }
            if (!Scalar(a[i], &v))
                return Fail("abbreviated record past the bitcode");
            values.push_back(v);
        }

        if (values.empty())
            return Fail("abbreviated record without a code");
        record->code = values[0];
        record->ops.assign(values.begin() + 1, values.end());
        return true;
    }

    // Reads a block entered with abbreviation width `width` up to and including its END_BLOCK.
    bool ReadBlock(uint64_t block, uint32_t width, int depth)
    {
        const uint64_t end = m_end;
        std::vector<Abbrev> abbrevs = m_blockInfo[block];
        uint64_t infoTarget = ~0ull;

        for (;;)
        {
            uint64_t id;
            if (m_bits.Pos() >= end || !m_bits.Fixed(width, &id))
                return Fail("block " + std::to_string(block) + " without END_BLOCK");

            if (id == EndBlock)
                return m_bits.Align32() ? true : Fail("END_BLOCK past the bitcode");

            if (id == EnterSubblock)
            {
                uint64_t sub;
                if (!Enter(&sub))
                    return false;
                uint64_t subEnd = m_end;
                bool wanted = block == ModuleBlock
                    && (sub == BlockInfoBlock || sub == ConstantsBlock || sub == MetadataBlock);
                if (subEnd > end)
                    return Fail("block " + std::to_string(sub) + " past its parent");
                if (wanted && depth < 2)
                {
                    if (!ReadBlock(sub, m_width, depth + 1))
                        return false;
                }
                if (!m_bits.Seek(subEnd))
                    return Fail("block " + std::to_string(sub) + " past the bitcode");
                m_end = end;
                continue;
            }

            if (id == DefineAbbrev)
            {
                Abbrev a;
                if (!ReadAbbrev(&a))
                    return false;
                if (block == BlockInfoBlock)
                {
                    if (infoTarget == ~0ull)
                        return Fail("BLOCKINFO abbreviation before SETBID");
                    m_blockInfo[infoTarget].push_back(std::move(a));
                }
                else
                {
                    abbrevs.push_back(std::move(a));
                }
                continue;
            }

            Record r;
            if (!ReadRecord(id, abbrevs, &r))
                return false;

            if (block == BlockInfoBlock)
            {
                if (r.code == BlockInfoSetBid && !r.ops.empty())
                    infoTarget = r.ops[0];
            }
            else if (block == ModuleBlock)
            {
                // Module values are numbered in record order: globals, functions and aliases, then the constants.
                if (r.code == ModuleGlobalVar || r.code == ModuleFunction || r.code == ModuleAliasOld || r.code == ModuleAlias)
                    m_values.push_back({});
            }
            else if (block == ConstantsBlock)
            {
                if (r.code == ConstSetType)
                    continue;
                Constant c;
                if (r.code == ConstNull)
                    c.integer = true;
                else if (r.code == ConstInteger && !r.ops.empty())
                {
                    // Sign rotated: the sign in bit 0 (BitcodeReader decodeSignRotatedValue).
                    uint64_t v = r.ops[0];
                    c.integer = true;
                    c.value = !(v & 1) ? int64_t(v >> 1) : v != 1 ? -int64_t(v >> 1) : INT64_MIN;
                }
                m_values.push_back(c);
            }
            else if (block == MetadataBlock)
            {
                MetadataRecord(r);
            }
        }
    }

    void MetadataRecord(const Record& r)
    {
        if (r.code == MdName)
        {
            m_name = Chars(r.ops);
            return;
        }
        if (r.code == MdNamedNode)
        {
            // Named nodes list metadata IDs directly.
            if (m_name == "dx.entryPoints")
                m_entryPoints = r.ops;
            m_name.clear();
            return;
        }
        if (!DefinesMetadata(r.code))
            return;

        Md md;
        if (r.code == MdString)
        {
            md.kind = Md::String;
            md.string = Chars(r.ops);
        }
        else if (r.code == MdValue && r.ops.size() >= 2)
        {
            md.kind = Md::Value;
            md.value = r.ops[1];
        }
        else if (r.code == MdNode || r.code == MdDistinctNode)
        {
            // Node operands are metadata ID + 1, 0 for null.
            md.kind = Md::Node;
            for (uint64_t op : r.ops)
                md.operands.push_back(op ? int64_t(op - 1) : -1);
        }
        m_md.push_back(std::move(md));
    }

    const Md* NodeAt(uint64_t id) const
    {
        return id < m_md.size() && m_md[size_t(id)].kind == Md::Node ? &m_md[size_t(id)] : nullptr;
    }

    bool Int(int64_t id, int64_t* value) const
    {
        if (id < 0 || uint64_t(id) >= m_md.size() || m_md[size_t(id)].kind != Md::Value)
            return false;
        uint64_t v = m_md[size_t(id)].value;
        if (v >= m_values.size() || !m_values[size_t(v)].integer)
            return false;
        *value = m_values[size_t(v)].value;
        return true;
    }

    // !{i32 id, !"name", i8 type, i8 kind, !{i32 index...}, i8 interpolation, i32 rows, i8 cols, i32 startRow,
    //   i8 startCol, !{i32 tag, value, ...}}
    bool ReadElement(uint64_t id, Element* e) const
    {
        const Md* node = NodeAt(id);
        if (!node || node->operands.size() < 10)
            return false;
        const std::vector<int64_t>& op = node->operands;
        int64_t name = op[1];
        if (name < 0 || uint64_t(name) >= m_md.size() || m_md[size_t(name)].kind != Md::String)
            return false;
        e->name = m_md[size_t(name)].string;

        int64_t kind, rows, cols, startRow, startCol;
        if (!Int(op[3], &kind) || !Int(op[6], &rows) || !Int(op[7], &cols) || !Int(op[8], &startRow)
                || !Int(op[9], &startCol) || rows < 1 || rows > 64 || cols < 1 || cols > 4)
            return false;
        e->kind = uint32_t(kind);
        e->rows = uint32_t(rows);
        e->cols = uint32_t(cols);
        e->startRow = int32_t(startRow);
        e->startCol = int32_t(startCol);

        if (op[4] >= 0)
        {
            const Md* indices = NodeAt(uint64_t(op[4]));
            if (!indices)
                return false;
            for (int64_t i : indices->operands)
            {
                int64_t v;
                if (!Int(i, &v))
                    return false;
                e->indices.push_back(uint32_t(v));
            }
        }
        if (e->indices.size() < e->rows)
            e->indices.resize(e->rows, e->indices.empty() ? 0u : e->indices.back() + 1u);

        if (op.size() > 10 && op[10] >= 0)
        {
            const Md* props = NodeAt(uint64_t(op[10]));
            if (!props)
                return false;
            for (size_t i = 0; i + 1 < props->operands.size(); i += 2)
            {
                int64_t tag, value;
                if (Int(props->operands[i], &tag) && tag == 0 && Int(props->operands[i + 1], &value))
                    e->stream = uint32_t(value);
            }
        }
        return true;
    }

    Bits m_bits;
    uint32_t m_width = 2;
    uint64_t m_end = 0;
    std::map<uint64_t, std::vector<Abbrev>> m_blockInfo;
    std::vector<Constant> m_values;
    std::vector<Md> m_md;
    std::string m_name;
    std::vector<uint64_t> m_entryPoints;
};

uint32_t Rd32(const uint8_t* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

} // namespace

bool ProgramBitcode(const uint8_t* part, size_t size, const uint8_t** bitcode, size_t* bitcodeSize)
{
    // DxilProgramHeader: ProgramVersion, SizeInUint32, then DxilBitcodeHeader: 'DXIL', DxilVersion,
    // BitcodeOffset (counted from the 'DXIL' magic), BitcodeSize.
    if (size < 24 || std::memcmp(part + 8, "DXIL", 4))
        return false;
    uint64_t offset = Rd32(part + 16);
    uint64_t length = Rd32(part + 20);
    if (offset < 16 || 8u + offset + length > size || length < 4 || std::memcmp(part + 8 + offset, "BC\xC0\xDE", 4))
        return false;
    *bitcode = part + 8 + offset;
    *bitcodeSize = size_t(length);
    return true;
}

bool ReadSignatures(const uint8_t* bitcode, size_t size, Signatures* signatures, std::string* error)
{
    *signatures = {};
    if (size < 4 || std::memcmp(bitcode, "BC\xC0\xDE", 4))
    {
        *error = "no LLVM bitcode magic";
        return false;
    }
    Reader reader(bitcode + 4, size - 4);
    if (!reader.Parse() || !reader.ReadEntry(signatures))
    {
        *error = reader.error;
        return false;
    }
    return true;
}

}
