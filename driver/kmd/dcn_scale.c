// Pipe 0's scaler and viewport for a smaller desktop source mode: see dcn_scale.h for the design and the Linux
// functions this follows (MIT, v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449). Every register name comes from
// gen_regs.py (regs.generated.h) and every field from dcn_2_0_1_sh_mask.h.
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dcn_scale.h"
#include "edid.h"

// filter_4tap_64p_upscale of dce_scl_filters.c, extracted by tools/import/extract_table.py (PROVENANCE.md).
const unsigned short g_Bc250SclFilter4tapUpscale[BC250_SCL_PHASES * BC250_SCL_TAPS] = {
#include "scl_filter_4tap_64p_upscale.inc"
};

#define ONE19 (1ul << 19)
#define DSCL_MODE_444_BYPASS 0ul            // dcn10_dpp.c enum dscl_mode_sel
#define DSCL_MODE_444_RGB_ENABLE 1ul
#define LB_MEMORY_CONFIG_0 0ul              // dc/inc/hw/transform.h enum lb_memory_config
#define LB_MEMORY_CONFIG_1 1ul
#define LB_MEMORY_CONFIG_2 2ul
#define LB_MAX_PARTITIONS 63ul              // dpp1_dscl_set_lb: "hardcoded on all ASICs before DCN 3.2"
#define COEF_LUMA_VERT 0ul                  // dcn10_dpp_dscl.c enum dcn10_coef_filter_type_sel
#define COEF_LUMA_HORZ 1ul
#define LOCK_WAIT_STEPS 10ul                // dcn.c DcnFlipWriteSequence: at most ten 1 us stalls

#define TRY(x) do { long s_ = (x); if (s_ < 0) return s_; } while (0)

const char* Bc250ScalingName(unsigned long Scaling)
{
    switch (Scaling) {
    case BC250_SCALING_IDENTITY: return "identity";
    case BC250_SCALING_CENTERED: return "centered";
    case BC250_SCALING_STRETCHED: return "stretched";
    case BC250_SCALING_ASPECT: return "aspect-ratio";
    default: return "?";
    }
}

static int DimensionOk(unsigned long V)
{
    return V >= BC250_SCALE_MIN_DIMENSION && V <= BC250_SCALE_MAX_DIMENSION;
}

long Bc250ScalingRecout(unsigned long SourceWidth, unsigned long SourceHeight, unsigned long TargetWidth,
                        unsigned long TargetHeight, unsigned long Scaling, BC250_RECT* Recout)
{
    unsigned long long sw = SourceWidth, sh = SourceHeight, tw = TargetWidth, th = TargetHeight;

    Recout->X = Recout->Y = Recout->Width = Recout->Height = 0;
    if (!DimensionOk(SourceWidth) || !DimensionOk(SourceHeight) || !DimensionOk(TargetWidth) ||
        !DimensionOk(TargetHeight) || Scaling >= BC250_SCALING_COUNT) return BC250_DISP_STATUS_INVALID;
    if (sw > tw || sh > th) return BC250_DISP_STATUS_NOT_SUPPORTED;     // no downscale in stage A
    switch (Scaling) {
    case BC250_SCALING_IDENTITY:
        if (sw != tw || sh != th) return BC250_DISP_STATUS_NOT_SUPPORTED;
        Recout->Width = TargetWidth;
        Recout->Height = TargetHeight;
        break;
    case BC250_SCALING_CENTERED:
        Recout->X = (TargetWidth - SourceWidth) / 2;
        Recout->Y = (TargetHeight - SourceHeight) / 2;
        Recout->Width = SourceWidth;
        Recout->Height = SourceHeight;
        break;
    case BC250_SCALING_STRETCHED:
        Recout->Width = TargetWidth;
        Recout->Height = TargetHeight;
        break;
    default:                                                            // BC250_SCALING_ASPECT
        if (sw * th >= sh * tw) {           // the source is as wide as the target or wider: full width, bars above
            Recout->Width = TargetWidth;
            Recout->Height = (unsigned long)(sh * tw / sw);
            Recout->Y = (TargetHeight - Recout->Height) / 2;
        } else {                            // narrower: full height, bars left and right
            Recout->Height = TargetHeight;
            Recout->Width = (unsigned long)(sw * th / sh);
            Recout->X = (TargetWidth - Recout->Width) / 2;
        }
        break;
    }
    return BC250_DISP_STATUS_SUCCESS;
}

// dscl2_calc_lb_num_partitions for RGB (viewport_c == viewport), alpha off.
static unsigned long LbPartitions(unsigned long LineSize, unsigned long Config)
{
    unsigned long memoryLine = (LineSize + 5ul) / 6ul, size, parts;
    if (memoryLine == 0) memoryLine = 1;
    size = Config == LB_MEMORY_CONFIG_1 ? 970ul : Config == LB_MEMORY_CONFIG_2 ? 1290ul : 970ul + 1290ul + 484ul;
    parts = size / memoryLine;
    return parts > 64ul ? 64ul : parts;
}

// dpp1_dscl_find_lb_memory_config: the first of config 1 and 2 that holds the vertical taps, else config 0. Every
// ratio here is 1 or below, so dpp1_dscl_is_lb_conf_valid reduces to taps <= partitions.
static unsigned long LbConfig(unsigned long LineSize, unsigned long VTaps, unsigned long VTapsC)
{
    unsigned long p = LbPartitions(LineSize, LB_MEMORY_CONFIG_1);
    if (VTaps <= p && VTapsC <= p) return LB_MEMORY_CONFIG_1;
    p = LbPartitions(LineSize, LB_MEMORY_CONFIG_2);
    if (VTaps <= p && VTapsC <= p) return LB_MEMORY_CONFIG_2;
    return LB_MEMORY_CONFIG_0;
}

// calculate_init_and_vp for a recout offset of 0 within the full recout: the viewport starts at 0, and its size is
// floor(init + ratio * (recout - 1)) capped at the source size.
static unsigned long ViewportSize(unsigned long Init19, unsigned long Ratio19, unsigned long Recout,
                                  unsigned long Source)
{
    unsigned long long v = ((unsigned long long)Init19 + (unsigned long long)Ratio19 * (Recout - 1ul)) >> 19;
    return v > Source ? Source : (unsigned long)v;
}

long Bc250ScalerPlan(unsigned long SourceWidth, unsigned long SourceHeight, unsigned long TargetWidth,
                     unsigned long TargetHeight, unsigned long Scaling, BC250_SCALER_PLAN* Plan)
{
    unsigned char* clear = (unsigned char*)Plan;
    unsigned long line, i;
    long s;

    for (i = 0; i < sizeof(*Plan); i++) clear[i] = 0;      // a refused plan programs nothing (Bc250ScalerProgram)
    Plan->SourceWidth = SourceWidth;
    Plan->SourceHeight = SourceHeight;
    Plan->TargetWidth = TargetWidth;
    Plan->TargetHeight = TargetHeight;
    Plan->Scaling = Scaling;
    s = Bc250ScalingRecout(SourceWidth, SourceHeight, TargetWidth, TargetHeight, Scaling, &Plan->Recout);
    if (s < 0) return s;
    // calculate_scaling_ratios: source / destination, truncated to 19 fraction bits. Chroma = luma for RGB.
    Plan->RatioH19 = (unsigned long)(((unsigned long long)SourceWidth << 19) / Plan->Recout.Width);
    Plan->RatioV19 = (unsigned long)(((unsigned long long)SourceHeight << 19) / Plan->Recout.Height);
    // dpp201_get_optimal_number_of_taps for ratios of 1 or below: 4 luma and 2 chroma taps, 1 for an identity ratio.
    Plan->TapsH = Plan->RatioH19 == ONE19 ? 1ul : 4ul;
    Plan->TapsV = Plan->RatioV19 == ONE19 ? 1ul : 4ul;
    Plan->TapsHC = Plan->RatioH19 == ONE19 ? 1ul : 2ul;
    Plan->TapsVC = Plan->RatioV19 == ONE19 ? 1ul : 2ul;
    // calculate_init_and_vp: init = (ratio + taps + 1) / 2, truncated to 19 fraction bits.
    Plan->InitH19 = (Plan->RatioH19 + ((Plan->TapsH + 1ul) << 19)) >> 1;
    Plan->InitV19 = (Plan->RatioV19 + ((Plan->TapsV + 1ul) << 19)) >> 1;
    Plan->Viewport.X = Plan->Viewport.Y = 0;
    Plan->Viewport.Width = ViewportSize(Plan->InitH19, Plan->RatioH19, Plan->Recout.Width, SourceWidth);
    Plan->Viewport.Height = ViewportSize(Plan->InitV19, Plan->RatioV19, Plan->Recout.Height, SourceHeight);
    // A viewport smaller than the source would crop it. It cannot happen for these ratios (dcn_scale_test.c checks
    // every mode of every list); refuse it rather than program a picture with a missing edge.
    if (Plan->Viewport.Width != SourceWidth || Plan->Viewport.Height != SourceHeight)
        return BC250_DISP_STATUS_NOT_SUPPORTED;
    // dpp1_dscl_get_dscl_mode.
    Plan->DsclMode = (Plan->RatioH19 == ONE19 && Plan->RatioV19 == ONE19) ? DSCL_MODE_444_BYPASS
                                                                           : DSCL_MODE_444_RGB_ENABLE;
    line = Plan->Viewport.Width < Plan->Recout.Width ? Plan->Viewport.Width : Plan->Recout.Width;
    Plan->LbConfig = LbConfig(line, Plan->TapsV, Plan->TapsVC);
    // get_filter_4tap_64p: filter_4tap_64p_upscale for a ratio below 1; no filter for 1 tap.
    Plan->FilterH = Plan->TapsH == 4ul;
    Plan->FilterV = Plan->TapsV == 4ul;
    return BC250_DISP_STATUS_SUCCESS;
}

unsigned long Bc250ScalingSupport(unsigned long Level, unsigned long SourceWidth, unsigned long SourceHeight,
                                  unsigned long TargetWidth, unsigned long TargetHeight)
{
    unsigned long long sw = SourceWidth, sh = SourceHeight, tw = TargetWidth, th = TargetHeight;
    BC250_RECT r;

    if (Bc250ScalingRecout(SourceWidth, SourceHeight, TargetWidth, TargetHeight, BC250_SCALING_CENTERED, &r) < 0)
        return 0;
    // The native size: every scaling is the same picture as identity (the plan is the 1:1 shape), and each one the
    // level offers is reported. dxgkrnl keeps a scaling per monitor in its display database and pins it again for
    // the next mode; a native mode that answered Identity alone would drop out of a source mode set whose path
    // has aspect-ratio scaling pinned.
    if (sw == tw && sh == th) {
        if (Level == BC250_DISPLAY_MODES_CENTERED)
            return BC250_SCALING_BIT(BC250_SCALING_IDENTITY) | BC250_SCALING_BIT(BC250_SCALING_CENTERED);
        if (Level != BC250_DISPLAY_MODES_SCALED) return BC250_SCALING_BIT(BC250_SCALING_IDENTITY);
        return BC250_SCALING_BIT(BC250_SCALING_IDENTITY) | BC250_SCALING_BIT(BC250_SCALING_CENTERED) |
               BC250_SCALING_BIT(BC250_SCALING_STRETCHED) | BC250_SCALING_BIT(BC250_SCALING_ASPECT);
    }
    if (Level == BC250_DISPLAY_MODES_CENTERED) return BC250_SCALING_BIT(BC250_SCALING_CENTERED);
    if (Level != BC250_DISPLAY_MODES_SCALED) return 0;
    // Same aspect ratio: aspect-ratio scaling is the same picture as stretched. It is offered all the same, for the
    // reason above: the pinned scaling of the path must not hide a mode.
    return BC250_SCALING_BIT(BC250_SCALING_CENTERED) | BC250_SCALING_BIT(BC250_SCALING_STRETCHED) |
           BC250_SCALING_BIT(BC250_SCALING_ASPECT);
}

unsigned long Bc250ScalingDefault(unsigned long Level, unsigned long SourceWidth, unsigned long SourceHeight,
                                  unsigned long TargetWidth, unsigned long TargetHeight)
{
    unsigned long support = Bc250ScalingSupport(Level, SourceWidth, SourceHeight, TargetWidth, TargetHeight);
    if (support & BC250_SCALING_BIT(BC250_SCALING_IDENTITY)) return BC250_SCALING_IDENTITY;
    if (support & BC250_SCALING_BIT(BC250_SCALING_ASPECT)) return BC250_SCALING_ASPECT;
    return BC250_SCALING_CENTERED;
}

// ---- the pipe the firmware lit ---------------------------------------------------------------------------------

const char* Bc250PipeVerdictText(unsigned long Verdict)
{
    switch (Verdict) {
    case BC250_PIPE_OK: return "ok";
    case BC250_PIPE_VIEWPORT: return "viewport is not the whole active area";
    case BC250_PIPE_RECOUT: return "recout is not the whole active area";
    case BC250_PIPE_MPC_SIZE: return "MPC size is not the active size";
    case BC250_PIPE_DSCL_MODE: return "DSCL mode is not 444 bypass or 444 RGB";
    case BC250_PIPE_MPCC_TOP: return "MPCC 0 is not fed by DPP 0";
    case BC250_PIPE_ODM: return "OPTC segment 0 is not fed by OPP 0";
    case BC250_PIPE_COEF_POWER: return "scaler coefficient memory forced off";
    default: return "?";
    }
}

long Bc250PipeShapeRead(BC250_DISP_IO* Io, BC250_PIPE_SHAPE* Shape)
{
    TRY(Bc250DispRead(Io, BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START, &Shape->ViewportStart));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, &Shape->ViewportDimension));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_DSCL0_RECOUT_START, &Shape->RecoutStart));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_DSCL0_RECOUT_SIZE, &Shape->RecoutSize));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_DSCL0_MPC_SIZE, &Shape->MpcSize));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_DSCL0_SCL_MODE, &Shape->SclMode));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_CTRL, &Shape->MemPwrCtrl));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_MPCC0_MPCC_TOP_SEL, &Shape->MpccTopSel));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_ODM0_OPTC_DATA_SOURCE_SELECT, &Shape->OdmSource));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &Shape->HubpCntl));
    return BC250_DISP_STATUS_SUCCESS;
}

unsigned long Bc250PipeShapeCheck(const BC250_PIPE_SHAPE* S, unsigned long W, unsigned long H, unsigned long* MaxLevel)
{
    unsigned long mode;

    *MaxLevel = BC250_DISPLAY_MODES_OFF;
    if (Bc250DispGet(S->ViewportStart, HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START_MASK) != 0 ||
        Bc250DispGet(S->ViewportStart, HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START_MASK) != 0 ||
        Bc250DispGet(S->ViewportDimension, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH_MASK) != W ||
        Bc250DispGet(S->ViewportDimension, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK) != H)
        return BC250_PIPE_VIEWPORT;
    if (Bc250DispGet(S->RecoutStart, DSCL0_RECOUT_START__RECOUT_START_X_MASK) != 0 ||
        Bc250DispGet(S->RecoutStart, DSCL0_RECOUT_START__RECOUT_START_Y_MASK) != 0 ||
        Bc250DispGet(S->RecoutSize, DSCL0_RECOUT_SIZE__RECOUT_WIDTH_MASK) != W ||
        Bc250DispGet(S->RecoutSize, DSCL0_RECOUT_SIZE__RECOUT_HEIGHT_MASK) != H)
        return BC250_PIPE_RECOUT;
    if (Bc250DispGet(S->MpcSize, DSCL0_MPC_SIZE__MPC_WIDTH_MASK) != W ||
        Bc250DispGet(S->MpcSize, DSCL0_MPC_SIZE__MPC_HEIGHT_MASK) != H)
        return BC250_PIPE_MPC_SIZE;
    mode = Bc250DispGet(S->SclMode, DSCL0_SCL_MODE__DSCL_MODE_MASK);
    if (mode != DSCL_MODE_444_BYPASS && mode != DSCL_MODE_444_RGB_ENABLE) return BC250_PIPE_DSCL_MODE;
    if (Bc250DispGet(S->MpccTopSel, MPCC0_MPCC_TOP_SEL__MPCC_TOP_SEL_MASK) != 0) return BC250_PIPE_MPCC_TOP;
    if (Bc250DispGet(S->OdmSource, ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL_MASK) != 0) return BC250_PIPE_ODM;
    // dpp1_power_on_dscl: LUT_MEM_PWR_FORCE 0 is "on"; anything else means the coefficient memory may be off, so
    // only the modes without a scaler ratio (centered) are safe.
    if (Bc250DispGet(S->MemPwrCtrl, DSCL0_DSCL_MEM_PWR_CTRL__LUT_MEM_PWR_FORCE_MASK) != 0) {
        *MaxLevel = BC250_DISPLAY_MODES_CENTERED;
        return BC250_PIPE_COEF_POWER;
    }
    *MaxLevel = BC250_DISPLAY_MODES_SCALED;
    return BC250_PIPE_OK;
}

// ---- the register writes ---------------------------------------------------------------------------------------

static unsigned long Pair(unsigned long LowMask, unsigned long Low, unsigned long HighMask, unsigned long High)
{
    return Bc250DispField(LowMask, Low) | Bc250DispField(HighMask, High);
}

// The INIT registers: "0.24 format for fraction, first five bits zeroed" (dpp1_dscl_set_manual_ratio_init).
static unsigned long Init(unsigned long FracMask, unsigned long IntMask, unsigned long Value19)
{
    return Pair(FracMask, (Value19 & (ONE19 - 1ul)) << 5, IntMask, Value19 >> 19);
}

// dpp1_dscl_set_scaler_filter for 4 taps: the select once, then 33 phases of two tap pairs; the data port
// advances by itself.
static long WriteFilter(BC250_DISP_IO* Io, unsigned long FilterType, BC250_SCALER_RESULT* Result)
{
    unsigned long phase, pair;
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_SELECT,
                       Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_SELECT__SCL_COEF_RAM_FILTER_TYPE_MASK, FilterType)));
    for (phase = 0; phase < BC250_SCL_PHASES; phase++)
        for (pair = 0; pair < (BC250_SCL_TAPS + 1ul) / 2ul; pair++) {
            unsigned long even = g_Bc250SclFilter4tapUpscale[phase * BC250_SCL_TAPS + 2ul * pair];
            unsigned long odd = g_Bc250SclFilter4tapUpscale[phase * BC250_SCL_TAPS + 2ul * pair + 1ul];
            TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_COEF_RAM_TAP_DATA,
                               Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_EVEN_TAP_COEF_MASK, even) |
                               DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_EVEN_TAP_COEF_EN_MASK |
                               Bc250DispField(DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_ODD_TAP_COEF_MASK, odd) |
                               DSCL0_SCL_COEF_RAM_TAP_DATA__SCL_COEF_RAM_ODD_TAP_COEF_EN_MASK));
            Result->CoefWrites++;
        }
    return BC250_DISP_STATUS_SUCCESS;
}

// dpp1_dscl_set_scaler_manual_scale for this plan (autocal off, recout, MPC size, mode, line buffer, and for a
// scaling mode the black offset, ratios, inits, taps and filters), then min_set_viewport.
static long WritePipe(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* P, BC250_SCALER_RESULT* Result)
{
    const BC250_RECT* r = &P->Recout;
    const BC250_RECT* v = &P->Viewport;
    unsigned long sclMode;

    // The colour outside the recout (mpc1_set_bg_color with black; RGB components 0).
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_MPCC0_MPCC_BG_R_CR, 0));
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_MPCC0_MPCC_BG_G_Y, 0));
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_MPCC0_MPCC_BG_B_CB, 0));
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_DSCL_AUTOCAL, 0));           // AUTOCAL_MODE_OFF, pipe 0 of 0
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_DSCL_CONTROL, 0));           // SCL_BOUNDARY_MODE 0
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_RECOUT_START,
                       Pair(DSCL0_RECOUT_START__RECOUT_START_X_MASK, r->X, DSCL0_RECOUT_START__RECOUT_START_Y_MASK, r->Y)));
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_RECOUT_SIZE,
                       Pair(DSCL0_RECOUT_SIZE__RECOUT_WIDTH_MASK, r->Width, DSCL0_RECOUT_SIZE__RECOUT_HEIGHT_MASK, r->Height)));
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_MPC_SIZE,
                       Pair(DSCL0_MPC_SIZE__MPC_WIDTH_MASK, P->TargetWidth, DSCL0_MPC_SIZE__MPC_HEIGHT_MASK, P->TargetHeight)));
    TRY(Bc250DispUpdate(Io, BC250_REG_DMU_DSCL0_SCL_MODE, DSCL0_SCL_MODE__DSCL_MODE_MASK,
                        Bc250DispField(DSCL0_SCL_MODE__DSCL_MODE_MASK, P->DsclMode)));
    // dpp1_dscl_set_lb, float data format (DCN 2.0.1's DSCL): interleave and alpha off.
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_LB_DATA_FORMAT, 0));
    TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_LB_MEMORY_CTRL,
                       Pair(DSCL0_LB_MEMORY_CTRL__MEMORY_CONFIG_MASK, P->LbConfig,
                            DSCL0_LB_MEMORY_CTRL__LB_MAX_PARTITIONS_MASK, LB_MAX_PARTITIONS)));
    if (P->DsclMode != DSCL_MODE_444_BYPASS) {
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_BLACK_OFFSET, 0));  // BLACK_OFFSET_RGB_Y 0x0 in both fields
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO,
                           Bc250DispField(DSCL0_SCL_HORZ_FILTER_SCALE_RATIO__SCL_H_SCALE_RATIO_MASK, P->RatioH19 << 5)));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO,
                           Bc250DispField(DSCL0_SCL_VERT_FILTER_SCALE_RATIO__SCL_V_SCALE_RATIO_MASK, P->RatioV19 << 5)));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO_C,
                           Bc250DispField(DSCL0_SCL_HORZ_FILTER_SCALE_RATIO_C__SCL_H_SCALE_RATIO_C_MASK, P->RatioH19 << 5)));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO_C,
                           Bc250DispField(DSCL0_SCL_VERT_FILTER_SCALE_RATIO_C__SCL_V_SCALE_RATIO_C_MASK, P->RatioV19 << 5)));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_INIT,
                           Init(DSCL0_SCL_HORZ_FILTER_INIT__SCL_H_INIT_FRAC_MASK,
                                DSCL0_SCL_HORZ_FILTER_INIT__SCL_H_INIT_INT_MASK, P->InitH19)));
        // The chroma inits use the chroma taps: (ratio + taps_c + 1) / 2.
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_INIT_C,
                           Init(DSCL0_SCL_HORZ_FILTER_INIT_C__SCL_H_INIT_FRAC_C_MASK,
                                DSCL0_SCL_HORZ_FILTER_INIT_C__SCL_H_INIT_INT_C_MASK,
                                (P->RatioH19 + ((P->TapsHC + 1ul) << 19)) >> 1)));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_INIT,
                           Init(DSCL0_SCL_VERT_FILTER_INIT__SCL_V_INIT_FRAC_MASK,
                                DSCL0_SCL_VERT_FILTER_INIT__SCL_V_INIT_INT_MASK, P->InitV19)));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_INIT_BOT,
                           Init(DSCL0_SCL_VERT_FILTER_INIT_BOT__SCL_V_INIT_FRAC_BOT_MASK,
                                DSCL0_SCL_VERT_FILTER_INIT_BOT__SCL_V_INIT_INT_BOT_MASK, P->InitV19 + P->RatioV19)));
        {
            unsigned long initVC = (P->RatioV19 + ((P->TapsVC + 1ul) << 19)) >> 1;
            TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_INIT_C,
                               Init(DSCL0_SCL_VERT_FILTER_INIT_C__SCL_V_INIT_FRAC_C_MASK,
                                    DSCL0_SCL_VERT_FILTER_INIT_C__SCL_V_INIT_INT_C_MASK, initVC)));
            TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_INIT_BOT_C,
                               Init(DSCL0_SCL_VERT_FILTER_INIT_BOT_C__SCL_V_INIT_FRAC_BOT_C_MASK,
                                    DSCL0_SCL_VERT_FILTER_INIT_BOT_C__SCL_V_INIT_INT_BOT_C_MASK, initVC + P->RatioV19)));
        }
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL,
                           Bc250DispField(DSCL0_SCL_TAP_CONTROL__SCL_V_NUM_TAPS_MASK, P->TapsV - 1ul) |
                           Bc250DispField(DSCL0_SCL_TAP_CONTROL__SCL_H_NUM_TAPS_MASK, P->TapsH - 1ul) |
                           Bc250DispField(DSCL0_SCL_TAP_CONTROL__SCL_V_NUM_TAPS_C_MASK, P->TapsVC - 1ul) |
                           Bc250DispField(DSCL0_SCL_TAP_CONTROL__SCL_H_NUM_TAPS_C_MASK, P->TapsHC - 1ul)));
        // dpp1_dscl_set_scl_filter: no 2-tap hard-coded filter (the luma taps are 4 or 1), sharpness off.
        TRY(Bc250DispUpdate(Io, BC250_REG_DMU_DSCL0_DSCL_2TAP_CONTROL,
                            DSCL0_DSCL_2TAP_CONTROL__SCL_H_2TAP_HARDCODE_COEF_EN_MASK |
                            DSCL0_DSCL_2TAP_CONTROL__SCL_H_2TAP_SHARP_EN_MASK |
                            DSCL0_DSCL_2TAP_CONTROL__SCL_H_2TAP_SHARP_FACTOR_MASK |
                            DSCL0_DSCL_2TAP_CONTROL__SCL_V_2TAP_HARDCODE_COEF_EN_MASK |
                            DSCL0_DSCL_2TAP_CONTROL__SCL_V_2TAP_SHARP_EN_MASK |
                            DSCL0_DSCL_2TAP_CONTROL__SCL_V_2TAP_SHARP_FACTOR_MASK, 0));
        TRY(Bc250DispRead(Io, BC250_REG_DMU_DSCL0_SCL_MODE, &sclMode));
        // Deviation: Linux skips the load when the filter equals the one it loaded last (dpp->filter_h/_v); this
        // code keeps no such cache across a stop and a restart, so it loads and swaps every time it scales.
        if (P->FilterH) TRY(WriteFilter(Io, COEF_LUMA_HORZ, Result));
        if (P->FilterV) TRY(WriteFilter(Io, COEF_LUMA_VERT, Result));
        if (P->FilterH || P->FilterV) {
            unsigned long current = Bc250DispGet(sclMode, DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_CURRENT_MASK);
            sclMode &= ~(DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK | DSCL0_SCL_MODE__SCL_CHROMA_COEF_MODE_MASK);
            sclMode |= Bc250DispField(DSCL0_SCL_MODE__SCL_COEF_RAM_SELECT_MASK, current ? 0ul : 1ul);
            TRY(Bc250DispWrite(Io, BC250_REG_DMU_DSCL0_SCL_MODE, sclMode));
        }
    }
    // min_set_viewport: primary and secondary (stereo), luma and chroma, all the same rectangle for RGB.
    {
        unsigned long dim = Pair(HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH_MASK, v->Width,
                                 HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK, v->Height);
        unsigned long start = Pair(HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_X_START_MASK, v->X,
                                   HUBP0_DCSURF_PRI_VIEWPORT_START__PRI_VIEWPORT_Y_START_MASK, v->Y);
        unsigned long dimSec = Pair(HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION__SEC_VIEWPORT_WIDTH_MASK, v->Width,
                                    HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION__SEC_VIEWPORT_HEIGHT_MASK, v->Height);
        unsigned long startSec = Pair(HUBP0_DCSURF_SEC_VIEWPORT_START__SEC_VIEWPORT_X_START_MASK, v->X,
                                      HUBP0_DCSURF_SEC_VIEWPORT_START__SEC_VIEWPORT_Y_START_MASK, v->Y);
        unsigned long dimC = Pair(HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C__PRI_VIEWPORT_WIDTH_C_MASK, v->Width,
                                  HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C__PRI_VIEWPORT_HEIGHT_C_MASK, v->Height);
        unsigned long startC = Pair(HUBP0_DCSURF_PRI_VIEWPORT_START_C__PRI_VIEWPORT_X_START_C_MASK, v->X,
                                    HUBP0_DCSURF_PRI_VIEWPORT_START_C__PRI_VIEWPORT_Y_START_C_MASK, v->Y);
        unsigned long dimSecC = Pair(HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C__SEC_VIEWPORT_WIDTH_C_MASK, v->Width,
                                     HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C__SEC_VIEWPORT_HEIGHT_C_MASK, v->Height);
        unsigned long startSecC = Pair(HUBP0_DCSURF_SEC_VIEWPORT_START_C__SEC_VIEWPORT_X_START_C_MASK, v->X,
                                       HUBP0_DCSURF_SEC_VIEWPORT_START_C__SEC_VIEWPORT_Y_START_C_MASK, v->Y);
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION, dim));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START, start));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION, dimSec));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_SEC_VIEWPORT_START, startSec));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION_C, dimC));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START_C, startC));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_SEC_VIEWPORT_DIMENSION_C, dimSecC));
        TRY(Bc250DispWrite(Io, BC250_REG_DMU_HUBP0_DCSURF_SEC_VIEWPORT_START_C, startSecC));
    }
    return BC250_DISP_STATUS_SUCCESS;
}

long Bc250ScalerProgram(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* Plan, BC250_SCALER_RESULT* Result)
{
    unsigned long value = 0, waited = 0;
    long status;

    Result->LockWaitUs = Result->CoefWrites = Result->SclModeBefore = Result->SclModeAfter = 0;
    Result->HubpCntlAfter = 0;
    if (Plan->Viewport.Width == 0 || Plan->Recout.Width == 0) return BC250_DISP_STATUS_INVALID;
    status = Bc250DispRead(Io, BC250_REG_DMU_DSCL0_SCL_MODE, &Result->SclModeBefore);
    if (status < 0) return status;
    // optc1_lock: take the lock and wait for its acknowledgement before the first double-buffered write.
    status = Bc250DispWrite(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,
                            OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK);
    if (status < 0) return status;
    for (;;) {
        status = Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &value);
        if (status < 0 || (value & OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK) != 0) break;
        if (waited == LOCK_WAIT_STEPS) { status = BC250_DISP_STATUS_TIMEOUT; break; }
        status = Bc250DispStall(Io, 1);
        if (status < 0) break;
        waited++;
    }
    Result->LockWaitUs = waited;
    if (status >= 0) status = WritePipe(Io, Plan, Result);
    // Unlock in every case; the manual trigger only after a complete sequence (DcnFlipWriteSequence's rule).
    {
        long unlock = Bc250DispWrite(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0);
        if (status >= 0) status = unlock;
    }
    if (status >= 0)
        status = Bc250DispWrite(Io, BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG,
                                OTG0_OTG_TRIGA_MANUAL_TRIG__OTG_TRIGA_MANUAL_TRIG_MASK);
    (void)Bc250DispRead(Io, BC250_REG_DMU_DSCL0_SCL_MODE, &Result->SclModeAfter);
    (void)Bc250DispRead(Io, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &Result->HubpCntlAfter);
    return status;
}

long Bc250ScalerRestoreNative(BC250_DISP_IO* Io, unsigned long NativeWidth, unsigned long NativeHeight,
                              BC250_SCALER_RESULT* Result)
{
    BC250_SCALER_PLAN plan;
    long s = Bc250ScalerPlan(NativeWidth, NativeHeight, NativeWidth, NativeHeight, BC250_SCALING_IDENTITY, &plan);
    if (s < 0) return s;
    return Bc250ScalerProgram(Io, &plan, Result);
}
