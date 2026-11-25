// SPDX-License-Identifier: MIT
// The wire format of a shared linear surface: driver/contract/bc250_shared_surface.h, checked against byte literals.
//
// The two records travel between processes and between APIs, so their bytes are the contract, not the structures
// this driver happens to compile today. Every case below writes or reads an explicit little-endian byte array, so a
// change to a field's order, width or offset fails here even when both sides of this build agree with each other.
//
// The surface is the one the shared cells of tools/win/capture-share create: 256x254 B8G8R8A8_UNORM, row pitch 1024
// (RADV rounds a row to 256 bytes), a backing of 0x40000 bytes (the rows of four-row blocks, page rounded), a shader
// resource and a render target, shared, with no access intent. Height 254 is deliberate: it is not a multiple of
// four, so the geometry rule and the page rounding are both exercised by one record.
//
// No Windows header and no API header: the kernel driver's C, both shells' C++ and the Mesa winsys's C all include
// the contract header. This test includes only it and the kernel driver's admission of a format
// (driver/kmd/surface_format.h, plain integers too). The format numbers come from the surface format table.
#include "../bc250_shared_surface.h"
#include "../../kmd/surface_format.h"

#include <cstdio>
#include <cstring>

namespace {

unsigned failures = 0;
unsigned checks = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++failures;
    } else {
        std::printf("ok    %s\n", what);
    }
}

// LB7A v1, 32 bytes: Magic "LB7A", Version 1, Width 256, Height 254, Pitch 1024, Format D3DDDIFMT_A8R8G8B8 (21),
// Size 0x40000.
const unsigned char kAllocation[32] = {
    0x4C, 0x42, 0x37, 0x41, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// E26R v3, 64 bytes: Magic "E26R", Version 3, Shared 1, Access 0, then the serialized D3D11_TEXTURE2D_DESC1:
// Width 256, Height 254, MipLevels 1, ArraySize 1, Format DXGI B8G8R8A8_UNORM (87), SampleCount 1, SampleQuality 0,
// Usage DEFAULT (0), BindFlags SHADER_RESOURCE|RENDER_TARGET (0x28), CpuAccessFlags 0, MiscFlags 0,
// TextureLayout UNDEFINED (0).
const unsigned char kResource[64] = {
    0x45, 0x32, 0x36, 0x52, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0xFE, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x57, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

const unsigned long kWidth = 256, kHeight = 254, kPitch = 1024, kDxgi = 87, kD3dDdi = 21, kBind = 0x28;
const unsigned long long kSize = 0x40000;

BC250_SHARED_SURFACE lab_surface() {
    BC250_SHARED_SURFACE s;
    std::memset(&s, 0, sizeof(s));
    s.Width = kWidth;
    s.Height = kHeight;
    s.Pitch = kPitch;
    s.DxgiFormat = kDxgi;
    s.D3dDdiFormat = kD3dDdi;
    s.BindFlags = kBind;
    s.BytesPerPixel = 4;
    s.Size = kSize;
    s.Shared = 1;
    return s;
}

int decode(const void* resource, unsigned resource_bytes, const void* allocation, unsigned allocation_bytes,
           BC250_SHARED_SURFACE* out) {
    return Bc250SharedSurfaceDecode(resource, resource_bytes, allocation, allocation_bytes, out);
}

// The strict policy's decode of one record with one field changed, as a byte copy of the lab's own.
int decode_with(unsigned offset, unsigned long value, BC250_SHARED_SURFACE* out) {
    unsigned char resource[64];
    std::memcpy(resource, kResource, sizeof(resource));
    for (unsigned i = 0; i < 4; ++i) resource[offset + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
    return decode(resource, sizeof(resource), kAllocation, sizeof(kAllocation), out);
}

// The same, with the check the decode decided at. The string is a literal of the header, so comparing the pointer
// would pass by accident in one translation unit; the text is compared instead, which is what a log line carries.
const char* why_with(unsigned offset, unsigned long value) {
    unsigned char resource[64];
    BC250_SHARED_SURFACE out;
    const char* why = 0;
    std::memcpy(resource, kResource, sizeof(resource));
    for (unsigned i = 0; i < 4; ++i) resource[offset + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
    Bc250SharedSurfaceDecodeWhy(resource, sizeof(resource), kAllocation, sizeof(kAllocation), &out, &why);
    return why ? why : "(none)";
}

bool is(const char* got, const char* want) { return got && std::strcmp(got, want) == 0; }

} // namespace

int main() {
    // The structures are exactly the bytes they describe. sizeof is the wire size, not a convention.
    check(sizeof(BC250_WDDM_ALLOCATION_PRIVATE) == 32, "LB7A v1 is 32 bytes");
    check(sizeof(BC250_SURFACE_RESOURCE_PRIVATE) == 64, "E26R v3 is 64 bytes");
    // The version this driver writes is the version byte of the record below, not a number beside it.
    check(BC250_SURFACE_RESOURCE_TEXTURE_VERSION == kResource[4], "the texture record is version 3");

    // The encoder writes the lab's bytes, byte for byte.
    {
        const BC250_SHARED_SURFACE s = lab_surface();
        BC250_WDDM_ALLOCATION_PRIVATE allocation;
        BC250_SURFACE_RESOURCE_PRIVATE resource;
        std::memset(&allocation, 0xCD, sizeof(allocation));
        std::memset(&resource, 0xCD, sizeof(resource));
        const int status = Bc250SharedSurfaceEncode(&s, &allocation, &resource);
        check(status == BC250_SHARED_SURFACE_OK, "encode: the lab's surface is admitted");
        check(std::memcmp(&allocation, kAllocation, sizeof(kAllocation)) == 0,
              "encode: the allocation record is the 32 bytes of the wire");
        check(std::memcmp(&resource, kResource, sizeof(kResource)) == 0,
              "encode: the resource record is the 64 bytes of the wire");
    }

    // The decoder reads them back into the same surface.
    {
        BC250_SHARED_SURFACE out;
        std::memset(&out, 0xCD, sizeof(out));
        const int status = decode(kResource, sizeof(kResource), kAllocation, sizeof(kAllocation), &out);
        check(status == BC250_SHARED_SURFACE_OK, "decode: the lab's records are admitted");
        check(out.Width == kWidth && out.Height == kHeight && out.Pitch == kPitch && out.Size == kSize,
              "decode: geometry");
        check(out.DxgiFormat == kDxgi && out.D3dDdiFormat == kD3dDdi && out.BytesPerPixel == 4, "decode: format");
        check(out.BindFlags == kBind && out.Shared == 1 && out.Access == 0 && out.MiscFlags == 0,
              "decode: bind flags and policy");
    }

    // The kernel driver's own parser admits the same resource record, and says what it means.
    {
        int shared = -1, cached = -1;
        const int ok = Bc250SurfaceResourcePolicy(kResource, sizeof(kResource), &shared, &cached);
        check(ok == 1 && shared == 1 && cached == 0, "the kernel driver's parser admits the record: shared, not cached");
        check(Bc250SurfaceResourceScanout(kResource, sizeof(kResource)) == 0, "the record asks for no scan-out");
    }

    // Structural refusals. A record of the wrong length is refused whatever it contains, so a v1 or v2 record never
    // reaches a reader that expects the texture description.
    {
        BC250_SHARED_SURFACE out;
        check(decode(kResource, 63, kAllocation, sizeof(kAllocation), &out) == BC250_SHARED_SURFACE_MALFORMED &&
                  decode(kResource, 65, kAllocation, sizeof(kAllocation), &out) == BC250_SHARED_SURFACE_MALFORMED &&
                  decode(kResource, 16, kAllocation, sizeof(kAllocation), &out) == BC250_SHARED_SURFACE_MALFORMED &&
                  decode(kResource, 12, kAllocation, sizeof(kAllocation), &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: a resource record of 63, 65, 16 or 12 bytes is refused");
        check(decode(kResource, sizeof(kResource), kAllocation, 31, &out) == BC250_SHARED_SURFACE_MALFORMED &&
                  decode(kResource, sizeof(kResource), kAllocation, 33, &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: an allocation record of 31 or 33 bytes is refused");
        check(decode(0, sizeof(kResource), kAllocation, sizeof(kAllocation), &out) == BC250_SHARED_SURFACE_MALFORMED &&
                  decode(kResource, sizeof(kResource), 0, sizeof(kAllocation), &out) == BC250_SHARED_SURFACE_MALFORMED &&
                  decode(kResource, sizeof(kResource), kAllocation, sizeof(kAllocation), 0) ==
                      BC250_SHARED_SURFACE_MALFORMED,
              "decode: a null record or a null result is refused");
        unsigned char magic[64];
        std::memcpy(magic, kResource, sizeof(magic));
        magic[0] = 0x44;
        check(decode(magic, sizeof(magic), kAllocation, sizeof(kAllocation), &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: another writer's magic is refused");
        unsigned char lb7a[32];
        std::memcpy(lb7a, kAllocation, sizeof(lb7a));
        lb7a[4] = 2;
        check(decode(kResource, sizeof(kResource), lb7a, sizeof(lb7a), &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: an allocation record of another version is refused");
    }

    // Field refusals, each one field of the lab's own record (offsets of E26R v3).
    {
        BC250_SHARED_SURFACE out;
        check(decode_with(4, 2, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: version 2 in a 64-byte record");
        check(decode_with(8, 0, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: Shared 0 (the strict policy)");
        check(decode_with(8, 2, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: Shared 2");
        check(decode_with(12, BC250_SURFACE_RESOURCE_PRIMARY, &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: a primary's access intent (the strict policy)");
        check(decode_with(12, 0x8, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: an access bit of no meaning");
        check(decode_with(16, 255, &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: a width that is not the allocation's");
        check(decode_with(20, 253, &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: a height that is not the allocation's");
        check(decode_with(24, 2, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: two mip levels");
        check(decode_with(28, 2, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: an array of two");
        check(decode_with(32, 42, &out) == BC250_SHARED_SURFACE_FORMAT,
              "decode: a format no shared surface has is not implemented, not malformed");
        check(decode_with(36, 4, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: four samples");
        check(decode_with(40, 1, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: a sample quality");
        check(decode_with(44, 1, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: a usage that is not DEFAULT");
        check(decode_with(48, 0x40, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: a bind flag outside the mask");
        check(decode_with(52, 1, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: a CPU access flag");
        check(decode_with(56, 0x2, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: a misc flag outside the mask");
        check(decode_with(60, 1, &out) == BC250_SHARED_SURFACE_MALFORMED, "decode: a texture layout of its own");
        // The two misc flags the wire admits, and no bind flag at all (a surface only copied from).
        check(decode_with(56, BC250_SHARED_MISC_GENERATE_MIPS, &out) == BC250_SHARED_SURFACE_OK &&
                  out.MiscFlags == BC250_SHARED_MISC_GENERATE_MIPS,
              "decode: GENERATE_MIPS is carried through");
        check(decode_with(48, 0, &out) == BC250_SHARED_SURFACE_OK && out.BindFlags == 0,
              "decode: a surface with no bind flag is admitted");
    }

    // Which check decided, by name. One status for seventeen checks puts the reason behind one word: a cross-API
    // open that declined "at record" had written "resource private data 16 bytes (version 2)" on the same line, and
    // what cost a lab pass was that the kit never surfaced that line (BD-075 round 2, 2026-10-06). These names are
    // what the driver's refusal line prints and what the lab kit matches, so they are part of the contract.
    {
        BC250_SHARED_SURFACE out;
        const char* why = 0;
        check(Bc250SharedSurfaceDecodeWhy(kResource, sizeof(kResource), kAllocation, sizeof(kAllocation), &out,
                                          &why) == BC250_SHARED_SURFACE_OK &&
                  is(why, BC250_SHARED_SURFACE_WHY_OK),
              "why: an admitted pair says \"record\"");
        why = 0;
        Bc250SharedSurfaceDecodeWhy(kResource, 16, kAllocation, sizeof(kAllocation), &out, &why);
        check(is(why, BC250_SHARED_SURFACE_WHY_LENGTH), "why: a 16-byte E26R v2 record is \"record length\"");
        why = 0;
        Bc250SharedSurfaceDecodeWhy(kResource, sizeof(kResource), kAllocation, 31, &out, &why);
        check(is(why, BC250_SHARED_SURFACE_WHY_ALLOCATION_LENGTH),
              "why: a short allocation record is \"allocation length\"");
        why = 0;
        Bc250SharedSurfaceDecodeWhy(0, sizeof(kResource), kAllocation, sizeof(kAllocation), &out, &why);
        check(is(why, BC250_SHARED_SURFACE_WHY_ARGUMENTS), "why: a null blob is \"record arguments\"");
        check(is(why_with(4, 2), BC250_SHARED_SURFACE_WHY_VERSION), "why: version 2 in 64 bytes is \"record version\"");
        check(is(why_with(0, 0x52363246ul), BC250_SHARED_SURFACE_WHY_MAGIC), "why: another magic is \"record magic\"");
        check(is(why_with(8, 0), BC250_SHARED_SURFACE_WHY_SHARED), "why: Shared 0 is \"record shared\"");
        // The kernel driver's own parser, before any field of ours: Shared above 1 and a SCANOUT intent with no
        // PRIMARY are both records it refuses, and an opener that admitted one would describe memory the kernel
        // placed by another rule. Pinned because the name is part of the contract (BD-075 review, 2026-10-06).
        check(is(why_with(8, 2), BC250_SHARED_SURFACE_WHY_POLICY), "why: Shared 2 is \"record policy\"");
        check(is(why_with(12, BC250_SURFACE_RESOURCE_SCANOUT), BC250_SHARED_SURFACE_WHY_POLICY),
              "why: a SCANOUT intent with no PRIMARY is \"record policy\"");
        check(is(why_with(12, BC250_SURFACE_RESOURCE_CPU_READ), BC250_SHARED_SURFACE_WHY_ACCESS),
              "why: a CPU_READ intent is \"record access\"");
        check(is(why_with(16, 255), BC250_SHARED_SURFACE_WHY_GEOMETRY),
              "why: a width that is not the allocation's is \"record geometry\"");
        check(is(why_with(24, 2), BC250_SHARED_SURFACE_WHY_SUBRESOURCES), "why: two mips is \"record subresources\"");
        check(is(why_with(32, 42), BC250_SHARED_SURFACE_WHY_FORMAT), "why: a format with no row is \"record format\"");
        check(is(why_with(44, 1), BC250_SHARED_SURFACE_WHY_USAGE), "why: a usage of its own is \"record usage\"");
        check(is(why_with(48, 0x40), BC250_SHARED_SURFACE_WHY_FLAGS),
              "why: a bind flag outside the mask is \"record flags\"");
        unsigned char lb7a[32];
        std::memcpy(lb7a, kAllocation, sizeof(lb7a));
        lb7a[4] = 2;
        why = 0;
        Bc250SharedSurfaceDecodeWhy(kResource, sizeof(kResource), lb7a, sizeof(lb7a), &out, &why);
        check(is(why, BC250_SHARED_SURFACE_WHY_ALLOCATION_RECORD),
              "why: an LB7A of another version is \"allocation record\"");
        std::memcpy(lb7a, kAllocation, sizeof(lb7a));
        lb7a[20] = 32;                                  // D3DDDIFMT_A8B8G8R8 under a BGRA8 resource record
        why = 0;
        Bc250SharedSurfaceDecodeWhy(kResource, sizeof(kResource), lb7a, sizeof(lb7a), &out, &why);
        check(is(why, BC250_SHARED_SURFACE_WHY_ALLOCATION_FORMAT),
              "why: an LB7A format that is not the row's is \"allocation format\"");
        std::memcpy(lb7a, kAllocation, sizeof(lb7a));
        lb7a[16] = 0;                                   // a pitch below one row of pixels
        lb7a[17] = 0;
        why = 0;
        Bc250SharedSurfaceDecodeWhy(kResource, sizeof(kResource), lb7a, sizeof(lb7a), &out, &why);
        check(is(why, BC250_SHARED_SURFACE_WHY_ALLOCATION_GEOMETRY),
              "why: a pitch below one row is \"allocation geometry\"");
        // The extent check needs the edge wrong in BOTH records: an edge wrong in one of them is the geometry
        // disagreement above, which decides first. 20000 is past BC250_SHARED_MAX_EDGE (16384), and the kernel
        // driver's policy does not read the width, so the extent name is what the refusal says. Pinned because
        // the name is part of the contract (BD-075 review, 2026-10-06).
        unsigned char wide[64];
        std::memcpy(wide, kResource, sizeof(wide));
        wide[16] = 0x20;                                // Width 20000 (0x4E20) in the resource record
        wide[17] = 0x4E;
        std::memcpy(lb7a, kAllocation, sizeof(lb7a));
        lb7a[8] = 0x20;                                 // and the same width in the allocation record
        lb7a[9] = 0x4E;
        why = 0;
        Bc250SharedSurfaceDecodeWhy(wide, sizeof(wide), lb7a, sizeof(lb7a), &out, &why);
        check(is(why, BC250_SHARED_SURFACE_WHY_EXTENT), "why: an edge past 16384 in both records is \"record extent\"");
    }

    // BD-408: the bare form, a surface with no resource record. The blob is the one measured on unit A (2026-11-25,
    // a D3D9On12 windowed Present): the kernel driver's GDI texture of 624x441, LB7A v1 and the GDI1 trailer, 48
    // bytes. Pitch 2560 is the width rounded up to 64 pixels, Size 1136640 is 2560 times 444 rows (the height rounded
    // up to four rows, DcnSharedTextureLayout), Format D3DDDIFMT_A8R8G8B8 (21), GDI1 Type 1 (a GDI texture), Flags 0.
    {
        const unsigned char kGdi[48] = {
            0x4C, 0x42, 0x37, 0x41, 0x01, 0x00, 0x00, 0x00, 0x70, 0x02, 0x00, 0x00, 0xB9, 0x01, 0x00, 0x00,
            0x00, 0x0A, 0x00, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x58, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x47, 0x44, 0x49, 0x31, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        BC250_SHARED_SURFACE out;
        const char* why = 0;
        std::memset(&out, 0, sizeof(out));
        check(Bc250SharedSurfaceDecodeBareWhy(kGdi, sizeof(kGdi), &out, &why) == BC250_SHARED_SURFACE_OK &&
                  is(why, BC250_SHARED_SURFACE_WHY_OK) && out.Width == 624 && out.Height == 441 &&
                  out.Pitch == 2560 && out.DxgiFormat == kDxgi && out.D3dDdiFormat == kD3dDdi &&
                  out.BindFlags == kBind && out.BytesPerPixel == 4 && out.Size == 1136640ull && !out.MiscFlags &&
                  out.Shared == 1 && !out.Access,
              "bare: the measured 48-byte GDI texture blob decodes to a shared 624x441 B8G8R8A8_UNORM texture");
        // The same LB7A alone: the 32-byte form of the compositor's hosted surfaces.
        check(Bc250SharedSurfaceDecodeBareWhy(kGdi, 32, &out, &why) == BC250_SHARED_SURFACE_OK && out.Width == 624,
              "bare: the 32-byte LB7A prefix alone decodes");
        unsigned char gdi[48];
        const auto bare_why = [&](unsigned offset, unsigned long value, unsigned bytes, BC250_SHARED_SURFACE* o) {
            std::memcpy(gdi, kGdi, sizeof(gdi));
            for (unsigned i = 0; i < 4; ++i) gdi[offset + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
            const char* w = 0;
            const int status = Bc250SharedSurfaceDecodeBareWhy(gdi, bytes, o, &w);
            return status == BC250_SHARED_SURFACE_OK ? "" : w;
        };
        check(is(bare_why(36, 3, 48, &out), BC250_SHARED_SURFACE_WHY_BARE_TYPE),
              "bare: a GDI staging surface (type 3) is \"bare allocation type\"");
        check(is(bare_why(40, 1, 48, &out), BC250_SHARED_SURFACE_WHY_BARE_TYPE),
              "bare: GDI1 flags are \"bare allocation type\"");
        check(is(bare_why(44, 1, 48, &out), BC250_SHARED_SURFACE_WHY_BARE_TYPE),
              "bare: a GDI1 reserved word is \"bare allocation type\"");
        check(is(bare_why(32, 0, 48, &out), BC250_SHARED_SURFACE_WHY_BARE_TYPE),
              "bare: 48 bytes without the GDI1 magic are \"bare allocation type\"");
        check(is(bare_why(32, 0x31494447ul, 47, &out), BC250_SHARED_SURFACE_WHY_BARE_LENGTH),
              "bare: a blob of 47 bytes is \"bare allocation length\"");
        check(is(bare_why(32, 0x31494447ul, 64, &out), BC250_SHARED_SURFACE_WHY_BARE_LENGTH),
              "bare: a blob of 64 bytes is \"bare allocation length\"");
        check(is(bare_why(4, 2, 48, &out), BC250_SHARED_SURFACE_WHY_ALLOCATION_RECORD),
              "bare: an LB7A v2 is \"allocation record\"");
        check(is(bare_why(20, 42, 48, &out), BC250_SHARED_SURFACE_WHY_ALLOCATION_FORMAT),
              "bare: a D3DDDI format with no COMPOSED row is \"allocation format\"");
        check(is(bare_why(12, 16385, 48, &out), BC250_SHARED_SURFACE_WHY_EXTENT),
              "bare: an edge past 16384 is \"record extent\"");
        check(is(bare_why(16, 2048, 48, &out), BC250_SHARED_SURFACE_WHY_ALLOCATION_GEOMETRY),
              "bare: a pitch below one row is \"allocation geometry\"");
        check(is(bare_why(24, 2560ul * 440, 48, &out), BC250_SHARED_SURFACE_WHY_ALLOCATION_GEOMETRY),
              "bare: a size below the rows is \"allocation geometry\"");
        why = 0;
        check(Bc250SharedSurfaceDecodeBareWhy(0, 48, &out, &why) == BC250_SHARED_SURFACE_MALFORMED &&
                  is(why, BC250_SHARED_SURFACE_WHY_ARGUMENTS),
              "bare: a null blob is \"record arguments\"");
        // The format refusal is "not implemented", as in the pair's decode.
        std::memcpy(gdi, kGdi, sizeof(gdi));
        gdi[20] = 42;
        check(Bc250SharedSurfaceDecodeBareWhy(gdi, sizeof(gdi), &out, 0) == BC250_SHARED_SURFACE_FORMAT,
              "bare: a format with no COMPOSED row reports the format status");
    }

    // The allocation record's geometry, against the row the resource record names: 4 bytes a pixel here.
    {
        BC250_SHARED_SURFACE out;
        unsigned char lb7a[32];
        const auto with = [&](unsigned offset, unsigned long long value, unsigned bytes) {
            std::memcpy(lb7a, kAllocation, sizeof(lb7a));
            for (unsigned i = 0; i < bytes; ++i) lb7a[offset + i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
            return decode(kResource, sizeof(kResource), lb7a, sizeof(lb7a), &out);
        };
        check(with(16, 1008, 4) == BC250_SHARED_SURFACE_MALFORMED, "decode: a pitch below one row of pixels");
        check(with(16, 0, 4) == BC250_SHARED_SURFACE_MALFORMED, "decode: a pitch of zero");
        check(with(16, 1026, 4) == BC250_SHARED_SURFACE_MALFORMED, "decode: a pitch that is not whole pixels");
        check(with(24, kPitch * 254ull - 1, 8) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: a size that does not cover every row");
        check(with(24, kPitch * 254ull, 8) == BC250_SHARED_SURFACE_OK,
              "decode: a size of exactly the rows it describes is admitted");
        check(with(20, 22, 4) == BC250_SHARED_SURFACE_MALFORMED,
              "decode: an allocation format that is not the resource format's row");
        check(with(8, 0, 4) == BC250_SHARED_SURFACE_MALFORMED, "decode: a width of zero");
    }

    // The admitted form: the policy is the opener's, not the format's. The D3D11 shell opens a primary and a surface
    // that shares nothing, which the strict form refuses; no policy admits an access bit of no meaning.
    {
        BC250_SHARED_SURFACE out;
        unsigned char resource[64];
        BC250_SHARED_SURFACE_ADMIT admit;
        admit.RequireShared = 0;
        admit.AccessMask = BC250_SURFACE_RESOURCE_ACCESS_MASK;
        std::memcpy(resource, kResource, sizeof(resource));
        resource[8] = 0;                                        // Shared 0
        check(Bc250SharedSurfaceDecodeAdmitted(resource, sizeof(resource), kAllocation, sizeof(kAllocation), &admit,
                                               &out) == BC250_SHARED_SURFACE_OK &&
                  out.Shared == 0,
              "decode admitted: a record that shares nothing, for an opener that admits it");
        std::memcpy(resource, kResource, sizeof(resource));
        resource[12] = BC250_SURFACE_RESOURCE_PRIMARY;           // Access PRIMARY
        check(Bc250SharedSurfaceDecodeAdmitted(resource, sizeof(resource), kAllocation, sizeof(kAllocation), &admit,
                                               &out) == BC250_SHARED_SURFACE_OK &&
                  out.Access == BC250_SURFACE_RESOURCE_PRIMARY,
              "decode admitted: a primary's record, for an opener that admits it");
        admit.AccessMask = 0;
        check(Bc250SharedSurfaceDecodeAdmitted(resource, sizeof(resource), kAllocation, sizeof(kAllocation), &admit,
                                               &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode admitted: the same record refused by an opener that admits no access intent");
        admit.AccessMask = BC250_SURFACE_RESOURCE_ACCESS_MASK;
        std::memcpy(resource, kResource, sizeof(resource));
        resource[12] = 0x8;                                     // an access bit the kernel driver does not define
        check(Bc250SharedSurfaceDecodeAdmitted(resource, sizeof(resource), kAllocation, sizeof(kAllocation), &admit,
                                               &out) == BC250_SHARED_SURFACE_MALFORMED,
              "decode admitted: an access bit of no meaning is refused by every policy");
        // BD-223 step R3: the scan-out record of a displayable surface with no primary descriptor. The D3D11 opener
        // (whose mask is the whole access mask) takes it; an opener whose mask lacks the bit refuses it by the
        // access name, and DISPLAYABLE without SCANOUT is a record the kernel driver refuses, so the policy name.
        const unsigned long shown = BC250_SURFACE_RESOURCE_SCANOUT | BC250_SURFACE_RESOURCE_DISPLAYABLE;
        std::memcpy(resource, kResource, sizeof(resource));
        resource[12] = static_cast<unsigned char>(shown);
        check(Bc250SharedSurfaceDecodeAdmitted(resource, sizeof(resource), kAllocation, sizeof(kAllocation), &admit,
                                               &out) == BC250_SHARED_SURFACE_OK &&
                  out.Access == shown,
              "decode admitted: SCANOUT | DISPLAYABLE, for the D3D11 opener's mask");
        const char* why = 0;
        admit.AccessMask = BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT |
                           BC250_SURFACE_RESOURCE_CPU_READ;
        check(Bc250SharedSurfaceDecodeDetail(resource, sizeof(resource), kAllocation, sizeof(kAllocation), &admit,
                                             &out, &why) == BC250_SHARED_SURFACE_MALFORMED &&
                  is(why, BC250_SHARED_SURFACE_WHY_ACCESS),
              "decode admitted: a mask without DISPLAYABLE refuses it as \"record access\"");
        check(is(why_with(12, shown), BC250_SHARED_SURFACE_WHY_ACCESS),
              "why: the strict form refuses SCANOUT | DISPLAYABLE as \"record access\"");
        check(is(why_with(12, BC250_SURFACE_RESOURCE_DISPLAYABLE), BC250_SHARED_SURFACE_WHY_POLICY),
              "why: DISPLAYABLE with no SCANOUT is \"record policy\"");
        check(is(why_with(12, shown | BC250_SURFACE_RESOURCE_PRIMARY), BC250_SHARED_SURFACE_WHY_POLICY),
              "why: DISPLAYABLE next to PRIMARY is \"record policy\"");
        admit.AccessMask = BC250_SURFACE_RESOURCE_ACCESS_MASK;
    }

    // The encoder writes the displayable scan-out record, and the D3D11 opener's form reads it back unchanged.
    {
        BC250_SHARED_SURFACE s, back;
        BC250_WDDM_ALLOCATION_PRIVATE allocation;
        BC250_SURFACE_RESOURCE_PRIVATE resource;
        BC250_SHARED_SURFACE_ADMIT admit;
        admit.RequireShared = 1;
        admit.AccessMask = BC250_SURFACE_RESOURCE_ACCESS_MASK;
        std::memset(&s, 0, sizeof(s));
        s.Width = 1920;
        s.Height = 1080;
        s.Pitch = 7680;
        s.DxgiFormat = 87;                                      // DXGI_FORMAT_B8G8R8A8_UNORM
        s.D3dDdiFormat = 21;                                    // D3DDDIFMT_A8R8G8B8
        s.BindFlags = BC250_SHARED_BIND_RENDER_TARGET;
        s.BytesPerPixel = 4;
        s.Size = 7680ull * 1080;
        s.Shared = 1;
        s.Access = BC250_SURFACE_RESOURCE_SCANOUT | BC250_SURFACE_RESOURCE_DISPLAYABLE;
        check(Bc250SharedSurfaceEncode(&s, &allocation, &resource) == BC250_SHARED_SURFACE_OK &&
                  resource.Access == s.Access,
              "encode: SCANOUT | DISPLAYABLE is a record the kernel driver admits");
        check(Bc250SharedSurfaceDecodeAdmitted(&resource, sizeof(resource), &allocation, sizeof(allocation), &admit,
                                               &back) == BC250_SHARED_SURFACE_OK &&
                  back.Access == s.Access && back.Width == 1920 && back.Height == 1080,
              "encode: the D3D11 opener reads the displayable record back");
        s.Access = BC250_SURFACE_RESOURCE_DISPLAYABLE;
        check(Bc250SharedSurfaceEncode(&s, &allocation, &resource) == BC250_SHARED_SURFACE_MALFORMED,
              "encode: DISPLAYABLE alone is refused, because the kernel driver refuses it");
    }

    // BD-408: the X views of the BGRA8 storage row. The HLK FlipQueue writer creates a D3D9 X8R8G8B8 texture with a
    // shared handle; D3D9On12 creates it as B8G8R8X8_UNORM (88), and its shared surface had no record at all. The
    // shape is the lab's: 624x441, pitch 2560, a backing of 1138688 bytes.
    {
        BC250_SHARED_SURFACE s, back;
        BC250_WDDM_ALLOCATION_PRIVATE allocation;
        BC250_SURFACE_RESOURCE_PRIVATE resource;
        std::memset(&s, 0, sizeof(s));
        s.Width = 624;
        s.Height = 441;
        s.Pitch = 2560;
        s.DxgiFormat = 88;                                      // DXGI_FORMAT_B8G8R8X8_UNORM
        s.D3dDdiFormat = 21;                                    // D3DDDIFMT_A8R8G8B8, the storage row's LB7A format
        s.BindFlags = BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET;
        s.BytesPerPixel = 4;
        s.Size = 1138688;
        s.Shared = 1;
        const char* why = 0;
        check(Bc250SharedSurfaceEncode(&s, &allocation, &resource) == BC250_SHARED_SURFACE_OK &&
                  allocation.Format == 21 && resource.Format == 88 && allocation.Pitch == 2560 &&
                  allocation.Size == 1138688,
              "x view: 88 encodes as LB7A A8R8G8B8 under an E26R record of 88");
        check(Bc250SharedSurfaceDecodeWhy(&resource, sizeof(resource), &allocation, sizeof(allocation), &back, &why) ==
                      BC250_SHARED_SURFACE_OK &&
                  back.DxgiFormat == 88 && back.D3dDdiFormat == 21 && back.BytesPerPixel == 4 &&
                  back.Width == 624 && back.Height == 441 && back.Pitch == 2560 && back.Size == 1138688,
              "x view: the strict decode gives back 88, never the A format");
        s.DxgiFormat = 93;                                      // DXGI_FORMAT_B8G8R8X8_UNORM_SRGB
        check(Bc250SharedSurfaceEncode(&s, &allocation, &resource) == BC250_SHARED_SURFACE_OK &&
                  allocation.Format == 21 && resource.Format == 93 &&
                  decode(&resource, sizeof(resource), &allocation, sizeof(allocation), &back) == BC250_SHARED_SURFACE_OK &&
                  back.DxgiFormat == 93 && back.D3dDdiFormat == 21,
              "x view: 93, the sRGB view of the same bytes, by the same rule");
        s.DxgiFormat = 88;
        s.D3dDdiFormat = 22;                                    // D3DDDIFMT_X8R8G8B8: the X8 row, not composed
        check(Bc250SharedSurfaceEncode(&s, &allocation, &resource) == BC250_SHARED_SURFACE_MALFORMED,
              "x view: an LB7A format of X8R8G8B8 is refused; the record names the composed storage row");
        s.D3dDdiFormat = 21;
        check(Bc250SharedSurfaceEncode(&s, &allocation, &resource) == BC250_SHARED_SURFACE_OK, "x view: re-encode");
        allocation.Format = 22;
        check(Bc250SharedSurfaceDecodeWhy(&resource, sizeof(resource), &allocation, sizeof(allocation), &back, &why) ==
                      BC250_SHARED_SURFACE_MALFORMED &&
                  is(why, BC250_SHARED_SURFACE_WHY_ALLOCATION_FORMAT),
              "x view: a received LB7A X8R8G8B8 under an 88 record is \"allocation format\"");
        const unsigned long typeless[2] = {90ul, 92ul};        // B8G8R8A8_TYPELESS, B8G8R8X8_TYPELESS
        for (unsigned i = 0; i < 2; ++i) {
            s.DxgiFormat = typeless[i];
            check(Bc250SharedSurfaceEncode(&s, &allocation, &resource) == BC250_SHARED_SURFACE_FORMAT,
                  "x view: a typeless form has no record");
        }
    }

    // BD-408: the lookups. Only the record lookup knows the X views; the composition lookup, the table and the
    // kernel driver's admission of these bytes are the ones they were before.
    {
        const AMDGPU_WDDM_SURFACE_FORMAT* bgra8 = Bc250SharedSurfaceFormat(87);
        check(bgra8 && bgra8->d3dddi == 21 && Bc250SharedSurfaceFormat(91) == bgra8,
              "lookup: 87 and its sRGB view 91 are the BGRA8 row");
        check(Bc250SharedSurfaceRecordFormat(88) == bgra8 && Bc250SharedSurfaceRecordFormat(93) == bgra8,
              "lookup: the record rows of 88 and 93 are the BGRA8 storage row");
        check(Bc250SharedSurfaceRecordFormat(87) == bgra8 && Bc250SharedSurfaceRecordFormat(28) == Bc250SharedSurfaceFormat(28),
              "lookup: every other format has the row it had");
        check(!Bc250SharedSurfaceRecordFormat(90) && !Bc250SharedSurfaceRecordFormat(92) &&
                  !Bc250SharedSurfaceRecordFormat(0),
              "lookup: typeless and unknown formats have no record row");
        check(!Bc250SharedSurfaceFormat(88) && !Bc250SharedSurfaceFormat(93),
              "lookup: composition, the present copy and scan-out still see no row for 88 and 93");
        check(!amdgpu_wddm_surface_format_by_dxgi(88) && !amdgpu_wddm_surface_format_by_dxgi(93),
              "table: no row has 88 or 93 as its DXGI number");
        const AMDGPU_WDDM_SURFACE_FORMAT* x8 = amdgpu_wddm_surface_format_by_d3dddi(22);
        check(x8 && !x8->dxgi && !(x8->policy & AMDGPU_WDDM_SURFACE_COMPOSED) &&
                  (x8->policy & AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY),
              "table: the X8 row is unchanged: no DXGI number, scan-out only");
        check(WddmSurfaceFormatBpp(21, BC250_SURFACE_COMPOSED) == 4 && WddmSurfaceFormatBpp(21, BC250_SURFACE_SCANOUT) == 4 &&
                  WddmSurfaceFormatBpp(22, BC250_SURFACE_COMPOSED) == 4 && WddmSurfaceFormatBpp(22, BC250_SURFACE_SCANOUT) == 4,
              "kmd: the admission of A8R8G8B8 and X8R8G8B8 at the composed and scan-out stages is unchanged");
    }

    // Every composed row, encoded and decoded: the pitch and size rules are the same at 1, 4 and 8 bytes a pixel.
    {
        unsigned count = 0;
        const AMDGPU_WDDM_SURFACE_FORMAT* rows = amdgpu_wddm_surface_formats(&count);
        unsigned composed = 0, round_trips = 0;
        for (unsigned i = 0; i < count; ++i) {
            const AMDGPU_WDDM_SURFACE_FORMAT* row = amdgpu_wddm_surface_admit(&rows[i], AMDGPU_WDDM_SURFACE_COMPOSED);
            if (!row || !row->dxgi) continue;
            ++composed;
            for (unsigned view = 0; view < 2; ++view) {
                const unsigned long dxgi = view ? row->dxgi_srgb : row->dxgi;
                if (!dxgi) continue;
                BC250_SHARED_SURFACE s;
                std::memset(&s, 0, sizeof(s));
                s.Width = 65;
                s.Height = 17;
                s.Pitch = (65 * row->bytes_per_pixel + 255) & ~255u;
                s.DxgiFormat = dxgi;
                s.D3dDdiFormat = row->d3dddi;
                s.BindFlags = BC250_SHARED_BIND_RENDER_TARGET;
                s.BytesPerPixel = row->bytes_per_pixel;
                s.Size = static_cast<unsigned long long>(s.Pitch) * 20;
                s.Shared = 1;
                BC250_WDDM_ALLOCATION_PRIVATE allocation;
                BC250_SURFACE_RESOURCE_PRIVATE resource;
                BC250_SHARED_SURFACE back;
                if (Bc250SharedSurfaceEncode(&s, &allocation, &resource) != BC250_SHARED_SURFACE_OK) continue;
                if (decode(&resource, sizeof(resource), &allocation, sizeof(allocation), &back) !=
                    BC250_SHARED_SURFACE_OK)
                    continue;
                if (back.DxgiFormat == dxgi && back.D3dDdiFormat == row->d3dddi &&
                    back.BytesPerPixel == row->bytes_per_pixel && back.Pitch == s.Pitch && back.Size == s.Size)
                    ++round_trips;
            }
        }
        std::printf("      %u composed rows, %u format round trips\n", composed, round_trips);
        check(composed >= 5 && round_trips >= composed, "every composed row encodes and decodes, with its sRGB view");
    }

    // The count the records quote, printed by the thing that knows it. Three documents disagreed about it before
    // this line existed (44, 48 and 49), because each one was counted by hand (BD-075 review, 2026-10-06).
    std::printf("%u checks, %u failures\n", checks, failures);
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
