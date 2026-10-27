// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// PROVENANCE: Microsoft DirectX-Specs, MIT, PartialGraphicsPrograms.md, DDI 0121.
// Local ABI declarations until the build's WDK defines these structures.
// OutputElement follows the pinned Agility 1.721.3 DDI, not the newer spec's
// IsPrimitive extension: its converter emits 16-byte x64 elements, with fields
// at 0/8/12/13. PrerasterElement is the converted register/mask pair (8 bytes).
// No capability is enabled by these declarations.
namespace engine_ddi::partial {
enum class ProgramType : UINT { None, Prerasterization, Pixel };
struct Description {
    LPCWSTR name;
    UINT export_count;
    LPCWSTR* exports;
    UINT subobject_count;
    const D3D12DDI_STATE_SUBOBJECT_0054* const* subobjects;
    ProgramType type;
};
struct OutputElement {
    LPCSTR semantic;
    UINT index;
    BYTE start;
    BYTE count;
};
struct OutputSignature { const OutputElement* elements; UINT count; };
struct PrerasterElement { UINT index; BYTE mask; };
struct PrerasterSignature { const PrerasterElement* elements; UINT count; };
struct PrerasterFields { BOOL exclude_ps; BOOL late_input_layout; };
struct PixelFields {
    D3D12DDI_LINE_RASTERIZATION_MODE line_mode;
    UINT forced_samples;
    BOOL alpha_to_coverage;
    BOOL dual_source_blend;
    BOOL late_rasterizer;
    BOOL late_blend;
    BOOL late_sample_mask;
    BOOL late_sample_desc;
    BOOL late_depth_format;
    BOOL late_rt_formats;
    BOOL late_depth_stencil;
};
static_assert(sizeof(ProgramType) == 4 && sizeof(PrerasterElement) == 8);
static_assert(sizeof(PrerasterFields) == 8 && sizeof(PixelFields) == 44);
static_assert(offsetof(OutputElement, start) == sizeof(void*) + 4);
static_assert(sizeof(OutputElement) == (sizeof(void*) == 8 ? 16 : 12));
static_assert(sizeof(Description) == (sizeof(void*) == 8 ? 48 : 24));

// Collection input is caller-owned. Store strings and values, never borrowed
// semantic-name pointers, so later links remain valid after the caller returns.
struct OwnedOutputElement {
    std::string semantic;
    UINT index;
    BYTE start;
    BYTE count;
};
struct Linkage {
    std::vector<OwnedOutputElement> output;
    std::vector<PrerasterElement> preraster;
    static constexpr UINT bound = 65536;

    HRESULT capture(const OutputSignature& input) {
        if (input.count > bound || (input.count && !input.elements)) return E_INVALIDARG;
        std::vector<OwnedOutputElement> next;
        next.reserve(input.count);
        for (UINT i = 0; i < input.count; ++i) {
            const auto& e = input.elements[i];
            if (!e.semantic) return E_INVALIDARG;
            const size_t length = strnlen(e.semantic, bound);
            if (!length || length == bound) return E_INVALIDARG;
            next.push_back({std::string(e.semantic, length), e.index, e.start, e.count});
        }
        output.swap(next);
        return S_OK;
    }
    HRESULT capture(const PrerasterSignature& input) {
        if (input.count > bound || (input.count && !input.elements)) return E_INVALIDARG;
        std::vector<PrerasterElement> next;
        if (input.count) next.assign(input.elements, input.elements + input.count);
        preraster.swap(next);
        return S_OK;
    }
};

// Resolve subobject references during creation. Only stable indices and owned
// names survive this operation; no pointer into the application's array escapes.
struct OwnedDescription {
    std::wstring name;
    std::vector<std::wstring> exports;
    std::vector<UINT> subobjects;
    ProgramType type = ProgramType::None;

    HRESULT capture(const Description& input, const D3D12DDI_STATE_SUBOBJECT_0054* local, UINT count) {
        if (input.type != ProgramType::Prerasterization && input.type != ProgramType::Pixel) return E_INVALIDARG;
        if (count > Linkage::bound || (count && !local) || !input.name ||
            !input.export_count || input.export_count > Linkage::bound || !input.exports ||
            input.subobject_count > Linkage::bound || (input.subobject_count && !input.subobjects)) return E_INVALIDARG;
        const auto length = wcsnlen(input.name, Linkage::bound);
        if (!length || length == Linkage::bound) return E_INVALIDARG;
        OwnedDescription next;
        next.name.assign(input.name, length);
        next.type = input.type;
        next.exports.reserve(input.export_count);
        for (UINT i = 0; i < input.export_count; ++i) {
            if (!input.exports[i]) return E_INVALIDARG;
            const auto n = wcsnlen(input.exports[i], Linkage::bound);
            if (!n || n == Linkage::bound) return E_INVALIDARG;
            next.exports.emplace_back(input.exports[i], n);
        }
        next.subobjects.reserve(input.subobject_count);
        const uintptr_t base = reinterpret_cast<uintptr_t>(local);
        for (UINT i = 0; i < input.subobject_count; ++i) {
            const uintptr_t address = reinterpret_cast<uintptr_t>(input.subobjects[i]);
            if (!local || address < base) return E_INVALIDARG;
            const uintptr_t delta = address - base;
            if (delta % sizeof(*local) || delta / sizeof(*local) >= count) return E_INVALIDARG;
            next.subobjects.push_back(static_cast<UINT>(delta / sizeof(*local)));
        }
        name.swap(next.name); exports.swap(next.exports); subobjects.swap(next.subobjects);
        type = next.type;
        return S_OK;
    }
};
}
