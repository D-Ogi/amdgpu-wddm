// SPDX-License-Identifier: MIT
#pragma once
#include <d3d12.h>

namespace engine_ddi::cross_adapter {
// Tier 1 of the cross-adapter resources (driver/kmd/cross_adapter.h has the kernel contract): a buffer placed in a
// SHARED|SHARED_CROSS_ADAPTER heap. The D3D12 runtime admits ALLOW_CROSS_ADAPTER on a resource only in such a heap,
// and on a buffer only as a placed resource (a committed cross-adapter resource must be a row-major TEXTURE2D, tier 2,
// which this driver does not offer). The cross-adapter property belongs to the heap's kernel allocation: the runtime
// creates it with D3DKMT_CREATEALLOCATION.Flags.CrossAdapter, and the kernel driver describes it as a buffer.
//
// The engine does not get the flag on a buffer. vkd3d-proton reads ALLOW_CROSS_ADAPTER on a buffer as the
// OpenExistingHeapFrom* case and creates the VkBuffer with VkExternalMemoryBufferCreateInfo of
// VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT (libs/vkd3d/resource.c, vkd3d_create_buffer). The memory of
// a placed resource here is the heap's imported runtime allocation, not host memory, so a buffer created for host
// memory would be bound out of the Vulkan contract. A linear buffer is the same bytes with or without the flag.
//
// Only the description handed to the engine's create changes. Sizing keeps the flag, so that a cross-adapter buffer
// with tight alignment keeps the 64 KiB placement alignment of the D3D12 rule (tight-alignment.h, normalize).
constexpr D3D12_RESOURCE_FLAGS engine_create_flags(D3D12_RESOURCE_DIMENSION dimension,
                                                   D3D12_RESOURCE_FLAGS flags) noexcept {
    // In UINT: the enum's ~ gives a value outside the enumeration's range, which is not a constant expression.
    return dimension == D3D12_RESOURCE_DIMENSION_BUFFER
               ? static_cast<D3D12_RESOURCE_FLAGS>(static_cast<UINT>(flags) &
                                                   ~static_cast<UINT>(D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER))
               : flags;
}
// Every other resource with the flag is a texture: tier 2 (row-major texture), which this driver does not offer. Its
// create is refused before any memory or engine object exists, with a named log line; the create DDI reports the
// refusal as E_OUTOFMEMORY, which does not remove the device (engine-ddi.h, admitted_create_failure). A texture
// therefore never gets a raw BC2A allocation that no opener reads. The record-pair gate calls this function.
constexpr bool create_refused(D3D12_RESOURCE_DIMENSION dimension, D3D12_RESOURCE_FLAGS flags) noexcept {
    return (flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) && dimension != D3D12_RESOURCE_DIMENSION_BUFFER;
}
} // namespace engine_ddi::cross_adapter
