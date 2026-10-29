// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "bc250_dc_types.h"

enum BC250_SCALER_FILTER_KIND {
    BC250_SCALER_FILTER_AMD = 0,
    BC250_SCALER_FILTER_LINEAR = 1
};

typedef struct BC250_SCALER_FILTER_PLAN {
    uint32_t Taps, PairsPerPhase, CoefficientCount;
    // 33 stored phases, pairs in even/odd order, unused odd tap zero.
    // AMD S1.12 coefficient encoding, bits 1:0 zero. No register addresses.
    uint16_t Coefficients[33 * 8];
} BC250_SCALER_FILTER_PLAN;

// Pure preparation for a later coefficient-bank transaction. Ratio is 3.19,
// nonzero and below 8. One tap is identity only, with no coefficient upload.
// Linear requests require two taps, AMD requests allow 2..8. AMD's two-tap
// table is not linear. No hardware two-tap mode is selected by this helper.
// Horizontal chroma tap restrictions, geometry, bandwidth and bank lifetime
// remain caller responsibilities. A valid plan is not admission for scanout.
// Refusal clears the entire plan. NULL output is refused.
int Bc250ScalerFilterPlan(uint32_t Kind, uint32_t Taps, uint32_t Ratio19,
                         BC250_SCALER_FILTER_PLAN* Plan);

// AMD's own table for Taps 2..8 and a 3.19 Ratio, nonzero and below 8
// (dce_scl_filters.c get_filter_<n>tap_64p): 33 phases of Taps coefficients,
// phase-major, in the encoding the coefficient RAM takes. NULL otherwise.
// A pointer into constant data: no copy, safe at any IRQL.
const uint16_t* Bc250ScalerFilterTable(uint32_t Taps, uint32_t Ratio19);
