// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
#pragma once
#include "bc250_scaler_geometry.h"
#include "bc250_scaler_buffer.h"
#include "bc250_scaler_filter.h"

typedef struct BC250_SCALER_REQUEST {
    // Tap fields: zero = AMD defaults; 1..8 = requested taps (preference).
    // Identity axes use one tap. AMD rounds odd horizontal chroma taps down.
    // Linear mode accepts zero/one/two only and selects two on scaled axes.
    BC250_SCALER_PLANE_INPUT Geometry;
    uint32_t FilterKind, Alpha;
} BC250_SCALER_REQUEST;

typedef struct BC250_SCALER_PREPARED {
    BC250_SCALER_PLANE_PLAN Geometry;
    BC250_SCALER_BUFFER_PLAN Buffer;
    BC250_SCALER_FILTER_PLAN LumaH, LumaV, ChromaH, ChromaV;
} BC250_SCALER_PREPARED;

// CPU preparation only. No format admission, DML, colour conversion, MMIO,
// coefficient-bank ownership or scanout-capability claim. Run outside the
// interrupt lock. Caller owns output storage (about 2.3 KB, avoid kernel stack).
// Output must be private preparation storage, never the live scanout plan.
// Returns BC250_SCALER_GEOMETRY_*; any refusal/empty clears the entire output.
// Geometry uses FINAL selected taps, never a provisional tap count. Capacity
// failure does not silently degrade the requested filter.
int Bc250ScalerPrepare(const BC250_SCALER_REQUEST* Input,
                       BC250_SCALER_PREPARED* Plan);
