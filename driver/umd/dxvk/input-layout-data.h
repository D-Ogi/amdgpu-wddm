// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <vulkan/vulkan.h>
namespace bc250::umd {
// No WDK or DXVK types cross this internal translation boundary.
constexpr unsigned vertex_attribute_limit = 32;
constexpr unsigned vertex_binding_limit = 32;
struct VertexAttribute { unsigned location, binding; VkFormat format; unsigned offset; };
struct VertexBinding { unsigned binding, extent; VkVertexInputRate inputRate; unsigned divisor; };
struct InputLayout {
    std::array<VertexAttribute, vertex_attribute_limit> attributes{};
    std::array<VertexBinding, vertex_binding_limit> bindings{};
    unsigned attribute_count = 0;
    unsigned binding_count = 0;
};
}
