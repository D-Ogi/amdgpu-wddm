// Project adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: Linux amdgpu DC, MIT, 7d0a66e4bb90; original setup and tables
// remain in ../shim/generated/dc_cnvc_color.inc.
#include "dcn_cnvc_color.h"
#include "display_io.h"
#include "dcn_2_0_1_sh_mask.h"
#include "../shim/bc250_dc_types.h"
#include <stddef.h>

#define __maybe_unused
#include "../shim/generated/dc_cnvc_color.inc"
#undef __maybe_unused

enum { FORMAT_CONTROL, ALPHA_2BIT_LUT, CNVC_SURFACE_PIXEL_FORMAT,
       CURSOR_CONTROL, CURSOR0_CONTROL };
struct dpp { unsigned unused; };
struct dcn201_dpp {
    struct dpp base;
    unsigned long word[5];
    unsigned short matrix[12];
    unsigned invalid, cscCalls, cscRequired, powerCalls, lutWritten;
};
#define TO_DCN201_DPP(p) ((struct dcn201_dpp*)(p))
#define MASK_CNVC_BYPASS CNVC_CFG0_FORMAT_CONTROL__CNVC_BYPASS_MASK
#define MASK_FORMAT_EXPANSION_MODE CNVC_CFG0_FORMAT_CONTROL__FORMAT_EXPANSION_MODE_MASK
#define MASK_FORMAT_CNV16 CNVC_CFG0_FORMAT_CONTROL__FORMAT_CNV16_MASK
#define MASK_CNVC_BYPASS_MSB_ALIGN CNVC_CFG0_FORMAT_CONTROL__CNVC_BYPASS_MSB_ALIGN_MASK
#define MASK_CLAMP_POSITIVE CNVC_CFG0_FORMAT_CONTROL__CLAMP_POSITIVE_MASK
#define MASK_CLAMP_POSITIVE_C CNVC_CFG0_FORMAT_CONTROL__CLAMP_POSITIVE_C_MASK
#define MASK_FORMAT_CONTROL__ALPHA_EN CNVC_CFG0_FORMAT_CONTROL__ALPHA_EN_MASK
#define MASK_CNVC_SURFACE_PIXEL_FORMAT CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT__CNVC_SURFACE_PIXEL_FORMAT_MASK
#define MASK_ALPHA_2BIT_LUT0 CNVC_CFG0_ALPHA_2BIT_LUT__ALPHA_2BIT_LUT0_MASK
#define MASK_ALPHA_2BIT_LUT1 CNVC_CFG0_ALPHA_2BIT_LUT__ALPHA_2BIT_LUT1_MASK
#define MASK_ALPHA_2BIT_LUT2 CNVC_CFG0_ALPHA_2BIT_LUT__ALPHA_2BIT_LUT2_MASK
#define MASK_ALPHA_2BIT_LUT3 CNVC_CFG0_ALPHA_2BIT_LUT__ALPHA_2BIT_LUT3_MASK
// These cursor updates belong to planar formats outside this adapter. Refuse
// any such side effect rather than claiming cursor ownership in a CPU plan.
#define MASK_CURSOR_ENABLE 0ul
#define MASK_CUR0_ENABLE 0ul
#define REG_UPDATE(reg,field,v) do { \
    if (!MASK_##field) dpp->invalid = 1; \
    else dpp->word[reg] = (dpp->word[reg] & ~MASK_##field) | Bc250DispField(MASK_##field,(unsigned long)(v)); \
    if ((reg) == ALPHA_2BIT_LUT) dpp->lutWritten = 1; \
} while (0)
#define REG_SET(reg,initial,f,v) do { dpp->word[reg] = (initial); REG_UPDATE(reg,f,v); } while (0)
#define REG_SET_2(reg,initial,f1,v1,f2,v2) do { REG_SET(reg,initial,f1,v1); REG_UPDATE(reg,f2,v2); } while (0)

// Preparation boundary in place of hardware dpp1_program_input_csc: select
// the same table by explicit color space, but defer active-bank observation,
// alternate-bank writes and selection to the transaction owner. No debug read
// or claimed bank exists here. The imported setup passes NULL for custom CSC.
static void dpp1_program_input_csc(struct dpp* base, enum dc_color_space color,
    enum dcn10_input_csc_select select, const void* custom)
{
    struct dcn201_dpp* capture = TO_DCN201_DPP(base);
    size_t i;
    ++capture->cscCalls;
    if (custom || (select != INPUT_CSC_SELECT_BYPASS && select != INPUT_CSC_SELECT_ICSC)) {
        capture->invalid = 1; return;
    }
    if (select == INPUT_CSC_SELECT_BYPASS) return;
    capture->cscRequired = 1;
    for (i = 0; i < sizeof(dpp_input_csc_matrix)/sizeof(dpp_input_csc_matrix[0]); ++i)
        if (dpp_input_csc_matrix[i].color_space == color) {
            unsigned j;
            for (j = 0; j < 12; ++j) capture->matrix[j] = dpp_input_csc_matrix[i].regval[j];
            return;
        }
    capture->invalid = 1;
}
static void dpp2_power_on_obuf(struct dpp* base, bool power)
{
    struct dcn201_dpp* capture = TO_DCN201_DPP(base);
    ++capture->powerCalls;
    if (!power) capture->invalid = 1;
}

// AMD's DCN201 function accepts but does not use the custom matrix argument.
// Keep the original function unchanged; our public request does not expose it.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4100 4127) // original callback parameters and constant capture masks
#endif
#define BC250_CNVC_FUNCTION
#include "../shim/generated/dc_cnvc_color.inc"
#undef BC250_CNVC_FUNCTION
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#undef REG_SET_2
#undef REG_SET
#undef REG_UPDATE

int Bc250CnvcPrepare(const BC250_CNVC_REQUEST* Request, BC250_CNVC_PLAN* Plan)
{
    BC250_CNVC_REQUEST in = {0};
    struct dcn201_dpp capture = {0};
    struct dc_csc_transform unused = {0};
    struct cnv_alpha_2bit_lut lut = {0,85,170,255};
    static const enum surface_pixel_format formats[BC250_LINEAR_FORMAT_COUNT] = {
        SURFACE_PIXEL_FORMAT_GRPH_ARGB8888, SURFACE_PIXEL_FORMAT_GRPH_ABGR8888,
        SURFACE_PIXEL_FORMAT_GRPH_ABGR2101010, SURFACE_PIXEL_FORMAT_VIDEO_AYCrCb8888,
        SURFACE_PIXEL_FORMAT_GRPH_ARGB8888
    };
    static const enum dc_color_space colors[BC250_INPUT_COLOR_COUNT] = {
        COLOR_SPACE_SRGB, COLOR_SPACE_YCBCR601, COLOR_SPACE_YCBCR601_LIMITED,
        COLOR_SPACE_YCBCR709, COLOR_SPACE_YCBCR709_LIMITED
    };
    unsigned j, yuv;
    if (!Plan) return 0;
    if (Request) in = *Request;
    *Plan = (BC250_CNVC_PLAN){0};
    if (!Request || in.Format >= BC250_LINEAR_FORMAT_COUNT || in.Color >= BC250_INPUT_COLOR_COUNT) return 0;
    yuv = in.Format == BC250_LINEAR_AMD_AYCRCB8;
    if (yuv != (unsigned)(in.Color != BC250_INPUT_RGB_FULL)) return 0;
    dpp201_cnv_setup(&capture.base,formats[in.Format],EXPANSION_MODE_ZERO,unused,colors[in.Color],&lut);
    if (capture.invalid || capture.cscCalls != 1 || capture.powerCalls != 1 || (unsigned)capture.cscRequired != yuv) return 0;
    // AMD maps ARGB and XRGB to the same surface encoding. Our XRGB owner
    // explicitly disables pixel alpha; it must also choose matching MPC mode.
    if (in.Format == BC250_LINEAR_BGRX8) capture.word[FORMAT_CONTROL] &= ~MASK_FORMAT_CONTROL__ALPHA_EN;
    Plan->FormatControl = capture.word[FORMAT_CONTROL];
    Plan->PixelFormat = capture.word[CNVC_SURFACE_PIXEL_FORMAT];
    Plan->AlphaLut = capture.word[ALPHA_2BIT_LUT];
    for (j = 0; j < 12; ++j) Plan->Matrix[j] = capture.matrix[j];
    Plan->Format = in.Format; Plan->Color = in.Color;
    Plan->CscRequired = capture.cscRequired;
    Plan->HasPixelAlpha = in.Format != BC250_LINEAR_BGRX8;
    Plan->WriteAlphaLut = capture.lutWritten; Plan->Ready = 1;
    return 1;
}
