// SPDX-License-Identifier: MIT
// Offline oracle: original DXC containers versus reconstruction from whole containers and raw DXIL parts.
#include "../shader-container.h"
#include "../dxil-metadata.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace sc = engine_ddi::shader_container;
namespace {
using Bytes = std::vector<uint8_t>;
uint32_t read32(const Bytes& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) throw std::runtime_error("truncated DWORD");
    uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
}
struct Signature {
    std::string name;
    // Index, system value, component type, register, mask, stream and minimum precision.
    std::array<uint32_t, 7> fields;
    bool operator==(const Signature&) const = default;
};
struct Container {
    std::map<std::string, std::vector<Signature>> signatures;
    std::vector<UINT> program;
};
Container parse(const Bytes& bytes) {
    if (bytes.size() < 32 || std::memcmp(bytes.data(), "DXBC", 4) || read32(bytes, 24) != bytes.size())
        throw std::runtime_error("invalid container");
    Container result;
    for (UINT i = 0; i < read32(bytes, 28); ++i) {
        const size_t offset = read32(bytes, 32 + 4ull * i);
        const size_t size = read32(bytes, offset + 4), base = offset + 8;
        if (base > bytes.size() || size > bytes.size() - base) throw std::runtime_error("invalid part bounds");
        const std::string tag(reinterpret_cast<const char*>(bytes.data() + offset), 4);
        if (tag == "DXIL") {
            if (size % 4 || !result.program.empty()) throw std::runtime_error("invalid DXIL part");
            result.program.resize(size / 4); std::memcpy(result.program.data(), bytes.data() + base, size);
        }
        if (tag != "ISG1" && tag != "OSG1" && tag != "PSG1") continue;
        auto& signature = result.signatures[tag];
        const UINT count = read32(bytes, base);
        if (size < 8 || count > (size - 8) / 32) throw std::runtime_error("invalid signature bounds");
        for (UINT j = 0; j < count; ++j) {
            const size_t row = base + 8 + 32ull * j, name = base + read32(bytes, row + 4);
            if (name >= base + size) throw std::runtime_error("invalid semantic name");
            const auto end = std::find(bytes.begin() + name, bytes.begin() + base + size, uint8_t{0});
            if (end == bytes.begin() + base + size) throw std::runtime_error("unterminated semantic name");
            std::string text(bytes.begin() + name, end);
            for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            signature.push_back({text, {read32(bytes, row + 8), read32(bytes, row + 12), read32(bytes, row + 16),
                read32(bytes, row + 20), bytes[row + 24], read32(bytes, row), read32(bytes, row + 28)}});
        }
    }
    if (result.program.empty()) throw std::runtime_error("missing DXIL part");
    return result;
}
}
int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: generic-signature-test <original.cso>...\n"); return 2; }
    unsigned checks = 0;
    try {
        for (int file = 1; file < argc; ++file) {
            std::ifstream stream(argv[file], std::ios::binary);
            const Bytes bytes{std::istreambuf_iterator<char>(stream), {}};
            const auto original = parse(bytes);
            std::vector<UINT> whole((bytes.size() + 3) / 4); std::memcpy(whole.data(), bytes.data(), bytes.size());
            const std::vector<UINT>* payloads[] = {&whole, &original.program};
            for (const auto* payload : payloads) {
                sc::Container output; sc::dxil::Signatures metadata;
                const auto status = sc::BuildGenericContainer(payload->data(), payload->size(), &output, &metadata);
                if (!status) throw std::runtime_error(status.detail);
                const auto rebuilt = parse(output.bytes);
                if (original.signatures != rebuilt.signatures || original.program != rebuilt.program)
                    throw std::runtime_error(std::string(argv[file]) + ": signature or DXIL bytes differ");
                ++checks;
            }
            std::printf("PASS %s (whole container and raw DXIL)\n", argv[file]);
        }
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
    std::printf("PASS %u signature reconstructions; no GPU or rendering claim\n", checks);
    return 0;
}
