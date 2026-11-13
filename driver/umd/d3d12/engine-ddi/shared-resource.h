// SPDX-License-Identifier: MIT
// engine-ddi: the D3D12 adapter of the ENGINE LAYOUT form of shared resources (BD-373). The wire form, its shape rule
// and THE match rule are in driver/contract/bc250_shared_resource.h, the BC2A check in
// driver/contract/bc250_shared_resource_alloc.h. This adapter reads a received record with them, makes the record of
// what this engine binds for the same description, and lets Bc250SharedResourceMatchWhy decide. It copies no part
// of the rule.
//
// This file has no engine call. The open itself (adopt, the engine heap over the adopted memory, the placed buffer)
// is in resources.cpp, beside the open of the LINEAR SURFACE form, and it calls plan_engine_layout_open first.
//
// What is opened today: buffers. A D3D12 buffer placed at offset 0 of an engine heap over the adopted allocation
// reads the creator's bytes, because a buffer has no layout. Textures of this form need the layout of every
// subresource of the engine's LINEAR image (engine ABI V15) and are refused with a named reason until then. A
// D3D12-created record is not written: the D3D12 DDI does not say that a create is shared (engine-ddi.h,
// kMemoryShareable).
#pragma once
#include <windows.h>
#include <d3d12.h>
#include <vulkan/vulkan_core.h>
#include <cstdint>

#include "../../../contract/bc250_shared_resource.h"

namespace engine_ddi {

// The reasons this adapter adds to the contract's (BC250_SR_WHY_*). Each is a literal of this header; the open
// prints it in its refusal line.
inline constexpr const char* kWhyEngineLayoutTexture = "engine layout texture";
inline constexpr const char* kWhyEngineLayoutIdentity = "engine layout device identity";

// The identity of the opener's Vulkan device (VkPhysicalDeviceIDProperties). known is false until the shell has
// set it (set_device_identity, engine-ddi.h).
struct OpenerIdentity {
    bool known;
    uint8_t device_uuid[16];
    uint8_t driver_uuid[16];
};

// The memory a buffer of this engine takes, for the opener's own record. vkd3d-proton places a buffer as a range of
// the heap's own VkBuffer: it needs heap offset + Width <= the heap size (resource.c,
// d3d12_resource_create_placed_flags),
// and a heap over borrowed memory that allows buffers needs a GPU virtual address aligned to the heap alignment,
// 64 KiB (memory.c, vkd3d_memory_allocation_init). The usage is the one vkd3d_create_buffer gives every buffer
// (resource.c); extensions add bits to it, and the match rule does not compare it.
inline constexpr uint64_t kOpenerBufferAlignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
inline constexpr uint32_t kOpenerBufferUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

// What the open of one received record binds: the D3D12 description of the opener's resource, and the memory it
// takes from the kernel allocation.
struct EngineLayoutOpen {
    D3D12_RESOURCE_DESC1 desc;
    // The size of the kernel allocation (BC2A alloc_size, a page multiple): the adopt maps all of it, and it is the
    // size of the engine heap the resource is placed on.
    uint64_t allocation_bytes;
    // The GPU virtual address alignment of the adopt's mapping (kOpenerBufferAlignment for a buffer).
    uint64_t address_alignment;
};

// True when the resource private data of an open starts with the S26R magic: the ENGINE LAYOUT form, whatever else
// the bytes say. Such an open goes to plan_engine_layout_open, never to the LINEAR SURFACE decoder.
bool is_engine_layout_record(const void* data, uint32_t bytes) noexcept;

// The D3D12 resource flags that the D3D11 bind flags of a shared resource mean (D3D11 numbers: BC250_SR_BIND_* and
// BC250_SHARED_BIND_* are the same values). A texture: RENDER_TARGET gives ALLOW_RENDER_TARGET, UNORDERED_ACCESS gives
// ALLOW_UNORDERED_ACCESS, and no SHADER_RESOURCE gives DENY_SHADER_RESOURCE. A buffer: UNORDERED_ACCESS gives
// ALLOW_UNORDERED_ACCESS and nothing else, because D3D12 has no render-target buffer and lets only a depth-stencil
// resource deny shader access. The other D3D11 binds and misc flags have no D3D12 resource flag; D3D12 views decide.
D3D12_RESOURCE_FLAGS opened_resource_flags(uint32_t d3d11_bind, bool buffer) noexcept;

// The opener's own record of a received buffer record: the record of the buffer that this engine places for the
// same description (Bc250SharedResourceMatchWhy compares the two). The description is the received one, because the
// opener builds its D3D12 description from it. The create info is the engine's buffer (kOpenerBufferUsage,
// exclusive, Width bytes). The memory is what the placement needs: Width bytes at an address aligned to
// kOpenerBufferAlignment. engine-ddi has no Vulkan query of its own, so MemoryTypeBits says "not narrowed" (every
// bit): the engine checks the memory type at CreateHeapFromMemory, as for MemoryRequest::memory_type_bits 0. The
// rule does not compare it. CreatorApi is BC250_SR_CREATOR_D3D12, the UUIDs are identity's.
BC250_SHARED_RESOURCE opener_buffer_record(const BC250_SHARED_RESOURCE& received,
                                           const OpenerIdentity& identity) noexcept;

// The plan of one ENGINE LAYOUT open, before anything is adopted. resource_data is the resource private data of the
// open, allocation_data the private data of its one allocation. In order:
//   1. Bc250SharedResourceDecodeWhy: the record's length, header and shape. Refused: E_INVALIDARG, *why is the
//      contract's reason.
//   2. A texture record: E_NOTIMPL, kWhyEngineLayoutTexture (until engine ABI V15).
//   3. Bc250SharedResourceAllocWhy: the BC2A blob against the record. Refused: E_INVALIDARG, the contract's reason.
//   4. The opener's identity is not known: E_NOTIMPL, kWhyEngineLayoutIdentity.
//   5. Bc250SharedResourceMatchWhy of the received record and opener_buffer_record, with the allocation size of
//      step 3. Refused: E_INVALIDARG, the contract's reason.
// On S_OK *out is the plan and *why is BC250_SR_WHY_OK. On a refusal *out is zero. *why is never null on return.
HRESULT plan_engine_layout_open(const void* resource_data, uint32_t resource_bytes, const void* allocation_data,
                                uint32_t allocation_bytes, const OpenerIdentity& identity, EngineLayoutOpen* out,
                                const char** why) noexcept;

} // namespace engine_ddi
