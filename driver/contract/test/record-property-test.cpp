// SPDX-License-Identifier: MIT
// record-property-test.cpp - the rules of the shared linear surface records as properties.
//
// driver/contract/test/record-pair-test.cpp sweeps a fixed matrix: every producer against every consumer over a
// chosen list of sizes, formats and flag sets. A matrix finds what the author of the matrix thought of. This test
// states the same contract as properties and lets RapidCheck choose the cases, so a rule that holds only for the
// sizes somebody listed fails here. RapidCheck also shrinks a failing case, so the report names a small surface
// instead of the first large one it found.
//
// The properties are the claims the contract header makes about itself (driver/contract/bc250_shared_surface.h):
//
//   P1 round trip          a surface that encodes decodes back to the same surface, in all eleven fields.
//   P2 encode implies open "a record this function writes is a record Bc250SharedSurfaceDecode admits".
//   P3 access agreement    the encoder admits exactly the access words the kernel driver's parser admits
//                          (driver/kmd/surface_resource_private.h, Bc250SurfaceResourcePolicy).
//   P4 named refusal       a decode names the check that decided: WHY_OK on success and never on a refusal.
//   P5 output untouched    "Out is written only on success": a refused decode leaves the caller's surface alone.
//   P6 creator and opener  the opener reads back the creator's own pitch and length, and the pixel size of the
//                          format row, for a creator that padded either one.
//
// The seed is fixed by the gate (RC_PARAMS, run_record_properties.ps1), so the quick gate runs the same cases on
// every machine and a failure is reproducible from the log. -Mutation in that runner breaks one rule in a copy of
// the contract header, and each control must fail here.
#include "driver/contract/bc250_shared_surface.h"
#include <rapidcheck.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Every DXGI format a shared linear surface may carry: each COMPOSED row's storage format, and its sRGB view where
// the row has one. Taken from the table rather than written out again, so a new row is covered without a change here.
struct SurfaceFormat {
    unsigned long dxgi;
    const AMDGPU_WDDM_SURFACE_FORMAT *row;
};

const std::vector<SurfaceFormat> &shared_formats() {
    static const std::vector<SurfaceFormat> formats = [] {
        std::vector<SurfaceFormat> out;
        unsigned int count = 0;
        const AMDGPU_WDDM_SURFACE_FORMAT *rows = amdgpu_wddm_surface_formats(&count);
        for (unsigned int i = 0; i < count; ++i) {
            if (rows[i].dxgi) {
                if (const AMDGPU_WDDM_SURFACE_FORMAT *row = Bc250SharedSurfaceFormat(rows[i].dxgi))
                    out.push_back({rows[i].dxgi, row});
            }
            if (rows[i].dxgi_srgb) {
                if (const AMDGPU_WDDM_SURFACE_FORMAT *row = Bc250SharedSurfaceFormat(rows[i].dxgi_srgb))
                    out.push_back({rows[i].dxgi_srgb, row});
            }
        }
        return out;
    }();
    return formats;
}

// The five bind flags and the two misc flags the wire carries, so a generated set is a subset of the mask by
// construction instead of by a discard.
const unsigned long kBindFlags[] = {BC250_SHARED_BIND_SHADER_RESOURCE, BC250_SHARED_BIND_RENDER_TARGET,
                                    BC250_SHARED_BIND_UNORDERED_ACCESS, BC250_SHARED_BIND_DECODER,
                                    BC250_SHARED_BIND_VIDEO_ENCODER};
const unsigned long kMiscFlags[] = {BC250_SHARED_MISC_GENERATE_MIPS, BC250_SHARED_MISC_RESOURCE_CLAMP};

// The access words the kernel driver's parser admits in a v3 texture record. Read from the parser itself, so this
// test states no second copy of the rule: P3 compares the encoder with it, and a change to either side shows up.
bool access_admitted(unsigned long access) {
    BC250_SURFACE_RESOURCE_PRIVATE record;
    int shared = 0, cached = 0;
    std::memset(&record, 0, sizeof record);
    record.Magic = BC250_SURFACE_RESOURCE_MAGIC;
    record.Version = BC250_SURFACE_RESOURCE_TEXTURE_VERSION;
    record.Shared = 1;
    record.Access = access;
    record.Width = 64;
    record.Height = 64;
    record.MipLevels = 1;
    record.ArraySize = 1;
    record.Format = 87;
    record.SampleCount = 1;
    record.Usage = BC250_SHARED_USAGE_DEFAULT;
    record.TextureLayout = BC250_SHARED_LAYOUT_UNDEFINED;
    return Bc250SurfaceResourcePolicy(&record, (unsigned int)sizeof record, &shared, &cached) != 0;
}

unsigned long gen_flag_set(const unsigned long *flags, size_t count) {
    unsigned long set = 0;
    for (size_t i = 0; i < count; ++i)
        if (*rc::gen::arbitrary<bool>()) set |= flags[i];
    return set;
}

// One surface that the contract admits. Every field is built so that it satisfies a rule of the header rather than
// generated and discarded: the pitch is a whole number of pixels and covers the row, and the length covers every
// row. Padding of both is generated, because a real producer pads: the engine rounds the pitch and the kernel
// driver rounds the length to whole pages.
BC250_SHARED_SURFACE gen_surface() {
    const std::vector<SurfaceFormat> &formats = shared_formats();
    const SurfaceFormat &format = formats[*rc::gen::inRange<size_t>(0, formats.size())];
    const unsigned long bpp = (unsigned long)format.row->bytes_per_pixel;

    BC250_SHARED_SURFACE surface;
    std::memset(&surface, 0, sizeof surface);
    surface.Width = *rc::gen::inRange<unsigned long>(1, BC250_SHARED_MAX_EDGE + 1);
    surface.Height = *rc::gen::inRange<unsigned long>(1, BC250_SHARED_MAX_EDGE + 1);
    surface.DxgiFormat = format.dxgi;
    surface.D3dDdiFormat = format.row->d3dddi;
    surface.BytesPerPixel = bpp;
    // The pitch in whole pixels: the row itself plus the padding the producer chose.
    const unsigned long padPixels = *rc::gen::inRange<unsigned long>(0, 257);
    surface.Pitch = (surface.Width + padPixels) * bpp;
    surface.BindFlags = gen_flag_set(kBindFlags, sizeof kBindFlags / sizeof kBindFlags[0]);
    surface.MiscFlags = gen_flag_set(kMiscFlags, sizeof kMiscFlags / sizeof kMiscFlags[0]);
    // The suffixes are needed: rc::gen::element builds a braced vector, in which an int literal narrows.
    surface.Shared = *rc::gen::element<unsigned long>(0ul, 1ul);
    // Only the access words the parser admits: P3 covers the refused ones.
    static const unsigned long kAccess[] = {
        0ul,
        BC250_SURFACE_RESOURCE_PRIMARY,
        BC250_SURFACE_RESOURCE_CPU_READ,
        BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_CPU_READ,
        BC250_SURFACE_RESOURCE_PRIMARY | BC250_SURFACE_RESOURCE_SCANOUT,
        BC250_SURFACE_RESOURCE_SCANOUT | BC250_SURFACE_RESOURCE_DISPLAYABLE};
    surface.Access = kAccess[*rc::gen::inRange<size_t>(0, sizeof kAccess / sizeof kAccess[0])];
    // The length covers every row, plus the page rounding a producer may add.
    const unsigned long long rows = (unsigned long long)surface.Pitch * surface.Height;
    surface.Size = rows + *rc::gen::element<unsigned long long>(0ull, 1ull, 4095ull, 4096ull);
    return surface;
}

bool same_surface(const BC250_SHARED_SURFACE &a, const BC250_SHARED_SURFACE &b) {
    return a.Width == b.Width && a.Height == b.Height && a.Pitch == b.Pitch && a.DxgiFormat == b.DxgiFormat &&
           a.D3dDdiFormat == b.D3dDdiFormat && a.BindFlags == b.BindFlags && a.BytesPerPixel == b.BytesPerPixel &&
           a.Size == b.Size && a.MiscFlags == b.MiscFlags && a.Shared == b.Shared && a.Access == b.Access;
}

}  // namespace

int main() {
    bool ok = true;

    // P1. The round trip. The opener's policy is the creator's own: it requires sharing when the creator shared,
    // and it admits every access bit of the mask, which is the widest opener in the tree.
    ok = rc::check("a surface that encodes decodes back to the same surface", [] {
             const BC250_SHARED_SURFACE in = gen_surface();
             BC250_WDDM_ALLOCATION_PRIVATE allocation;
             BC250_SURFACE_RESOURCE_PRIVATE resource;
             RC_ASSERT(Bc250SharedSurfaceEncode(&in, &allocation, &resource) == BC250_SHARED_SURFACE_OK);
             BC250_SHARED_SURFACE_ADMIT admit{in.Shared, BC250_SURFACE_RESOURCE_ACCESS_MASK};
             BC250_SHARED_SURFACE out;
             std::memset(&out, 0, sizeof out);
             const char *why = "";
             RC_ASSERT(Bc250SharedSurfaceDecodeDetail(&resource, (unsigned int)sizeof resource, &allocation,
                                                      (unsigned int)sizeof allocation, &admit, &out,
                                                      &why) == BC250_SHARED_SURFACE_OK);
             RC_ASSERT(same_surface(in, out));
         }) && ok;

    // P2. The header's own claim, for the strict opener: a shared texture with no access bit, which is what the
    // D3D12 shell writes and opens. BD-075 round 1 was exactly this claim being false across two shells.
    ok = rc::check("the strict open admits every record the encoder writes", [] {
             BC250_SHARED_SURFACE in = gen_surface();
             in.Shared = 1;
             in.Access = 0;
             BC250_WDDM_ALLOCATION_PRIVATE allocation;
             BC250_SURFACE_RESOURCE_PRIVATE resource;
             RC_ASSERT(Bc250SharedSurfaceEncode(&in, &allocation, &resource) == BC250_SHARED_SURFACE_OK);
             BC250_SHARED_SURFACE out;
             const char *why = "";
             RC_ASSERT(Bc250SharedSurfaceDecodeWhy(&resource, (unsigned int)sizeof resource, &allocation,
                                                   (unsigned int)sizeof allocation, &out,
                                                   &why) == BC250_SHARED_SURFACE_OK);
             RC_ASSERT(std::strcmp(why, BC250_SHARED_SURFACE_WHY_OK) == 0);
         }) && ok;

    // P3. The encoder and the kernel driver's parser agree on the access word, over every value of the five access
    // bits and the two bits next to them. A record the encoder wrote that the kernel refuses would describe memory
    // nobody placed; a record the encoder refused that the kernel admits is a shell that cannot share what the
    // hardware allows.
    ok = rc::check("the encoder admits exactly the access words the kernel parser admits", [] {
             BC250_SHARED_SURFACE in = gen_surface();
             in.Shared = 1;
             in.Access = *rc::gen::inRange<unsigned long>(0, 0x40);
             BC250_WDDM_ALLOCATION_PRIVATE allocation;
             BC250_SURFACE_RESOURCE_PRIVATE resource;
             const int status = Bc250SharedSurfaceEncode(&in, &allocation, &resource);
             RC_ASSERT((status == BC250_SHARED_SURFACE_OK) == access_admitted(in.Access));
         }) && ok;

    // P4. The refusal names the check. One status covers seventeen checks, and a line that says only "record"
    // cost a lab pass once (BD-075 round 1, 2026-10-06). The lengths are generated, so a short or long blob of
    // either record is among the cases, as are a correct pair and a pair with one byte of the magic changed.
    ok = rc::check("a decode names the check that decided, and names OK only on success", [] {
             const BC250_SHARED_SURFACE in = gen_surface();
             BC250_WDDM_ALLOCATION_PRIVATE allocation;
             BC250_SURFACE_RESOURCE_PRIVATE resource;
             RC_ASSERT(Bc250SharedSurfaceEncode(&in, &allocation, &resource) == BC250_SHARED_SURFACE_OK);
             // Damage the pair in one of the ways a wrong producer does, or leave it correct.
             const unsigned kind = *rc::gen::inRange<unsigned>(0, 6);
             unsigned int resourceBytes = (unsigned int)sizeof resource;
             unsigned int allocationBytes = (unsigned int)sizeof allocation;
             switch (kind) {
                 case 1: resourceBytes = *rc::gen::inRange<unsigned int>(0, (unsigned int)sizeof resource); break;
                 case 2: allocationBytes = *rc::gen::inRange<unsigned int>(0, (unsigned int)sizeof allocation); break;
                 case 3: resource.Magic ^= 1ul; break;
                 case 4: allocation.Magic ^= 1ul; break;
                 case 5: resource.MipLevels = *rc::gen::inRange<unsigned long>(2, 16); break;
                 default: break;
             }
             BC250_SHARED_SURFACE_ADMIT admit{in.Shared, BC250_SURFACE_RESOURCE_ACCESS_MASK};
             BC250_SHARED_SURFACE out;
             const char *why = "";
             const int status = Bc250SharedSurfaceDecodeDetail(&resource, resourceBytes, &allocation,
                                                               allocationBytes, &admit, &out, &why);
             RC_ASSERT(why != nullptr && *why != '\0');
             RC_ASSERT((status == BC250_SHARED_SURFACE_OK) ==
                       (std::strcmp(why, BC250_SHARED_SURFACE_WHY_OK) == 0));
         }) && ok;

    // P5. "Out is written only on success." An opener keeps the surface of an earlier open in that variable, and a
    // decode that overwrote part of it on the way to a refusal would leave the opener holding a mixture.
    ok = rc::check("a refused decode leaves the caller's surface untouched", [] {
             const BC250_SHARED_SURFACE in = gen_surface();
             BC250_WDDM_ALLOCATION_PRIVATE allocation;
             BC250_SURFACE_RESOURCE_PRIVATE resource;
             RC_ASSERT(Bc250SharedSurfaceEncode(&in, &allocation, &resource) == BC250_SHARED_SURFACE_OK);
             // A record the strict opener must refuse: it shares nothing.
             resource.Shared = 0;
             const BC250_SHARED_SURFACE before = gen_surface();
             BC250_SHARED_SURFACE out = before;
             const char *why = "";
             RC_ASSERT(Bc250SharedSurfaceDecodeWhy(&resource, (unsigned int)sizeof resource, &allocation,
                                                   (unsigned int)sizeof allocation, &out,
                                                   &why) != BC250_SHARED_SURFACE_OK);
             RC_ASSERT(std::strcmp(why, BC250_SHARED_SURFACE_WHY_SHARED) == 0);
             RC_ASSERT(same_surface(before, out));
         }) && ok;

    // P6. Creator and opener. The pair the creator wrote carries the creator's own padded pitch and length, not a
    // recomputed one, and the pixel size comes from the format row. BD-408 and BD-413 are both this agreement
    // failing: the opener decided the geometry differently from the creator.
    ok = rc::check("the opener reads back the creator's pitch, length and pixel size", [] {
             const BC250_SHARED_SURFACE in = gen_surface();
             BC250_WDDM_ALLOCATION_PRIVATE allocation;
             BC250_SURFACE_RESOURCE_PRIVATE resource;
             RC_ASSERT(Bc250SharedSurfaceEncode(&in, &allocation, &resource) == BC250_SHARED_SURFACE_OK);
             // What the two blobs carry is what the creator asked for.
             RC_ASSERT(allocation.Pitch == in.Pitch);
             RC_ASSERT(allocation.Size == in.Size);
             RC_ASSERT(allocation.Format == in.D3dDdiFormat);
             RC_ASSERT(resource.Format == in.DxgiFormat);
             RC_ASSERT(resource.BindFlags == in.BindFlags);
             BC250_SHARED_SURFACE_ADMIT admit{in.Shared, BC250_SURFACE_RESOURCE_ACCESS_MASK};
             BC250_SHARED_SURFACE out;
             const char *why = "";
             RC_ASSERT(Bc250SharedSurfaceDecodeDetail(&resource, (unsigned int)sizeof resource, &allocation,
                                                      (unsigned int)sizeof allocation, &admit, &out,
                                                      &why) == BC250_SHARED_SURFACE_OK);
             RC_ASSERT(out.Pitch == in.Pitch);
             RC_ASSERT(out.Size == in.Size);
             const AMDGPU_WDDM_SURFACE_FORMAT *row = Bc250SharedSurfaceFormat(in.DxgiFormat);
             RC_ASSERT(row != nullptr);
             RC_ASSERT(out.BytesPerPixel == (unsigned long)row->bytes_per_pixel);
             // The row holds the image: the opener may map fewer bytes than the creator reserved, never more.
             RC_ASSERT((unsigned long long)out.Pitch * out.Height <= out.Size);
         }) && ok;

    std::printf("record-property: %zu shared formats, %s\n", shared_formats().size(), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
