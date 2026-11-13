// SPDX-License-Identifier: MIT
// engine-ddi: the D3D12 adapter of the ENGINE LAYOUT form of shared resources (shared-resource.h says what it does
// and what it refuses).
#include "shared-resource.h"

#include "../../../contract/bc250_shared_resource_alloc.h"

#include <cstring>

namespace engine_ddi {

// The D3D11 numbers of the record and the API's (d3d11.h is not a header of this module; the contract states them).
static_assert(BC250_SR_BIND_SHADER_RESOURCE == 0x8u && BC250_SR_BIND_RENDER_TARGET == 0x20u &&
                  BC250_SR_BIND_UNORDERED_ACCESS == 0x80u,
              "D3D11 bind flags of the record");
static_assert(BC250_SR_VK_SHARING_EXCLUSIVE == VK_SHARING_MODE_EXCLUSIVE, "Vulkan sharing mode of the record");
static_assert(sizeof(BC250_SHARED_RESOURCE) == BC250_SHARED_RESOURCE_BYTES, "S26R");

bool is_engine_layout_record(const void* data, uint32_t bytes) noexcept {
    uint32_t magic = 0;
    if (!data || bytes < sizeof(magic)) return false;
    std::memcpy(&magic, data, sizeof(magic));
    return magic == BC250_SHARED_RESOURCE_MAGIC;
}

D3D12_RESOURCE_FLAGS opened_resource_flags(uint32_t d3d11_bind, bool buffer) noexcept {
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (d3d11_bind & BC250_SR_BIND_UNORDERED_ACCESS) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (buffer) return flags;
    if (d3d11_bind & BC250_SR_BIND_RENDER_TARGET) flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!(d3d11_bind & BC250_SR_BIND_SHADER_RESOURCE)) flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    return flags;
}

BC250_SHARED_RESOURCE opener_buffer_record(const BC250_SHARED_RESOURCE& received,
                                           const OpenerIdentity& identity) noexcept {
    BC250_SHARED_RESOURCE mine{};
    mine.Magic = BC250_SHARED_RESOURCE_MAGIC;
    mine.Version = BC250_SHARED_RESOURCE_VERSION;
    mine.Bytes = BC250_SHARED_RESOURCE_BYTES;
    mine.Dimension = BC250_SHARED_RESOURCE_BUFFER;
    // The description the opener builds its D3D12 buffer from: the received one, field by field.
    mine.Width = received.Width;
    mine.Height = received.Height;
    mine.MipLevels = received.MipLevels;
    mine.ArraySize = received.ArraySize;
    mine.Format = received.Format;
    mine.SampleCount = received.SampleCount;
    mine.SampleQuality = received.SampleQuality;
    mine.Usage = received.Usage;
    mine.BindFlags = received.BindFlags;
    mine.CpuAccessFlags = received.CpuAccessFlags;
    mine.MiscFlags = received.MiscFlags;
    mine.StructureByteStride = received.StructureByteStride;
    // The engine's buffer: no create flags, its usage, exclusive, Width bytes. The image fields stay 0.
    mine.CreateUsage = kOpenerBufferUsage;
    mine.SharingMode = BC250_SR_VK_SHARING_EXCLUSIVE;
    mine.BufferSize = received.Width;
    mine.MemorySize = received.Width;
    mine.MemoryAlignment = kOpenerBufferAlignment;
    mine.MemoryTypeBits = UINT32_MAX;
    mine.CreatorApi = BC250_SR_CREATOR_D3D12;
    std::memcpy(mine.DriverUuid, identity.driver_uuid, sizeof(mine.DriverUuid));
    std::memcpy(mine.DeviceUuid, identity.device_uuid, sizeof(mine.DeviceUuid));
    return mine;
}

HRESULT plan_engine_layout_open(const void* resource_data, uint32_t resource_bytes, const void* allocation_data,
                                uint32_t allocation_bytes, const OpenerIdentity& identity, EngineLayoutOpen* out,
                                const char** why) noexcept {
    const char* local = BC250_SR_WHY_ARGUMENTS;
    const char** reason = why ? why : &local;
    *reason = BC250_SR_WHY_ARGUMENTS;
    if (!out) return E_INVALIDARG;
    *out = EngineLayoutOpen{};

    BC250_SHARED_RESOURCE received{};
    *reason = Bc250SharedResourceDecodeWhy(resource_data, resource_bytes, &received);
    if (!Bc250SharedResourceWhyOk(*reason)) return E_INVALIDARG;
    if (received.Dimension != BC250_SHARED_RESOURCE_BUFFER) {
        *reason = kWhyEngineLayoutTexture;
        return E_NOTIMPL;
    }

    unsigned long long allocation = 0;
    *reason = Bc250SharedResourceAllocWhy(allocation_data, allocation_bytes, &received, &allocation);
    if (!Bc250SharedResourceWhyOk(*reason)) return E_INVALIDARG;

    if (!identity.known) {
        *reason = kWhyEngineLayoutIdentity;
        return E_NOTIMPL;
    }
    const BC250_SHARED_RESOURCE mine = opener_buffer_record(received, identity);
    *reason = Bc250SharedResourceMatchWhy(&received, &mine, allocation);
    if (!Bc250SharedResourceWhyOk(*reason)) return E_INVALIDARG;

    EngineLayoutOpen plan{};
    plan.desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    plan.desc.Alignment = 0;
    plan.desc.Width = mine.Width;
    plan.desc.Height = 1;
    plan.desc.DepthOrArraySize = 1;
    plan.desc.MipLevels = 1;
    plan.desc.Format = DXGI_FORMAT_UNKNOWN;
    plan.desc.SampleDesc = {1, 0};
    plan.desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    plan.desc.Flags = opened_resource_flags(mine.BindFlags, true);
    plan.allocation_bytes = allocation;
    plan.address_alignment = mine.MemoryAlignment;
    *out = plan;
    return S_OK;
}

} // namespace engine_ddi
