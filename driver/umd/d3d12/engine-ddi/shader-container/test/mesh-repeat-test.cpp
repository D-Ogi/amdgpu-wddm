// SPDX-License-Identifier: MIT
// Offline gate for BD-286: a DXIL signature whose rows repeat one element on one register. No engine or GPU.
//
// An older dxc writes `out vertices float4 v[4] : SV_POSITION` as one metadata element of one row with the semantic
// indices {0, 1, 2, 3} and one OSG1 row per index, all on register 0 with the same mask; the runtime passes those
// rows as DDI entries. BuildContainer and BuildMeshLinkContainer must keep the first row of such a run, because the
// metadata proves it is one element, and refuse equal entries that the metadata does not prove and an entry that
// shares only some components with another.
//
// The DDI form is this test's model of the runtime (as in shader-container-test.cpp): one entry per signature row,
// the system value from the row, SV_Target and other names without a tokenized-format value as UNDEFINED.
//
// Usage: mesh-repeat-test [--ms <mesh.cso>]... [--ps <pixel.cso>]... [--gs <geometry.cso>]... [--any <cso>]...
//   --ms  the original container of a mesh program: its rows as the runtime passes them must give OSG1 = the
//         original registered rows without repeats (the SDK dxc form unchanged) and PSG1 = the original registered
//         rows; four more copies of row 0 and a copy of row 0 on fewer components must be refused.
//   --ps  the original container of a pixel program of a mesh pipeline: its input rows must give ISG1 = the
//         original registered rows; four more copies of row 0 and a partial copy must be refused.
//   --any a DXIL program on its own slot (sweep): an "overlaps entry" refusal fails, other refusals are notes,
//         DXBC containers and libraries are skipped.
//   --ms-any, --ps-any  the same, but a program without such rows is skipped, not failed (for a sweep over the
//         programs of a test suite).
//   --gs  a geometry program with two streams of one layout: with every DDI stream 0 its rows are equal, and the
//         rebuilt OSG1 must still have every original row (no merge).
#include "../shader-container.h"
#include "../dxil-metadata.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace sc = engine_ddi::shader_container;
using Entry = D3D12DDIARG_SIGNATURE_ENTRY_0012;
using Bytes = std::vector<uint8_t>;

namespace {

unsigned g_checks, g_failures;

void Check(bool ok, const std::string& what)
{
    g_checks++;
    if (!ok)
        g_failures++;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

uint32_t Rd32(const Bytes& b, size_t offset)
{
    if (offset > b.size() || b.size() - offset < 4)
        throw std::runtime_error("truncated DWORD");
    uint32_t v;
    std::memcpy(&v, b.data() + offset, 4);
    return v;
}

struct Row
{
    std::string name;   // lower case: dxc and the rebuild may differ in case only
    uint32_t index, sysval, type, reg, mask, stream, precision;
    bool operator==(const Row&) const = default;
};

struct Parsed
{
    std::vector<Row> isg, osg, psg;
    std::vector<UINT> program;
};

Parsed Parse(const Bytes& b)
{
    if (b.size() < 32 || std::memcmp(b.data(), "DXBC", 4) || Rd32(b, 24) != b.size())
        throw std::runtime_error("not a container");
    Parsed p;
    for (uint32_t i = 0; i < Rd32(b, 28); i++)
    {
        const size_t off = Rd32(b, 32 + 4ull * i), size = Rd32(b, off + 4), base = off + 8;
        if (base > b.size() || size > b.size() - base)
            throw std::runtime_error("part out of bounds");
        const std::string tag(reinterpret_cast<const char*>(b.data() + off), 4);
        if (tag == "DXIL")
        {
            if (size % 4)
                throw std::runtime_error("DXIL part size");
            p.program.resize(size / 4);
            std::memcpy(p.program.data(), b.data() + base, size);
            continue;
        }
        std::vector<Row>* rows = tag == "ISG1" ? &p.isg : tag == "OSG1" ? &p.osg : tag == "PSG1" ? &p.psg : nullptr;
        if (!rows)
            continue;
        const uint32_t count = Rd32(b, base);
        if (size < 8 || count > (size - 8) / 32)
            throw std::runtime_error(tag + " out of bounds");
        for (uint32_t j = 0; j < count; j++)
        {
            const size_t row = base + 8 + 32ull * j, name = base + Rd32(b, row + 4);
            const auto end = std::find(b.begin() + std::min(name, base + size), b.begin() + base + size, uint8_t{0});
            if (name >= base + size || end == b.begin() + base + size)
                throw std::runtime_error(tag + " name");
            std::string text(b.begin() + name, end);
            for (char& c : text)
                if (c >= 'A' && c <= 'Z')
                    c = char(c + ('a' - 'A'));
            rows->push_back({ text, Rd32(b, row + 8), Rd32(b, row + 12), Rd32(b, row + 16), Rd32(b, row + 20),
                    uint32_t(b[row + 24]), Rd32(b, row), Rd32(b, row + 28) });
        }
    }
    if (p.program.empty())
        throw std::runtime_error("no DXIL part");
    return p;
}

Bytes Load(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error(std::string("cannot read ") + path);
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

std::vector<Entry> ToDdi(const std::vector<Row>& rows)
{
    std::vector<Entry> out;
    for (const Row& r : rows)
    {
        Entry e = {};
        // D3D_NAME and D3D10_SB_NAME have the same values in these two ranges (d3dcommon.h); SV_Target (64) and
        // the other pixel output names have no tokenized value.
        bool tokenized = r.sysval <= uint32_t(D3D10_SB_NAME_SAMPLE_INDEX)
            || (r.sysval >= uint32_t(D3D12_SB_NAME_BARYCENTRICS) && r.sysval <= uint32_t(D3D12_SB_NAME_CULLPRIMITIVE));
        e.SystemValue = tokenized ? D3D10_SB_NAME(r.sysval) : D3D10_SB_NAME_UNDEFINED;
        e.Register = r.reg;
        e.Mask = BYTE(r.mask & 0xf);
        e.Stream = BYTE(r.stream);
        e.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_TYPE(r.type);
        e.MinPrecision = D3D11_SB_OPERAND_MIN_PRECISION(r.precision);
        out.push_back(e);
    }
    return out;
}

// The rows a rebuilt chunk can hold: BuildContainer writes no row for a registerless entry (SV_CullPrimitive,
// depth, coverage), as shader-container-test.cpp compares (Registered).
std::vector<Row> Registered(const std::vector<Row>& rows)
{
    std::vector<Row> out;
    std::copy_if(rows.begin(), rows.end(), std::back_inserter(out), [](const Row& r) { return r.reg != ~0u; });
    return out;
}

// The registered rows without repeats: a row equal to an earlier one in everything but the semantic index is
// dropped.
std::vector<Row> WithoutRepeats(const std::vector<Row>& rows)
{
    std::vector<Row> out;
    for (const Row& r : Registered(rows))
        if (std::none_of(out.begin(), out.end(), [&](const Row& o) {
                return o.reg == r.reg && o.mask == r.mask && o.stream == r.stream && o.sysval == r.sysval
                    && o.type == r.type && o.precision == r.precision; }))
            out.push_back(r);
    return out;
}

// A copy of entry 0 without its lowest component: a partial overlap, which must stay refused. An entry of one
// component has no partial overlap; the caller skips it.
Entry PartialCopy(const Entry& e)
{
    Entry p = e;
    p.Mask = BYTE(e.Mask & (e.Mask - 1));
    return p;
}

bool OneComponent(const Entry& e) { return !(e.Mask & (e.Mask - 1)); }

// No rows to merge: an error for a gate fixture (required), a note for a program of a sweep.
bool HasRows(size_t count, bool required, const std::string& what)
{
    if (count || required)
        Check(count != 0, what);
    else
        std::printf("skip  %s\n", what.c_str());
    return count != 0;
}

void Mesh(const char* path, bool required)
{
    const Bytes bytes = Load(path);
    const Parsed original = Parse(bytes);
    const std::vector<Entry> output = ToDdi(original.osg), patch = ToDdi(original.psg);
    const std::vector<Row> expected = WithoutRepeats(original.osg);
    const std::string what = std::string(path) + ": ";
    if (!HasRows(output.size(), required, what + "the mesh program has vertex output rows (" + std::to_string(output.size())
            + ", " + std::to_string(Registered(original.osg).size() - expected.size()) + " repeated)"))
        return;

    auto build = [&](const std::vector<Entry>& out, sc::Container* c) {
        sc::ProgramDesc d;
        d.code = original.program.data();
        d.codeCapacity = original.program.size();
        d.slot = sc::Slot::Mesh;
        d.output = { out.data(), UINT(out.size()) };
        d.patchConstant = { patch.data(), UINT(patch.size()) };
        return sc::BuildContainer(d, c);
    };

    {
        sc::Container c;
        const sc::Result r = build(output, &c);
        Check(bool(r), what + "rows as the runtime passes them: BuildContainer" + (r ? "" : " refused: " + r.detail));
        if (r)
        {
            const Parsed rebuilt = Parse(c.bytes);
            Check(rebuilt.osg == expected, what + "OSG1 = the original rows without repeats ("
                    + std::to_string(rebuilt.osg.size()) + " of " + std::to_string(expected.size()) + ", first "
                    + (rebuilt.osg.empty() ? std::string("none") : rebuilt.osg[0].name + std::to_string(rebuilt.osg[0].index)) + ")");
            Check(rebuilt.psg == Registered(original.psg), what + "PSG1 = the original registered PSG1 rows");
            Check(c.output.size() == expected.size(), what + "container output entries without repeats");
            Check(rebuilt.program == original.program, what + "DXIL part unchanged");
        }
    }

    // One copy more than the metadata proves: two entries on the same components with no element of that many
    // indices, so two different elements would get one name. Refused.
    std::vector<Entry> repeated = output;
    for (UINT i = 0; i < 4; i++)
        repeated.push_back(output[0]);
    {
        sc::Container c;
        const sc::Result r = build(repeated, &c);
        Check(r.status == sc::Status::InvalidArgument, what + "four copies of row 0 more than the metadata proves are refused ("
                + (r ? std::string("accepted") : r.detail) + ")");
    }

    if (OneComponent(output[0]))
    {
        std::printf("skip  %sa partial overlap of row 0 (one component)\n", what.c_str());
        return;
    }
    std::vector<Entry> partial = output;
    partial.push_back(PartialCopy(output[0]));
    sc::Container c;
    const sc::Result r = build(partial, &c);
    Check(r.status == sc::Status::InvalidArgument,
            what + "a copy of row 0 on fewer components is refused (" + (r ? std::string("accepted") : r.detail) + ")");
}

void Pixel(const char* path, bool required)
{
    const Bytes bytes = Load(path);
    const Parsed original = Parse(bytes);
    const std::string what = std::string(path) + ": ";
    std::vector<Entry> input = ToDdi(original.isg);
    const std::vector<Entry> output = ToDdi(original.osg);
    if (!HasRows(input.size(), required, what + "the pixel program has input rows (" + std::to_string(input.size()) + ")"))
        return;
    sc::Container source;
    source.bytes = bytes;
    source.dxil = true;

    sc::Container c;
    sc::Result r = sc::BuildMeshLinkContainer(source, { input.data(), UINT(input.size()) },
            { output.data(), UINT(output.size()) }, &c);
    Check(bool(r), what + "input rows as the runtime passes them: BuildMeshLinkContainer" + (r ? "" : " refused: " + r.detail));
    if (r)
        Check(Parse(c.bytes).isg == WithoutRepeats(original.isg), what + "ISG1 = the original rows without repeats");

    // A copy of row 0 that the metadata does not prove (the element has one index): refused.
    std::vector<Entry> repeated = input;
    for (UINT i = 0; i < 4; i++)
        repeated.push_back(input[0]);
    r = sc::BuildMeshLinkContainer(source, { repeated.data(), UINT(repeated.size()) },
            { output.data(), UINT(output.size()) }, &c);
    Check(r.status == sc::Status::InvalidArgument, what + "pixel input copies of row 0 that the metadata does not prove are refused ("
            + (r ? std::string("accepted") : r.detail) + ")");

    if (OneComponent(input[0]))
    {
        std::printf("skip  %sa partial overlap of pixel input row 0 (one component)\n", what.c_str());
        return;
    }
    std::vector<Entry> partial = input;
    partial.push_back(PartialCopy(input[0]));
    r = sc::BuildMeshLinkContainer(source, { partial.data(), UINT(partial.size()) },
            { output.data(), UINT(output.size()) }, &c);
    Check(r.status == sc::Status::InvalidArgument,
            what + "a pixel input copy of row 0 on fewer components is refused (" + (r ? std::string("accepted") : r.detail) + ")");
}

// A geometry program with streams: the DDI form with every stream 0 (the runtime may not fill Stream) has rows
// that are equal in DDI form, and they must not merge. The rebuilt OSG1 must have every original row, streams
// derived.
void Geometry(const char* path)
{
    const Bytes bytes = Load(path);
    const Parsed original = Parse(bytes);
    const std::string what = std::string(path) + ": ";
    std::vector<Entry> output = ToDdi(original.osg), input = ToDdi(original.isg);
    for (Entry& e : output)
        e.Stream = 0;
    const size_t distinct = WithoutRepeats(original.osg).size();
    std::vector<Row> zeroed = original.osg;
    for (Row& r : zeroed)
        r.stream = 0;
    Check(WithoutRepeats(zeroed).size() < Registered(original.osg).size(),
            what + "rows of two streams are equal with stream 0 (" + std::to_string(distinct) + " rows)");
    sc::ProgramDesc d;
    d.code = original.program.data();
    d.codeCapacity = original.program.size();
    d.input = { input.data(), UINT(input.size()) };
    d.output = { output.data(), UINT(output.size()) };
    sc::Container c;
    const sc::Result r = sc::BuildContainer(d, &c);
    Check(bool(r), what + "BuildContainer with every stream 0" + (r ? "" : " refused: " + r.detail));
    if (r)
        Check(Parse(c.bytes).osg == Registered(original.osg), what + "OSG1 = every original row, streams derived");
}

// The merge rule on its own, with metadata elements as an older dxc and the SDK dxc write them. The SDK dxc of this
// gate cannot write the older form, so the end-to-end case of the exact older bytes is a sweep (--ms-any) over the
// compiled shaders of a test suite.
void Rule()
{
    auto element = [](const char* name, std::vector<uint32_t> indices, uint32_t rows, int32_t row, int32_t col,
            uint32_t cols, uint32_t stream = 0) {
        engine_ddi::shader_container::dxil::Element e;
        e.name = name;
        e.indices = std::move(indices);
        e.rows = rows;
        e.startRow = row;
        e.startCol = col;
        e.cols = cols;
        e.stream = stream;
        return e;
    };
    auto entry = [](UINT reg, BYTE mask, BYTE stream = 0) {
        Entry e = {};
        e.SystemValue = D3D10_SB_NAME_POSITION;
        e.Register = reg;
        e.Mask = mask;
        e.Stream = stream;
        e.RegisterComponentType = D3D10_SB_REGISTER_COMPONENT_FLOAT32;
        return e;
    };
    struct Case
    {
        const char* what;
        std::vector<engine_ddi::shader_container::dxil::Element> elements;
        std::vector<Entry> entries;
        size_t expected;
    };
    const auto oldForm = element("SV_Position", { 0, 1, 2, 3 }, 1, 0, 0, 4);
    const std::vector<Case> cases = {
        { "older dxc: one row, indices 0 to 3, four equal entries -> one", { oldForm },
            { entry(0, 0xf), entry(0, 0xf), entry(0, 0xf), entry(0, 0xf) }, 1 },
        { "older dxc: a fifth equal entry has no index -> kept", { oldForm },
            { entry(0, 0xf), entry(0, 0xf), entry(0, 0xf), entry(0, 0xf), entry(0, 0xf) }, 2 },
        { "SDK dxc: one index, two equal entries -> both kept", { element("SV_Position", { 0 }, 1, 0, 0, 4) },
            { entry(0, 0xf), entry(0, 0xf) }, 2 },
        { "two elements on the same components -> both kept",
            { element("TEXCOORD", { 0 }, 1, 1, 0, 4), element("COLOR", { 0 }, 1, 1, 0, 4) },
            { entry(1, 0xf), entry(1, 0xf) }, 2 },
        { "element on other components than the entries -> kept", { element("TEXCOORD", { 0, 1 }, 1, 0, 0, 2) },
            { entry(0, 0xf), entry(0, 0xf) }, 2 },
        { "element of another stream -> kept", { element("TEXCOORD", { 0, 1 }, 1, 0, 0, 4, 1) },
            { entry(0, 0xf), entry(0, 0xf) }, 2 },
        { "element of two rows -> kept", { element("TEXCOORD", { 0, 1 }, 2, 0, 0, 4) },
            { entry(0, 0xf), entry(0, 0xf) }, 2 },
        { "registerless entries -> kept", { oldForm }, { entry(~0u, 0x1), entry(~0u, 0x1) }, 2 },
        { "no repeat -> unchanged", { oldForm }, { entry(0, 0xf), entry(1, 0x3) }, 2 },
    };
    for (const Case& c : cases)
    {
        std::vector<Entry> out;
        const bool dropped = sc::MergeRepeatedSignatureEntries({ c.entries.data(), UINT(c.entries.size()) }, c.elements, &out);
        const size_t got = dropped ? out.size() : c.entries.size();
        Check(got == c.expected && dropped == (c.expected < c.entries.size()),
                std::string("rule: ") + c.what + " (" + std::to_string(got) + ")");
    }
}

// Sweep: any DXIL program on its own slot with its rows as the runtime passes them. An "overlaps entry" refusal is a
// failure (the BD-286 class); another refusal is printed as a note. DXBC containers and DXIL libraries are skipped.
struct SweepCounts
{
    unsigned accepted = 0, overlap = 0, other = 0, skipped = 0;
};
SweepCounts g_sweep;

void Any(const char* path)
{
    const Bytes bytes = Load(path);
    bool dxil = false;
    if (bytes.size() >= 32 && !std::memcmp(bytes.data(), "DXBC", 4))
        for (uint32_t i = 0; i < Rd32(bytes, 28); i++)
        {
            const size_t off = Rd32(bytes, 32 + 4ull * i);
            dxil = dxil || (off + 4 <= bytes.size() && !std::memcmp(bytes.data() + off, "DXIL", 4));
        }
    if (!dxil)
    {
        g_sweep.skipped++;
        return;
    }
    const Parsed original = Parse(bytes);
    const uint32_t kind = (original.program[0] >> 16) & 0xffffu;
    if (kind > 5 && kind != sc::kDxilProgramKindMesh && kind != sc::kDxilProgramKindAmplification)
    {
        g_sweep.skipped++;
        return;
    }
    const std::vector<Entry> input = ToDdi(original.isg), output = ToDdi(original.osg), patch = ToDdi(original.psg);
    sc::ProgramDesc d;
    d.code = original.program.data();
    d.codeCapacity = original.program.size();
    d.slot = kind == sc::kDxilProgramKindMesh ? sc::Slot::Mesh
        : kind == sc::kDxilProgramKindAmplification ? sc::Slot::Amplification : sc::Slot::Standard;
    if (d.slot != sc::Slot::Amplification)
    {
        d.input = { input.data(), UINT(input.size()) };
        d.output = { output.data(), UINT(output.size()) };
        d.patchConstant = { patch.data(), UINT(patch.size()) };
    }
    sc::Container c;
    const sc::Result r = sc::BuildContainer(d, &c);
    const std::string what = std::string(path) + " (kind " + std::to_string(kind) + ")";
    if (r)
    {
        g_sweep.accepted++;
        const Parsed rebuilt = Parse(c.bytes);
        Check(rebuilt.osg == WithoutRepeats(original.osg), what + ": OSG1 = the original rows without repeats");
    }
    else if (r.detail.find("overlaps entry") != std::string::npos)
    {
        g_sweep.overlap++;
        Check(false, what + ": refused: " + r.detail);
    }
    else
    {
        g_sweep.other++;
        std::printf("note  %s: refused: %s\n", what.c_str(), r.detail.c_str());
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: mesh-repeat-test [--ms <mesh.cso>]... [--ps <pixel.cso>]...\n");
        return 2;
    }
    try
    {
        Rule();
        for (int i = 1; i + 1 < argc; i += 2)
        {
            if (!std::strcmp(argv[i], "--ms") || !std::strcmp(argv[i], "--ms-any"))
                Mesh(argv[i + 1], !std::strcmp(argv[i], "--ms"));
            else if (!std::strcmp(argv[i], "--ps") || !std::strcmp(argv[i], "--ps-any"))
                Pixel(argv[i + 1], !std::strcmp(argv[i], "--ps"));
            else if (!std::strcmp(argv[i], "--gs"))
                Geometry(argv[i + 1]);
            else if (!std::strcmp(argv[i], "--any"))
                Any(argv[i + 1]);
            else
                throw std::runtime_error(std::string("unknown argument ") + argv[i]);
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    if (g_sweep.accepted + g_sweep.overlap + g_sweep.other + g_sweep.skipped)
        std::printf("sweep: %u accepted, %u refused with an overlap, %u refused otherwise, %u skipped (DXBC or library)\n",
                g_sweep.accepted, g_sweep.overlap, g_sweep.other, g_sweep.skipped);
    std::printf("%s (%u checks, %u failed); no GPU or rendering claim\n", g_failures ? "FAILED" : "PASSED", g_checks,
            g_failures);
    return g_failures ? 1 : 0;
}
