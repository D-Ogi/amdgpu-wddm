// SPDX-License-Identifier: MIT
// BD-373 step 3: the ENGINE LAYOUT form of a shared resource against the kernel driver's own readers. The S26R record
// and the BC2A blob below come from the one writer (bc250_shared_resource.h, bc250_shared_resource_alloc.h); every
// statement about the kernel driver is a call of its own code: driver/kmd/umd_blob.c (linked), surface_resource_private.h,
// gdi_private.h and cpu_view_policy.h (included). No number of the kernel driver is copied here. The LINEAR SURFACE
// reader (bc250_shared_surface.h, the D3D12 shell's and the Mesa winsys's) must refuse the new form.
#include "../bc250_shared_resource_alloc.h"
#include "../bc250_shared_surface.h"
#include "../../kmd/umd_blob.h"
#include "../../kmd/cpu_view_policy.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
namespace {
unsigned checks = 0;
void check(bool b, const char *what)
{
    ++checks;
    if (!b) { std::fprintf(stderr, "FAIL shared-resource check %u: %s\n", checks, what); std::exit(1); }
}
bool is(const char *got, const char *want) { return got && want && std::strcmp(got, want) == 0; }
BC250_SHARED_RESOURCE TextureRecord()
{
    BC250_SHARED_RESOURCE r{};
    r.Magic = BC250_SHARED_RESOURCE_MAGIC; r.Version = BC250_SHARED_RESOURCE_VERSION; r.Bytes = BC250_SHARED_RESOURCE_BYTES;
    r.CreatorApi = BC250_SR_CREATOR_D3D11;
    r.Dimension = BC250_SHARED_RESOURCE_TEXTURE2D;
    r.Width = 256; r.Height = 128; r.MipLevels = 9; r.ArraySize = 4; r.Format = 28; /* R8G8B8A8_UNORM */
    r.SampleCount = 1; r.BindFlags = BC250_SR_BIND_SHADER_RESOURCE | BC250_SR_BIND_RENDER_TARGET;
    r.MiscFlags = BC250_SR_MISC_GENERATE_MIPS;
    r.CreateFlags = 0x8; /* MUTABLE_FORMAT */ r.CreateUsage = 0x17; r.ImageType = BC250_SR_VK_IMAGE_TYPE_2D;
    r.VkFormat = 37; r.ExtentWidth = 256; r.ExtentHeight = 128; r.ExtentDepth = 1; r.VkMipLevels = 9;
    r.VkArrayLayers = 4; r.VkSamples = BC250_SR_VK_SAMPLE_COUNT_1; r.Tiling = BC250_SR_VK_TILING_LINEAR;
    r.ViewFormatCount = 2; r.ViewFormats[0] = 37; r.ViewFormats[1] = 43;
    r.MemorySize = 700000; r.MemoryAlignment = 4096; r.MemoryTypeBits = 0x3; r.LayoutHash = 0x0123456789ABCDEFull;
    for (unsigned i = 0; i < 16; ++i) { r.DriverUuid[i] = (unsigned char)i; r.DeviceUuid[i] = (unsigned char)(0x80 + i); }
    return r;
}
BC250_SHARED_RESOURCE BufferRecord()
{
    BC250_SHARED_RESOURCE r{};
    r.Magic = BC250_SHARED_RESOURCE_MAGIC; r.Version = BC250_SHARED_RESOURCE_VERSION; r.Bytes = BC250_SHARED_RESOURCE_BYTES;
    r.CreatorApi = BC250_SR_CREATOR_D3D11;
    r.Dimension = BC250_SHARED_RESOURCE_BUFFER;
    r.Width = 1000; r.Height = 1; r.MipLevels = 1; r.ArraySize = 1; r.SampleCount = 1;
    r.BindFlags = BC250_SR_BIND_SHADER_RESOURCE | BC250_SR_BIND_UNORDERED_ACCESS; r.MiscFlags = BC250_SR_MISC_BUFFER_ALLOW_RAW;
    r.CreateUsage = 0x20023; r.BufferSize = 1024;
    r.MemorySize = 1024; r.MemoryAlignment = 256; r.MemoryTypeBits = 0x1;
    for (unsigned i = 0; i < 16; ++i) { r.DriverUuid[i] = (unsigned char)i; r.DeviceUuid[i] = (unsigned char)(0x80 + i); }
    return r;
}
}
int main()
{
    // The wire: 240 bytes at fixed offsets, "S26R" first. A field that moves is a new version.
    static_assert(sizeof(BC250_SHARED_RESOURCE) == 240, "S26R size");
    // Every field is pinned, so the x86 and the x64 build of each shell read the same bytes.
#define S26R_AT(field, at) static_assert(offsetof(BC250_SHARED_RESOURCE, field) == at, "S26R offset of " #field)
    S26R_AT(Magic, 0); S26R_AT(Version, 4); S26R_AT(Bytes, 8); S26R_AT(Dimension, 12);
    S26R_AT(Width, 16); S26R_AT(Height, 20); S26R_AT(MipLevels, 24); S26R_AT(ArraySize, 28);
    S26R_AT(Format, 32); S26R_AT(SampleCount, 36); S26R_AT(SampleQuality, 40); S26R_AT(Usage, 44);
    S26R_AT(BindFlags, 48); S26R_AT(CpuAccessFlags, 52); S26R_AT(MiscFlags, 56); S26R_AT(StructureByteStride, 60);
    S26R_AT(CreateFlags, 64); S26R_AT(CreateUsage, 68); S26R_AT(SharingMode, 72); S26R_AT(ImageType, 76);
    S26R_AT(VkFormat, 80); S26R_AT(ExtentWidth, 84); S26R_AT(ExtentHeight, 88); S26R_AT(ExtentDepth, 92);
    S26R_AT(VkMipLevels, 96); S26R_AT(VkArrayLayers, 100); S26R_AT(VkSamples, 104); S26R_AT(Tiling, 108);
    S26R_AT(InitialLayout, 112); S26R_AT(ViewFormatCount, 116); S26R_AT(ViewFormats, 120); S26R_AT(BufferSize, 168);
    S26R_AT(MemorySize, 176); S26R_AT(MemoryAlignment, 184); S26R_AT(MemoryTypeBits, 192); S26R_AT(CreatorApi, 196);
    S26R_AT(LayoutHash, 200); S26R_AT(DriverUuid, 208); S26R_AT(DeviceUuid, 224);
#undef S26R_AT
    const BC250_SHARED_RESOURCE record = TextureRecord();
    check(Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&record)), "the test record is a record of this form");
    const unsigned char *rb = reinterpret_cast<const unsigned char *>(&record);
    check(rb[0] == 'S' && rb[1] == '2' && rb[2] == '6' && rb[3] == 'R' && rb[8] == 240 && rb[12] == 2, "S26R bytes");
    // The hash is FNV-1a 64 of the eight bytes of each value, low byte first. The pin is the basis times the prime to the
    // eighth power (eight zero bytes), computed outside this header.
    check(Bc250SharedResourceHash(BC250_SR_FNV_BASIS, 0) == 0x47FE0D7EAF8E51E3ull, "FNV-1a 64 of eight zero bytes");
    {
        // The one hash step: the four values of one subresource, in their order. The size is not one of them.
        unsigned long long h = BC250_SR_FNV_BASIS;
        for (unsigned long long v : {0x8000ull, 0x100ull, 0x8000ull, 0ull}) h = Bc250SharedResourceHash(h, v);
        check(Bc250SharedResourceLayoutAdd(BC250_SR_FNV_BASIS, 0x8000, 0x100, 0x8000, 0) == h, "LayoutAdd order");
        check(Bc250SharedResourceLayoutAdd(BC250_SR_FNV_BASIS, 0x100, 0x8000, 0x8000, 0) != h,
              "LayoutAdd sees the order of offset and rowPitch");
    }
    {
        // The RADV vector (lab, unit A, 2026-11-13, engine V15 QueryLinearImageLayouts): a LINEAR R8G8B8A8 64x64
        // image with 7 mips and 1 layer, memory 32512, the smallest mip first. RADV gives size 32512 for every mip,
        // the size of the layer. The rows of each mip (rows x rowPitch) tile the memory exactly, and the size would
        // claim that mip 0 runs past it: the reason LayoutAdd does not take the size.
        struct L { unsigned long long offset, size, row_pitch, array_pitch; unsigned rows; };
        const L radv[7] = {{16128, 32512, 256, 32512, 64}, {7936, 32512, 256, 32512, 32}, {3840, 32512, 256, 32512, 16},
                           {1792, 32512, 256, 32512, 8},   {768, 32512, 256, 32512, 4},   {256, 32512, 256, 32512, 2},
                           {0, 32512, 256, 32512, 1}};
        const unsigned long long memory = 32512;
        bool tiled = true;
        for (int m = 0; m < 7; ++m) {
            const unsigned long long end = radv[m].offset + radv[m].rows * radv[m].row_pitch;
            tiled = tiled && end <= memory && (m == 0 ? end == memory : end == radv[m - 1].offset);
        }
        check(tiled, "RADV vector: the rows of the 7 mips tile the 32512 bytes, no overlap");
        check(radv[0].offset + radv[0].size > memory, "RADV vector: offset + size of mip 0 is past the memory");
        unsigned long long h = BC250_SR_FNV_BASIS, again = BC250_SR_FNV_BASIS;
        for (const L &l : radv) {
            h = Bc250SharedResourceLayoutAdd(h, l.offset, l.row_pitch, l.array_pitch, 0);
            again = Bc250SharedResourceHash(Bc250SharedResourceHash(Bc250SharedResourceHash(
                        Bc250SharedResourceHash(again, l.offset), l.row_pitch), l.array_pitch), 0);
        }
        check(h == again && h != BC250_SR_FNV_BASIS, "RADV vector: LayoutHash of offset, rowPitch, arrayPitch, depthPitch");
        // 100x60, 7 mips: memory 46080, mip 0 at 15360 with rowPitch 512 (60 rows end at the memory end), mip 6 at
        // 0 with rowPitch 256. RADV gives size 46080 for every mip.
        check(15360ull + 60ull * 512ull == 46080ull && 0ull + 1ull * 256ull <= 15360ull,
              "RADV vector 100x60: mip 0 ends at the memory end, mip 6 is first");
    }

    // Versions: a writer writes 2 with its CreatorApi; a reader reads 1 (CreatorApi 0) and 2, and refuses 3.
    check(record.Version == 2 && BC250_SHARED_RESOURCE_VERSION == 2u, "the writer's version");
    {
        auto v1 = record; v1.Version = BC250_SHARED_RESOURCE_VERSION_1; v1.CreatorApi = BC250_SR_CREATOR_NONE;
        BC250_SHARED_RESOURCE out{};
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceDecodeWhy(&v1, sizeof(v1), &out)), "a version 1 record");
        v1.CreatorApi = BC250_SR_CREATOR_D3D11;
        check(is(Bc250SharedResourceRecordWhy(&v1), BC250_SR_WHY_RESERVED), "version 1 has 0 in Reserved0");
        auto v3 = record; v3.Version = 3;
        check(is(Bc250SharedResourceDecodeWhy(&v3, sizeof(v3), &out), BC250_SR_WHY_VERSION), "version 3 is refused");
        auto v0 = record; v0.Version = 0;
        check(is(Bc250SharedResourceRecordWhy(&v0), BC250_SR_WHY_VERSION), "version 0 is refused");
        auto api = record; api.CreatorApi = BC250_SR_CREATOR_D3D12;
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&api)), "a D3D12 creator");
        api.CreatorApi = BC250_SR_CREATOR_NONE;
        check(is(Bc250SharedResourceRecordWhy(&api), BC250_SR_WHY_CREATOR), "version 2 names its creator");
        api.CreatorApi = 3;
        check(is(Bc250SharedResourceRecordWhy(&api), BC250_SR_WHY_CREATOR), "an unknown creator");
    }

    // The BC2A blob, read by the kernel driver's own parser (umd_blob.c).
    bc250_umd_alloc_private blob{};
    check(Bc250SharedResourceAllocInit(&blob, &record) == 1, "the writer");
    check(sizeof(blob) == UMD_BLOB_ALLOC_BYTES, "BC2A size is the kernel reader's");
    check(UmdBlobIsAlloc(&blob, sizeof(blob)), "the kernel driver sees a UMD allocation");
    struct umd_alloc_view view;
    check(UmdBlobParseAlloc(&blob, sizeof(blob), &view) == UMD_BLOB_OK, "the kernel driver admits the blob");
    check(view.bytes == 700416 && view.alignment == 4096 && view.heap == UMD_BLOB_HEAP_VRAM && view.version == 2 &&
          view.cache_policy_valid && !view.exact_va && !view.scanout && view.requested_va == 0, "the kernel driver's view");
    check((view.flags & BC250_UMD_A_SHARED) && (view.gem_flags & UMD_BLOB_GEM_NO_CPU_ACCESS) &&
          !(view.gem_flags & UMD_BLOB_GEM_DISCARDABLE), "shared, no CPU access, not discardable");
    check(!UmdBlobAllocCpuVisible(&view) && !UmdBlobAllocCpuCached(&view) &&
          UmdBlobCpuViewClass(&view) == BC250_CPU_VIEW_GPU_ONLY, "no CPU view of the shared memory");
    struct umd_placement placement;
    UmdBlobPlacement(&view, 1, 2, 1, &placement);
    check(placement.preferred[0] == 1 && placement.preferred[1] == 2, "VRAM first, the aperture second under shared residency");
    UmdBlobPlacement(&view, 1, 2, 0, &placement);
    check(placement.preferred[0] == 1 && placement.preferred[1] == 0, "VRAM alone without shared residency");
    check(UmdBlobStatusText(UMD_BLOB_OK) != nullptr, "status text");
    // The opener's check of the blob: fields, not the bytes of one writer.
    unsigned long long bytes = 0;
    check(Bc250SharedResourceWhyOk(Bc250SharedResourceAllocWhy(&blob, sizeof(blob), &record, &bytes)) && bytes == 700416,
          "the opener admits the creator's blob");
    {
        // A writer with another alignment (the D3D12 shell's GPU-only blob uses the request's alignment): admitted, and
        // the kernel driver reads it as written.
        auto d3d12 = blob; d3d12.phys_alignment = 65536; d3d12.alloc_size = d3d12.va_size = 720896;
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceAllocWhy(&d3d12, sizeof(d3d12), &record, &bytes)) &&
              bytes == 720896 && UmdBlobParseAlloc(&d3d12, sizeof(d3d12), &view) == UMD_BLOB_OK && view.bytes == 720896,
              "a blob of another writer's alignment");
        auto larger = blob; larger.alloc_size = larger.va_size = 1u << 20;
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceAllocWhy(&larger, sizeof(larger), &record, &bytes)) &&
              bytes == (1u << 20), "a larger allocation");
    }
    {
        // Each field this form does not write is a named refusal, and the size is 0.
        auto refused = [&](bc250_umd_alloc_private b, const char *what) {
            bytes = 7;
            check(is(Bc250SharedResourceAllocWhy(&b, sizeof(b), &record, &bytes), BC250_SR_WHY_ALLOCATION_RECORD) && !bytes,
                  what);
        };
        auto b = blob;
        b = blob; b.magic ^= 1; refused(b, "blob magic");
        b = blob; b.version = BC250_UMD_ALLOC_VERSION_SCANOUT; refused(b, "blob version 3");
        b = blob; b.version = BC250_UMD_ALLOC_VERSION; refused(b, "blob version 1");
        b = blob; b.size = 128; refused(b, "blob size field");
        b = blob; b.flags |= BC250_UMD_A_EXACT_VA; refused(b, "a flag other than shared");
        b = blob; b.flags = 0; refused(b, "not shared");
        b = blob; b.preferred_heap = UMD_BLOB_HEAP_GTT;
        check(UmdBlobParseAlloc(&b, sizeof(b), &view) == UMD_BLOB_OK, "the kernel driver admits a GTT blob");
        refused(b, "a blob the kernel driver admits but this form does not have: GTT");
        b = blob; b.gem_flags = 0; refused(b, "a CPU-visible allocation");
        b = blob; b.gem_flags |= AMDGPU_GEM_CREATE_VRAM_CLEARED; refused(b, "another gem flag");
        b = blob; b.va_size = b.alloc_size + 4096; refused(b, "va_size other than alloc_size");
        b = blob; b.alloc_size = b.va_size = 700000; refused(b, "not a page multiple");
        b = blob; b.alloc_size = b.va_size = 696320; refused(b, "smaller than the record's memory");
        b = blob; b.alloc_size = b.va_size = 0; refused(b, "no size");
        b = blob; b.phys_alignment = 2048; refused(b, "an alignment below the record's");
        b = blob; b.phys_alignment = 12288; refused(b, "an alignment that is not a power of two");
        b = blob; b.phys_alignment = 131072; refused(b, "an alignment above 64 KiB");
        b = blob; b.reserved0 = 1; refused(b, "reserved0");
        b = blob; b.requested_va = 1u << 20; refused(b, "a requested address");
        b = blob; b.va_flags = 1; refused(b, "VA flags");
        b = blob; b.va_offset = 4096; refused(b, "a VA offset");
        b = blob; b.metadata_size = 4; refused(b, "metadata size");
        b = blob; b.metadata[15] = 1; refused(b, "metadata");
        b = blob; b.scanout_width = 256; refused(b, "a scan-out field");
        b = blob; b.scanout_format = 21; refused(b, "the last field");
        check(is(Bc250SharedResourceAllocWhy(&blob, sizeof(blob) - 1, &record, &bytes), BC250_SR_WHY_ALLOCATION_LENGTH),
              "blob length");
        auto noMemory = record; noMemory.MemorySize = 0;
        check(is(Bc250SharedResourceAllocWhy(&blob, sizeof(blob), &noMemory, &bytes), BC250_SR_WHY_MEMORY),
              "a record without memory");
        check(is(Bc250SharedResourceAllocWhy(nullptr, sizeof(blob), &record, &bytes), BC250_SR_WHY_ARGUMENTS), "no blob");
        // An unaligned blob is read through a copy.
        unsigned char raw[sizeof(blob) + 1];
        std::memcpy(raw + 1, &blob, sizeof(blob));
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceAllocWhy(raw + 1, sizeof(blob), &record, &bytes)) &&
              bytes == 700416, "an unaligned blob");
    }

    // THE match rule (Bc250SharedResourceMatchWhy): the same layout, format, memory fit and adapter. Fields that
    // belong to one engine's own image do not decide.
    {
        const unsigned long long alloc = 700416;
        auto match = [&](const BC250_SHARED_RESOURCE &mine) { return Bc250SharedResourceMatchWhy(&record, &mine, alloc); };
        check(Bc250SharedResourceWhyOk(match(record)), "the same record matches");
        auto m = record;
        m = record; m.CreateUsage = 0x1F; m.CreateFlags = 0;
        check(Bc250SharedResourceWhyOk(match(m)), "another engine's usage and flags");
        m = record; m.ViewFormatCount = 3; m.ViewFormats[2] = 44; m.ViewFormats[1] = 50;
        check(Bc250SharedResourceWhyOk(match(m)), "another view format list");
        m = record; m.ViewFormatCount = 0; m.ViewFormats[0] = m.ViewFormats[1] = 0;
        check(Bc250SharedResourceWhyOk(match(m)), "no view format list");
        m = record; m.MemoryTypeBits = 0x10; m.MemoryAlignment = 256; m.MemorySize = 600000;
        check(Bc250SharedResourceWhyOk(match(m)), "another memory type, a smaller alignment and size");
        m = record; m.DriverUuid[0] ^= 0xFF; m.CreatorApi = BC250_SR_CREATOR_D3D12;
        check(Bc250SharedResourceWhyOk(match(m)), "another driver UUID and another creator API");
        m = record; m.MemorySize = alloc;
        check(Bc250SharedResourceWhyOk(match(m)), "memory that fills the allocation");
        {
            auto v1 = record; v1.Version = BC250_SHARED_RESOURCE_VERSION_1; v1.CreatorApi = 0;
            check(Bc250SharedResourceWhyOk(Bc250SharedResourceMatchWhy(&v1, &record, alloc)), "a version 1 record matches");
            auto v3 = record; v3.Version = 3;
            check(is(Bc250SharedResourceMatchWhy(&v3, &record, alloc), BC250_SR_WHY_VERSION), "version 3 does not");
        }
        m = record; m.LayoutHash ^= 1; check(is(match(m), BC250_SR_WHY_LAYOUT), "another layout");
        m = record; m.VkFormat = 44; check(is(match(m), BC250_SR_WHY_VK_FORMAT), "another VkFormat");
        m = record; m.Format = 29; check(is(match(m), BC250_SR_WHY_DESCRIPTION), "another DXGI format");
        m = record; m.BindFlags = BC250_SR_BIND_SHADER_RESOURCE | BC250_SR_BIND_RENDER_TARGET | BC250_SR_BIND_UNORDERED_ACCESS;
        check(is(match(m), BC250_SR_WHY_DESCRIPTION), "another bind");
        m = record; m.MipLevels = m.VkMipLevels = 8; check(is(match(m), BC250_SR_WHY_DESCRIPTION), "another mip count");
        m = record; m.MemorySize = alloc + 1; check(is(match(m), BC250_SR_WHY_MEMORY), "memory above the allocation");
        check(is(Bc250SharedResourceMatchWhy(&record, &record, 0), BC250_SR_WHY_MEMORY), "no allocation");
        m = record; m.DeviceUuid[7] ^= 1; check(is(match(m), BC250_SR_WHY_IDENTITY), "another adapter");
        m = record; m.Magic = 0; check(is(match(m), BC250_SR_WHY_MAGIC), "the opener's own record is read too");
        check(is(Bc250SharedResourceMatchWhy(nullptr, &record, alloc), BC250_SR_WHY_ARGUMENTS) &&
              is(Bc250SharedResourceMatchWhy(&record, nullptr, alloc), BC250_SR_WHY_ARGUMENTS), "arguments");
        const auto buffer = BufferRecord();
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&buffer)), "the buffer record");
        check(is(Bc250SharedResourceMatchWhy(&record, &buffer, alloc), BC250_SR_WHY_DIMENSION), "a buffer is not a texture");
        auto b = buffer; b.BufferSize = 2048; b.MemorySize = 2048; b.CreateUsage = 0x3;
        check(Bc250SharedResourceWhyOk(Bc250SharedResourceMatchWhy(&buffer, &b, 4096)),
              "a buffer of another engine size and usage");
        b = buffer; b.Width = 1004;
        check(is(Bc250SharedResourceMatchWhy(&buffer, &b, 4096), BC250_SR_WHY_DESCRIPTION), "another ByteWidth");
        check(is(Bc250SharedResourceMatchWhy(&buffer, &buffer, 512), BC250_SR_WHY_MEMORY), "a buffer above the allocation");
    }
    // Alignment: the largest one maps to the kernel reader as written; one above it has no blob.
    {
        auto aligned = record; aligned.MemoryAlignment = BC250_SHARED_RESOURCE_MAX_ALIGNMENT;
        check(Bc250SharedResourceAllocInit(&blob, &aligned) && UmdBlobParseAlloc(&blob, sizeof(blob), &view) == UMD_BLOB_OK &&
              view.alignment == 65536 && view.bytes == 720896, "64 KiB alignment");
        aligned.MemoryAlignment = 2 * BC250_SHARED_RESOURCE_MAX_ALIGNMENT;
        check(!Bc250SharedResourceAllocInit(&blob, &aligned) && blob.magic == 0, "no blob above 64 KiB");
        check(Bc250SharedResourceAllocInit(&blob, &record), "restore");
    }

    // The resource record, read by the kernel driver's own readers. A record without the E26R magic is UMD-private
    // data: admitted, no CPU intent, no scan-out, the plain type-0 placement (which a BC2A allocation never takes).
    int shared = -1, cached = -1;
    check(Bc250SurfaceResourcePolicy(&record, sizeof(record), &shared, &cached) == 1 && shared == 0 && cached == 0,
          "CreateAllocation's record policy admits S26R");
    unsigned long version = 9, access = 9;
    check(Bc250SurfaceResourceIntent(&record, sizeof(record), &version, &access) == 1 && version == 0 && access == 0,
          "no E26R intent");
    check(!Bc250SurfaceResourceScanout(&record, sizeof(record)) && !Bc250SurfaceResourceNoCpuAccess(&record, sizeof(record)),
          "no scan-out, no v4 promise");
    BC250_GDI_ALLOCATION_POLICY policy;
    check(WddmGdiRecordPolicy(&record, sizeof(record), &policy) == 1, "CreateAllocation's record placement succeeds");
    check(!WddmGdiRecordScannable(&record, sizeof(record)) && !WddmGdiCreatedScannable(&record, sizeof(record)),
          "never a DirectFlip candidate");
    check(Bc250CpuViewClassOfRecord(&record, sizeof(record)) == BC250_CPU_VIEW_LEGACY, "no record CPU class");
    check(BC250_SHARED_RESOURCE_MAGIC != BC250_SURFACE_RESOURCE_MAGIC && BC250_SHARED_RESOURCE_MAGIC != UMD_BLOB_ALLOC_MAGIC &&
          BC250_SHARED_RESOURCE_MAGIC != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC, "three distinct magics");

    // The LINEAR SURFACE reader refuses the new pair, so an opener that knows only that form does not read it.
    BC250_SHARED_SURFACE surface{};
    const char *why = nullptr;
    check(Bc250SharedSurfaceDecodeWhy(&record, sizeof(record), &blob, sizeof(blob), &surface, &why) != BC250_SHARED_SURFACE_OK &&
          why && std::strcmp(why, BC250_SHARED_SURFACE_WHY_LENGTH) == 0, "the LINEAR SURFACE reader refuses S26R by length");
    {
        // Even at the E26R length, the S26R magic is refused by name.
        unsigned char prefix[sizeof(BC250_SURFACE_RESOURCE_PRIVATE)];
        std::memcpy(prefix, &record, sizeof(prefix));
        BC250_WDDM_ALLOCATION_PRIVATE lb7a{BC250_WDDM_ALLOCATION_PRIVATE_MAGIC, 1, 256, 128, 1024, 21, 131072};
        check(Bc250SharedSurfaceDecodeWhy(prefix, sizeof(prefix), &lb7a, sizeof(lb7a), &surface, &why) != BC250_SHARED_SURFACE_OK &&
              why && std::strcmp(why, BC250_SHARED_SURFACE_WHY_MAGIC) == 0, "the S26R magic at the E26R length");
    }
    // The new reader refuses the old pair: E26R is not S26R.
    {
        BC250_SURFACE_RESOURCE_PRIVATE e26r{BC250_SURFACE_RESOURCE_MAGIC, 3, 1, 0, 256, 128, 1, 1, 28, 1, 0, 0, 0x28, 0, 0, 0};
        BC250_SHARED_RESOURCE out{};
        check(is(Bc250SharedResourceDecodeWhy(&e26r, sizeof(e26r), &out), BC250_SR_WHY_LENGTH), "E26R by length");
        unsigned char padded[BC250_SHARED_RESOURCE_BYTES]{};
        std::memcpy(padded, &e26r, sizeof(e26r));
        check(is(Bc250SharedResourceDecodeWhy(padded, sizeof(padded), &out), BC250_SR_WHY_MAGIC), "E26R by magic");
    }
    std::printf("PASS shared resource contract: %u checks, S26R wire v1 and v2, the match rule, the BC2A field check, "
                "BC2A through umd_blob.c, the kernel driver's record readers, both forms refuse each other\n", checks);
    return 0;
}
