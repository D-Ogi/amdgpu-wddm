// SPDX-License-Identifier: MIT
#include "input-layout.h"
#include <algorithm>
#include <limits>

namespace bc250::umd {
HRESULT translate_input_layout(const D3D10DDIARG_CREATEELEMENTLAYOUT &input,
    LookupVertexFormat lookup, void *userdata, InputLayout &output) {
    if (input.NumElements > vertex_attribute_limit ||
        (input.NumElements && (!input.pVertexElements || !lookup)))
        return E_INVALIDARG;
    InputLayout result;
    std::array<bool, vertex_attribute_limit> registers{};
    std::array<bool, vertex_binding_limit> slots{};
    for (unsigned i = 0; i < input.NumElements; ++i) {
        const auto &e = input.pVertexElements[i];
        if (e.InputRegister >= registers.size() || e.InputSlot >= slots.size() ||
            registers[e.InputRegister] ||
            (e.InputSlotClass != D3D10_DDI_INPUT_PER_VERTEX_DATA &&
             e.InputSlotClass != D3D10_DDI_INPUT_PER_INSTANCE_DATA) ||
            (e.InputSlotClass == D3D10_DDI_INPUT_PER_VERTEX_DATA && e.InstanceDataStepRate))
            return E_INVALIDARG;
        const VertexFormat format = lookup(userdata, e.Format);
        if (format.format == VK_FORMAT_UNDEFINED || !format.bytes ||
            e.AlignedByteOffset > std::numeric_limits<unsigned>::max() - format.bytes)
            return E_INVALIDARG;
        // Runtime DDI offsets are already resolved; UINT_MAX is not an APPEND
        // request here. Check before DXVK's packed attribute can truncate it.
        const unsigned alignment = std::min(format.bytes, 4u);
        if (e.AlignedByteOffset % alignment || e.AlignedByteOffset >= 2048u ||
            e.AlignedByteOffset + format.bytes >= 4096u)
            return E_INVALIDARG;
        auto &b = result.bindings[e.InputSlot];
        const auto rate = e.InputSlotClass == D3D10_DDI_INPUT_PER_INSTANCE_DATA
            ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        if (slots[e.InputSlot]) {
            if (b.inputRate != rate || b.divisor != e.InstanceDataStepRate)
                return E_INVALIDARG;
        } else {
            b.binding = e.InputSlot;
            b.inputRate = rate;
            b.divisor = e.InstanceDataStepRate;
            slots[e.InputSlot] = true;
        }
        b.extent = std::max(b.extent, e.AlignedByteOffset + format.bytes);
        result.attributes[result.attribute_count++] = {
            e.InputRegister, e.InputSlot, format.format, e.AlignedByteOffset};
        registers[e.InputRegister] = true;
    }
    // Compact the storage, never renumber the logical binding slots.
    for (unsigned i = 0; i < slots.size(); ++i)
        if (slots[i]) result.bindings[result.binding_count++] = result.bindings[i];
    output = result;
    return S_OK;
}
}
