// SPDX-License-Identifier: MIT
#include "input-layout-dxvk.h"
#include <cstdlib>
void verify_engine_layout(const bc250::umd::InputLayout &input) {
    bc250::umd::EngineInputLayout out;
    if (!bc250::umd::make_engine_input_layout(input,out) || out.attribute_count!=3 || out.binding_count!=2 ||
        out.attributes[0].location!=17 || out.attributes[0].binding!=31 || out.attributes[0].offset!=12 ||
        out.bindings[1].extent!=32 || out.bindings[1].binding!=31 || out.bindings[1].divisor!=0 ||
        out.bindings[1].inputRate!=VK_VERTEX_INPUT_RATE_INSTANCE) std::abort();
}
