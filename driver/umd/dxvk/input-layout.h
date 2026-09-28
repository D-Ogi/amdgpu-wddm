// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <winternl.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <d3d10umddi.h>
#include "input-layout-data.h"
#include <array>

namespace bc250::umd {
struct VertexFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    unsigned bytes = 0;
};
// Use the engine's format lookup and vertex-buffer feature checks. This adapter
// does not create a second DXGI-to-Vulkan format table.
using LookupVertexFormat = VertexFormat (*)(void *, DXGI_FORMAT);
HRESULT translate_input_layout(const D3D10DDIARG_CREATEELEMENTLAYOUT &input,
    LookupVertexFormat lookup, void *userdata, InputLayout &output);
}
