// SPDX-License-Identifier: MIT
#include "ddi-table.h"
#include "ddi-resource-status.h"
#include "ddi-format.h"
#include "ddi-draw.h"
#include "ddi-input-layout.h"
#include "ddi-raster.h"
#include "ddi-shader.h"
#include "ddi-sampler.h"
#include "ddi-fixed-state.h"
#include "ddi-blend.h"
#include "ddi-resource.h"
#include "ddi-buffer-binding.h"
#include "ddi-transfer.h"
#include "ddi-map.h"
#include "ddi-rtv.h"
#include "ddi-dsv.h"
#include "ddi-uav.h"
#include "ddi-output.h"
#include "ddi-srv.h"
#include "ddi-flush.h"
namespace bc250::umd {
D3D11_1DDI_DEVICEFUNCS make_render_device_table() {
    D3D11_1DDI_DEVICEFUNCS table{};
    install_draw_ddi(table);
    install_input_layout_ddi(table);
    install_raster_ddi(table);
    install_shader_ddi(table);
    install_sampler_ddi(table);
    install_fixed_state_ddi(table);
    install_blend_ddi(table);
    install_resource_ddi(table);
    install_buffer_binding_ddi(table);
    install_transfer_ddi(table);
    install_map_ddi(table);
    install_rtv_ddi(table);
    install_dsv_ddi(table);
    install_uav_ddi(table);
    install_output_ddi(table);
    install_srv_ddi(table);
    install_flush_ddi(table);
    install_format_ddi(table);
    install_resource_status_ddi(table);
    return table;
}
}
