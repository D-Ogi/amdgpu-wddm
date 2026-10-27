// SPDX-License-Identifier: MIT
#pragma once
#include "partial-programs.h"
#include "shader-container/dxil-metadata.h"
#include <map>
#include <utility>

namespace engine_ddi::partial {
using SemanticKey = std::pair<std::string, UINT>;
using SemanticMasks = std::map<SemanticKey, UINT>;
inline std::string linkage_name(std::string value) {
    for (auto& c : value) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return value;
}
inline bool metadata_masks(const std::vector<shader_container::dxil::Element>& elements,
        SemanticMasks& masks, bool user_only = true) {
    for (const auto& element : elements) {
        if (user_only && element.kind) continue;
        if (!element.rows || element.indices.size() != element.rows || element.startCol < 0 ||
                !element.cols || element.cols > 4 || UINT(element.startCol) + element.cols > 4)
            return false;
        const UINT mask = ((1u << element.cols) - 1u) << element.startCol;
        for (UINT row = 0; row < element.rows; ++row) {
            auto& previous = masks[{linkage_name(element.name), element.indices[row]}];
            if (previous & mask) return false;
            previous |= mask;
        }
    }
    return true;
}
inline bool output_masks(const Linkage& linkage, SemanticMasks& masks, bool user_only = false) {
    for (const auto& element : linkage.output) {
        if (element.semantic.empty() || !element.count || element.count > 4 ||
                element.start >= 4 || UINT(element.start) + element.count > 4)
            return false;
        const UINT mask = ((1u << element.count) - 1u) << element.start;
        const auto name = linkage_name(element.semantic);
        if (user_only && name.rfind("SV_", 0) == 0) continue;
        auto& previous = masks[{name, element.index}];
        if (previous & mask) return false;
        previous |= mask;
    }
    return true;
}
inline bool output_linkage_valid(const Linkage& linkage,
        const shader_container::dxil::Signatures& producer, bool mesh) {
    SemanticMasks requested, available;
    if (!output_masks(linkage, requested) || !metadata_masks(producer.output, available, false) ||
            (mesh && !metadata_masks(producer.patchConstant, available, false))) return false;
    for (const auto& [key, mask] : requested) {
        const auto found = available.find(key);
        if (found == available.end() || (mask & found->second) != mask) return false;
    }
    return true;
}
inline bool output_linkage_matches_pixel(const Linkage& linkage,
        const shader_container::dxil::Signatures& pixel) {
    SemanticMasks requested, consumed;
    return output_masks(linkage, requested, true) && metadata_masks(pixel.input, consumed) && requested == consumed;
}
inline bool preraster_linkage_matches_pixel(const Linkage& linkage,
        const shader_container::dxil::Signatures& pixel) {
    std::map<UINT, UINT> declared, consumed;
    for (const auto& e : linkage.preraster) {
        if (!e.mask || (e.mask & ~15u) || (declared[e.index] & e.mask)) return false;
        declared[e.index] |= e.mask;
    }
    for (const auto& e : pixel.input) {
        if (e.kind) continue;
        if (e.startRow < 0 || e.startCol < 0 || !e.rows || !e.cols ||
                e.cols > 4 || UINT(e.startCol) + e.cols > 4) return false;
        const UINT mask = ((1u << e.cols) - 1u) << e.startCol;
        for (UINT row = 0; row < e.rows; ++row) {
            auto& previous = consumed[UINT(e.startRow) + row];
            if (previous & mask) return false;
            previous |= mask;
        }
    }
    return declared == consumed;
}
inline bool representative_outputs_match(const shader_container::dxil::Signatures& a,
        const shader_container::dxil::Signatures& b) {
    const auto equal = [](const auto& x, const auto& y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); ++i)
            if (linkage_name(x[i].name) != linkage_name(y[i].name) || x[i].indices != y[i].indices ||
                    x[i].kind != y[i].kind || x[i].componentType != y[i].componentType ||
                    x[i].rows != y[i].rows || x[i].cols != y[i].cols || x[i].startRow != y[i].startRow ||
                    x[i].startCol != y[i].startCol || x[i].stream != y[i].stream) return false;
        return true;
    };
    return equal(a.output, b.output) && equal(a.patchConstant, b.patchConstant);
}
}
