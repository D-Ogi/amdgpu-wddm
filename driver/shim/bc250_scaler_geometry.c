// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: imported algorithm in generated/dc_scaler_geometry.inc remains
// Linux amdgpu DC, MIT, v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
#include "bc250_scaler_geometry.h"
#include <stdbool.h>
#include <limits.h>

// Platform adapter for the verbatim fixed-point functions. Only nonnegative,
// bounded operands reach them. Maximum destination width and source size are
// 16383, ratio is at most 8 and taps at most 8. No intermediate approaches 2^63.
// Assertions are active in the host test. Production depends on the checks in
// Bc250ScalerAxisPlan, not on a kernel assertion or a floating-point operation.
#if defined(BC250_SCALER_GEOMETRY_TEST)
#include <assert.h>
#define ASSERT(x) assert(x)
#else
#define ASSERT(x) ((void)sizeof(x))
#endif
#define FIXED31_32_BITS_PER_FRACTIONAL_PART 32
#define FRACTIONAL_PART_MASK ((1ULL << 32) - 1)
#define GET_INTEGER_PART(x) ((x) >> 32)
#define GET_FRACTIONAL_PART(x) (FRACTIONAL_PART_MASK & (x))
struct fixed31_32 { long long value; };
static const struct fixed31_32 dc_fixpt_half = { 0x80000000LL };
// Keep the upstream symbols private to this translation unit.
static struct fixed31_32 dc_fixpt_from_fraction(long long, long long);
static struct fixed31_32 dc_fixpt_mul(struct fixed31_32, struct fixed31_32);
static unsigned long long div64_u64_rem(unsigned long long n,
                                      unsigned long long d,
                                      unsigned long long* r)
{
    *r = n % d;
    return n / d;
}
#include "generated/dc_scaler_geometry.inc"

int Bc250ScalerComponentAxisPlan(const BC250_SCALER_AXIS_INPUT* Input,
                                uint32_t ComponentDivisor,
                                BC250_SCALER_AXIS_PLAN* Plan)
{
    BC250_SCALER_AXIS_INPUT in;
    struct fixed31_32 ratio, init;
    int64_t source, component_source, destination, begin, end, offset;
    int viewport_begin, viewport_size;
    if (!Plan) return BC250_SCALER_GEOMETRY_INVALID;
    // Copy before clearing, so an accidentally overlapping input/output is safe.
    if (Input) in = *Input;
    *Plan = (BC250_SCALER_AXIS_PLAN){0};
    if (!Input) return BC250_SCALER_GEOMETRY_INVALID;
    source = (int64_t)in.SourceEnd - in.SourceBegin;
    destination = (int64_t)in.DestinationEnd - in.DestinationBegin;
    if (!in.SurfaceSize || in.SurfaceSize > 16383 ||
        !in.ActiveSize || in.ActiveSize > 16383 ||
        in.SourceBegin < 0 || source <= 0 ||
        (uint32_t)in.SourceEnd > in.SurfaceSize ||
        destination <= 0 || in.ClipEnd < in.ClipBegin ||
        !in.Taps || in.Taps > 8 || in.Mirror > 1 ||
        (ComponentDivisor != 1 && ComponentDivisor != 2))
        return BC250_SCALER_GEOMETRY_INVALID;
    component_source = source / ComponentDivisor;
    // dc_resource.c requires src.x/y % vpc_div == 0. Keep this explicit until
    // odd source origins have a proven phase/cositing implementation. An odd
    // destination or clip coordinate is supported and must not be rounded.
    if ((uint32_t)in.SourceBegin % ComponentDivisor || component_source == 0)
        return BC250_SCALER_GEOMETRY_UNSUPPORTED;
    // The hardware geometry fields are 14 bits. A wider conceptual destination
    // requires a separate range analysis even if its visible fragment is small.
    if (destination > 16383 || source > 8 * destination)
        return BC250_SCALER_GEOMETRY_UNSUPPORTED;
    begin = in.DestinationBegin;
    if (begin < in.ClipBegin) begin = in.ClipBegin;
    if (begin < 0) begin = 0;
    end = in.DestinationEnd;
    if (end > in.ClipEnd) end = in.ClipEnd;
    if (end > in.ActiveSize) end = in.ActiveSize;
    if (begin >= end) return BC250_SCALER_GEOMETRY_EMPTY;
    offset = begin - in.DestinationBegin;
    // dc_resource.c calculate_scaling_ratios, with stream coordinates already
    // in active-output pixels (no second stream transform in this adapter).
    ratio = dc_fixpt_from_fraction(source, destination);
    // calculate_scaling_ratios divides the original 31.32 ratio for 4:2:0
    // before truncating to 19 bits. Deriving chroma from luma after its exact-8
    // adjustment, or using floor(source / 2), changes the intended ratio.
    ratio.value /= ComponentDivisor;
    ratio = dc_fixpt_truncate(ratio, 19);
    // dpp201_get_optimal_number_of_taps: exact 8 is not representable in 3.19.
    if (ratio.value == (8LL << 32)) --ratio.value;
    calculate_init_and_vp(in.Mirror != 0, (int)offset, (int)(end - begin),
                          (int)component_source, (int)in.Taps, ratio, &init,
                          &viewport_begin, &viewport_size);
    if (viewport_begin < 0 || viewport_size <= 0 ||
        (int64_t)viewport_begin + viewport_size > component_source ||
        ratio.value <= 0 || init.value < 0 || (init.value >> 32) > 15)
        return BC250_SCALER_GEOMETRY_UNSUPPORTED;
    Plan->ViewportBegin = (uint32_t)in.SourceBegin / ComponentDivisor + (uint32_t)viewport_begin;
    Plan->ViewportSize = (uint32_t)viewport_size;
    Plan->RecoutBegin = (uint32_t)begin;
    Plan->RecoutSize = (uint32_t)(end - begin);
    Plan->Ratio19 = (uint32_t)(ratio.value >> 13);
    Plan->Init19 = (uint32_t)(init.value >> 13);
    return BC250_SCALER_GEOMETRY_OK;
}

int Bc250ScalerAxisPlan(const BC250_SCALER_AXIS_INPUT* Input,
                       BC250_SCALER_AXIS_PLAN* Plan)
{
    return Bc250ScalerComponentAxisPlan(Input, 1, Plan);
}

int Bc250ScalerPlaneGeometry(const BC250_SCALER_PLANE_INPUT* Input,
                            BC250_SCALER_PLANE_PLAN* Plan)
{
    BC250_SCALER_PLANE_INPUT in;
    BC250_SCALER_PLANE_PLAN pending = {0};
    BC250_SCALER_AXIS_INPUT chroma_h, chroma_v;
    int status[4];
    unsigned i;
    if (!Plan) return BC250_SCALER_GEOMETRY_INVALID;
    if (Input) in = *Input;
    *Plan = (BC250_SCALER_PLANE_PLAN){0};
    if (!Input) return BC250_SCALER_GEOMETRY_INVALID;
    chroma_h = in.Horizontal;
    chroma_v = in.Vertical;
    chroma_h.Taps = in.ChromaTapsH;
    chroma_v.Taps = in.ChromaTapsV;
    status[0] = Bc250ScalerAxisPlan(&in.Horizontal, &pending.LumaH);
    status[1] = Bc250ScalerAxisPlan(&in.Vertical, &pending.LumaV);
    status[2] = Bc250ScalerComponentAxisPlan(&chroma_h, in.ChromaDivisor, &pending.ChromaH);
    status[3] = Bc250ScalerComponentAxisPlan(&chroma_v, in.ChromaDivisor, &pending.ChromaV);
    // Invalid inputs cannot become an accepted empty plane by hiding one axis.
    for (i = 0; i < 4; ++i)
        if (status[i] == BC250_SCALER_GEOMETRY_INVALID) return status[i];
    for (i = 0; i < 4; ++i)
        if (status[i] == BC250_SCALER_GEOMETRY_UNSUPPORTED) return status[i];
    for (i = 0; i < 4; ++i)
        if (status[i] == BC250_SCALER_GEOMETRY_EMPTY) return status[i];
    *Plan = pending;
    return BC250_SCALER_GEOMETRY_OK;
}
