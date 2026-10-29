// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#include "bc250_scaler_plan.h"
#include <limits.h>
#define ASSERT(x) ((void)sizeof(x))
#define FIXED31_32_BITS_PER_FRACTIONAL_PART 32
struct fixed31_32 { long long value; };
static const struct fixed31_32 dc_fixpt_one = {1LL << 32};
static const struct fixed31_32 dc_fixpt_epsilon = {1};
struct scaling_taps { unsigned h_taps, v_taps, h_taps_c, v_taps_c; };
struct scaler_data {
    struct { struct fixed31_32 horz, vert, horz_c, vert_c; } ratios;
    struct scaling_taps taps;
};
#include "generated/dc_scaler_taps.inc"

static unsigned SelectTap(unsigned selected, unsigned ratio, unsigned kind)
{
    // dpp201 identity bypass, with always_scale off. Linear is an explicit
    // Windows filter choice; use two taps on every nonidentity component.
    if (ratio == (1u << 19)) return 1;
    return kind == BC250_SCALER_FILTER_LINEAR ? 2 : selected;
}

int Bc250ScalerPrepare(const BC250_SCALER_REQUEST* Input,
                       BC250_SCALER_PREPARED* Plan)
{
    BC250_SCALER_REQUEST in = {0};
    BC250_SCALER_PLANE_INPUT geometry;
    BC250_SCALER_BUFFER_INPUT buffer = {0};
    struct scaler_data data = {0};
    struct scaling_taps requested;
    unsigned limit;
    int status;
    if (!Plan) return BC250_SCALER_GEOMETRY_INVALID;
    if (Input) in = *Input;
    *Plan = (BC250_SCALER_PREPARED){0};
    if (!Input || in.FilterKind > BC250_SCALER_FILTER_LINEAR || in.Alpha > 1)
        return BC250_SCALER_GEOMETRY_INVALID;
    limit = in.FilterKind == BC250_SCALER_FILTER_LINEAR ? 2 : 8;
    requested = (struct scaling_taps){in.Geometry.Horizontal.Taps,
        in.Geometry.Vertical.Taps, in.Geometry.ChromaTapsH, in.Geometry.ChromaTapsV};
    if (requested.h_taps > limit || requested.v_taps > limit ||
        requested.h_taps_c > limit || requested.v_taps_c > limit)
        return BC250_SCALER_GEOMETRY_INVALID;
    geometry = in.Geometry;
    geometry.Horizontal.Taps = geometry.Vertical.Taps = 1;
    geometry.ChromaTapsH = geometry.ChromaTapsV = 1;
    // First pass validates rectangles and derives ratios. It is not published.
    status = Bc250ScalerPlaneGeometry(&geometry, &Plan->Geometry);
    if (status != BC250_SCALER_GEOMETRY_OK) goto refuse;
    data.ratios.horz.value = (long long)Plan->Geometry.LumaH.Ratio19 << 13;
    data.ratios.vert.value = (long long)Plan->Geometry.LumaV.Ratio19 << 13;
    data.ratios.horz_c.value = (long long)Plan->Geometry.ChromaH.Ratio19 << 13;
    data.ratios.vert_c.value = (long long)Plan->Geometry.ChromaV.Ratio19 << 13;
    amd_select_taps(&data, &requested);
    geometry.Horizontal.Taps = SelectTap(data.taps.h_taps, Plan->Geometry.LumaH.Ratio19, in.FilterKind);
    geometry.Vertical.Taps = SelectTap(data.taps.v_taps, Plan->Geometry.LumaV.Ratio19, in.FilterKind);
    geometry.ChromaTapsH = SelectTap(data.taps.h_taps_c, Plan->Geometry.ChromaH.Ratio19, in.FilterKind);
    geometry.ChromaTapsV = SelectTap(data.taps.v_taps_c, Plan->Geometry.ChromaV.Ratio19, in.FilterKind);
    // Tap changes alter viewport margins and phase. Recompute all components.
    status = Bc250ScalerPlaneGeometry(&geometry, &Plan->Geometry);
    if (status != BC250_SCALER_GEOMETRY_OK) goto refuse;
    buffer.ViewportWidth = Plan->Geometry.LumaH.ViewportSize;
    buffer.ChromaViewportWidth = Plan->Geometry.ChromaH.ViewportSize;
    buffer.RecoutWidth = Plan->Geometry.LumaH.RecoutSize;
    buffer.VerticalRatio19 = Plan->Geometry.LumaV.Ratio19;
    buffer.ChromaVerticalRatio19 = Plan->Geometry.ChromaV.Ratio19;
    buffer.VerticalTaps = geometry.Vertical.Taps;
    buffer.ChromaVerticalTaps = geometry.ChromaTapsV;
    buffer.Is420 = geometry.ChromaDivisor == 2;
    buffer.Alpha = in.Alpha;
    status = BC250_SCALER_GEOMETRY_UNSUPPORTED;
    if (!Bc250ScalerBufferPlan(&buffer, &Plan->Buffer)) goto refuse;
    if (!Bc250ScalerFilterPlan(in.FilterKind, geometry.Horizontal.Taps,
                               Plan->Geometry.LumaH.Ratio19, &Plan->LumaH) ||
        !Bc250ScalerFilterPlan(in.FilterKind, geometry.Vertical.Taps,
                               Plan->Geometry.LumaV.Ratio19, &Plan->LumaV) ||
        !Bc250ScalerFilterPlan(in.FilterKind, geometry.ChromaTapsH,
                               Plan->Geometry.ChromaH.Ratio19, &Plan->ChromaH) ||
        !Bc250ScalerFilterPlan(in.FilterKind, geometry.ChromaTapsV,
                               Plan->Geometry.ChromaV.Ratio19, &Plan->ChromaV)) goto refuse;
    return BC250_SCALER_GEOMETRY_OK;
refuse:
    *Plan = (BC250_SCALER_PREPARED){0};
    return status;
}
