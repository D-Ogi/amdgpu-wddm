// SPDX-License-Identifier: MIT
#pragma once
#include "input-layout-data.h"
#include "dxvk/dxvk_constant_state.h"
namespace bc250::umd {
static_assert(vertex_attribute_limit == dxvk::MaxNumVertexAttributes);
static_assert(vertex_binding_limit == dxvk::MaxNumVertexBindings);
struct EngineInputLayout {
    std::array<dxvk::DxvkVertexAttribute, vertex_attribute_limit> attributes{};
    std::array<dxvk::DxvkVertexBinding, vertex_binding_limit> bindings{};
    unsigned attribute_count = 0, binding_count = 0;
};
inline bool make_engine_input_layout(const InputLayout &input, EngineInputLayout &out) {
    if (input.attribute_count > vertex_attribute_limit || input.binding_count > vertex_binding_limit)
        return false;
    EngineInputLayout result;
    result.attribute_count = input.attribute_count;
    result.binding_count = input.binding_count;
    for (unsigned i=0; i<input.attribute_count; ++i) {
        const auto &a=input.attributes[i];
        result.attributes[i]={a.location,a.binding,a.format,a.offset};
    }
    for (unsigned i=0; i<input.binding_count; ++i) {
        const auto &b=input.bindings[i];
        result.bindings[i]={b.binding,b.extent,b.inputRate,b.divisor};
    }
    out=result;
    return true;
}
}
