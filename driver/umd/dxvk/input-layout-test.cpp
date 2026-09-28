// SPDX-License-Identifier: MIT
#include "input-layout.h"
#include <cstdlib>
#include <iostream>
using namespace bc250::umd;
static void require(bool ok) { if (!ok) std::abort(); }
static VertexFormat lookup(void *, DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32_FLOAT: return {VK_FORMAT_R32G32B32_SFLOAT, 12};
    case DXGI_FORMAT_R8G8B8A8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
    default: return {};
    }
}
void verify_engine_layout(const InputLayout &input);
int main() {
    D3D10DDIARG_INPUT_ELEMENT_DESC elements[] = {
        {31, 12, DXGI_FORMAT_R32G32B32_FLOAT, D3D10_DDI_INPUT_PER_INSTANCE_DATA, 0, 17},
        {2, 0, DXGI_FORMAT_R8G8B8A8_UNORM, D3D10_DDI_INPUT_PER_VERTEX_DATA, 0, 5},
        {31, 28, DXGI_FORMAT_R8G8B8A8_UNORM, D3D10_DDI_INPUT_PER_INSTANCE_DATA, 0, 9},
    };
    D3D10DDIARG_CREATEELEMENTLAYOUT in{elements, 3};
    InputLayout result;
    require(translate_input_layout(in, lookup, nullptr, result) == S_OK);
    require(result.attribute_count == 3 && result.binding_count == 2);
    require(result.attributes[0].location == 17 && result.attributes[0].binding == 31);
    require(result.bindings[0].binding == 2 && result.bindings[0].extent == 4);
    require(result.bindings[1].binding == 31 && result.bindings[1].extent == 32);
    require(result.bindings[1].divisor == 0 && result.bindings[1].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE);
    verify_engine_layout(result);
    auto reject = [&] {
        require(translate_input_layout(in, lookup, nullptr, result) == E_INVALIDARG);
        require(result.attribute_count == 3 && result.attributes[0].location == 17);
    };
    elements[2].InstanceDataStepRate = 2; reject(); elements[2].InstanceDataStepRate = 0;
    elements[2].InputRegister = 17; reject(); elements[2].InputRegister = 9;
    elements[0].AlignedByteOffset = UINT_MAX; reject();
    elements[0].AlignedByteOffset = 2048; reject();
    elements[0].AlignedByteOffset = 2; reject(); elements[0].AlignedByteOffset = 12;
    elements[0].InputRegister = 32; reject(); elements[0].InputRegister = 17;
    elements[2].InputSlotClass = D3D10_DDI_INPUT_PER_VERTEX_DATA; reject();
    elements[2].InputSlotClass = D3D10_DDI_INPUT_PER_INSTANCE_DATA;
    elements[1].InputSlot = 32; reject(); elements[1].InputSlot = 2;
    elements[1].Format = DXGI_FORMAT_UNKNOWN; reject(); elements[1].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    elements[1].InstanceDataStepRate = 1; reject(); elements[1].InstanceDataStepRate = 0;
    in.NumElements = 33; reject(); in.NumElements = 3;
    in.pVertexElements = nullptr; reject();
    in.NumElements = 0;
    require(translate_input_layout(in, nullptr, nullptr, result) == S_OK);
    require(!result.attribute_count && !result.binding_count);
    std::cout << "PASS DDI input layout: sparse slots/registers, instance divisor zero, bounds, transactional failure\n";
}
