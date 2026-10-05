// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12umddi.h>
namespace native12 {
struct Adapter;
HRESULT fill_native_tables(Adapter&,D3D12DDI_TABLE_TYPE,void*,SIZE_T,UINT,D3D12DDI_HRTTABLE) noexcept;
}
