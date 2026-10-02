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
#include "entry.h"
#include "replay.h"
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

using List = ID3D12GraphicsCommandList;

// The engine call of each value-only slot, made by one function per slot: the slot below and its direct entry (further
// down) record the same lambda type, so an entry replays alike whichever of the two wrote it.
auto draw_instanced_op(UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) noexcept {
    return [=](List* e) { e->DrawInstanced(vertices, instances, first_vertex, first_instance); };
}
auto draw_indexed_instanced_op(UINT indices, UINT instances, UINT first_index, INT base_vertex,
                               UINT first_instance) noexcept {
    return [=](List* e) { e->DrawIndexedInstanced(indices, instances, first_index, base_vertex, first_instance); };
}
auto ia_set_topology_op(D3D12DDI_PRIMITIVE_TOPOLOGY topology) noexcept {
    return [=](List* e) { e->IASetPrimitiveTopology(static_cast<D3D12_PRIMITIVE_TOPOLOGY>(topology)); };
}
auto rs_set_viewports_op(UINT count) noexcept {
    return [=](List* e, const D3D12_VIEWPORT* v) { e->RSSetViewports(count, v); };
}
auto rs_set_scissor_rects_op(UINT count) noexcept {
    return [=](List* e, const D3D12DDI_RECT* r) { e->RSSetScissorRects(count, r); };
}
auto om_set_blend_factor_op() noexcept {
    return [](List* e, const FLOAT* f) { e->OMSetBlendFactor(f); };
}
auto om_set_stencil_ref_op(UINT ref) noexcept {
    return [=](List* e) { e->OMSetStencilRef(ref); };
}
auto set_graphics_root_table_op(UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base) noexcept {
    const D3D12_GPU_DESCRIPTOR_HANDLE handle{base.ptr};
    return [=](List* e) { e->SetGraphicsRootDescriptorTable(index, handle); };
}
auto set_graphics_root_constant_op(UINT index, UINT data, UINT offset) noexcept {
    return [=](List* e) { e->SetGraphicsRoot32BitConstant(index, data, offset); };
}
auto set_graphics_root_constants_op(UINT index, UINT count, UINT offset) noexcept {
    return [=](List* e, const UINT* d) { e->SetGraphicsRoot32BitConstants(index, count, d, offset); };
}
auto set_graphics_root_cbv_op(UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) noexcept {
    return [=](List* e) { e->SetGraphicsRootConstantBufferView(index, va); };
}
auto set_graphics_root_srv_op(UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) noexcept {
    return [=](List* e) { e->SetGraphicsRootShaderResourceView(index, va); };
}
auto set_graphics_root_uav_op(UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) noexcept {
    return [=](List* e) { e->SetGraphicsRootUnorderedAccessView(index, va); };
}
auto ia_set_index_buffer_op() noexcept {
    return [](List* e, const D3D12_INDEX_BUFFER_VIEW* v) { e->IASetIndexBuffer(v); };
}
auto ia_set_vertex_buffers_op(UINT start, UINT count) noexcept {
    return [=](List* e, const D3D12_VERTEX_BUFFER_VIEW* v) { e->IASetVertexBuffers(start, count, v); };
}
// The arguments the slots refuse (reported as E_INVALIDARG); the direct entry leaves them to the slot.
bool viewport_count_ok(UINT count, const void* data) noexcept {
    return count <= D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE && (!count || data);
}
bool vertex_slots_ok(UINT start, UINT count) noexcept {
    return start < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT && count <= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT - start;
}

void APIENTRY draw_instanced(D3D12DDI_HCOMMANDLIST hlist, UINT vertices, UINT instances, UINT first_vertex,
                             UINT first_instance) {
    if (CommandListRecord* l = list_of(hlist, "DrawInstanced"))
        record(l, draw_instanced_op(vertices, instances, first_vertex, first_instance));
}

void APIENTRY draw_indexed_instanced(D3D12DDI_HCOMMANDLIST hlist, UINT indices, UINT instances, UINT first_index,
                                     INT base_vertex, UINT first_instance) {
    if (CommandListRecord* l = list_of(hlist, "DrawIndexedInstanced"))
        record(l, draw_indexed_instanced_op(indices, instances, first_index, base_vertex, first_instance));
}

void APIENTRY ia_set_topology(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_PRIMITIVE_TOPOLOGY topology) {
    if (CommandListRecord* l = list_of(hlist, "IaSetTopology")) record(l, ia_set_topology_op(topology));
}

void APIENTRY rs_set_viewports(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_VIEWPORT* viewports) {
    CommandListRecord* l = list_of(hlist, "RsSetViewports");
    if (!l) return;
    if (!viewport_count_ok(count, viewports)) return invalid(l);
    record(l, rs_set_viewports_op(count), in(reinterpret_cast<const D3D12_VIEWPORT*>(viewports), count));
}

void APIENTRY rs_set_scissor_rects(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_RECT* rects) {
    CommandListRecord* l = list_of(hlist, "RsSetScissorRects");
    if (!l) return;
    if (!viewport_count_ok(count, rects)) return invalid(l);
    record(l, rs_set_scissor_rects_op(count), in(rects, count));
}

// A null factor sets the default (1, 1, 1, 1); otherwise the engine reads four floats.
void APIENTRY om_set_blend_factor(D3D12DDI_HCOMMANDLIST hlist, const FLOAT factor[4]) {
    if (CommandListRecord* l = list_of(hlist, "OmSetBlendFactor"))
        record(l, om_set_blend_factor_op(), in(factor, factor ? 4 : 0));
}

void APIENTRY om_set_stencil_ref(D3D12DDI_HCOMMANDLIST hlist, UINT ref) {
    if (CommandListRecord* l = list_of(hlist, "OmSetStencilRef")) record(l, om_set_stencil_ref_op(ref));
}

void APIENTRY set_graphics_root_signature(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HROOTSIGNATURE h) {
    CommandListRecord* l = list_of(hlist, "SetGraphicsRootSignature");
    if (!l) return;
    auto* r = record_of<RootSignatureRecord>(h.pDrvPrivate, Tag::RootSignature, l->h.device);
    if (h.pDrvPrivate && !r) return invalid(l);
    ID3D12RootSignature* signature = r ? static_cast<ID3D12RootSignature*>(r->h.engine) : nullptr;
    l->root_signatures[1] = r ? h.pDrvPrivate : nullptr;
    record(l, [=](List* e) { e->SetGraphicsRootSignature(signature); });
}

void APIENTRY set_graphics_root_table(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootDescriptorTable"))
        record(l, set_graphics_root_table_op(index, base));
}

void APIENTRY set_graphics_root_constant(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT data, UINT offset) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRoot32BitConstant"))
        record(l, set_graphics_root_constant_op(index, data, offset));
}

void APIENTRY set_graphics_root_constants(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT count, const void* data,
                                          UINT offset) {
    CommandListRecord* l = list_of(hlist, "SetGraphicsRoot32BitConstants");
    if (!l) return;
    if (count && !data) return invalid(l);
    record(l, set_graphics_root_constants_op(index, count, offset), in(static_cast<const UINT*>(data), count));
}

void APIENTRY set_graphics_root_cbv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootConstantBufferView"))
        record(l, set_graphics_root_cbv_op(index, va));
}

void APIENTRY set_graphics_root_srv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootShaderResourceView"))
        record(l, set_graphics_root_srv_op(index, va));
}

void APIENTRY set_graphics_root_uav(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetGraphicsRootUnorderedAccessView"))
        record(l, set_graphics_root_uav_op(index, va));
}

void APIENTRY ia_set_index_buffer(D3D12DDI_HCOMMANDLIST hlist, const D3D12DDI_INDEX_BUFFER_VIEW* view) {
    if (CommandListRecord* l = list_of(hlist, "IASetIndexBuffer"))
        record(l, ia_set_index_buffer_op(), in(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(view), view ? 1 : 0));
}

void APIENTRY ia_set_vertex_buffers(D3D12DDI_HCOMMANDLIST hlist, UINT start, UINT count,
                                    const D3D12DDI_VERTEX_BUFFER_VIEW* views) {
    CommandListRecord* l = list_of(hlist, "IASetVertexBuffers");
    if (!l) return;
    if (!vertex_slots_ok(start, count)) return invalid(l);
    record(l, ia_set_vertex_buffers_op(start, count),
           in(reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(views), views ? count : 0));
}

void APIENTRY so_set_targets(D3D12DDI_HCOMMANDLIST hlist, UINT start, UINT count,
                             const D3D12DDI_STREAM_OUTPUT_BUFFER_VIEW* views) {
    CommandListRecord* l = list_of(hlist, "SOSetTargets");
    if (!l) return;
    if (start >= D3D12_SO_BUFFER_SLOT_COUNT || count > D3D12_SO_BUFFER_SLOT_COUNT - start) return invalid(l);
    // No views unbinds the slots. The engine reads the array without checking it, so it gets empty views.
    const D3D12_STREAM_OUTPUT_BUFFER_VIEW none[D3D12_SO_BUFFER_SLOT_COUNT]{};
    record(l, [=](List* e, const D3D12_STREAM_OUTPUT_BUFFER_VIEW* v) { e->SOSetTargets(start, count, v); },
           in(views ? reinterpret_cast<const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>(views) : none, count));
}

// The engine reads the descriptors behind the handles at this call: a deferred call gets copies of them (Snap).
void APIENTRY om_set_render_targets(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* rtvs,
                                    BOOL single_range, const D3D12DDI_CPU_DESCRIPTOR_HANDLE* dsv) {
    CommandListRecord* l = list_of(hlist, "OMSetRenderTargets");
    if (!l) return;
    if (count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT || (count && !rtvs)) return invalid(l);
    record(l,
           [=](List* e, const D3D12_CPU_DESCRIPTOR_HANDLE* r, const D3D12_CPU_DESCRIPTOR_HANDLE* d) {
               e->OMSetRenderTargets(count, r, single_range, d);
           },
           Snap{reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(rtvs), count, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                single_range},
           Snap{reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(dsv), dsv ? 1u : 0u, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
                FALSE});
}

// ---- Direct entries (engine-ddi.h, "Entry path") ----------------------------------------------------------------------
// Each is the graphics table's entry for its slot while the experiment installs it (install_direct_list): it counts
// and times the call (entry.h), and in an arm with the direct entry writes the slot's ring entry itself
// (record_direct); otherwise, or when record_direct declines, it calls the slot as the table held it before (the
// shell's entry, or in the harness the slot itself), which then validates, reports and records as without it.
D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 g_direct_fallback{};  // written once by install_direct_list, before any call
SRWLOCK g_direct_lock = SRWLOCK_INIT;
bool g_direct_filled = false;                               // under g_direct_lock

void APIENTRY direct_draw_instanced(D3D12DDI_HCOMMANDLIST hlist, UINT vertices, UINT instances, UINT first_vertex,
                                    UINT first_instance) {
    EntryTimer timer(EntryClass::DrawInstanced);
    if (entry_direct_arm() &&
        record_direct(timer, hlist, draw_instanced_op(vertices, instances, first_vertex, first_instance)))
        return;
    g_direct_fallback.pfnDrawInstanced(hlist, vertices, instances, first_vertex, first_instance);
}

void APIENTRY direct_draw_indexed_instanced(D3D12DDI_HCOMMANDLIST hlist, UINT indices, UINT instances,
                                            UINT first_index, INT base_vertex, UINT first_instance) {
    EntryTimer timer(EntryClass::DrawIndexedInstanced);
    if (entry_direct_arm() &&
        record_direct(timer, hlist,
                      draw_indexed_instanced_op(indices, instances, first_index, base_vertex, first_instance)))
        return;
    g_direct_fallback.pfnDrawIndexedInstanced(hlist, indices, instances, first_index, base_vertex, first_instance);
}

void APIENTRY direct_ia_set_topology(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_PRIMITIVE_TOPOLOGY topology) {
    EntryTimer timer(EntryClass::IaSetTopology);
    if (entry_direct_arm() && record_direct(timer, hlist, ia_set_topology_op(topology))) return;
    g_direct_fallback.pfnIaSetTopology(hlist, topology);
}

void APIENTRY direct_rs_set_viewports(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_VIEWPORT* viewports) {
    EntryTimer timer(EntryClass::RsSetViewports);
    if (entry_direct_arm() && viewport_count_ok(count, viewports) &&
        record_direct(timer, hlist, rs_set_viewports_op(count),
                      in(reinterpret_cast<const D3D12_VIEWPORT*>(viewports), count)))
        return;
    g_direct_fallback.pfnRsSetViewports(hlist, count, viewports);
}

void APIENTRY direct_rs_set_scissor_rects(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDI_RECT* rects) {
    EntryTimer timer(EntryClass::RsSetScissorRects);
    if (entry_direct_arm() && viewport_count_ok(count, rects) &&
        record_direct(timer, hlist, rs_set_scissor_rects_op(count), in(rects, count)))
        return;
    g_direct_fallback.pfnRsSetScissorRects(hlist, count, rects);
}

void APIENTRY direct_om_set_blend_factor(D3D12DDI_HCOMMANDLIST hlist, const FLOAT factor[4]) {
    EntryTimer timer(EntryClass::OmSetBlendFactor);
    if (entry_direct_arm() && record_direct(timer, hlist, om_set_blend_factor_op(), in(factor, factor ? 4 : 0)))
        return;
    g_direct_fallback.pfnOmSetBlendFactor(hlist, factor);
}

void APIENTRY direct_om_set_stencil_ref(D3D12DDI_HCOMMANDLIST hlist, UINT ref) {
    EntryTimer timer(EntryClass::OmSetStencilRef);
    if (entry_direct_arm() && record_direct(timer, hlist, om_set_stencil_ref_op(ref))) return;
    g_direct_fallback.pfnOmSetStencilRef(hlist, ref);
}

void APIENTRY direct_set_graphics_root_table(D3D12DDI_HCOMMANDLIST hlist, UINT index,
                                             D3D12DDI_GPU_DESCRIPTOR_HANDLE base) {
    EntryTimer timer(EntryClass::SetGraphicsRootDescriptorTable);
    if (entry_direct_arm() && record_direct(timer, hlist, set_graphics_root_table_op(index, base))) return;
    g_direct_fallback.pfnSetGraphicsRootDescriptorTable(hlist, index, base);
}

void APIENTRY direct_set_graphics_root_constant(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT data, UINT offset) {
    EntryTimer timer(EntryClass::SetGraphicsRoot32BitConstant);
    if (entry_direct_arm() && record_direct(timer, hlist, set_graphics_root_constant_op(index, data, offset))) return;
    g_direct_fallback.pfnSetGraphicsRoot32BitConstant(hlist, index, data, offset);
}

void APIENTRY direct_set_graphics_root_constants(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT count,
                                                 const void* data, UINT offset) {
    EntryTimer timer(EntryClass::SetGraphicsRoot32BitConstants);
    if (entry_direct_arm() && (!count || data) &&
        record_direct(timer, hlist, set_graphics_root_constants_op(index, count, offset),
                      in(static_cast<const UINT*>(data), count)))
        return;
    g_direct_fallback.pfnSetGraphicsRoot32BitConstants(hlist, index, count, data, offset);
}

void APIENTRY direct_set_graphics_root_cbv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    EntryTimer timer(EntryClass::SetGraphicsRootConstantBufferView);
    if (entry_direct_arm() && record_direct(timer, hlist, set_graphics_root_cbv_op(index, va))) return;
    g_direct_fallback.pfnSetGraphicsRootConstantBufferView(hlist, index, va);
}

void APIENTRY direct_set_graphics_root_srv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    EntryTimer timer(EntryClass::SetGraphicsRootShaderResourceView);
    if (entry_direct_arm() && record_direct(timer, hlist, set_graphics_root_srv_op(index, va))) return;
    g_direct_fallback.pfnSetGraphicsRootShaderResourceView(hlist, index, va);
}

void APIENTRY direct_set_graphics_root_uav(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    EntryTimer timer(EntryClass::SetGraphicsRootUnorderedAccessView);
    if (entry_direct_arm() && record_direct(timer, hlist, set_graphics_root_uav_op(index, va))) return;
    g_direct_fallback.pfnSetGraphicsRootUnorderedAccessView(hlist, index, va);
}

void APIENTRY direct_ia_set_index_buffer(D3D12DDI_HCOMMANDLIST hlist, const D3D12DDI_INDEX_BUFFER_VIEW* view) {
    EntryTimer timer(EntryClass::IASetIndexBuffer);
    if (entry_direct_arm() &&
        record_direct(timer, hlist, ia_set_index_buffer_op(),
                      in(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(view), view ? 1 : 0)))
        return;
    // C6387: a null view unbinds the index buffer; the slot takes it as the runtime passed it.
#pragma warning(suppress : 6387)
    g_direct_fallback.pfnIASetIndexBuffer(hlist, view);
}

void APIENTRY direct_ia_set_vertex_buffers(D3D12DDI_HCOMMANDLIST hlist, UINT start, UINT count,
                                           const D3D12DDI_VERTEX_BUFFER_VIEW* views) {
    EntryTimer timer(EntryClass::IASetVertexBuffers);
    if (entry_direct_arm() && vertex_slots_ok(start, count) &&
        record_direct(timer, hlist, ia_set_vertex_buffers_op(start, count),
                      in(reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(views), views ? count : 0)))
        return;
    g_direct_fallback.pfnIASetVertexBuffers(hlist, start, count, views);
}

// The direct entries over table's slots, X(member, function).
#define ENGINE_DDI_DIRECT_SLOTS(X)                                                                                     \
    X(pfnDrawInstanced, direct_draw_instanced)                                                                         \
    X(pfnDrawIndexedInstanced, direct_draw_indexed_instanced)                                                          \
    X(pfnIaSetTopology, direct_ia_set_topology)                                                                        \
    X(pfnRsSetViewports, direct_rs_set_viewports)                                                                      \
    X(pfnRsSetScissorRects, direct_rs_set_scissor_rects)                                                               \
    X(pfnOmSetBlendFactor, direct_om_set_blend_factor)                                                                 \
    X(pfnOmSetStencilRef, direct_om_set_stencil_ref)                                                                   \
    X(pfnSetGraphicsRootDescriptorTable, direct_set_graphics_root_table)                                               \
    X(pfnSetGraphicsRoot32BitConstant, direct_set_graphics_root_constant)                                              \
    X(pfnSetGraphicsRoot32BitConstants, direct_set_graphics_root_constants)                                            \
    X(pfnSetGraphicsRootConstantBufferView, direct_set_graphics_root_cbv)                                              \
    X(pfnSetGraphicsRootShaderResourceView, direct_set_graphics_root_srv)                                              \
    X(pfnSetGraphicsRootUnorderedAccessView, direct_set_graphics_root_uav)                                             \
    X(pfnIASetIndexBuffer, direct_ia_set_index_buffer)                                                                 \
    X(pfnIASetVertexBuffers, direct_ia_set_vertex_buffers)
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

bool install_direct_list(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t table_index) noexcept {
    if (!t || table_index != 1 || !entry_direct_wanted()) return false;
    bool ok = true;
    AcquireSRWLockExclusive(&g_direct_lock);
    // The first table names the fallback; a later one (another device) must hold the same slots, or keeps its own.
#define ENGINE_DDI_DIRECT_CHECK(member, function)                                                                      \
    if (!t->member || t->member == function || (g_direct_filled && g_direct_fallback.member != t->member)) ok = false;
    ENGINE_DDI_DIRECT_SLOTS(ENGINE_DDI_DIRECT_CHECK)
#undef ENGINE_DDI_DIRECT_CHECK
    if (ok && !g_direct_filled) {
#define ENGINE_DDI_DIRECT_KEEP(member, function) g_direct_fallback.member = t->member;
        ENGINE_DDI_DIRECT_SLOTS(ENGINE_DDI_DIRECT_KEEP)
#undef ENGINE_DDI_DIRECT_KEEP
        g_direct_filled = true;
    }
    ReleaseSRWLockExclusive(&g_direct_lock);
    if (!ok) {
        log_line("entry path: a graphics table with other slots than the first one's keeps them (no direct entries)");
        return false;
    }
#define ENGINE_DDI_DIRECT_SET(member, function) t->member = function;
    ENGINE_DDI_DIRECT_SLOTS(ENGINE_DDI_DIRECT_SET)
#undef ENGINE_DDI_DIRECT_SET
    return true;
}

void set_direct_entry(DeviceContext* context, bool on) noexcept {
    if (context) context->direct.store(on, std::memory_order_release);
}

} // namespace engine_ddi
