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
}
int main()
{
    // The wire: 240 bytes at fixed offsets, "S26R" first. A field that moves is a new version.
    static_assert(sizeof(BC250_SHARED_RESOURCE) == 240, "S26R size");
    static_assert(offsetof(BC250_SHARED_RESOURCE, Dimension) == 12 && offsetof(BC250_SHARED_RESOURCE, Format) == 32 &&
                  offsetof(BC250_SHARED_RESOURCE, StructureByteStride) == 60 &&
                  offsetof(BC250_SHARED_RESOURCE, CreateFlags) == 64 && offsetof(BC250_SHARED_RESOURCE, VkFormat) == 80 &&
                  offsetof(BC250_SHARED_RESOURCE, Tiling) == 108 && offsetof(BC250_SHARED_RESOURCE, ViewFormatCount) == 116 &&
                  offsetof(BC250_SHARED_RESOURCE, ViewFormats) == 120 && offsetof(BC250_SHARED_RESOURCE, BufferSize) == 168 &&
                  offsetof(BC250_SHARED_RESOURCE, MemorySize) == 176 && offsetof(BC250_SHARED_RESOURCE, MemoryTypeBits) == 192 &&
                  offsetof(BC250_SHARED_RESOURCE, LayoutHash) == 200 && offsetof(BC250_SHARED_RESOURCE, DriverUuid) == 208 &&
                  offsetof(BC250_SHARED_RESOURCE, DeviceUuid) == 224, "S26R offsets");
    const BC250_SHARED_RESOURCE record = TextureRecord();
    check(Bc250SharedResourceWhyOk(Bc250SharedResourceRecordWhy(&record)), "the test record is a record of this form");
    const unsigned char *rb = reinterpret_cast<const unsigned char *>(&record);
    check(rb[0] == 'S' && rb[1] == '2' && rb[2] == '6' && rb[3] == 'R' && rb[8] == 240 && rb[12] == 2, "S26R bytes");
    // The hash is FNV-1a 64 of the eight bytes of each value, low byte first. The pin is the basis times the prime to the\n    // eighth power (eight zero bytes), computed outside this header.
    check(Bc250SharedResourceHash(BC250_SR_FNV_BASIS, 0) == 0x47FE0D7EAF8E51E3ull, "FNV-1a 64 of eight zero bytes");

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
    // The opener's check of the blob is the writer's exact bytes for the record.
    unsigned long long bytes = 0;
    check(Bc250SharedResourceWhyOk(Bc250SharedResourceAllocWhy(&blob, sizeof(blob), &record, &bytes)) && bytes == 700416,
          "the opener admits the creator's blob");
    {
        auto other = blob; other.preferred_heap = UMD_BLOB_HEAP_GTT;
        check(UmdBlobParseAlloc(&other, sizeof(other), &view) == UMD_BLOB_OK &&
              is(Bc250SharedResourceAllocWhy(&other, sizeof(other), &record, &bytes), BC250_SR_WHY_ALLOCATION_RECORD),
              "a blob the kernel driver admits but this form did not write");
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
    std::printf("PASS shared resource contract: %u checks, S26R wire, BC2A through umd_blob.c, the kernel driver's record "
                "readers, both forms refuse each other\n", checks);
    return 0;
}
