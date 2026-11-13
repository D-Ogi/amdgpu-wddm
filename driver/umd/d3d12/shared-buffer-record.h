// SPDX-License-Identifier: MIT
// BD-373 step 5: the ENGINE LAYOUT record (S26R, driver/contract/bc250_shared_resource.h) of a buffer that this shell
// creates, so that a D3D12 opener on another device or in another process can open it.
//
// The D3D12 DDI does not say that a create is shared (engine-ddi/INTEGRATION.md, "The shared surface"), so the
// shell cannot write the record only for a shared buffer. It writes it for every committed buffer on a GPU-only heap,
// unless engine-layout-buffers-off is on (ddi-trace.h). A buffer has no layout: the record and the BC2A blob with
// BC250_UMD_A_SHARED change nothing on the GPU, and the kernel driver reads neither field. The lab probe
// (tools/win/shared-interop, unit A, 2026-11-13) measured that the runtime keeps the 240 bytes for every committed
// buffer and that a create costs the same with and without the record (353.2 us without it, 342.5 and 353.6 us with
// it, mean of 256 creates), so the record is the default.
//
// The reader of this record is the ENGINE LAYOUT open of a D3D12 opener (engine-ddi/resources.cpp). The D3D11
// runtime refused OpenSharedResource1 of a D3D12 buffer with E_INVALIDARG on the development PC (another vendor's
// driver, 2026-11-13), so the D3D11 shell's opener has no path to this record; the lab probe
// (tools/win/shared-interop, case d12-buffer-*-12to11) holds that rule on this driver. The record still names the
// D3D11 binds that an opener's buffer would need, because the form has the field: vertex, index and shader resource
// for every buffer, and unordered access when the D3D12 resource allows it. A constant buffer bind is not written,
// because D3D11.0 does not let it combine with other binds.
#pragma once
#include "../../contract/bc250_shared_resource.h"
#include <cstdint>
#include <cstring>

namespace native12 {

// What the shell knows about the buffer it creates: the D3D12 description and the requirements of the engine's
// buffer for the heap (the buffer probe of heap-import.cpp).
struct EngineLayoutBufferCreate {
    uint64_t width;                // D3D12 resource Width
    bool unordered_access;         // the resource allows unordered access
    uint32_t create_usage;         // VkBufferCreateInfo::usage of the probe
    uint64_t buffer_size;          // VkBufferCreateInfo::size of the probe: the heap size
    uint64_t memory_size;          // what the allocation holds for the buffer
    uint64_t memory_alignment;     // VkMemoryRequirements::alignment
    uint32_t memory_type_bits;     // the memory types the import can take
    uint8_t device_uuid[16];       // VkPhysicalDeviceIDProperties of the creator's device
    uint8_t driver_uuid[16];
};

inline constexpr uint32_t kEngineLayoutBufferBinds =
    BC250_SR_BIND_VERTEX_BUFFER | BC250_SR_BIND_INDEX_BUFFER | BC250_SR_BIND_SHADER_RESOURCE;

// Writes the record of one buffer to *out and returns true, or returns false and writes zeros. False means that the
// record cannot describe the buffer (a Width above 4 GiB - 1, which the record's 32-bit Width cannot carry, or
// requirements that the form refuses), and the buffer keeps the ordinary BC2A allocation.
inline bool engine_layout_buffer_record(const EngineLayoutBufferCreate& c, BC250_SHARED_RESOURCE* out) noexcept {
    if (!out) return false;
    *out = BC250_SHARED_RESOURCE{};
    if (!c.width || c.width > UINT32_MAX) return false;
    BC250_SHARED_RESOURCE r{};
    r.Magic = BC250_SHARED_RESOURCE_MAGIC;
    r.Version = BC250_SHARED_RESOURCE_VERSION;
    r.Bytes = BC250_SHARED_RESOURCE_BYTES;
    r.Dimension = BC250_SHARED_RESOURCE_BUFFER;
    r.Width = static_cast<uint32_t>(c.width);
    r.Height = 1;
    r.MipLevels = 1;
    r.ArraySize = 1;
    r.SampleCount = 1;
    r.Usage = BC250_SR_USAGE_DEFAULT;
    r.BindFlags = kEngineLayoutBufferBinds | (c.unordered_access ? BC250_SR_BIND_UNORDERED_ACCESS : 0u);
    r.CreateUsage = c.create_usage;
    r.SharingMode = BC250_SR_VK_SHARING_EXCLUSIVE;
    r.BufferSize = c.buffer_size;
    r.MemorySize = c.memory_size;
    r.MemoryAlignment = c.memory_alignment;
    r.MemoryTypeBits = c.memory_type_bits;
    r.CreatorApi = BC250_SR_CREATOR_D3D12;
    std::memcpy(r.DriverUuid, c.driver_uuid, sizeof(r.DriverUuid));
    std::memcpy(r.DeviceUuid, c.device_uuid, sizeof(r.DeviceUuid));
    bool identity = false;
    for (const uint8_t byte : r.DeviceUuid) identity = identity || byte != 0;
    if (!identity || !Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&r))) return false;
    *out = r;
    return true;
}

} // namespace native12
