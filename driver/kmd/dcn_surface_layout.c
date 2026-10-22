// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: the selected DCN201 HUBP1 functions remain AMD MIT in
// ../shim/generated/dc_surface_layout.inc, Linux 7d0a66e4bb90.
#include "dcn_surface_layout.h"
#include "dcn_translate.h"
#include "display_io.h"
#include "dcn_2_0_1_sh_mask.h"
#include <stdint.h>
#include <stdbool.h>

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
#include "../shim/generated/dc_surface_layout.inc"
#undef REG_UPDATE
#undef REG_UPDATE_2
#undef ASSERT
#undef BREAK_TO_DEBUGGER

int Bc250SurfaceLayoutPrepare(const BC250_SURFACE_LAYOUT_REQUEST* Request,
                             BC250_SURFACE_LAYOUT* Plan)
{
    BC250_SURFACE_LAYOUT_REQUEST in;
    struct dcn10_hubp capture = {0};
    struct plane_size size = {0};
    struct dc_plane_dcc_param dcc = {0};
    static const enum surface_pixel_format formats[BC250_LINEAR_FORMAT_COUNT] = {
        SURFACE_PIXEL_FORMAT_GRPH_ARGB8888, SURFACE_PIXEL_FORMAT_GRPH_ABGR8888,
        SURFACE_PIXEL_FORMAT_GRPH_ABGR2101010, SURFACE_PIXEL_FORMAT_VIDEO_AYCrCb8888,
        SURFACE_PIXEL_FORMAT_GRPH_ARGB8888
    };
    unsigned long long offset, bytes, physical, addressLimit;
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

    // Both half-open segment intervals must be representable. Derive the
    // physical address width from the named AMD field, not a truncated cast.
    addressLimit = ((unsigned long long)HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH__PRIMARY_SURFACE_ADDRESS_HIGH_MASK + 1ull) << 32;
    if (!in.VramLength || in.VramPhysical >= addressLimit ||
        in.VramLength > addressLimit - in.VramPhysical ||
        in.VramLength > UINT64_MAX - in.McBase || in.AllocationMcAddress < in.McBase) return 0;
    offset = in.AllocationMcAddress - in.McBase;
    if (offset >= in.VramLength || !in.AllocationBytes || in.AllocationBytes > in.VramLength - offset ||
        in.SurfaceOffset > in.AllocationBytes || bytes > in.AllocationBytes - in.SurfaceOffset) return 0;
    if (!DcnTranslateCardAddress(in.AllocationMcAddress + in.SurfaceOffset,
            in.McBase,in.VramPhysical,in.VramLength,&physical) ||
        !physical || !DcnAddressFits(physical,in.VramPhysical,in.VramLength,bytes)) return 0;

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
