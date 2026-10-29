// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: the selected DCN201 HUBP1 functions remain AMD MIT in
// ../shim/generated/dc_surface_layout.inc, Linux 7d0a66e4bb90.
#include "dcn_surface_layout.h"
#include "dcn_translate.h"
#include "display_io.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../shim/bc250_dc_types.h"

enum { DCSURF_SURFACE_PITCH, DCSURF_SURFACE_PITCH_C, DCSURF_SURFACE_CONFIG, HUBPRET_CONTROL };
struct hubp { unsigned unused; };
struct dcn10_hubp { struct hubp base; unsigned long word[4]; unsigned invalid; };
#define TO_DCN10_HUBP(p) ((struct dcn10_hubp*)(p))
#define MASK_PITCH HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK
#define MASK_META_PITCH HUBPREQ0_DCSURF_SURFACE_PITCH__META_PITCH_MASK
#define MASK_PITCH_C HUBPREQ0_DCSURF_SURFACE_PITCH_C__PITCH_C_MASK
#define MASK_META_PITCH_C HUBPREQ0_DCSURF_SURFACE_PITCH_C__META_PITCH_C_MASK
#define MASK_SURFACE_PIXEL_FORMAT HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK
// The shared function also serves later RGBE formats. DCN201 has no such
// field; reject that operation rather than inventing a mask. None of this
// adapter's formats reaches it, including AMD's packed AYCrCb with pixel alpha.
#define MASK_ALPHA_PLANE_EN 0ul
#define MASK_CROSSBAR_SRC_CB_B HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK
#define MASK_CROSSBAR_SRC_CR_R HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK
#define REG_UPDATE(reg,field,v) do { \
    if (!MASK_##field) hubp1->invalid = 1; \
    else hubp1->word[reg] = (hubp1->word[reg] & ~MASK_##field) | Bc250DispField(MASK_##field,(unsigned long)(v)); \
} while (0)
#define REG_UPDATE_2(reg,f1,v1,f2,v2) do { REG_UPDATE(reg,f1,v1); REG_UPDATE(reg,f2,v2); } while (0)
#define ASSERT(x) do { if (!(x)) hubp1->invalid = 1; } while (0)
#define BREAK_TO_DEBUGGER() (hubp1->invalid = 1)
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(disable:4127) // constant masks in the capture macros
#endif
#include "../shim/generated/dc_surface_layout.inc"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif
#undef REG_UPDATE
#undef REG_UPDATE_2
#undef ASSERT
#undef BREAK_TO_DEBUGGER

// B contains trusted allocation/segment bounds. All extent tests precede
// address addition, including MC and UMA half-open segment endpoints.
static int SurfaceRange(const BC250_SURFACE_LAYOUT_REQUEST* B,
                        unsigned long long SurfaceOffset, unsigned long long Bytes,
                        unsigned long long* Physical)
{
    unsigned long long offset;
    const unsigned long long addressLimit =
        ((unsigned long long)HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK + 1ull) << 32;
    *Physical = 0;
    if (!B->VramLength || B->VramPhysical >= addressLimit ||
        B->VramLength > addressLimit - B->VramPhysical ||
        B->VramLength > UINT64_MAX - B->McBase || B->AllocationMcAddress < B->McBase) return 0;
    offset = B->AllocationMcAddress - B->McBase;
    if (offset >= B->VramLength || !B->AllocationBytes || B->AllocationBytes > B->VramLength - offset ||
        SurfaceOffset > B->AllocationBytes || !Bytes || Bytes > B->AllocationBytes - SurfaceOffset) return 0;
    if (!DcnTranslateCardAddress(B->AllocationMcAddress + SurfaceOffset,
            B->McBase,B->VramPhysical,B->VramLength,Physical) || !*Physical ||
        !DcnAddressFits(*Physical,B->VramPhysical,B->VramLength,Bytes)) return 0;
    return 1;
}

int Bc250SurfaceLayoutPrepare(const BC250_SURFACE_LAYOUT_REQUEST* Request,
                             BC250_SURFACE_LAYOUT* Plan)
{
    BC250_SURFACE_LAYOUT_REQUEST in = {0};
    struct dcn10_hubp capture = {0};
    struct plane_size size = {0};
    struct dc_plane_dcc_param dcc = {0};
    static const enum surface_pixel_format formats[BC250_LINEAR_FORMAT_COUNT] = {
        SURFACE_PIXEL_FORMAT_GRPH_ARGB8888, SURFACE_PIXEL_FORMAT_GRPH_ABGR8888,
        SURFACE_PIXEL_FORMAT_GRPH_ABGR2101010, SURFACE_PIXEL_FORMAT_VIDEO_AYCrCb8888,
        SURFACE_PIXEL_FORMAT_GRPH_ARGB8888
    };
    unsigned long long bytes, physical;
    unsigned pixels;
    if (!Plan) return 0;
    if (Request) in = *Request;
    *Plan = (BC250_SURFACE_LAYOUT){0};
    if (!Request) return 0;
    if (in.Format >= BC250_LINEAR_FORMAT_COUNT || in.Rotation || in.MirrorH || in.MirrorV ||
        !in.Width || in.Width > 16383 || !in.Height || in.Height > 16383 ||
        !in.PitchBytes || (in.PitchBytes % (32u * 4u))) return 0;
    pixels = in.PitchBytes / 4;
    if (!pixels || (pixels - 1) > Bc250DispGet(MASK_PITCH, MASK_PITCH)) return 0;
    if (!DcnLinearSurfaceBytes(in.Width,in.Height,in.PitchBytes,4,&bytes)) return 0;

    if (!SurfaceRange(&in,in.SurfaceOffset,bytes,&physical)) return 0;

    size.surface_pitch = (int)pixels;
    // DCC disabled: set neutral inputs before the imported subtraction.
    dcc.meta_pitch = dcc.meta_pitch_c = 1;
    hubp1_program_size(&capture.base,formats[in.Format],&size,&dcc);
    hubp1_program_pixel_format(&capture.base,formats[in.Format]);
    if (capture.invalid) return 0;
    Plan->Pitch = capture.word[DCSURF_SURFACE_PITCH];
    Plan->PitchC = capture.word[DCSURF_SURFACE_PITCH_C];
    Plan->SurfaceConfig = capture.word[DCSURF_SURFACE_CONFIG];
    Plan->Crossbar = capture.word[HUBPRET_CONTROL];
    Plan->Physical = physical; Plan->SurfaceBytes = bytes;
    Plan->AddressHigh = (unsigned long)(physical >> 32);
    Plan->AddressLow = (unsigned long)physical;
    Plan->Format = in.Format; Plan->Width = in.Width; Plan->Height = in.Height;
    Plan->PitchBytes = in.PitchBytes;
    Plan->HasPixelAlpha = in.Format != BC250_LINEAR_BGRX8;
    Plan->Yuv = in.Format == BC250_LINEAR_AMD_AYCRCB8; Plan->Ready = 1;
    return 1;
}

int Bc250Nv12LayoutPrepare(const BC250_NV12_LAYOUT_REQUEST* Request,
                          BC250_NV12_LAYOUT* Plan)
{
    BC250_NV12_LAYOUT_REQUEST in = {0};
    BC250_SURFACE_LAYOUT_REQUEST bounds = {0};
    struct dcn10_hubp capture = {0};
    struct plane_size size = {0};
    struct dc_plane_dcc_param dcc = {0};
    unsigned long long bytes, chromaBytes, physical, chromaPhysical;
    unsigned chromaSamples;
    _Static_assert(HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK ==
        HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH_C__PRIMARY_SURFACE_ADDRESS_HIGH_C_MASK,
        "NV12 address widths must agree");
    if (!Plan) return 0;
    if (Request) in = *Request;
    *Plan = (BC250_NV12_LAYOUT){0};
    if (!Request || !in.Width || in.Width > 16383 || !in.Height || in.Height > 16383 ||
        (in.Width & 1) || (in.Height & 1) || !in.PitchBytes || !in.ChromaPitchBytes ||
        (in.PitchBytes & 255) || (in.ChromaPitchBytes & 255)) return 0;
    chromaSamples = in.ChromaPitchBytes / 2;
    if (in.PitchBytes - 1 > Bc250DispGet(MASK_PITCH,MASK_PITCH) ||
        chromaSamples - 1 > Bc250DispGet(MASK_PITCH_C,MASK_PITCH_C)) return 0;
    // NV12 has Width/2 two-byte UV samples per row: its unpadded byte width
    // equals luma's Width. Use the byte-row helper, not its unsupported bpp=2.
    if (!DcnLinearSurfaceBytes(in.Width,in.Height,in.PitchBytes,1,&bytes) ||
        !DcnLinearSurfaceBytes(in.Width,in.Height/2,in.ChromaPitchBytes,1,&chromaBytes)) return 0;
    bounds.AllocationMcAddress = in.AllocationMcAddress; bounds.AllocationBytes = in.AllocationBytes;
    bounds.McBase = in.McBase; bounds.VramPhysical = in.VramPhysical; bounds.VramLength = in.VramLength;
    if (!SurfaceRange(&bounds,in.SurfaceOffset,bytes,&physical) ||
        !SurfaceRange(&bounds,in.ChromaOffset,chromaBytes,&chromaPhysical)) return 0;
    // Range checks made these additions safe. Keep all row padding disjoint.
    if (in.SurfaceOffset < in.ChromaOffset + chromaBytes && in.ChromaOffset < in.SurfaceOffset + bytes) return 0;
    size.surface_pitch = (int)in.PitchBytes;
    size.chroma_pitch = (int)chromaSamples;
    dcc.meta_pitch = dcc.meta_pitch_c = 1;
    // AMD amdgpu_dm.c maps DRM_FORMAT_NV12 to VIDEO_420_YCrCb, not YCbCr
    // (the latter is NV21). The names alone would reverse U/V.
    hubp1_program_size(&capture.base,SURFACE_PIXEL_FORMAT_VIDEO_420_YCrCb,&size,&dcc);
    hubp1_program_pixel_format(&capture.base,SURFACE_PIXEL_FORMAT_VIDEO_420_YCrCb);
    if (capture.invalid) return 0;
    Plan->Pitch = capture.word[DCSURF_SURFACE_PITCH];
    Plan->PitchC = capture.word[DCSURF_SURFACE_PITCH_C];
    Plan->SurfaceConfig = capture.word[DCSURF_SURFACE_CONFIG];
    Plan->Crossbar = capture.word[HUBPRET_CONTROL];
    Plan->Physical = physical; Plan->ChromaPhysical = chromaPhysical;
    Plan->SurfaceBytes = bytes; Plan->ChromaBytes = chromaBytes;
    Plan->AddressHigh = (unsigned long)(physical >> 32); Plan->AddressLow = (unsigned long)physical;
    Plan->ChromaAddressHigh = (unsigned long)(chromaPhysical >> 32); Plan->ChromaAddressLow = (unsigned long)chromaPhysical;
    Plan->Width = in.Width; Plan->Height = in.Height;
    Plan->PitchBytes = in.PitchBytes; Plan->ChromaPitchBytes = in.ChromaPitchBytes; Plan->Ready = 1;
    return 1;
}
