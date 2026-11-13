// SPDX-License-Identifier: MIT
// The D3D12 adapter of the ENGINE LAYOUT form (shared-resource.h), linked against the native engine-ddi.lib. No engine
// is loaded: the adapter has no engine call. Checks the plan of a buffer open, the reason and the HRESULT of each
// refusal in the documented order, the opener's own record, and the flag table of an opened resource.
#include "shared-resource.h"

#include "../../../contract/bc250_shared_resource_alloc.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <io.h>

namespace {
int failures = 0;
void check(bool ok, const char* format, ...) {
    char what[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(what, sizeof(what), format, args);
    va_end(args);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

bool same(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

const uint8_t kDevice[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint8_t kDriver[16] = {0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
                             0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf};

engine_ddi::OpenerIdentity identity_known() {
    engine_ddi::OpenerIdentity identity{};
    identity.known = true;
    std::memcpy(identity.device_uuid, kDevice, sizeof(kDevice));
    std::memcpy(identity.driver_uuid, kDriver, sizeof(kDriver));
    return identity;
}

// A buffer record as the D3D11 shell writes it: DXVK's usage and memory requirements for a buffer of width bytes.
BC250_SHARED_RESOURCE d3d11_buffer(uint32_t width, uint64_t memory) {
    BC250_SHARED_RESOURCE r{};
    r.Magic = BC250_SHARED_RESOURCE_MAGIC;
    r.Version = BC250_SHARED_RESOURCE_VERSION;
    r.Bytes = BC250_SHARED_RESOURCE_BYTES;
    r.Dimension = BC250_SHARED_RESOURCE_BUFFER;
    r.Width = width;
    r.Height = r.MipLevels = r.ArraySize = 1;
    r.SampleCount = 1;
    r.Usage = BC250_SR_USAGE_DEFAULT;
    r.BindFlags = BC250_SR_BIND_SHADER_RESOURCE | BC250_SR_BIND_UNORDERED_ACCESS;
    r.CreateUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
    r.SharingMode = BC250_SR_VK_SHARING_EXCLUSIVE;
    r.BufferSize = (width + 255u) & ~255ull;
    r.MemorySize = memory;
    r.MemoryAlignment = 256;
    r.MemoryTypeBits = 0x7;
    r.CreatorApi = BC250_SR_CREATOR_D3D11;
    std::memcpy(r.DriverUuid, kDriver, sizeof(kDriver));
    std::memcpy(r.DeviceUuid, kDevice, sizeof(kDevice));
    return r;
}

// A valid LINEAR texture record: 64x64, one mip, one layer.
BC250_SHARED_RESOURCE d3d11_texture() {
    BC250_SHARED_RESOURCE r{};
    r.Magic = BC250_SHARED_RESOURCE_MAGIC;
    r.Version = BC250_SHARED_RESOURCE_VERSION;
    r.Bytes = BC250_SHARED_RESOURCE_BYTES;
    r.Dimension = BC250_SHARED_RESOURCE_TEXTURE2D;
    r.Width = r.Height = 64;
    r.MipLevels = r.ArraySize = 1;
    r.Format = 28; // DXGI_FORMAT_R8G8B8A8_UNORM
    r.SampleCount = 1;
    r.BindFlags = BC250_SR_BIND_SHADER_RESOURCE;
    r.CreateUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    r.SharingMode = BC250_SR_VK_SHARING_EXCLUSIVE;
    r.ImageType = BC250_SR_VK_IMAGE_TYPE_2D;
    r.VkFormat = VK_FORMAT_R8G8B8A8_UNORM;
    r.ExtentWidth = r.ExtentHeight = 64;
    r.ExtentDepth = 1;
    r.VkMipLevels = r.VkArrayLayers = 1;
    r.VkSamples = BC250_SR_VK_SAMPLE_COUNT_1;
    r.Tiling = BC250_SR_VK_TILING_LINEAR;
    r.MemorySize = 64 * 64 * 4;
    r.MemoryAlignment = 4096;
    r.MemoryTypeBits = 0x7;
    r.LayoutHash = Bc250SharedResourceLayoutAdd(BC250_SR_FNV_BASIS, 0, 64 * 64 * 4, 256, 0, 0);
    r.CreatorApi = BC250_SR_CREATOR_D3D11;
    std::memcpy(r.DeviceUuid, kDevice, sizeof(kDevice));
    return r;
}

bc250_umd_alloc_private blob_of(const BC250_SHARED_RESOURCE& r) {
    bc250_umd_alloc_private blob{};
    check(Bc250SharedResourceAllocInit(&blob, &r) == 1, "fixture: the BC2A writer accepts the record");
    return blob;
}

struct Result {
    HRESULT hr;
    const char* why;
    engine_ddi::EngineLayoutOpen plan;
};

Result plan(const BC250_SHARED_RESOURCE& r, const bc250_umd_alloc_private& blob,
            const engine_ddi::OpenerIdentity& identity, uint32_t record_bytes = BC250_SHARED_RESOURCE_BYTES,
            uint32_t blob_bytes = sizeof(bc250_umd_alloc_private)) {
    Result result{};
    std::memset(&result.plan, 0xcc, sizeof(result.plan));
    result.why = nullptr;
    result.hr = engine_ddi::plan_engine_layout_open(&r, record_bytes, &blob, blob_bytes, identity, &result.plan,
                                                    &result.why);
    return result;
}

bool zero_plan(const engine_ddi::EngineLayoutOpen& p) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(&p);
    for (size_t i = 0; i < sizeof(p); ++i)
        if (b[i]) return false;
    return true;
}

void refused(const Result& r, HRESULT hr, const char* why, const char* what) {
    check(r.hr == hr && same(r.why, why) && zero_plan(r.plan), "%s: hr 0x%08lx, why \"%s\" (expected 0x%08lx, \"%s\")",
          what, static_cast<unsigned long>(r.hr), r.why ? r.why : "(null)", static_cast<unsigned long>(hr), why);
}

void accepted(const Result& r, uint32_t width, uint64_t allocation, D3D12_RESOURCE_FLAGS flags, const char* what) {
    const D3D12_RESOURCE_DESC1& d = r.plan.desc;
    check(r.hr == S_OK && same(r.why, BC250_SR_WHY_OK), "%s: accepted (hr 0x%08lx, why \"%s\")", what,
          static_cast<unsigned long>(r.hr), r.why ? r.why : "(null)");
    check(d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d.Alignment == 0 && d.Width == width && d.Height == 1 &&
              d.DepthOrArraySize == 1 && d.MipLevels == 1 && d.Format == DXGI_FORMAT_UNKNOWN &&
              d.SampleDesc.Count == 1 && d.SampleDesc.Quality == 0 && d.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
              d.Flags == flags,
          "%s: the D3D12 buffer description (Width %llu, flags 0x%x)", what,
          static_cast<unsigned long long>(d.Width), static_cast<unsigned>(d.Flags));
    check(r.plan.allocation_bytes == allocation && r.plan.address_alignment == 65536,
          "%s: the allocation is %llu bytes, mapped at 64 KiB alignment (got %llu, %llu)", what,
          static_cast<unsigned long long>(allocation), static_cast<unsigned long long>(r.plan.allocation_bytes),
          static_cast<unsigned long long>(r.plan.address_alignment));
}
} // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    _dup2(_fileno(stdout), _fileno(stderr));
    const engine_ddi::OpenerIdentity identity = identity_known();

    // Accepted buffers.
    {
        const BC250_SHARED_RESOURCE r = d3d11_buffer(4096, 4096);
        accepted(plan(r, blob_of(r), identity), 4096, 4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 "4096-byte buffer");
    }
    {
        BC250_SHARED_RESOURCE r = d3d11_buffer(100, 256);
        r.BindFlags = BC250_SR_BIND_VERTEX_BUFFER;
        accepted(plan(r, blob_of(r), identity), 100, 4096, D3D12_RESOURCE_FLAG_NONE,
                 "100-byte buffer in a 4096-byte allocation");
    }
    {
        BC250_SHARED_RESOURCE r = d3d11_buffer(65536 * 3, 65536 * 3);
        r.MiscFlags = BC250_SR_MISC_BUFFER_STRUCTURED;
        r.StructureByteStride = 16;
        accepted(plan(r, blob_of(r), identity), 65536 * 3, 65536 * 3, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 "structured buffer of 192 KiB");
    }
    {
        BC250_SHARED_RESOURCE r = d3d11_buffer(4096, 4096);
        r.Version = BC250_SHARED_RESOURCE_VERSION_1;
        r.CreatorApi = BC250_SR_CREATOR_NONE;
        accepted(plan(r, blob_of(r), identity), 4096, 4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 "version 1 record");
    }
    {
        // The fields that the rule does not compare: the creator's create flags and usage, and DriverUuid.
        BC250_SHARED_RESOURCE r = d3d11_buffer(4096, 4096);
        r.CreateFlags = 0x1;
        r.CreateUsage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        r.DriverUuid[0] ^= 0xff;
        accepted(plan(r, blob_of(r), identity), 4096, 4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 "other create flags, usage and DriverUuid");
    }
    {
        // A larger blob alignment than the record asks for (a D3D12 writer could select one).
        const BC250_SHARED_RESOURCE r = d3d11_buffer(4096, 4096);
        bc250_umd_alloc_private blob = blob_of(r);
        blob.phys_alignment = 65536;
        blob.alloc_size = blob.va_size = 65536;
        accepted(plan(r, blob, identity), 4096, 65536, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 "blob of 64 KiB for a 4096-byte buffer");
    }

    // Refusals, in the documented order.
    {
        const BC250_SHARED_RESOURCE r = d3d11_buffer(4096, 4096);
        const bc250_umd_alloc_private blob = blob_of(r);
        refused(plan(r, blob, identity, BC250_SHARED_RESOURCE_BYTES - 1), E_INVALIDARG, BC250_SR_WHY_LENGTH,
                "short record");
        BC250_SHARED_RESOURCE bad = r;
        bad.Magic ^= 1;
        refused(plan(bad, blob, identity), E_INVALIDARG, BC250_SR_WHY_MAGIC, "magic");
        bad = r;
        bad.Version = 3;
        refused(plan(bad, blob, identity), E_INVALIDARG, BC250_SR_WHY_VERSION, "version 3");
        bad = r;
        bad.Usage = 1;
        refused(plan(bad, blob, identity), E_INVALIDARG, BC250_SR_WHY_USAGE, "IMMUTABLE usage");
        bad = r;
        bad.BufferSize = r.Width - 1;
        refused(plan(bad, blob, identity), E_INVALIDARG, BC250_SR_WHY_CREATE_INFO, "BufferSize below Width");
        refused(plan(r, blob, identity, BC250_SHARED_RESOURCE_BYTES, sizeof(blob) - 1), E_INVALIDARG,
                BC250_SR_WHY_ALLOCATION_LENGTH, "short allocation blob");
        bc250_umd_alloc_private bad_blob = blob;
        bad_blob.flags |= 0x100;
        refused(plan(r, bad_blob, identity), E_INVALIDARG, BC250_SR_WHY_ALLOCATION_RECORD, "unknown allocation flag");
        refused(plan(r, blob, engine_ddi::OpenerIdentity{}), E_NOTIMPL, engine_ddi::kWhyEngineLayoutIdentity,
                "identity not set");
        engine_ddi::OpenerIdentity other = identity;
        other.device_uuid[15] ^= 1;
        refused(plan(r, blob, other), E_INVALIDARG, BC250_SR_WHY_IDENTITY, "other DeviceUuid");
        engine_ddi::OpenerIdentity other_driver = identity;
        other_driver.driver_uuid[0] ^= 1;
        accepted(plan(r, blob, other_driver), 4096, 4096, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                 "other opener DriverUuid");
    }
    {
        // The record's memory exceeds the blob: the BC2A check refuses before the match.
        const BC250_SHARED_RESOURCE one_page = d3d11_buffer(4096, 4096);
        BC250_SHARED_RESOURCE r = d3d11_buffer(8192, 8192);
        refused(plan(r, blob_of(one_page), identity), E_INVALIDARG, BC250_SR_WHY_ALLOCATION_RECORD,
                "allocation below the record's memory");
        // The creator says 4096 bytes of memory for 8192 bytes of Width: the opener's buffer does not fit.
        r.MemorySize = 4096;
        refused(plan(r, blob_of(one_page), identity), E_INVALIDARG, BC250_SR_WHY_MEMORY, "opener memory fit");
    }
    {
        // A texture is refused after its shape is read, before the blob is read.
        const BC250_SHARED_RESOURCE t = d3d11_texture();
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&t)), "fixture: the texture record is valid");
        const bc250_umd_alloc_private blob = blob_of(t);
        refused(plan(t, blob, identity), E_NOTIMPL, engine_ddi::kWhyEngineLayoutTexture, "texture record");
        refused(plan(t, blob, identity, BC250_SHARED_RESOURCE_BYTES, 4), E_NOTIMPL,
                engine_ddi::kWhyEngineLayoutTexture, "texture record with a short blob");
        BC250_SHARED_RESOURCE bad = t;
        bad.Tiling = 0;
        refused(plan(bad, blob, identity), E_INVALIDARG, BC250_SR_WHY_CREATE_INFO, "OPTIMAL texture record");
    }
    {
        // Null arguments.
        const BC250_SHARED_RESOURCE r = d3d11_buffer(4096, 4096);
        const bc250_umd_alloc_private blob = blob_of(r);
        const char* why = nullptr;
        check(engine_ddi::plan_engine_layout_open(&r, sizeof(r), &blob, sizeof(blob), identity, nullptr, &why) ==
                      E_INVALIDARG &&
                  same(why, BC250_SR_WHY_ARGUMENTS),
              "null plan refused");
        engine_ddi::EngineLayoutOpen out{};
        check(engine_ddi::plan_engine_layout_open(nullptr, sizeof(r), &blob, sizeof(blob), identity, &out, &why) ==
                      E_INVALIDARG &&
                  same(why, BC250_SR_WHY_ARGUMENTS),
              "null record refused");
        check(engine_ddi::plan_engine_layout_open(&r, sizeof(r), nullptr, sizeof(blob), identity, &out, &why) ==
                      E_INVALIDARG &&
                  same(why, BC250_SR_WHY_ARGUMENTS),
              "null blob refused");
        check(engine_ddi::plan_engine_layout_open(&r, sizeof(r), &blob, sizeof(blob), identity, &out, nullptr) == S_OK,
              "null why is allowed");
    }

    // The opener's own record passes the shape rule, and it is what the match compares.
    {
        const BC250_SHARED_RESOURCE r = d3d11_buffer(100, 256);
        const BC250_SHARED_RESOURCE mine = engine_ddi::opener_buffer_record(r, identity);
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&mine)), "opener record: valid S26R v2");
        check(mine.CreatorApi == BC250_SR_CREATOR_D3D12 && mine.MemorySize == 100 && mine.BufferSize == 100 &&
                  mine.MemoryAlignment == 65536 && mine.CreateUsage == engine_ddi::kOpenerBufferUsage &&
                  std::memcmp(mine.DeviceUuid, kDevice, 16) == 0 && std::memcmp(mine.DriverUuid, kDriver, 16) == 0,
              "opener record: D3D12, Width bytes at 64 KiB, the engine usage, the identity");
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceMatchWhy(&r, &mine, 4096)), "opener record matches");
        check(same(Bc250SharedResourceMatchWhy(&r, &mine, 64), BC250_SR_WHY_MEMORY),
              "opener record: 64 bytes of allocation do not fit");
    }

    // The flag table of an opened resource.
    {
        using engine_ddi::opened_resource_flags;
        const uint32_t srv = BC250_SR_BIND_SHADER_RESOURCE, rt = BC250_SR_BIND_RENDER_TARGET,
                       uav = BC250_SR_BIND_UNORDERED_ACCESS;
        check(opened_resource_flags(0, true) == D3D12_RESOURCE_FLAG_NONE, "flags: buffer, no binds");
        check(opened_resource_flags(rt | srv | BC250_SR_BIND_VERTEX_BUFFER, true) == D3D12_RESOURCE_FLAG_NONE,
              "flags: buffer, RT and SRV give nothing");
        check(opened_resource_flags(uav, true) == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "flags: buffer, UAV");
        check(opened_resource_flags(srv, false) == D3D12_RESOURCE_FLAG_NONE, "flags: texture, SRV");
        check(opened_resource_flags(0, false) == D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
              "flags: texture, no SRV denies shader access");
        check(opened_resource_flags(srv | rt | uav, false) ==
                  (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS),
              "flags: texture, SRV RT UAV");
    }

    // The dispatch test of the open.
    {
        const BC250_SHARED_RESOURCE r = d3d11_buffer(4096, 4096);
        const uint32_t e26r = 0x52363245u; // "E26R"
        check(engine_ddi::is_engine_layout_record(&r, sizeof(r)), "dispatch: S26R goes to the ENGINE LAYOUT open");
        check(!engine_ddi::is_engine_layout_record(&e26r, sizeof(e26r)), "dispatch: E26R does not");
        check(!engine_ddi::is_engine_layout_record(&r, 3), "dispatch: three bytes do not");
        check(!engine_ddi::is_engine_layout_record(nullptr, 240), "dispatch: no data does not");
    }

    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
