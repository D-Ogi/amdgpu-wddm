// BD-223: the admission and the scaler plan of multiplane-overlay plane 0 (dcn_plane.h). No register access.
#include "dcn_plane.h"
#include "plane_format.h"

const char* Bc250PlaneVerdictText(unsigned long Verdict)
{
    static const char* const text[BC250_PLANE_VERDICTS] = {
        "ok", "args", "count", "layer", "disabled", "source-mode", "format", "surface", "src-crop", "dst-bounds",
        "clip", "mirror", "rotation", "blend", "interlaced", "stereo", "color-space", "downscale", "upscale",
        "scaler", "dml", "clocks", "stretch-quality",
    };
    return Verdict < BC250_PLANE_VERDICTS ? text[Verdict] : "?";
}

static int RectOk(const BC250_PLANE_RECT* R)
{
    return R->Left >= 0 && R->Top >= 0 && R->Right > R->Left && R->Bottom > R->Top;
}

static unsigned long Width(const BC250_PLANE_RECT* R) { return (unsigned long)(R->Right - R->Left); }
static unsigned long Height(const BC250_PLANE_RECT* R) { return (unsigned long)(R->Bottom - R->Top); }

// Clip in the source mode's coordinates, as DstRect. Clip leaves Dst whole when it holds all of Dst; an equal
// rectangle is the case DWM sends for a plane it does not clip. Either reading of the DDI's comment ("relative to
// the DstRect") gives that answer for a Dst at the origin, which is the full-screen case of this stage.
static int ClipKeepsDst(const BC250_PLANE_RECT* Clip, const BC250_PLANE_RECT* Dst)
{
    return Clip->Left <= Dst->Left && Clip->Top <= Dst->Top && Clip->Right >= Dst->Right && Clip->Bottom >= Dst->Bottom;
}

static unsigned long Refuse(BC250_PLANE_PLAN* Plan, unsigned long Verdict)
{
    unsigned char* clear = (unsigned char*)Plan;
    unsigned long i;
    for (i = 0; i < sizeof(*Plan); i++) clear[i] = 0;
    return Verdict;
}

unsigned long Bc250PlanePrepare(const BC250_PLANE_REQUEST* R, BC250_PLANE_PLAN* Plan)
{
    unsigned long sw, sh, dw, dh;
    BC250_RECT recout;
    long s;

    if (!Plan) return BC250_PLANE_ARGS;
    Refuse(Plan, BC250_PLANE_OK);
    if (!R) return BC250_PLANE_ARGS;
    // The order: the request's shape first, then the plane's properties, then the geometry, then the scaler.
    if (R->PlaneCount != 1ul) return BC250_PLANE_COUNT;
    if (R->LayerIndex != 0ul) return BC250_PLANE_LAYER;
    if (!R->Enabled) return BC250_PLANE_DISABLED;
    if (!RectOk(&R->Src) || !RectOk(&R->Dst) || !RectOk(&R->Clip)) return BC250_PLANE_ARGS;
    // A source mode that stage A scales to the timing already would need the post-composition step of MPO3; the
    // plane's Dst is in source-mode pixels and DCN has no scaler after the blend (docs/design/mpo.md).
    if (R->SourceModeWidth != R->TargetWidth || R->SourceModeHeight != R->TargetHeight) return BC250_PLANE_SOURCE_MODE;
    if (R->Mirror) return BC250_PLANE_MIRROR;
    if (R->Rotated) return BC250_PLANE_ROTATION;
    if (R->AlphaBlend) return BC250_PLANE_BLEND;
    if (R->Interlaced) return BC250_PLANE_INTERLACED;
    if (R->Stereo) return BC250_PLANE_STEREO;
    if (R->NotSrgb) return BC250_PLANE_COLOR_SPACE;
    if (!R->StretchQualityKnown) return BC250_PLANE_STRETCH_QUALITY;
    // The 8-bit rows of the scan-out rule; RGB10A2 under the same condition as bc250_scanout_format_admitted (a
    // committed 10-bit source mode). FP16 has no plane encoding here, and DCN 2.0.1 cannot scale it (fp16 = 1).
    if (R->PlaneFormat != BC250_PLANE_FORMAT_ARGB8888 && R->PlaneFormat != BC250_PLANE_FORMAT_ABGR8888 &&
        !(R->PlaneFormat == BC250_PLANE_FORMAT_ABGR2101010 && R->TenBitSource))
        return BC250_PLANE_FORMAT;
    if (R->SurfaceWidth < BC250_PLANE_MIN_DIMENSION || R->SurfaceHeight < BC250_PLANE_MIN_DIMENSION ||
        R->SurfaceWidth > BC250_SCALE_MAX_DIMENSION || R->SurfaceHeight > BC250_SCALE_MAX_DIMENSION)
        return BC250_PLANE_SURFACE;
    // The viewport is the whole allocation: dcn_scale.c reads the source from its origin, and a crop needs the
    // viewport start and the init phases of a clipped source (calculate_init_and_vp with an offset), not here.
    if (R->Src.Left != 0 || R->Src.Top != 0 || Width(&R->Src) != R->SurfaceWidth || Height(&R->Src) != R->SurfaceHeight)
        return BC250_PLANE_SRC_CROP;
    if ((unsigned long)R->Dst.Right > R->SourceModeWidth || (unsigned long)R->Dst.Bottom > R->SourceModeHeight)
        return BC250_PLANE_DST_BOUNDS;
    if (!ClipKeepsDst(&R->Clip, &R->Dst)) return BC250_PLANE_CLIP;
    sw = R->SurfaceWidth; sh = R->SurfaceHeight; dw = Width(&R->Dst); dh = Height(&R->Dst);
    if (dw < sw || dh < sh) return BC250_PLANE_DOWNSCALE;
    if ((unsigned long long)dw * 1000ull > (unsigned long long)sw * BC250_PLANE_MAX_UPSCALE_MILLI ||
        (unsigned long long)dh * 1000ull > (unsigned long long)sh * BC250_PLANE_MAX_UPSCALE_MILLI)
        return BC250_PLANE_UPSCALE;
    recout.X = (unsigned long)R->Dst.Left;
    recout.Y = (unsigned long)R->Dst.Top;
    recout.Width = dw;
    recout.Height = dh;
    s = Bc250ScalerPlanRecout(sw, sh, R->TargetWidth, R->TargetHeight, &recout, &Plan->Scaler);
    if (s < 0) return Refuse(Plan, BC250_PLANE_SCALER);
    Plan->Identity = sw == R->TargetWidth && sh == R->TargetHeight && recout.X == 0 && recout.Y == 0 &&
                     dw == R->TargetWidth && dh == R->TargetHeight;
    Plan->DmlViewportWidth = sw;
    Plan->DmlViewportHeight = sh;
    Plan->DmlRecoutWidth = dw;
    Plan->DmlRecoutHeight = dh;
    Plan->DmlHTaps = Plan->Scaler.TapsH;
    Plan->DmlVTaps = Plan->Scaler.TapsV;
    return BC250_PLANE_OK;
}

unsigned long Bc250PlaneDmlVerdict(const BC250_PLANE_PLAN* Plan, int DmlStatus, unsigned long DmlValid,
                                   unsigned long DispClockKhz, unsigned long DppClockKhz,
                                   unsigned long BudgetDispClockKhz, unsigned long BudgetDppClockKhz)
{
    if (!Plan) return BC250_PLANE_ARGS;
    if (Plan->Identity) return BC250_PLANE_OK;
    if (DmlStatus != 0 || DmlValid != 1ul) return BC250_PLANE_DML;
    // The flip changes no clock (the DENTIST is the modeset's), so the scaled pipe must fit in the clocks that run.
    if (!BudgetDispClockKhz || !BudgetDppClockKhz || DispClockKhz > BudgetDispClockKhz || DppClockKhz > BudgetDppClockKhz)
        return BC250_PLANE_CLOCKS;
    return BC250_PLANE_OK;
}

int Bc250PlaneSameShape(const BC250_PLANE_PLAN* A, const BC250_PLANE_PLAN* B)
{
    const BC250_SCALER_PLAN* a = &A->Scaler;
    const BC250_SCALER_PLAN* b = &B->Scaler;
    return a->SourceWidth == b->SourceWidth && a->SourceHeight == b->SourceHeight &&
           a->TargetWidth == b->TargetWidth && a->TargetHeight == b->TargetHeight &&
           a->Recout.X == b->Recout.X && a->Recout.Y == b->Recout.Y &&
           a->Recout.Width == b->Recout.Width && a->Recout.Height == b->Recout.Height &&
           a->Viewport.X == b->Viewport.X && a->Viewport.Y == b->Viewport.Y &&
           a->Viewport.Width == b->Viewport.Width && a->Viewport.Height == b->Viewport.Height;
}
