// SPDX-License-Identifier: MIT
// engine-ddi: graphics state objects (D3-D14: element layouts, blend, depth-stencil and rasterizer states) and the
// graphics state and draws of a command list (L2, L3, L24-L28, L32, L34, L36, L38, L40, L42, L44-L48).
//
// The state objects keep the API form of their description; CreatePipelineState (pipelines.cpp) reads them. An
// element layout keeps the DDI elements, because their semantics come from the vertex program's rebuilt input
// signature, which only the pipeline sees. The list slots go to the engine one to one: the DDI and API layouts and
// enums they pass are equal (static_asserts below). They are installed in the graphics table only; the compute
// table keeps the fail-safe that rejects them.
#include "internal.h"
#include <cstring>

namespace engine_ddi {

static_assert(D3D12DDI_BLEND_ZERO == static_cast<int>(D3D12_BLEND_ZERO) &&
                  D3D12DDI_BLEND_SRC_ALPHASAT == static_cast<int>(D3D12_BLEND_SRC_ALPHA_SAT) &&
                  D3D12DDI_BLEND_BLEND_FACTOR == static_cast<int>(D3D12_BLEND_BLEND_FACTOR) &&
                  D3D12DDI_BLEND_INV_SRC1_ALPHA == static_cast<int>(D3D12_BLEND_INV_SRC1_ALPHA) &&
                  D3D12DDI_BLEND_ALPHA_FACTOR == static_cast<int>(D3D12_BLEND_ALPHA_FACTOR) &&
                  D3D12DDI_BLEND_INV_ALPHA_FACTOR == static_cast<int>(D3D12_BLEND_INV_ALPHA_FACTOR),
              "blend factors");
static_assert(D3D12DDI_BLEND_OP_ADD == static_cast<int>(D3D12_BLEND_OP_ADD) &&
                  D3D12DDI_BLEND_OP_MAX == static_cast<int>(D3D12_BLEND_OP_MAX), "blend operations");
static_assert(D3D12DDI_LOGIC_OP_CLEAR == static_cast<int>(D3D12_LOGIC_OP_CLEAR) &&
                  D3D12DDI_LOGIC_OP_OR_INVERTED == static_cast<int>(D3D12_LOGIC_OP_OR_INVERTED), "logic operations");
static_assert(D3D12DDI_COLOR_WRITE_ENABLE_ALL == static_cast<int>(D3D12_COLOR_WRITE_ENABLE_ALL), "write masks");
static_assert(D3D12DDI_DEPTH_WRITE_MASK_ALL == static_cast<int>(D3D12_DEPTH_WRITE_MASK_ALL), "depth write mask");
static_assert(D3D12DDI_COMPARISON_FUNC_NEVER == static_cast<int>(D3D12_COMPARISON_FUNC_NEVER) &&
                  D3D12DDI_COMPARISON_FUNC_ALWAYS == static_cast<int>(D3D12_COMPARISON_FUNC_ALWAYS), "comparisons");
static_assert(D3D12DDI_STENCIL_OP_KEEP == static_cast<int>(D3D12_STENCIL_OP_KEEP) &&
                  D3D12DDI_STENCIL_OP_DECR == static_cast<int>(D3D12_STENCIL_OP_DECR), "stencil operations");
static_assert(D3D12DDI_FILL_MODE_WIREFRAME == static_cast<int>(D3D12_FILL_MODE_WIREFRAME) &&
                  D3D12DDI_FILL_MODE_SOLID == static_cast<int>(D3D12_FILL_MODE_SOLID) &&
                  D3D12DDI_CULL_MODE_NONE == static_cast<int>(D3D12_CULL_MODE_NONE) &&
                  D3D12DDI_CULL_MODE_BACK == static_cast<int>(D3D12_CULL_MODE_BACK) &&
                  D3D12DDI_CONSERVATIVE_RASTERIZATION_MODE_ON == static_cast<int>(D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON),
              "rasterizer enums");
static_assert(D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST == static_cast<int>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST) &&
                  D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ == static_cast<int>(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) &&
                  D3D12DDI_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST ==
                      static_cast<int>(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST),
              "primitive topologies");
static_assert(sizeof(D3D12DDI_VIEWPORT) == sizeof(D3D12_VIEWPORT) && sizeof(D3D12DDI_RECT) == sizeof(D3D12_RECT),
              "viewports and scissor rectangles");
static_assert(sizeof(D3D12DDI_VERTEX_BUFFER_VIEW) == sizeof(D3D12_VERTEX_BUFFER_VIEW) &&
                  offsetof(D3D12DDI_VERTEX_BUFFER_VIEW, StrideInBytes) == offsetof(D3D12_VERTEX_BUFFER_VIEW, StrideInBytes) &&
                  sizeof(D3D12DDI_INDEX_BUFFER_VIEW) == sizeof(D3D12_INDEX_BUFFER_VIEW) &&
                  offsetof(D3D12DDI_INDEX_BUFFER_VIEW, Format) == offsetof(D3D12_INDEX_BUFFER_VIEW, Format) &&
                  sizeof(D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW) == sizeof(D3D12_STREAM_OUTPUT_BUFFER_VIEW) &&
                  offsetof(D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW, BufferFilledSizeLocation) ==
                      offsetof(D3D12_STREAM_OUTPUT_BUFFER_VIEW, BufferFilledSizeLocation),
              "buffer views");

namespace {
// ---- State objects ------------------------------------------------------------------------------------------------
// Every create constructs its record first, marked invalid, so that the destroy is valid whatever happens; it
// clears the mark only when the description was taken.

SIZE_T APIENTRY calc_element_layout(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010* args) {
    return sizeof(ElementLayoutRecord) +
           ((args && args->NumElements <= D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT)
                ? SIZE_T{args->NumElements} * sizeof(D3D12DDIARG_INPUT_ELEMENT_DESC)
                : 0);
}

void APIENTRY create_element_layout(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATEELEMENTLAYOUT_0010* args,
                                    D3D12DDI_HELEMENTLAYOUT h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!h.pDrvPrivate) {
        c->report(E_INVALIDARG);
        return;
    }
    auto* r = new (h.pDrvPrivate) ElementLayoutRecord{{Tag::ElementLayout, kRecordInvalid, nullptr, c}, 0, 0};
    if (!args || args->NumElements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT ||
        (args->NumElements && !args->pVertexElements)) {
        c->report(E_INVALIDARG);
        return;
    }
    if (args->LibraryReference.hLibrary.pDrvPrivate) {
        c->report(E_NOTIMPL);                           // pipeline libraries (P4)
        return;
    }
    if (args->NumElements)
        std::memcpy(r->elements(), args->pVertexElements, SIZE_T{args->NumElements} * sizeof(D3D12DDIARG_INPUT_ELEMENT_DESC));
    r->count = args->NumElements;
    r->h.flags = 0;
}

void APIENTRY destroy_element_layout(D3D12DDI_HDEVICE device, D3D12DDI_HELEMENTLAYOUT h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<ElementLayoutRecord>(h.pDrvPrivate, Tag::ElementLayout, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    poison(r->h);
}

SIZE_T APIENTRY calc_blend(D3D12DDI_HDEVICE, const D3D12DDI_BLEND_DESC_0010*) { return sizeof(BlendStateRecord); }

void APIENTRY create_blend(D3D12DDI_HDEVICE device, const D3D12DDI_BLEND_DESC_0010* in, D3D12DDI_HBLENDSTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!h.pDrvPrivate) {
        c->report(E_INVALIDARG);
        return;
    }
    auto* r = new (h.pDrvPrivate) BlendStateRecord{{Tag::StateBlend, kRecordInvalid, nullptr, c}, {}};
    if (!in) {
        c->report(E_INVALIDARG);
        return;
    }
    if (in->LibraryReference.hLibrary.pDrvPrivate) {
        c->report(E_NOTIMPL);
        return;
    }
    D3D12_BLEND_DESC& d = r->desc;
    d.AlphaToCoverageEnable = in->AlphaToCoverageEnable;
    d.IndependentBlendEnable = in->IndependentBlendEnable;
    for (UINT i = 0; i < D3D12DDI_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        const D3D12DDI_RENDER_TARGET_BLEND_DESC& s = in->RenderTarget[i];
        d.RenderTarget[i] = {s.BlendEnable,
                             s.LogicOpEnable,
                             static_cast<D3D12_BLEND>(s.SrcBlend),
                             static_cast<D3D12_BLEND>(s.DestBlend),
                             static_cast<D3D12_BLEND_OP>(s.BlendOp),
                             static_cast<D3D12_BLEND>(s.SrcBlendAlpha),
                             static_cast<D3D12_BLEND>(s.DestBlendAlpha),
                             static_cast<D3D12_BLEND_OP>(s.BlendOpAlpha),
                             static_cast<D3D12_LOGIC_OP>(s.LogicOp),
                             s.RenderTargetWriteMask};
    }
    r->h.flags = 0;
}

void APIENTRY destroy_blend(D3D12DDI_HDEVICE device, D3D12DDI_HBLENDSTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<BlendStateRecord>(h.pDrvPrivate, Tag::StateBlend, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    poison(r->h);
}

// A face whose FrontEnable/BackEnable is FALSE leaves the stencil untouched: KEEP everywhere, test ALWAYS. The D3D12
// API has no such flags; that the runtime sets both whenever StencilEnable is set is an INFERENCE, not measured.
D3D12_DEPTH_STENCILOP_DESC stencil_face(BOOL enabled, const D3D12DDI_DEPTH_STENCILOP_DESC& s) noexcept {
    if (!enabled) return {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    return {static_cast<D3D12_STENCIL_OP>(s.StencilFailOp), static_cast<D3D12_STENCIL_OP>(s.StencilDepthFailOp),
            static_cast<D3D12_STENCIL_OP>(s.StencilPassOp), static_cast<D3D12_COMPARISON_FUNC>(s.StencilFunc)};
}

SIZE_T APIENTRY calc_depth_stencil(D3D12DDI_HDEVICE, const D3D12DDI_DEPTH_STENCIL_DESC_0025*) {
    return sizeof(DepthStencilStateRecord);
}

void APIENTRY create_depth_stencil(D3D12DDI_HDEVICE device, const D3D12DDI_DEPTH_STENCIL_DESC_0025* in,
                                   D3D12DDI_HDEPTHSTENCILSTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!h.pDrvPrivate) {
        c->report(E_INVALIDARG);
        return;
    }
    auto* r = new (h.pDrvPrivate) DepthStencilStateRecord{{Tag::StateDepth, kRecordInvalid, nullptr, c}, {}, FALSE};
    if (!in) {
        c->report(E_INVALIDARG);
        return;
    }
    if (in->LibraryReference.hLibrary.pDrvPrivate) {
        c->report(E_NOTIMPL);
        return;
    }
    D3D12_DEPTH_STENCIL_DESC& d = r->desc;
    d.DepthEnable = in->DepthEnable;
    d.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(in->DepthWriteMask);
    d.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(in->DepthFunc);
    d.StencilEnable = in->StencilEnable;
    d.StencilReadMask = in->StencilReadMask;
    d.StencilWriteMask = in->StencilWriteMask;
    d.FrontFace = stencil_face(in->FrontEnable, in->FrontFace);
    d.BackFace = stencil_face(in->BackEnable, in->BackFace);
    r->depth_bounds = in->DepthBoundsTestEnable;
    r->h.flags = 0;
}

void APIENTRY destroy_depth_stencil(D3D12DDI_HDEVICE device, D3D12DDI_HDEPTHSTENCILSTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<DepthStencilStateRecord>(h.pDrvPrivate, Tag::StateDepth, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    poison(r->h);
}

SIZE_T APIENTRY calc_rasterizer(D3D12DDI_HDEVICE, const D3D12DDI_RASTERIZER_DESC_0010*) {
    return sizeof(RasterizerStateRecord);
}

// ScissorEnable has no API form: D3D12 always tests the scissor rectangles, and the runtime is taken to pass TRUE
// (INFERENCE, not measured). The flag is not read.
void APIENTRY create_rasterizer(D3D12DDI_HDEVICE device, const D3D12DDI_RASTERIZER_DESC_0010* in,
                                D3D12DDI_HRASTERIZERSTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!h.pDrvPrivate) {
        c->report(E_INVALIDARG);
        return;
    }
    auto* r = new (h.pDrvPrivate) RasterizerStateRecord{{Tag::StateRaster, kRecordInvalid, nullptr, c}, {}};
    if (!in) {
        c->report(E_INVALIDARG);
        return;
    }
    if (in->LibraryReference.hLibrary.pDrvPrivate) {
        c->report(E_NOTIMPL);
        return;
    }
    r->desc = {static_cast<D3D12_FILL_MODE>(in->FillMode),
               static_cast<D3D12_CULL_MODE>(in->CullMode),
               in->FrontCounterClockwise,
               in->DepthBias,
               in->DepthBiasClamp,
               in->SlopeScaledDepthBias,
               in->DepthClipEnable,
               in->MultisampleEnable,
               in->AntialiasedLineEnable,
               in->ForcedSampleCount,
               static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(in->ConservativeRasterizationMode)};
    r->h.flags = 0;
}

void APIENTRY destroy_rasterizer(D3D12DDI_HDEVICE device, D3D12DDI_HRASTERIZERSTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<RasterizerStateRecord>(h.pDrvPrivate, Tag::StateRaster, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    poison(r->h);
}

// ---- Command-list slots (graphics table) --------------------------------------------------------------------------
void invalid(CommandListRecord* l) noexcept { l->h.device->report_list(l->rt, E_INVALIDARG); }

void APIENTRY draw_instanced(D3D12DDI_HCOMMANDLIST hlist, UINT vertices, UINT instances, UINT first_vertex,
                             UINT first_instance) {
    if (CommandListRecord* l = list_of(hlist, "DrawInstanced"))
        l->list()->DrawInstanced(vertices, instances, first_vertex, first_instance);
}

void APIENTRY draw_indexed_instanced(D3D12DDI_HCOMMANDLIST hlist, UINT indices, UINT instances, UINT first_index,
                                     INT base_vertex, UINT first_instance) {
    if (CommandListRecord* l = list_of(hlist, "DrawIndexedInstanced"))
        l->list()->DrawIndexedInstanced(indices, instances, first_index, base_vertex, first_instance);
}

void APIENTRY ia_set_topology(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_PRIMITIVE_TOPOLOGY topology) {
    if (CommandListRecord* l = list_of(hlist, "IaSetTopology"))
        l->list()->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topology));
}

void APIENTRY rs_set_viewports(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_VIEWPORT* viewports) {
    CommandListRecord* l = list_of(hlist, "RsSetViewports");
    if (!l) return;
    if (count > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE || (count && !viewports)) return invalid(l);
    l->list()->RSSetViewports(count, reinterpret_cast<const D3D12_VIEWPORT*>(viewports));
}

void APIENTRY rs_set_scissor_rects(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_RECT* rects) {
    CommandListRecord* l = list_of(hlist, "RsSetScissorRects");
    if (!l) return;
    if (count > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE || (count && !rects)) return invalid(l);
    l->list()->RSSetScissorRects(count, rects);
}

void APIENTRY om_set_blend_factor(D3D12DDI_HCOMMANDLIST hlist, const FLOAT factor[4]) {
    if (CommandListRecord* l = list_of(hlist, "OmSetBlendFactor")) l->list()->OMSetBlendFactor(factor);
}

void APIENTRY om_set_stencil_ref(D3D12DDI_HCOMMANDLIST hlist, UINT ref) {
    if (CommandListRecord* l = list_of(hlist, "OmSetStencilRef")) l->list()->OMSetStencilRef(ref);
}

void APIENTRY set_graphics_root_signature(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HROOTSIGNATURE h) {
    CommandListRecord* l = list_of(hlist, "SetGraphicsRootSignature");
    if (!l) return;
    auto* r = record_of<RootSignatureRecord>(h.pDrvPrivate, Tag::RootSignature, l->h.device);
    if (h.pDrvPrivate && !r) return invalid(l);
    l->list()->SetGraphicsRootSignature(r ? static_cast<ID3D12RootSignature*>(r->h.engine) : nullptr);
}

void APIENTRY set_graphics_root_table(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootDescriptorTable"))
        l->list()->SetGraphicsRootDescriptorTable(index, D3D12_GPU_DESCRIPTOR_HANDLE{base.ptr});
}

void APIENTRY set_graphics_root_constant(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT data, UINT offset) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRoot32BitConstant"))
        l->list()->SetGraphicsRoot32BitConstant(index, data, offset);
}

void APIENTRY set_graphics_root_constants(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT count, const void* data,
                                          UINT offset) {
    CommandListRecord* l = list_of(hlist, "SetGraphicsRoot32BitConstants");
    if (!l) return;
    if (count && !data) return invalid(l);
    l->list()->SetGraphicsRoot32BitConstants(index, count, data, offset);
}

void APIENTRY set_graphics_root_cbv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootConstantBufferView"))
        l->list()->SetGraphicsRootConstantBufferView(index, va);
}

void APIENTRY set_graphics_root_srv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootShaderResourceView"))
        l->list()->SetGraphicsRootShaderResourceView(index, va);
}

void APIENTRY set_graphics_root_uav(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootUnorderedAccessView"))
        l->list()->SetGraphicsRootUnorderedAccessView(index, va);
}

void APIENTRY ia_set_index_buffer(D3D12DDI_HCOMMANDLIST hlist, const D3D12DDI_INDEX_BUFFER_VIEW* view) {
    if (CommandListRecord* l = list_of(hlist, "IASetIndexBuffer"))
        l->list()->IASetIndexBuffer(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(view));
}

void APIENTRY ia_set_vertex_buffers(D3D12DDI_HCOMMANDLIST hlist, UINT start, UINT count,
                                    const D3D12DDI_VERTEX_BUFFER_VIEW* views) {
    CommandListRecord* l = list_of(hlist, "IASetVertexBuffers");
    if (!l) return;
    if (start >= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT || count > D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - start)
        return invalid(l);
    l->list()->IASetVertexBuffers(start, count, reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(views));
}

void APIENTRY so_set_targets(D3D12DDI_HCOMMANDLIST hlist, UINT start, UINT count,
                             const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW* views) {
    CommandListRecord* l = list_of(hlist, "SOSetTargets");
    if (!l) return;
    if (start >= D3D12_SO_BUFFER_SLOT_COUNT || count > D3D12_SO_BUFFER_SLOT_COUNT - start) return invalid(l);
    l->list()->SOSetTargets(start, count, reinterpret_cast<const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>(views));
}

void APIENTRY om_set_render_targets(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* rtvs,
                                    BOOL single_range, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dsv) {
    CommandListRecord* l = list_of(hlist, "OMSetRenderTargets");
    if (!l) return;
    if (count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT || (count && !rtvs)) return invalid(l);
    l->list()->OMSetRenderTargets(count, reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(rtvs), single_range,
                                  reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(dsv));
}
} // namespace

void fill_core_graphics(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCalcPrivateElementLayoutSize = calc_element_layout;
    t->pfnCreateElementLayout = create_element_layout;
    t->pfnDestroyElementLayout = destroy_element_layout;
    t->pfnCalcPrivateBlendStateSize = calc_blend;
    t->pfnCreateBlendState = create_blend;
    t->pfnDestroyBlendState = destroy_blend;
    t->pfnCalcPrivateDepthStencilStateSize = calc_depth_stencil;
    t->pfnCreateDepthStencilState = create_depth_stencil;
    t->pfnDestroyDepthStencilState = destroy_depth_stencil;
    t->pfnCalcPrivateRasterizerStateSize = calc_rasterizer;
    t->pfnCreateRasterizerState = create_rasterizer;
    t->pfnDestroyRasterizerState = destroy_rasterizer;
}

void fill_list_graphics(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t table_index) noexcept {
    if (table_index != 1) return;                       // the compute table keeps its rejections
    t->pfnDrawInstanced = draw_instanced;
    t->pfnDrawIndexedInstanced = draw_indexed_instanced;
    t->pfnIaSetTopology = ia_set_topology;
    t->pfnRsSetViewports = rs_set_viewports;
    t->pfnRsSetScissorRects = rs_set_scissor_rects;
    t->pfnOmSetBlendFactor = om_set_blend_factor;
    t->pfnOmSetStencilRef = om_set_stencil_ref;
    t->pfnSetGraphicsRootSignature = set_graphics_root_signature;
    t->pfnSetGraphicsRootDescriptorTable = set_graphics_root_table;
    t->pfnSetGraphicsRoot32BitConstant = set_graphics_root_constant;
    t->pfnSetGraphicsRoot32BitConstants = set_graphics_root_constants;
    t->pfnSetGraphicsRootConstantBufferView = set_graphics_root_cbv;
    t->pfnSetGraphicsRootShaderResourceView = set_graphics_root_srv;
    t->pfnSetGraphicsRootUnorderedAccessView = set_graphics_root_uav;
    t->pfnIASetIndexBuffer = ia_set_index_buffer;
    t->pfnIASetVertexBuffers = ia_set_vertex_buffers;
    t->pfnSOSetTargets = so_set_targets;
    t->pfnOMSetRenderTargets = om_set_render_targets;
}

} // namespace engine_ddi
