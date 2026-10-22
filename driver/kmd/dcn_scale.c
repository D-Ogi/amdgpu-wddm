// Pipe 0's scaler and viewport for a smaller desktop source mode: see dcn_scale.h for the design and the Linux
// functions this follows (MIT, v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449). Every register name comes from
// gen_regs.py (regs.generated.h) and every field from dcn_2_0_1_sh_mask.h.
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "dcn_scale.h"
#include "edid.h"
#include "../shim/bc250_scaler_buffer.h"

// filter_4tap_64p_upscale of dce_scl_filters.c, extracted by tools/import/extract_table.py (PROVENANCE.md).
const unsigned short g_Bc250SclFilter4tapUpscale[BC250_SCL_PHASES * BC250_SCL_TAPS] = {
#include "scl_filter_4tap_64p_upscale.inc"
};

#define ONE19 (1ul << 19)
#define DSCL_MODE_444_BYPASS 0ul            // dcn10_dpp.c enum dscl_mode_sel
#define DSCL_MODE_444_RGB_ENABLE 1ul
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
    case BC250_SCALING_CUSTOM: return "inherited-custom";
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
        !DimensionOk(TargetHeight) || Scaling >= BC250_SCALING_CUSTOM) return BC250_DISP_STATUS_INVALID;
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

// calculate_init_and_vp for a recout offset of 0 within the full recout: the viewport starts at 0, and its size is
// floor(init + ratio * (recout - 1)) capped at the source size.
static unsigned long ViewportSize(unsigned long Init19, unsigned long Ratio19, unsigned long Recout,
                                  unsigned long Source)
{
    unsigned long long v = ((unsigned long long)Init19 + (unsigned long long)Ratio19 * (Recout - 1ul)) >> 19;
    return v > Source ? Source : (unsigned long)v;
}

static void PlanOpen(unsigned long SourceWidth, unsigned long SourceHeight, unsigned long TargetWidth,
                     unsigned long TargetHeight, unsigned long Scaling, BC250_SCALER_PLAN* Plan)
{
    unsigned char* clear = (unsigned char*)Plan;
    unsigned long i;

    for (i = 0; i < sizeof(*Plan); i++) clear[i] = 0;      // a refused plan programs nothing (Bc250ScalerProgram)
    Plan->SourceWidth = SourceWidth;
    Plan->SourceHeight = SourceHeight;
    Plan->TargetWidth = TargetWidth;
    Plan->TargetHeight = TargetHeight;
    Plan->Scaling = Scaling;
}

// The rest of the plan, from the source size and Plan->Recout.
static long PlanFromRecout(BC250_SCALER_PLAN* Plan)
{
    const unsigned long SourceWidth = Plan->SourceWidth, SourceHeight = Plan->SourceHeight;
    unsigned char* clear = (unsigned char*)Plan;
    unsigned long i;
    BC250_SCALER_BUFFER_INPUT buffer = {0};
    BC250_SCALER_BUFFER_PLAN capacity;

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
    buffer.ViewportWidth = buffer.ChromaViewportWidth = Plan->Viewport.Width;
    buffer.RecoutWidth = Plan->Recout.Width;
    buffer.VerticalRatio19 = buffer.ChromaVerticalRatio19 = Plan->RatioV19;
    buffer.VerticalTaps = Plan->TapsV;
    buffer.ChromaVerticalTaps = Plan->TapsVC;
    if (!Bc250ScalerBufferPlan(&buffer, &capacity)) {
        // Config 0 is not an unconditional fallback: it can lack lines too.
        for (i = 0; i < sizeof(*Plan); ++i) clear[i] = 0;
        return BC250_DISP_STATUS_NOT_SUPPORTED;
    }
    Plan->LbConfig = capacity.Config;
    // get_filter_4tap_64p: filter_4tap_64p_upscale for a ratio below 1; no filter for 1 tap.
    Plan->FilterH = Plan->TapsH == 4ul;
    Plan->FilterV = Plan->TapsV == 4ul;
    return BC250_DISP_STATUS_SUCCESS;
}

long Bc250ScalerPlan(unsigned long SourceWidth, unsigned long SourceHeight, unsigned long TargetWidth,
                     unsigned long TargetHeight, unsigned long Scaling, BC250_SCALER_PLAN* Plan)
{
    long s;

    PlanOpen(SourceWidth, SourceHeight, TargetWidth, TargetHeight, Scaling, Plan);
    s = Bc250ScalingRecout(SourceWidth, SourceHeight, TargetWidth, TargetHeight, Scaling, &Plan->Recout);
    if (s < 0) return s;
    return PlanFromRecout(Plan);
}

long Bc250ScalerPlanRecout(unsigned long SourceWidth, unsigned long SourceHeight, unsigned long TargetWidth,
                           unsigned long TargetHeight, const BC250_RECT* Recout, BC250_SCALER_PLAN* Plan)
{
    PlanOpen(SourceWidth, SourceHeight, TargetWidth, TargetHeight, BC250_SCALING_PLANE, Plan);
    if (!Recout || !DimensionOk(SourceWidth) || !DimensionOk(SourceHeight) || !DimensionOk(TargetWidth) ||
        !DimensionOk(TargetHeight) || !DimensionOk(Recout->Width) || !DimensionOk(Recout->Height) ||
        Recout->X > TargetWidth - Recout->Width || Recout->Y > TargetHeight - Recout->Height ||
        Recout->Width > TargetWidth || Recout->Height > TargetHeight)
        return BC250_DISP_STATUS_INVALID;
    if (SourceWidth > Recout->Width || SourceHeight > Recout->Height)
        return BC250_DISP_STATUS_NOT_SUPPORTED;                         // no downscale, as in stage A
    Plan->Recout = *Recout;
    return PlanFromRecout(Plan);
}

unsigned long Bc250ScalingSupport(unsigned long Level, unsigned long SourceWidth, unsigned long SourceHeight,
                                  unsigned long TargetWidth, unsigned long TargetHeight)
{
    unsigned long support = 0, scaling;
    BC250_RECT r;
    BC250_SCALER_PLAN plan;

    if (Bc250ScalingRecout(SourceWidth, SourceHeight, TargetWidth, TargetHeight, BC250_SCALING_CENTERED, &r) < 0)
        return 0;
    // The native size: every scaling is the same picture as identity (the plan is the 1:1 shape), and each one the
    // level offers is reported. dxgkrnl keeps a scaling per monitor in its display database and pins it again for
    // the next mode; a native mode that answered Identity alone would drop out of a source mode set whose path
    // has aspect-ratio scaling pinned.
    if (SourceWidth == TargetWidth && SourceHeight == TargetHeight)
        support |= BC250_SCALING_BIT(BC250_SCALING_IDENTITY);
    if (Level == BC250_DISPLAY_MODES_CENTERED || Level == BC250_DISPLAY_MODES_SCALED)
        support |= BC250_SCALING_BIT(BC250_SCALING_CENTERED);
    // Same aspect ratio: aspect-ratio scaling is the same picture as stretched. It is offered all the same, for the
    // reason above: the pinned scaling of the path must not hide a mode.
    if (Level == BC250_DISPLAY_MODES_SCALED)
        support |= BC250_SCALING_BIT(BC250_SCALING_STRETCHED) | BC250_SCALING_BIT(BC250_SCALING_ASPECT);
    // Mode enumeration and programming must agree about line-buffer admission.
    for (scaling = 0; scaling < BC250_SCALING_CUSTOM; ++scaling)
        if ((support & BC250_SCALING_BIT(scaling)) &&
            Bc250ScalerPlan(SourceWidth, SourceHeight, TargetWidth, TargetHeight, scaling, &plan) < 0)
            support &= ~BC250_SCALING_BIT(scaling);
    return support;
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

// No writes, waits or allocation: the complete pair can be compared before it
// is published. Keep reading after a failure for useful start diagnostics.
long Bc250InheritedPipeRead(BC250_DISP_IO* Io, BC250_INHERITED_PIPE* P)
{
    static const unsigned long offsets[BC250_INHERITED_PIPE_READS] = {
        BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_START,
        BC250_REG_DMU_HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION,
        BC250_REG_DMU_DSCL0_RECOUT_START, BC250_REG_DMU_DSCL0_RECOUT_SIZE,
        BC250_REG_DMU_DSCL0_MPC_SIZE, BC250_REG_DMU_DSCL0_SCL_MODE,
        BC250_REG_DMU_DSCL0_DSCL_MEM_PWR_CTRL, BC250_REG_DMU_MPCC0_MPCC_TOP_SEL,
        BC250_REG_DMU_ODM0_OPTC_DATA_SOURCE_SELECT, BC250_REG_DMU_HUBP0_DCHUBP_CNTL,
        BC250_REG_DMU_DSCL0_SCL_HORZ_FILTER_SCALE_RATIO,
        BC250_REG_DMU_DSCL0_SCL_VERT_FILTER_SCALE_RATIO,
        BC250_REG_DMU_DSCL0_SCL_TAP_CONTROL, BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG,
        BC250_REG_DMU_HUBP0_DCSURF_TILING_CONFIG,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH, BC250_REG_DMU_OTG1_OTG_CONTROL,
        BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL, BC250_REG_DMU_MPCC0_MPCC_BOT_SEL,
        BC250_REG_DMU_MPCC0_MPCC_CONTROL, BC250_REG_DMU_MPC_OUT0_MUX,
        BC250_REG_DMU_DSCL0_DSCL_AUTOCAL, BC250_REG_DMU_DSCL0_DSCL_CONTROL
    };
    unsigned long* fields[BC250_INHERITED_PIPE_READS] = {
        &P->Shape.ViewportStart, &P->Shape.ViewportDimension, &P->Shape.RecoutStart,
        &P->Shape.RecoutSize, &P->Shape.MpcSize, &P->Shape.SclMode, &P->Shape.MemPwrCtrl,
        &P->Shape.MpccTopSel, &P->Shape.OdmSource, &P->Shape.HubpCntl,
        &P->RatioH, &P->RatioV, &P->Taps, &P->SurfaceConfig, &P->TilingConfig, &P->Pitch, &P->Otg1,
        &P->SurfaceControl, &P->MpccBottom, &P->MpccControl, &P->MpcOut,
        &P->Autocal, &P->DsclControl
    };
    unsigned long i;
    long status = BC250_DISP_STATUS_SUCCESS;
    P->Valid = 0;
    for (i = 0; i < BC250_INHERITED_PIPE_READS; ++i) {
        long step = Bc250DispRead(Io, offsets[i], fields[i]);
        if (step >= 0) P->Valid |= 1ul << i;
        else if (status >= 0) status = step;
    }
    return status;
}

int Bc250InheritedPipeSame(const BC250_INHERITED_PIPE* A, const BC250_INHERITED_PIPE* B)
{
    // These are all unsigned-long words (no padding). HUBP status may change
    // without a modeset; compare only the stable programming bits in that word.
    BC250_INHERITED_PIPE a = *A, b = *B;
    const unsigned char* pa = (const unsigned char*)&a;
    const unsigned char* pb = (const unsigned char*)&b;
    unsigned long i;
    a.Shape.HubpCntl &= HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
    b.Shape.HubpCntl &= HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
    a.MpcOut &= MPC_OUT0_MUX__MPC_OUT_MUX_MASK;
    b.MpcOut &= MPC_OUT0_MUX__MPC_OUT_MUX_MASK;
    for (i = 0; i < sizeof(a); ++i) if (pa[i] != pb[i]) return 0;
    return 1;
}

// BD-169: the gate numbers are the vocabulary of a field support file (the start
// record's Detail and the inherited: log line carry the number and the word that
// decided it), so a report from a board this classifier refuses says which check
// to extend. Keep the text short and stable: it is read next to the number.
const char* Bc250InheritedPipeGateText(unsigned long Gate)
{
    static const char* const text[BC250_PIPE_GATE_COUNT] = {
        "none", "reads", "source-size", "target-size", "pitch-arg", "viewport-start", "viewport-size",
        "mpc-size", "mpcc-top", "mpc-out", "dcc", "odm", "otg1", "rotation", "tiling", "pitch-reg",
        "mpcc-mode", "mpcc-bottom", "pixel-format", "recout", "autocal-mode", "autocal-pipe",
        "autocal-dscl", "autocal-upscale", "autocal-coef", "bypass-recout", "coef-power",
        "ratio-h-zero", "ratio-v-zero", "ratio-h", "ratio-v", "ratio-upscale", "dscl-mode", "unstable"
    };
    return Gate < BC250_PIPE_GATE_COUNT ? text[Gate] : "gate";
}

static unsigned long Refuse(BC250_INHERITED_PIPE_REASON* Reason, unsigned long Gate, unsigned long Word)
{
    Reason->Gate = Gate;
    Reason->Word = Word;
    return 0;
}

static unsigned long Pair16(unsigned long Low, unsigned long High)
{
    return ((Low & 0xFFFFul) << 16) | (High & 0xFFFFul);
}

unsigned long Bc250InheritedPipeSupport(const BC250_INHERITED_PIPE* P,
    unsigned long W, unsigned long H, unsigned long Pitch, unsigned long TW, unsigned long TH)
{
    BC250_INHERITED_PIPE_REASON reason;
    return Bc250InheritedPipeSupportEx(P, W, H, Pitch, TW, TH, &reason);
}

// One gate per statement, in the order the checks ran before BD-169 split them,
// so Reason names the *first* one that refused. The verdict is unchanged: the
// host suite (driver/kmd/test/inherited_pipe_cases.h) compares every word table
// against the old decision.
unsigned long Bc250InheritedPipeSupportEx(const BC250_INHERITED_PIPE* P,
    unsigned long W, unsigned long H, unsigned long Pitch, unsigned long TW, unsigned long TH,
    BC250_INHERITED_PIPE_REASON* Reason)
{
    const BC250_PIPE_SHAPE* s = &P->Shape;
    BC250_RECT recout, wanted;
    unsigned long mode, scaling, support = 0, mpccMode, autocal;
    unsigned long long ratio, actual;
    int ratioMatches = 1;
    Reason->Gate = BC250_PIPE_GATE_NONE;
    Reason->Word = 0;
    if (P->Valid != BC250_INHERITED_PIPE_ALL) return Refuse(Reason, BC250_PIPE_GATE_READS, P->Valid);
    if (!DimensionOk(W) || !DimensionOk(H)) return Refuse(Reason, BC250_PIPE_GATE_SOURCE_SIZE, Pair16(W, H));
    if (!DimensionOk(TW) || !DimensionOk(TH)) return Refuse(Reason, BC250_PIPE_GATE_TARGET_SIZE, Pair16(TW, TH));
    if (Pitch < W * 4ul || Pitch % 4ul) return Refuse(Reason, BC250_PIPE_GATE_PITCH_ARG, Pitch);
    if (s->ViewportStart != 0) return Refuse(Reason, BC250_PIPE_GATE_VIEWPORT_START, s->ViewportStart);
    if (Bc250DispGet(s->ViewportDimension, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_WIDTH_MASK) != W ||
        Bc250DispGet(s->ViewportDimension, HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION__PRI_VIEWPORT_HEIGHT_MASK) != H)
        return Refuse(Reason, BC250_PIPE_GATE_VIEWPORT_SIZE, s->ViewportDimension);
    if (Bc250DispGet(s->MpcSize, DSCL0_MPC_SIZE__MPC_WIDTH_MASK) != TW ||
        Bc250DispGet(s->MpcSize, DSCL0_MPC_SIZE__MPC_HEIGHT_MASK) != TH)
        return Refuse(Reason, BC250_PIPE_GATE_MPC_SIZE, s->MpcSize);
    if (Bc250DispGet(s->MpccTopSel, MPCC0_MPCC_TOP_SEL__MPCC_TOP_SEL_MASK) != 0)
        return Refuse(Reason, BC250_PIPE_GATE_MPCC_TOP, s->MpccTopSel);
    if (Bc250DispGet(P->MpcOut, MPC_OUT0_MUX__MPC_OUT_MUX_MASK) != 0)
        return Refuse(Reason, BC250_PIPE_GATE_MPC_OUT, P->MpcOut);
    if (P->SurfaceControl & HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK)
        return Refuse(Reason, BC250_PIPE_GATE_DCC, P->SurfaceControl);
    if (Bc250DispGet(s->OdmSource, ODM0_OPTC_DATA_SOURCE_SELECT__OPTC_SEG0_SRC_SEL_MASK) != 0)
        return Refuse(Reason, BC250_PIPE_GATE_ODM, s->OdmSource);
    if (P->Otg1 & OTG1_OTG_CONTROL__OTG_MASTER_EN_MASK) return Refuse(Reason, BC250_PIPE_GATE_OTG1, P->Otg1);
    if (P->SurfaceConfig & (HUBP0_DCSURF_SURFACE_CONFIG__ROTATION_ANGLE_MASK |
                            HUBP0_DCSURF_SURFACE_CONFIG__H_MIRROR_EN_MASK))
        return Refuse(Reason, BC250_PIPE_GATE_ROTATION, P->SurfaceConfig);
    if (P->TilingConfig & HUBP0_DCSURF_TILING_CONFIG__SW_MODE_MASK)
        return Refuse(Reason, BC250_PIPE_GATE_TILING, P->TilingConfig);
    if ((Bc250DispGet(P->Pitch, HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) + 1ul) * 4ul != Pitch)
        return Refuse(Reason, BC250_PIPE_GATE_PITCH_REG, P->Pitch);
    // dc/inc/hw/mpc.h: mode 1 passes the top layer through; the bottom
    // selector is unused. Mode 2 retains the established disconnected-bottom
    // requirement. Mode 0 bypass and mode 3 two-layer blending are not ours.
    mpccMode = Bc250DispGet(P->MpccControl, MPCC0_MPCC_CONTROL__MPCC_MODE_MASK);
    if (mpccMode != 1ul && mpccMode != 2ul) return Refuse(Reason, BC250_PIPE_GATE_MPCC_MODE, P->MpccControl);
    if (mpccMode == 2ul && Bc250DispGet(P->MpccBottom, MPCC0_MPCC_BOT_SEL__MPCC_BOT_SEL_MASK) != 0xFul)
        return Refuse(Reason, BC250_PIPE_GATE_MPCC_BOTTOM, P->MpccBottom);
    // dcn10_hubp.c: RGB8888 uses surface format 8; no YUV or metadata decoding.
    if (Bc250DispGet(P->SurfaceConfig, HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) != 8ul)
        return Refuse(Reason, BC250_PIPE_GATE_PIXEL_FORMAT, P->SurfaceConfig);
    recout.X = Bc250DispGet(s->RecoutStart, DSCL0_RECOUT_START__RECOUT_START_X_MASK);
    recout.Y = Bc250DispGet(s->RecoutStart, DSCL0_RECOUT_START__RECOUT_START_Y_MASK);
    recout.Width = Bc250DispGet(s->RecoutSize, DSCL0_RECOUT_SIZE__RECOUT_WIDTH_MASK);
    recout.Height = Bc250DispGet(s->RecoutSize, DSCL0_RECOUT_SIZE__RECOUT_HEIGHT_MASK);
    if (!recout.Width || !recout.Height || recout.X > TW || recout.Y > TH ||
        recout.Width > TW - recout.X || recout.Height > TH - recout.Y)
        return Refuse(Reason, BC250_PIPE_GATE_RECOUT, s->RecoutSize);
    mode = Bc250DispGet(s->SclMode, DSCL0_SCL_MODE__DSCL_MODE_MASK);
    autocal = Bc250DispGet(P->Autocal, DSCL0_DSCL_AUTOCAL__AUTOCAL_MODE_MASK);
    // Firmware automatic centering and replication (modes 2/3) need a
    // separate geometry contract. NUM_PIPE/PIPE_ID are inactive when
    // AUTOCAL_MODE_OFF selects manual calculation (dcn10_dpp_dscl.c).
    if (autocal > 1ul) return Refuse(Reason, BC250_PIPE_GATE_AUTOCAL_MODE, P->Autocal);
    if (autocal == 1ul) {
        // dcn10_dpp_dscl.c enum dscl_autocal_mode: automatic scale computes
        // ratio AND phase and requires DSCL_MODE=1. Manual ratio registers
        // may be stale, including 1.0; do not validate them as active input.
        // Nonzero automatic pipe fields have no verified encoding here.
        if (P->Autocal & (DSCL0_DSCL_AUTOCAL__AUTOCAL_NUM_PIPE_MASK |
                          DSCL0_DSCL_AUTOCAL__AUTOCAL_PIPE_ID_MASK))
            return Refuse(Reason, BC250_PIPE_GATE_AUTOCAL_PIPE, P->Autocal);
        if (mode != DSCL_MODE_444_RGB_ENABLE) return Refuse(Reason, BC250_PIPE_GATE_AUTOCAL_DSCL, s->SclMode);
        if (W > recout.Width || H > recout.Height)
            return Refuse(Reason, BC250_PIPE_GATE_AUTOCAL_UPSCALE, s->RecoutSize);
        if (Bc250DispGet(s->MemPwrCtrl, DSCL0_DSCL_MEM_PWR_CTRL__LUT_MEM_PWR_FORCE_MASK))
            return Refuse(Reason, BC250_PIPE_GATE_AUTOCAL_COEF, s->MemPwrCtrl);
        // Classify the automatic transform from viewport and recout below.
        // Full-target recout is STRETCHED; centering and aspect preservation
        // use the same rectangle matcher as manual ratios. This names the
        // existing transform only: commit still keeps every pipe register.
    } else if (mode == DSCL_MODE_444_BYPASS) {
        if (recout.Width != W || recout.Height != H)
            return Refuse(Reason, BC250_PIPE_GATE_BYPASS_RECOUT, s->RecoutSize);
    } else if (mode == DSCL_MODE_444_RGB_ENABLE) {
        if (Bc250DispGet(s->MemPwrCtrl, DSCL0_DSCL_MEM_PWR_CTRL__LUT_MEM_PWR_FORCE_MASK))
            return Refuse(Reason, BC250_PIPE_GATE_COEF_POWER, s->MemPwrCtrl);
        // dpp1_dscl_set_manual_ratio_init: Q3.19 ratio stored as Q3.24. Allow
        // one Q3.19 unit for the firmware's rounding; never accept a zero ratio.
        ratio = ((unsigned long long)W << 24) / recout.Width;
        actual = P->RatioH & DSCL0_SCL_HORZ_FILTER_SCALE_RATIO__SCL_H_SCALE_RATIO_MASK;
        if (!actual) return Refuse(Reason, BC250_PIPE_GATE_RATIO_H_ZERO, P->RatioH);
        if ((actual > ratio ? actual - ratio : ratio - actual) > 32u) ratioMatches = 0;
        ratio = ((unsigned long long)H << 24) / recout.Height;
        actual = P->RatioV & DSCL0_SCL_VERT_FILTER_SCALE_RATIO__SCL_V_SCALE_RATIO_MASK;
        if (!actual) return Refuse(Reason, BC250_PIPE_GATE_RATIO_V_ZERO, P->RatioV);
        if ((actual > ratio ? actual - ratio : ratio - actual) > 32u) ratioMatches = 0;
        if (!ratioMatches) {
            // Retain a firmware unity-ratio pipe with a larger output as
            // custom, not stretched. HUBP still fetches only the validated
            // source viewport/stride. We neither enlarge the source nor
            // change ratios, phases, boundary mode, coefficients or timing.
            // This is image preservation, not proof of edge/black fill.
            if (P->RatioH != (ONE19 << 5)) return Refuse(Reason, BC250_PIPE_GATE_RATIO_H, P->RatioH);
            if (P->RatioV != (ONE19 << 5)) return Refuse(Reason, BC250_PIPE_GATE_RATIO_V, P->RatioV);
            if (W > recout.Width || H > recout.Height)
                return Refuse(Reason, BC250_PIPE_GATE_RATIO_UPSCALE, s->RecoutSize);
            return BC250_SCALING_BIT(BC250_SCALING_CUSTOM);
        }
    } else return Refuse(Reason, BC250_PIPE_GATE_DSCL_MODE, s->SclMode);
    for (scaling = 0; scaling < BC250_SCALING_CUSTOM; ++scaling) {
        if (Bc250ScalingRecout(W, H, TW, TH, scaling, &wanted) >= 0 &&
            wanted.X == recout.X && wanted.Y == recout.Y && wanted.Width == recout.Width &&
            wanted.Height == recout.Height) support |= BC250_SCALING_BIT(scaling);
    }
    return support ? support : BC250_SCALING_BIT(BC250_SCALING_CUSTOM);
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

static long ScalerBegin(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* Plan, BC250_SCALER_RESULT* Result,
                       int Stopped)
{
    BC250_SCALER_RESULT empty = {0};
    unsigned long control, clock;
    long status;
    if (!Result) return BC250_DISP_STATUS_INVALID;
    *Result = empty;
    if (!Io || !Io->Read || !Io->Write || !Plan ||
        Plan->Viewport.Width == 0 || Plan->Recout.Width == 0) return BC250_DISP_STATUS_INVALID;
    TRY(Bc250DispRead(Io, BC250_REG_DMU_DSCL0_SCL_MODE, &Result->SclModeBefore));
    TRY(Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_CONTROL, &control));
    if (Stopped) {
        // optc1_disable_crtc clears MASTER_EN and waits for OTG_BUSY=0.
        // dcn10_hwseq skips the TG lock for an already disabled controller.
        if (control & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) return BC250_DISP_STATUS_MISMATCH;
        TRY(Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_CLOCK_CONTROL, &clock));
        if (clock & OTG0_OTG_CLOCK_CONTROL__OTG_BUSY_MASK) return BC250_DISP_STATUS_MISMATCH;
        status = Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &Result->LockStatus);
        Result->LockStatusValid = status >= 0;
        if (status < 0) { Result->LockStatus = 0; return status; }
        if (Result->LockStatus & (OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                                 OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK))
            return BC250_DISP_STATUS_MISMATCH;
    } else if (!(control & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK)) {
        return BC250_DISP_STATUS_MISMATCH;
    }
    return BC250_DISP_STATUS_SUCCESS;
}

static void ScalerAfter(BC250_DISP_IO* Io, BC250_SCALER_RESULT* Result)
{
    (void)Bc250DispRead(Io, BC250_REG_DMU_DSCL0_SCL_MODE, &Result->SclModeAfter);
    (void)Bc250DispRead(Io, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &Result->HubpCntlAfter);
}

long Bc250ScalerProgramLocked(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* Plan,
                              BC250_SCALER_RESULT* Result)
{
    unsigned long select;
    const unsigned long held = OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK |
                               OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK;
    long status = ScalerBegin(Io, Plan, Result, 0);
    if (status < 0) return status;
    // AMD dcn20_pipe_control_lock protects scaler, viewport and address updates
    // with the top pipe's TG lock. Do not create a second latch boundary here.
    TRY(Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0, &select));
    if (select & OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK)
        return BC250_DISP_STATUS_MISMATCH;
    status = Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &Result->LockStatus);
    Result->LockStatusValid = status >= 0;
    if (status < 0) { Result->LockStatus = 0; return status; }
    if ((Result->LockStatus & held) != held) return BC250_DISP_STATUS_MISMATCH;
    status = WritePipe(Io, Plan, Result);
    ScalerAfter(Io, Result);
    return status;
}

long Bc250ScalerProgramStopped(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* Plan, BC250_SCALER_RESULT* Result)
{
    long status = ScalerBegin(Io, Plan, Result, 1);
    if (status < 0) return status;
    status = WritePipe(Io, Plan, Result);
    ScalerAfter(Io, Result);
    return status;
}

long Bc250ScalerProgram(BC250_DISP_IO* Io, const BC250_SCALER_PLAN* Plan, BC250_SCALER_RESULT* Result)
{
    unsigned long value = 0, waited = 0;
    long status = ScalerBegin(Io, Plan, Result, 0);

    if (status < 0) return status;
    // optc1_lock selects this OTG before requesting the update lock. REG_SET
    // uses an initial value of zero in AMD's implementation, including keepout.
    status = Bc250DispWrite(Io, BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0,
                           Bc250DispField(OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK, 0));
    if (status < 0) return status;
    status = Bc250DispWrite(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK,
                            OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK);
    if (status < 0) {
        // A failed posted write may still have reached the device.
        (void)Bc250DispWrite(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0);
        return status;
    }
    for (;;) {
        status = Bc250DispRead(Io, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &value);
        Result->LockStatus = status >= 0 ? value : 0;
        Result->LockStatusValid = status >= 0;
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
    ScalerAfter(Io, Result);
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
