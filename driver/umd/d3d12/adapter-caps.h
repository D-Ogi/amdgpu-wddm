// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
#include <vulkan/vulkan_core.h>
#include "bc250_vkd3d_engine.h"
namespace native12 {
struct Adapter;
struct AdapterCapsOwner;
// Borrowed entry points. Adapter ownership pins both sibling DLLs until every
// device is destroyed and CloseAdapter releases the caps owner. The caller must
// install its own device bootstrap; the query-only GIPA must never create devices.
struct AdapterEngineAccess {
    BC250_VKD3D_ENGINE_FUNCS functions{};
    PFN_vkGetInstanceProcAddr driver_entry{};
};
HRESULT get_adapter_engine(Adapter& adapter, AdapterEngineAccess* access) noexcept;
HRESULT get_adapter_caps(Adapter& adapter,const D3D12DDIARG_GETCAPS* request) noexcept;
void close_adapter_caps(Adapter& adapter) noexcept;
}
