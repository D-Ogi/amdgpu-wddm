// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstring>

namespace engine_ddi::byte_offset_view {
struct BufferSrv {
    UINT64 offset;
    UINT64 size;
    UINT stride;
    D3D12DDI_BUFFER_SRV_FLAGS flags;
};
static_assert(sizeof(BufferSrv) == 24 && offsetof(BufferSrv, stride) == 16);
inline BufferSrv read(const D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002* args) noexcept {
    BufferSrv b{};
    std::memcpy(&b, reinterpret_cast<const unsigned char*>(args) +
        offsetof(D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002, Buffer), sizeof(b));
    return b;
}
inline D3D12_BUFFER_SRV_BYTE_OFFSET translate(const BufferSrv& b) noexcept {
    return {b.offset, b.size, b.stride, static_cast<D3D12_BUFFER_SRV_FLAGS>(b.flags)};
}
// PROVENANCE: Microsoft DirectX-Specs, MIT, D3D12RevisedCreateViews.md, DDI 0121.
// The pinned WDK stops at 0119. Runtime 1.721.3 uses this payload at DDI 0122;
// the callback slot is unchanged, but dimension 7 selects a different union arm.
struct BufferUav {
    D3D12DDI_HRESOURCE counter;
    UINT64 offset;
    UINT64 size;
    UINT64 counter_offset;
    UINT stride;
    D3D12DDI_BUFFER_UAV_FLAGS flags;
};
struct CreateUav {
    D3D12DDI_HRESOURCE resource;
    DXGI_FORMAT format;
    D3D12DDI_RESOURCE_DIMENSION dimension;
    BufferUav buffer;
};
static_assert(offsetof(CreateUav, buffer) == offsetof(D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002, Buffer));
static_assert(offsetof(BufferUav, offset) == 8 && offsetof(BufferUav, size) == 16);
static_assert(offsetof(BufferUav, counter_offset) == 24 && offsetof(BufferUav, stride) == 32);
static_assert(sizeof(BufferUav) == 40);

inline BufferUav read(const D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002* args) noexcept {
    BufferUav b{};
    // Copy the extended union payload without aliasing it through an incompatible struct type.
    std::memcpy(&b, reinterpret_cast<const unsigned char*>(args) + offsetof(CreateUav, buffer), sizeof(b));
    return b;
}
inline D3D12_BUFFER_UAV_BYTE_OFFSET translate(const BufferUav& b) noexcept {
    // Keep byte units and the size-zero sentinel. Dividing by stride loses valid
    // non-stride-aligned offsets and truncates ranges larger than UINT elements.
    return {b.offset, b.size, b.stride, b.counter_offset, static_cast<D3D12_BUFFER_UAV_FLAGS>(b.flags)};
}
}
