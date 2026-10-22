// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: Linux amdgpu DC, MIT, v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
#pragma once
#include <stdint.h>

typedef struct BC250_SCALER_BUFFER_INPUT {
    uint32_t ViewportWidth, ChromaViewportWidth, RecoutWidth;
    uint32_t VerticalRatio19, ChromaVerticalRatio19;
    uint32_t VerticalTaps, ChromaVerticalTaps;
    uint32_t Is420, Alpha;
} BC250_SCALER_BUFFER_INPUT;

typedef struct BC250_SCALER_BUFFER_PLAN {
    uint32_t Config, LumaPartitions, ChromaPartitions;
} BC250_SCALER_BUFFER_PLAN;

// 1 = a fitting DCN20 buffer configuration exists, 0 = refused and output zero.
// Uses the first fitting configuration in AMD order 1, 2, [3 for 420], 0.
// This checks line-buffer capacity only, not clocks/bandwidth or filter quality.
int Bc250ScalerBufferPlan(const BC250_SCALER_BUFFER_INPUT* Input,
                         BC250_SCALER_BUFFER_PLAN* Plan);
