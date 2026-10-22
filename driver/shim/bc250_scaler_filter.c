// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: generated/dc_scaler_filters.inc is Linux amdgpu DC, MIT,
// v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
#include "bc250_scaler_filter.h"
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>
#define ASSERT(x) ((void)sizeof(x))
#define FIXED31_32_BITS_PER_FRACTIONAL_PART 32
struct fixed31_32 { long long value; };
static const struct fixed31_32 dc_fixpt_one = {1LL << 32};
static unsigned long long div64_u64_rem(unsigned long long n,
                                      unsigned long long d,
                                      unsigned long long* r)
{
    *r = n % d;
    return n / d;
}
#include "generated/dc_scaler_filters.inc"

int Bc250ScalerFilterPlan(uint32_t Kind, uint32_t Taps, uint32_t Ratio19,
                         BC250_SCALER_FILTER_PLAN* Plan)
{
    struct fixed31_32 ratio;
    const uint16_t* coefficients = NULL;
    uint32_t phase, tap, stride;
    if (!Plan) return 0;
    *Plan = (BC250_SCALER_FILTER_PLAN){0};
    if (Kind > BC250_SCALER_FILTER_LINEAR || Taps < 1 || Taps > 8 ||
        !Ratio19 || Ratio19 >= (8u << 19)) return 0;
    if (Taps == 1) {
        if (Ratio19 != (1u << 19)) return 0;
        Plan->Taps = 1;
        return 1;
    }
    if (Kind == BC250_SCALER_FILTER_LINEAR && Taps != 2) return 0;
    ratio.value = (long long)Ratio19 << 13;
    if (Kind == BC250_SCALER_FILTER_AMD) {
        switch (Taps) {
        case 2: coefficients = get_filter_2tap_64p(); break;
        case 3: coefficients = get_filter_3tap_64p(ratio); break;
        case 4: coefficients = get_filter_4tap_64p(ratio); break;
        case 5: coefficients = get_filter_5tap_64p(ratio); break;
        case 6: coefficients = get_filter_6tap_64p(ratio); break;
        case 7: coefficients = get_filter_7tap_64p(ratio); break;
        case 8: coefficients = get_filter_8tap_64p(ratio); break;
        }
    }
    stride = ((Taps + 1) / 2) * 2;
    Plan->Taps = Taps;
    Plan->PairsPerPhase = stride / 2;
    Plan->CoefficientCount = 33 * stride;
    for (phase = 0; phase < 33; ++phase) {
        if (Kind == BC250_SCALER_FILTER_LINEAR) {
            // Exact separable linear interpolation: t = phase/64, S1.12.
            // Unlike AMD's ModifiedLanczos table, weights are (1-t, t).
            Plan->Coefficients[phase * stride] = (uint16_t)(4096 - phase * 64);
            Plan->Coefficients[phase * stride + 1] = (uint16_t)(phase * 64);
        } else {
            for (tap = 0; tap < Taps; ++tap)
                Plan->Coefficients[phase * stride + tap] = coefficients[phase * Taps + tap];
        }
    }
    return 1;
}
