// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
namespace native12 {
struct Adapter;
struct AdapterCapsOwner;
HRESULT get_adapter_caps(Adapter& adapter,const D3D12DDIARG_GETCAPS* request) noexcept;
void close_adapter_caps(Adapter& adapter) noexcept;
}
