// Adapter: PolyForm Noncommercial 1.0.0, see LICENSE.md and NOTICE.
// PROVENANCE: imported functions in generated/dc_scaler_buffer.inc remain
// Linux amdgpu DC, MIT, v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
#include "bc250_scaler_buffer.h"
#include <stdbool.h>

// Local views contain only members used by the extracted arithmetic functions.
// They are not Linux ABI objects and are never passed to other imported code.
struct scaler_data {
    struct { int width; } viewport, viewport_c, recout;
    struct { bool alpha_en; } lb_params;
};
#include "generated/dc_scaler_buffer.inc"

int Bc250ScalerBufferPlan(const BC250_SCALER_BUFFER_INPUT* Input,
                         BC250_SCALER_BUFFER_PLAN* Plan)
{
    BC250_SCALER_BUFFER_INPUT in = {0};
    struct scaler_data data = {0};
    // dpp1_dscl_find_lb_memory_config order, with the debug override disabled.
    const enum lb_memory_config order[] = {
        LB_MEMORY_CONFIG_1, LB_MEMORY_CONFIG_2, LB_MEMORY_CONFIG_3, LB_MEMORY_CONFIG_0
    };
    int y, c, ceil_y, ceil_c;
    unsigned i;
    if (!Plan) return 0;
    if (Input) in = *Input;
    *Plan = (BC250_SCALER_BUFFER_PLAN){0};
    if (!Input || !in.ViewportWidth || in.ViewportWidth > 16383 ||
        !in.ChromaViewportWidth || in.ChromaViewportWidth > 16383 ||
        !in.RecoutWidth || in.RecoutWidth > 16383 ||
        !in.VerticalRatio19 || in.VerticalRatio19 >= (8u << 19) ||
        !in.ChromaVerticalRatio19 || in.ChromaVerticalRatio19 >= (8u << 19) ||
        !in.VerticalTaps || in.VerticalTaps > 8 ||
        !in.ChromaVerticalTaps || in.ChromaVerticalTaps > 8 ||
        in.Is420 > 1 || in.Alpha > 1)
        return 0;
    data.viewport.width = (int)in.ViewportWidth;
    data.viewport_c.width = (int)in.ChromaViewportWidth;
    data.recout.width = (int)in.RecoutWidth;
    data.lb_params.alpha_en = in.Alpha != 0;
    ceil_y = (int)((in.VerticalRatio19 + (1u << 19) - 1) >> 19);
    ceil_c = (int)((in.ChromaVerticalRatio19 + (1u << 19) - 1) >> 19);
    for (i = 0; i < sizeof(order)/sizeof(order[0]); ++i) {
        if (order[i] == LB_MEMORY_CONFIG_3 && !in.Is420) continue;
        dscl2_calc_lb_num_partitions(&data, order[i], &y, &c);
        if (dpp1_dscl_is_lb_conf_valid(ceil_y, y, (int)in.VerticalTaps) &&
            dpp1_dscl_is_lb_conf_valid(ceil_c, c, (int)in.ChromaVerticalTaps)) {
            Plan->Config = (uint32_t)order[i];
            Plan->LumaPartitions = (uint32_t)y;
            Plan->ChromaPartitions = (uint32_t)c;
            return 1;
        }
    }
    // Intentional deviation: AMD asserts its final config-0 choice. Windows
    // admission receives untrusted geometry, so refuse if even config 0 fails.
    return 0;
}
