/* SPDX-License-Identifier: MIT */
/*
 * driver/contract/bc250_shared_resource.h - the one writer and the one reader of a shared resource in the
 * ENGINE LAYOUT form (BD-373 step 3).
 *
 * A shared resource has one of two wire forms:
 *
 *   LINEAR SURFACE  a single-mip, single-layer 2D texture of a composed format, or NV12. LB7A per
 *                   allocation, E26R v3 per resource (bc250_shared_surface.h). DWM, the compositor's UMD,
 *                   the D3D12 shell and the display path of the kernel driver read it. Unchanged.
 *   ENGINE LAYOUT   every other resource that D3D11 lets an application share: buffers, mip chains,
 *                   arrays, formats with no composed row. The memory holds the Vulkan engine's own layout of
 *                   the resource, so only an engine that makes the same layout can read it. On the wire:
 *                     per allocation  BC2A, struct bc250_umd_alloc_private of bc250_umd_submit.h, version 2,
 *                                     VRAM heap, NO_CPU_ACCESS, the BC250_UMD_A_SHARED flag, written by
 *                                     Bc250SharedResourceAllocInit of
 *                                     bc250_shared_resource_alloc.h and by nothing else.
 *                     per resource    BC250_SHARED_RESOURCE, "S26R", version 1, exactly 240 bytes.
 *
 * The kernel driver needs no change for the second form: it admits a BC2A allocation on CreateAllocation and on
 * OpenAllocation, and it reads a resource record without the E26R magic as UMD-private data (no policy, no
 * refusal). It takes placement and the CPU view from the BC2A blob. driver/contract/test/shared-resource-test.cpp
 * holds these statements to the kernel driver's own readers.
 *
 * Textures of this form are LINEAR. The record carries the whole VkImageCreateInfo that the creator's engine gave
 * and a hash of the layout of every subresource, and the opener compares all of it with what its own engine gives
 * for the same description. A LINEAR image has no metadata, so an opener that creates the same image on the same
 * memory reads the same texels. An OPTIMAL image can have DCC metadata that an opener's engine initializes again
 * when it first uses the image, which would destroy the creator's content; a later version of this record can add
 * OPTIMAL with explicit metadata handling, the Tiling field is there for it.
 *
 * The rules of which descriptions may have this form are D3D11's for shared resources (direct3d-11-1-features.md,
 * "Extended support for shared Texture2D resources"): DEFAULT usage, no CPU access, no multisampling, no depth
 * stencil, no cube, no RESOURCE_CLAMP. Bc250SharedResourceDescWhy is that rule; the shells refuse what it refuses.
 *
 * Plain integers and no API headers. The D3D11 and Vulkan numbers below are the wire's; a consumer with the SDK and
 * Vulkan headers static_asserts them. The BC2A writer is in bc250_shared_resource_alloc.h, which needs the amdgpu
 * uapi headers of bc250_umd_submit.h; only the code that allocates or opens the allocation includes it.
 */
#ifndef BC250_SHARED_RESOURCE_H
#define BC250_SHARED_RESOURCE_H

#if defined(__cplusplus)
extern "C" {
#endif

#define BC250_SHARED_RESOURCE_MAGIC    0x52363253u /* "S26R" little endian */
#define BC250_SHARED_RESOURCE_VERSION  1u
#define BC250_SHARED_RESOURCE_BYTES    240u

#define BC250_SHARED_RESOURCE_BUFFER     1u
#define BC250_SHARED_RESOURCE_TEXTURE2D  2u

/* The view formats of a mutable image, as the engine lists them. DXVK lists the formats of one family. */
#define BC250_SHARED_RESOURCE_VIEW_FORMATS 12u

/* The largest memory alignment of this form. The shell maps the allocation at an address that VidMm selects, and
 * VidMm gives 64 KiB alignment (driver/umd/d3d12/paging.h, kMapGranularity). A larger alignment needs a reserved
 * address range; this form refuses it. LINEAR images and buffers ask for much less. */
#define BC250_SHARED_RESOURCE_MAX_ALIGNMENT 65536ull
#define BC250_SHARED_RESOURCE_PAGE          4096ull
/* The largest memory size; the rounding to the alignment stays inside 64 bits. */
#define BC250_SR_MEMORY_MAX                 0xFFFFFFFFFFFF0000ull

/* D3D11 wire numbers (d3d11.h). */
#define BC250_SR_USAGE_DEFAULT            0u
#define BC250_SR_BIND_VERTEX_BUFFER       0x1u
#define BC250_SR_BIND_INDEX_BUFFER        0x2u
#define BC250_SR_BIND_CONSTANT_BUFFER     0x4u
#define BC250_SR_BIND_SHADER_RESOURCE     0x8u
#define BC250_SR_BIND_STREAM_OUTPUT       0x10u
#define BC250_SR_BIND_RENDER_TARGET       0x20u
#define BC250_SR_BIND_DEPTH_STENCIL       0x40u
#define BC250_SR_BIND_UNORDERED_ACCESS    0x80u
#define BC250_SR_BIND_DECODER             0x200u
#define BC250_SR_BIND_VIDEO_ENCODER       0x400u
#define BC250_SR_MISC_GENERATE_MIPS       0x1u
#define BC250_SR_MISC_DRAWINDIRECT_ARGS   0x10u
#define BC250_SR_MISC_BUFFER_ALLOW_RAW    0x20u
#define BC250_SR_MISC_BUFFER_STRUCTURED   0x40u
#define BC250_SR_BUFFER_BIND_MASK  (BC250_SR_BIND_VERTEX_BUFFER | BC250_SR_BIND_INDEX_BUFFER | \
                                    BC250_SR_BIND_CONSTANT_BUFFER | BC250_SR_BIND_SHADER_RESOURCE | \
                                    BC250_SR_BIND_STREAM_OUTPUT | BC250_SR_BIND_RENDER_TARGET | \
                                    BC250_SR_BIND_UNORDERED_ACCESS)
#define BC250_SR_BUFFER_MISC_MASK  (BC250_SR_MISC_DRAWINDIRECT_ARGS | BC250_SR_MISC_BUFFER_ALLOW_RAW | \
                                    BC250_SR_MISC_BUFFER_STRUCTURED)
#define BC250_SR_TEXTURE_BIND_MASK (BC250_SR_BIND_SHADER_RESOURCE | BC250_SR_BIND_RENDER_TARGET | \
                                    BC250_SR_BIND_UNORDERED_ACCESS | BC250_SR_BIND_DECODER | \
                                    BC250_SR_BIND_VIDEO_ENCODER)
#define BC250_SR_TEXTURE_MISC_MASK BC250_SR_MISC_GENERATE_MIPS
#define BC250_SR_MAX_EDGE          16384u
#define BC250_SR_MAX_MIPS          15u
#define BC250_SR_MAX_LAYERS        2048u
#define BC250_SR_MAX_STRIDE        2048u

/* Vulkan wire numbers (vulkan_core.h). */
#define BC250_SR_VK_IMAGE_TYPE_2D        1u
#define BC250_SR_VK_TILING_LINEAR        1u
#define BC250_SR_VK_SAMPLE_COUNT_1       1u
#define BC250_SR_VK_SHARING_EXCLUSIVE    0u
#define BC250_SR_VK_LAYOUT_UNDEFINED     0u

/* Why a record or an allocation blob was refused. "record" is the OK value, as in bc250_shared_surface.h. Every
 * string is a literal of this header; the shells print it in their refusal line, so a name is part of the contract
 * and is not renamed silently. */
#define BC250_SR_WHY_OK                 "record"
#define BC250_SR_WHY_ARGUMENTS          "record arguments"
#define BC250_SR_WHY_LENGTH             "record length"
#define BC250_SR_WHY_MAGIC              "record magic"
#define BC250_SR_WHY_VERSION            "record version"
#define BC250_SR_WHY_DIMENSION          "record dimension"
#define BC250_SR_WHY_USAGE              "record usage"
#define BC250_SR_WHY_SAMPLES            "record samples"
#define BC250_SR_WHY_EXTENT             "record extent"
#define BC250_SR_WHY_SUBRESOURCES       "record subresources"
#define BC250_SR_WHY_FORMAT             "record format"
#define BC250_SR_WHY_BIND               "record bind flags"
#define BC250_SR_WHY_MISC               "record misc flags"
#define BC250_SR_WHY_STRIDE             "record stride"
#define BC250_SR_WHY_CREATE_INFO        "record create info"
#define BC250_SR_WHY_VIEW_FORMATS       "record view formats"
#define BC250_SR_WHY_MEMORY             "record memory"
#define BC250_SR_WHY_RESERVED           "record reserved"
#define BC250_SR_WHY_DESCRIPTION        "record description"
#define BC250_SR_WHY_LAYOUT             "record layout"
#define BC250_SR_WHY_IDENTITY           "record identity"
#define BC250_SR_WHY_ALLOCATION_LENGTH  "allocation length"
#define BC250_SR_WHY_ALLOCATION_RECORD  "allocation record"

/* One shared resource of the ENGINE LAYOUT form, as it is on the wire: little endian, no padding. */
typedef struct _BC250_SHARED_RESOURCE {
    unsigned int Magic;               /* BC250_SHARED_RESOURCE_MAGIC */
    unsigned int Version;             /* BC250_SHARED_RESOURCE_VERSION */
    unsigned int Bytes;               /* BC250_SHARED_RESOURCE_BYTES */
    unsigned int Dimension;           /* BC250_SHARED_RESOURCE_BUFFER or _TEXTURE2D */

    /* The D3D11 description (D3D11_BUFFER_DESC or D3D11_TEXTURE2D_DESC1 without the layout), without the
     * sharing flags. A buffer has its ByteWidth in Width, 1 in Height, MipLevels and ArraySize, 0 in Format. */
    unsigned int Width, Height, MipLevels, ArraySize;
    unsigned int Format, SampleCount, SampleQuality, Usage;
    unsigned int BindFlags, CpuAccessFlags, MiscFlags, StructureByteStride;

    /* The engine's create info: VkImageCreateInfo for a texture (and the formats of its
     * VkImageFormatListCreateInfo), VkBufferCreateInfo for a buffer (flags, usage, sharing mode and
     * BufferSize; the image fields are 0). */
    unsigned int CreateFlags, CreateUsage, SharingMode, ImageType;
    unsigned int VkFormat, ExtentWidth, ExtentHeight, ExtentDepth;
    unsigned int VkMipLevels, VkArrayLayers, VkSamples, Tiling;
    unsigned int InitialLayout, ViewFormatCount;
    unsigned int ViewFormats[BC250_SHARED_RESOURCE_VIEW_FORMATS];
    unsigned long long BufferSize;

    /* VkMemoryRequirements of the creator's image or buffer. */
    unsigned long long MemorySize, MemoryAlignment;
    unsigned int MemoryTypeBits, Reserved0;

    /* FNV-1a 64 of the VkSubresourceLayout (offset, size, rowPitch, arrayPitch, depthPitch) of every
     * subresource, layer-major then mip, as Bc250SharedResourceHash adds them. 0 for a buffer. */
    unsigned long long LayoutHash;

    /* VkPhysicalDeviceIDProperties of the creator's device: the Vulkan rule for "memory and layouts are
     * compatible". */
    unsigned char DriverUuid[16], DeviceUuid[16];
} BC250_SHARED_RESOURCE;

#if defined(__cplusplus)
static_assert(sizeof(BC250_SHARED_RESOURCE) == BC250_SHARED_RESOURCE_BYTES, "S26R is 240 bytes");
#else
typedef char Bc250SharedResourceSizeCheck[(sizeof(BC250_SHARED_RESOURCE) == BC250_SHARED_RESOURCE_BYTES) ? 1 : -1];
#endif

#define BC250_SR_FNV_BASIS 1469598103934665603ull
#define BC250_SR_FNV_PRIME 1099511628211ull

/* Adds the eight bytes of Value, low byte first, to Hash (FNV-1a 64). */
static __inline unsigned long long Bc250SharedResourceHash(unsigned long long Hash, unsigned long long Value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i)
        Hash = (Hash ^ ((Value >> (i * 8)) & 0xFFull)) * BC250_SR_FNV_PRIME;
    return Hash;
}

static __inline int Bc250SharedResourcePowerOfTwo(unsigned long long Value)
{
    return Value && !(Value & (Value - 1));
}

/* True when Why is BC250_SR_WHY_OK. It compares the text: the language does not make two copies of one string
 * literal the same object, so a pointer compare can see a refusal where there is none. */
static __inline int Bc250SharedResourceWhyOk(const char *Why)
{
    const char *ok = BC250_SR_WHY_OK;
    if (!Why) return 0;
    while (*Why && *Why == *ok) { ++Why; ++ok; }
    return *Why == *ok;
}

/* The D3D11 sharing rule for a description of this form. BC250_SR_WHY_OK when the description may be shared in
 * the ENGINE LAYOUT form. The shells refuse what this refuses. Excluded by D3D11: usage other than DEFAULT, CPU access,
 * multisampling, depth stencil, cube, RESOURCE_CLAMP. Excluded by this form: texture dimensions other than 2D (D3D11
 * shares Texture2D only), GENERATE_MIPS without both shader resource and render target binds. */
static __inline const char *Bc250SharedResourceDescWhy(const BC250_SHARED_RESOURCE *R)
{
    unsigned int edge, chain = 1;
    if (!R) return BC250_SR_WHY_ARGUMENTS;
    if (R->Usage != BC250_SR_USAGE_DEFAULT || R->CpuAccessFlags) return BC250_SR_WHY_USAGE;
    if (R->SampleCount != 1 || R->SampleQuality) return BC250_SR_WHY_SAMPLES;
    if (R->Dimension == BC250_SHARED_RESOURCE_BUFFER) {
        if (!R->Width || R->Height != 1) return BC250_SR_WHY_EXTENT;
        if (R->MipLevels != 1 || R->ArraySize != 1) return BC250_SR_WHY_SUBRESOURCES;
        if (R->Format) return BC250_SR_WHY_FORMAT;
        if (R->BindFlags & ~BC250_SR_BUFFER_BIND_MASK) return BC250_SR_WHY_BIND;
        if (R->MiscFlags & ~BC250_SR_BUFFER_MISC_MASK) return BC250_SR_WHY_MISC;
        if (R->StructureByteStride > BC250_SR_MAX_STRIDE ||
            ((R->MiscFlags & BC250_SR_MISC_BUFFER_STRUCTURED) &&
             (!R->StructureByteStride || (R->StructureByteStride & 3u) || R->Width % R->StructureByteStride)))
            return BC250_SR_WHY_STRIDE;
        return BC250_SR_WHY_OK;
    }
    if (R->Dimension != BC250_SHARED_RESOURCE_TEXTURE2D) return BC250_SR_WHY_DIMENSION;
    if (!R->Width || !R->Height || R->Width > BC250_SR_MAX_EDGE || R->Height > BC250_SR_MAX_EDGE)
        return BC250_SR_WHY_EXTENT;
    for (edge = R->Width > R->Height ? R->Width : R->Height; edge > 1; edge >>= 1) ++chain;
    if (!R->MipLevels || R->MipLevels > BC250_SR_MAX_MIPS || R->MipLevels > chain ||
        !R->ArraySize || R->ArraySize > BC250_SR_MAX_LAYERS) return BC250_SR_WHY_SUBRESOURCES;
    if (!R->Format) return BC250_SR_WHY_FORMAT;
    if (R->BindFlags & ~BC250_SR_TEXTURE_BIND_MASK) return BC250_SR_WHY_BIND;
    if ((R->MiscFlags & ~BC250_SR_TEXTURE_MISC_MASK) ||
        ((R->MiscFlags & BC250_SR_MISC_GENERATE_MIPS) &&
         (R->BindFlags & (BC250_SR_BIND_SHADER_RESOURCE | BC250_SR_BIND_RENDER_TARGET)) !=
             (BC250_SR_BIND_SHADER_RESOURCE | BC250_SR_BIND_RENDER_TARGET)))
        return BC250_SR_WHY_MISC;
    if (R->StructureByteStride) return BC250_SR_WHY_STRIDE;
    return BC250_SR_WHY_OK;
}

/* The shape rule of a whole record: the header, the description (Bc250SharedResourceDescWhy), and an engine
 * create info and memory that this form can carry. It does not compare the record with any engine; that is the
 * opener's work. */
static __inline const char *Bc250SharedResourceRecordWhy(const BC250_SHARED_RESOURCE *R)
{
    const char *why;
    unsigned int i;
    if (!R) return BC250_SR_WHY_ARGUMENTS;
    if (R->Magic != BC250_SHARED_RESOURCE_MAGIC) return BC250_SR_WHY_MAGIC;
    if (R->Version != BC250_SHARED_RESOURCE_VERSION) return BC250_SR_WHY_VERSION;
    if (R->Bytes != BC250_SHARED_RESOURCE_BYTES) return BC250_SR_WHY_LENGTH;
    why = Bc250SharedResourceDescWhy(R);
    if (!Bc250SharedResourceWhyOk(why)) return why;
    if (R->SharingMode != BC250_SR_VK_SHARING_EXCLUSIVE || !R->CreateUsage) return BC250_SR_WHY_CREATE_INFO;
    if (R->Dimension == BC250_SHARED_RESOURCE_BUFFER) {
        if (R->ImageType || R->VkFormat || R->ExtentWidth || R->ExtentHeight || R->ExtentDepth ||
            R->VkMipLevels || R->VkArrayLayers || R->VkSamples || R->Tiling || R->InitialLayout ||
            R->BufferSize < R->Width) return BC250_SR_WHY_CREATE_INFO;
        if (R->ViewFormatCount) return BC250_SR_WHY_VIEW_FORMATS;
        if (R->LayoutHash) return BC250_SR_WHY_CREATE_INFO;
    } else {
        if (R->ImageType != BC250_SR_VK_IMAGE_TYPE_2D || !R->VkFormat || R->ExtentWidth != R->Width ||
            R->ExtentHeight != R->Height || R->ExtentDepth != 1 || R->VkMipLevels != R->MipLevels ||
            R->VkArrayLayers != R->ArraySize || R->VkSamples != BC250_SR_VK_SAMPLE_COUNT_1 ||
            R->Tiling != BC250_SR_VK_TILING_LINEAR || R->InitialLayout != BC250_SR_VK_LAYOUT_UNDEFINED ||
            R->BufferSize) return BC250_SR_WHY_CREATE_INFO;
        if (R->ViewFormatCount > BC250_SHARED_RESOURCE_VIEW_FORMATS) return BC250_SR_WHY_VIEW_FORMATS;
    }
    for (i = R->ViewFormatCount; i < BC250_SHARED_RESOURCE_VIEW_FORMATS; ++i)
        if (R->ViewFormats[i]) return BC250_SR_WHY_VIEW_FORMATS;
    for (i = 0; i < R->ViewFormatCount; ++i)
        if (!R->ViewFormats[i]) return BC250_SR_WHY_VIEW_FORMATS;
    if (!R->MemorySize || R->MemorySize > BC250_SR_MEMORY_MAX || !R->MemoryTypeBits ||
        !Bc250SharedResourcePowerOfTwo(R->MemoryAlignment) || R->MemoryAlignment > BC250_SHARED_RESOURCE_MAX_ALIGNMENT)
        return BC250_SR_WHY_MEMORY;
    if (R->Reserved0) return BC250_SR_WHY_RESERVED;
    return BC250_SR_WHY_OK;
}

/* Reads a received record: exactly BC250_SHARED_RESOURCE_BYTES bytes that pass Bc250SharedResourceRecordWhy.
 * *Out is written only on BC250_SR_WHY_OK. Data may be unaligned. */
static __inline const char *Bc250SharedResourceDecodeWhy(const void *Data, unsigned int Bytes,
                                                        BC250_SHARED_RESOURCE *Out)
{
    BC250_SHARED_RESOURCE r;
    const unsigned char *s = (const unsigned char *)Data;
    unsigned char *d = (unsigned char *)&r;
    const char *why;
    unsigned int i;
    if (!Data || !Out) return BC250_SR_WHY_ARGUMENTS;
    if (Bytes != BC250_SHARED_RESOURCE_BYTES) return BC250_SR_WHY_LENGTH;
    for (i = 0; i < BC250_SHARED_RESOURCE_BYTES; ++i) d[i] = s[i];
    why = Bc250SharedResourceRecordWhy(&r);
    if (Bc250SharedResourceWhyOk(why)) *Out = r;
    return why;
}

#if defined(__cplusplus)
}
#endif

#endif /* BC250_SHARED_RESOURCE_H */
