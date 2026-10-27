// SPDX-License-Identifier: MIT
// Generic programs are named engine PSOs. The identifiers are registry keys, never caller-provided pointers.
#include "shader-container/shader-container.h"
#include "shader-container/dxil-metadata.h"
#include "generic-programs.h"
#include "partial-programs.h"
#include "partial-linkage-validation.h"
#include "partial-library.h"
#include "partial-specialization.h"
#include "program-import.h"
#if defined(D3D12DDI_BUILD_VERSION_0119)
#include "ddi-modern-core.h"
#include "ddi-modern-list.h"
#include "pso-async.h"
#include "replay.h"
#include "work-graphs.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <array>

namespace engine_ddi {
namespace gp {
namespace sc = shader_container;
constexpr UINT bound = 65536;
struct State {
    D3D12DDI_STATE_SUBOBJECT_TYPE type{};
    const void* identity = nullptr;             // identity only for resolved root associations; never dereferenced
    ID3D12RootSignature* root = nullptr;
    D3D12_BLEND_DESC blend{};
    D3D12_RASTERIZER_DESC2 raster{};
    D3D12_DEPTH_STENCIL_DESC2 depth{};
    UINT value = 0;
    DXGI_SAMPLE_DESC samples{1, 0};
    D3D12_RT_FORMAT_ARRAY formats{};
    std::vector<D3D12DDIARG_INPUT_ELEMENT_DESC> input;
    std::vector<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY> so;
    std::vector<UINT> strides;
    std::vector<D3D12_VIEW_INSTANCE_LOCATION> views;
    partial::Linkage linkage;
    partial::PrerasterFields preraster_fields{};
    partial::PixelFields pixel_fields{};
    ~State() { if (root) root->Release(); }
};
struct Shader { sc::Container container; sc::dxil::Signatures metadata; };
struct Export {
    std::shared_ptr<Shader> shader;
    std::shared_ptr<State> root;
    // Runtime summaries can retain associations from the collection that owns this export.
    // These are validated identities, never pointers that we dereference after the create call.
    std::set<const D3D12DDI_STATE_SUBOBJECT_0054*> configs;
};
struct Program;
std::mutex registry_lock;
std::map<UINT64, std::weak_ptr<Program>> registry;
std::atomic<UINT64> next_id{1};
struct Program {
    DeviceContext* device = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    std::shared_ptr<BackgroundPipeline<ID3D12PipelineState>> background;
    partial::Library library;
    UINT64 id = 0;
    ~Program() {
        // Drain/cancel before touching registry or releasing device-dependent owners.
        background.reset();
        if (id) { std::lock_guard lock(registry_lock); registry.erase(id); }
        if (pipeline) pipeline->Release();
    }
};
struct PartialProgram {
    partial::OwnedDescription description;
    // Keep resolved state and shader metadata alive after the defining collection
    // is destroyed. Description indices alone cannot refer to another collection.
    std::vector<std::shared_ptr<State>> states;
    std::map<std::wstring, Export> exports;
    std::shared_ptr<Program> compiled;
};
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (!n) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
bool valid_count(UINT count, const void* p) { return count <= bound && (!count || p); }
bool name_ok(LPCWSTR name) { return name && *name && wcsnlen(name, bound) < bound; }
bool uses_dual_source(const D3D12_BLEND_DESC& blend) {
    const auto dual = [](D3D12_BLEND factor) {
        return factor == D3D12_BLEND_SRC1_COLOR || factor == D3D12_BLEND_INV_SRC1_COLOR ||
            factor == D3D12_BLEND_SRC1_ALPHA || factor == D3D12_BLEND_INV_SRC1_ALPHA;
    };
    const auto& rt = blend.RenderTarget[0];
    return rt.BlendEnable && (dual(rt.SrcBlend) || dual(rt.DestBlend) || dual(rt.SrcBlendAlpha) || dual(rt.DestBlendAlpha));
}
D3D12_BLEND_DESC default_blend() {
    D3D12_BLEND_DESC d{};
    for (auto& r : d.RenderTarget) {
        r.SrcBlend = r.SrcBlendAlpha = D3D12_BLEND_ONE; r.DestBlend = r.DestBlendAlpha = D3D12_BLEND_ZERO;
        r.BlendOp = r.BlendOpAlpha = D3D12_BLEND_OP_ADD; r.LogicOp = D3D12_LOGIC_OP_NOOP;
        r.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return d;
}
D3D12_RASTERIZER_DESC2 default_raster() {
    D3D12_RASTERIZER_DESC2 d{}; d.FillMode = D3D12_FILL_MODE_SOLID; d.CullMode = D3D12_CULL_MODE_BACK;
    d.DepthClipEnable = TRUE; return d;
}
D3D12_DEPTH_STENCIL_DESC2 default_depth() {
    D3D12_DEPTH_STENCIL_DESC2 d{}; d.DepthEnable = TRUE; d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    d.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                   D3D12_COMPARISON_FUNC_ALWAYS, 0xff, 0xff}; d.BackFace = d.FrontFace; return d;
}
HRESULT snapshot(DeviceContext* c, const D3D12DDI_STATE_SUBOBJECT_0054& in, std::shared_ptr<State>& out) {
    if (!in.pDesc) return E_INVALIDARG;
    auto s = std::make_shared<State>(); s->type = in.Type; s->identity = &in;
    const UINT kind = static_cast<UINT>(in.Type);
    if (kind >= 38 && kind <= 41) {
        if (c->ddi_version < 122) return E_INVALIDARG;
        HRESULT hr = S_OK;
        switch (kind) {
        case 38: hr = s->linkage.capture(*static_cast<const partial::OutputSignature*>(in.pDesc)); break;
        case 39: hr = s->linkage.capture(*static_cast<const partial::PrerasterSignature*>(in.pDesc)); break;
        case 40: s->preraster_fields = *static_cast<const partial::PrerasterFields*>(in.pDesc); break;
        case 41: s->pixel_fields = *static_cast<const partial::PixelFields*>(in.pDesc); break;
        }
        if (FAILED(hr)) return hr;
        out = std::move(s);
        return S_OK;
    }
    switch (in.Type) {
    case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE: {
        const auto& a = *static_cast<const D3D12DDI_GLOBAL_ROOT_SIGNATURE_0054*>(in.pDesc);
        auto* r = record_of<RootSignatureRecord>(a.hGlobalRootSignature.pDrvPrivate, Tag::RootSignature, c);
        if (!r || !r->h.engine || r->h.flags & kRecordInvalid) return E_INVALIDARG;
        s->root = static_cast<ID3D12RootSignature*>(r->h.engine); s->root->AddRef(); break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_GLOBAL_SERIALIZED_ROOT_SIGNATURE: {
        const auto& a = *static_cast<const D3D12DDI_GLOBAL_SERIALIZED_ROOT_SIGNATURE_0114*>(in.pDesc);
        std::vector<uint8_t> bytes;
        HRESULT hr = serialize_root_signature_modern(&a.RootSignature, bytes);
        if (SUCCEEDED(hr)) hr = c->device->CreateRootSignature(0, bytes.data(), bytes.size(),
            __uuidof(ID3D12RootSignature), reinterpret_cast<void**>(&s->root));
        if (FAILED(hr)) return hr; break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND: {
        const auto& a = *static_cast<const D3D12DDI_BLEND_DESC*>(in.pDesc);
        s->blend.AlphaToCoverageEnable = a.AlphaToCoverageEnable; s->blend.IndependentBlendEnable = a.IndependentBlendEnable;
        for (UINT i = 0; i != 8; ++i) {
            const auto& x = a.RenderTarget[i]; auto& y = s->blend.RenderTarget[i];
            y.BlendEnable = x.BlendEnable; y.LogicOpEnable = x.LogicOpEnable;
            y.SrcBlend = static_cast<D3D12_BLEND>(x.SrcBlend); y.DestBlend = static_cast<D3D12_BLEND>(x.DestBlend);
            y.BlendOp = static_cast<D3D12_BLEND_OP>(x.BlendOp);
            y.SrcBlendAlpha = static_cast<D3D12_BLEND>(x.SrcBlendAlpha); y.DestBlendAlpha = static_cast<D3D12_BLEND>(x.DestBlendAlpha);
            y.BlendOpAlpha = static_cast<D3D12_BLEND_OP>(x.BlendOpAlpha); y.LogicOp = static_cast<D3D12_LOGIC_OP>(x.LogicOp);
            y.RenderTargetWriteMask = x.RenderTargetWriteMask;
        }
        break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER: {
        const auto& a = *static_cast<const D3D12DDI_RASTERIZER_DESC_0102*>(in.pDesc); auto& d = s->raster;
        d.FillMode = static_cast<D3D12_FILL_MODE>(a.FillMode); d.CullMode = static_cast<D3D12_CULL_MODE>(a.CullMode);
        d.FrontCounterClockwise = a.FrontCounterClockwise; d.DepthBias = a.DepthBias; d.DepthBiasClamp = a.DepthBiasClamp;
        d.SlopeScaledDepthBias = a.SlopeScaledDepthBias; d.DepthClipEnable = a.DepthClipEnable;
        d.LineRasterizationMode = static_cast<D3D12_LINE_RASTERIZATION_MODE>(a.LineRasterizationMode);
        d.ForcedSampleCount = a.ForcedSampleCount; d.ConservativeRaster = static_cast<D3D12_CONSERVATIVE_RASTERIZATION_MODE>(a.ConservativeRasterizationMode);
        break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL: {
        const auto& a = *static_cast<const D3D12DDI_DEPTH_STENCIL_DESC_0095*>(in.pDesc); auto& d = s->depth;
        d.DepthEnable = a.DepthEnable; d.DepthWriteMask = static_cast<D3D12_DEPTH_WRITE_MASK>(a.DepthWriteMask);
        d.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(a.DepthFunc); d.StencilEnable = a.StencilEnable;
        d.DepthBoundsTestEnable = a.DepthBoundsTestEnable;
        auto face = [](const D3D12DDI_DEPTH_STENCILOP_DESC& x, BOOL enabled, BYTE read, BYTE write) {
            return D3D12_DEPTH_STENCILOP_DESC1{enabled ? static_cast<D3D12_STENCIL_OP>(x.StencilFailOp) : D3D12_STENCIL_OP_KEEP,
                enabled ? static_cast<D3D12_STENCIL_OP>(x.StencilDepthFailOp) : D3D12_STENCIL_OP_KEEP,
                enabled ? static_cast<D3D12_STENCIL_OP>(x.StencilPassOp) : D3D12_STENCIL_OP_KEEP,
                enabled ? static_cast<D3D12_COMPARISON_FUNC>(x.StencilFunc) : D3D12_COMPARISON_FUNC_ALWAYS,
                static_cast<UINT8>(enabled ? read : 0), static_cast<UINT8>(enabled ? write : 0)};
        };
        d.FrontFace = face(a.FrontFace, a.FrontEnable, a.FrontFaceStencilReadMask, a.FrontFaceStencilWriteMask);
        d.BackFace = face(a.BackFace, a.BackEnable, a.BackFaceStencilReadMask, a.BackFaceStencilWriteMask); break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT: {
        const auto& a = *static_cast<const D3D12DDIARG_CREATEELEMENTLAYOUT*>(in.pDesc);
        if (a.NumElements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT || !valid_count(a.NumElements, a.pVertexElements)) return E_INVALIDARG;
        if (a.NumElements) s->input.assign(a.pVertexElements, a.pVertexElements + a.NumElements); break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT: {
        const auto& a = *static_cast<const D3D12DDI_STREAM_OUTPUT_DESC_0108*>(in.pDesc);
        if (!valid_count(a.NumEntries, a.pSODeclaration) || a.NumStrides > 4 || !valid_count(a.NumStrides, a.pBufferStrides)) return E_INVALIDARG;
        if (a.NumEntries) s->so.assign(a.pSODeclaration, a.pSODeclaration + a.NumEntries);
        if (a.NumStrides) s->strides.assign(a.pBufferStrides, a.pBufferStrides + a.NumStrides);
        s->value = a.RasterizedStream; break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
        const auto& a = *static_cast<const D3D12DDI_RT_FORMAT_ARRAY*>(in.pDesc);
        if (a.NumRenderTargets > 8) return E_INVALIDARG; s->formats.NumRenderTargets = a.NumRenderTargets;
        for (UINT i = 0; i != a.NumRenderTargets; ++i) s->formats.RTFormats[i] = a.RTFormats[i]; break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_DESC: s->samples = *static_cast<const DXGI_SAMPLE_DESC*>(in.pDesc); break;
    case D3D12DDI_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
        const auto& a = *static_cast<const D3D12DDI_VIEW_INSTANCING_DESC*>(in.pDesc);
        if (a.ViewInstanceCount > 4 || !valid_count(a.ViewInstanceCount, a.pViewInstanceLocations)) return E_INVALIDARG;
        for (UINT i = 0; i != a.ViewInstanceCount; ++i) s->views.push_back({a.pViewInstanceLocations[i].ViewportArrayIndex, a.pViewInstanceLocations[i].RenderTargetArrayIndex});
        s->value = a.Flags; break;
    }
    case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_MASK: s->value = static_cast<const D3D12DDI_SAMPLE_MASK_0084*>(in.pDesc)->SampleMask; break;
    case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT: s->value = static_cast<const D3D12DDI_DEPTH_STENCIL_FORMAT_0084*>(in.pDesc)->DepthStencilFormat; break;
    case D3D12DDI_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: s->value = *static_cast<const D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE*>(in.pDesc); break;
    case D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY: s->value = *static_cast<const D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE*>(in.pDesc); break;
    case D3D12DDI_STATE_SUBOBJECT_TYPE_FLAGS: s->value = *static_cast<const D3D12DDI_PIPELINE_STATE_FLAGS*>(in.pDesc); break;
    default: return E_NOTIMPL;
    }
    out = std::move(s); return S_OK;
}
}

struct GenericStateObject {
    DeviceContext* device = nullptr;
    bool executable = false;
    bool additions = false;
    UINT node_mask = 0;
    gp::CompilationPolicy compilation = gp::CompilationPolicy::Default;
    std::map<const void*, std::shared_ptr<gp::State>> states;
    std::map<std::wstring, gp::Export> exports;
    std::map<std::wstring, std::shared_ptr<gp::Program>> programs;
    std::map<std::wstring, std::shared_ptr<gp::PartialProgram>> partial_programs;
};

namespace gp {
HRESULT schedule_specialization(GenericStateObject&, const D3D12DDI_GENERIC_PROGRAM_DESC_0108&,
        const std::vector<std::shared_ptr<Program>>&, Program&);
HRESULT compile_linked_exports(GenericStateObject&, const D3D12DDI_GENERIC_PROGRAM_DESC_0108&,
                               std::shared_ptr<Program>&);
HRESULT compile(GenericStateObject& object, const D3D12DDI_GENERIC_PROGRAM_DESC_0108& desc,
                std::shared_ptr<Program>& result, partial::ProgramType partial_type = partial::ProgramType::None,
                const std::vector<std::shared_ptr<Program>>* linked = nullptr) {
    if (linked && (partial_type != partial::ProgramType::None || linked->empty() || linked->size() > 4)) return E_INVALIDARG;
    const bool partial_pixel = partial_type == partial::ProgramType::Pixel;
    const bool partial_preraster = partial_type == partial::ProgramType::Prerasterization;
    if (!name_ok(desc.ProgramName) || !valid_count(desc.NumExports, desc.pExports) || !desc.NumExports ||
        !valid_count(desc.NumSubobjects, desc.pSubobjects)) return E_INVALIDARG;

    if (!linked && partial_type == partial::ProgramType::None) {
        for (UINT i = 0; i < desc.NumExports; ++i) {
            if (!name_ok(desc.pExports[i])) return E_INVALIDARG;
            if (object.partial_programs.contains(desc.pExports[i])) return compile_linked_exports(object, desc, result);
        }
    }
    std::array<const Shader*, 15> shaders{};
    ID3D12RootSignature* root = nullptr;
    for (UINT i = 0; i != desc.NumExports; ++i) {
        if (!name_ok(desc.pExports[i])) return E_INVALIDARG;
        auto e = object.exports.find(desc.pExports[i]); if (e == object.exports.end()) return E_INVALIDARG;
        const auto kind = e->second.shader->container.programType;
        if (kind >= shaders.size() || shaders[kind]) return E_INVALIDARG;
        shaders[kind] = e->second.shader.get();
        if (e->second.root && !(partial_pixel && kind == 13)) {
            // The runtime resolves and validates matching root definitions. Different COM objects can
            // represent identical roots (notably two serialized-root subobjects), so identity is not equality.
            if (!root) root = e->second.root->root;
        }
    }
    const bool partial_combined = partial_preraster && shaders[0];
    const bool partial_pixel_state = partial_pixel || partial_combined;
    std::map<D3D12DDI_STATE_SUBOBJECT_TYPE, const State*> selected;
    for (UINT i = 0; i != desc.NumSubobjects; ++i) {
        const auto found = object.states.find(desc.pSubobjects[i]);
        if (found == object.states.end() || !selected.emplace(found->second->type, found->second.get()).second) return E_INVALIDARG;
        // These inputs must go through partial-stage compilation/linking. Never
        // silently discard them while constructing an ordinary graphics PSO.
        const UINT kind = static_cast<UINT>(found->second->type);
        if (kind >= 37 && kind <= 41 && !(partial_preraster && kind == 40) &&
                !(partial_pixel_state && kind == 41) &&
                !(partial_preraster && !partial_combined && kind == 38) &&
                !(partial_pixel && kind == 39)) return E_NOTIMPL;
    }
    auto find = [&](D3D12DDI_STATE_SUBOBJECT_TYPE type) -> const State* { auto it = selected.find(type); return it == selected.end() ? nullptr : it->second; };
    auto value = [&](D3D12DDI_STATE_SUBOBJECT_TYPE type, UINT fallback) { const auto* s = find(type); return s ? s->value : fallback; };
    if (partial_preraster) {
        // A standalone mesh library needs an explicit interface; final linking
        // validates it against the actual pixel shader without recompilation.
        if (shaders[13] && !partial_combined && !find(static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(38)))
            return E_INVALIDARG;
        if ((!shaders[1] && !shaders[13]) || shaders[5]) return E_INVALIDARG;
        for (const auto& entry : selected) switch (static_cast<UINT>(entry.first)) {
        case D3D12DDI_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            break;
        case 38: if (partial_combined) return E_INVALIDARG; break;
        case 40: break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        case 41: if (!partial_combined) return E_INVALIDARG; break;
        default: return E_INVALIDARG;
        }
    }
    if (partial_pixel) {
        if (!shaders[0]) return E_INVALIDARG;
        for (UINT i = 1; i < shaders.size(); ++i) if (i != 13 && shaders[i]) return E_INVALIDARG;
        for (const auto& entry : selected) switch (static_cast<UINT>(entry.first)) {
        case D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        case 39:
        case 41: break;
        default: return E_INVALIDARG;
        }
    }
    if (const auto* linkage = find(static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(38))) {
        const auto* producer = shaders[13] ? shaders[13] : shaders[2] ? shaders[2] : shaders[4] ? shaders[4] : shaders[1];
        if (!producer || !partial::output_linkage_valid(linkage->linkage, producer->metadata, shaders[13] != nullptr))
            return E_INVALIDARG;
    }
    if (const auto* linkage = find(static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(39)))
        if (!shaders[0] || !partial::preraster_linkage_matches_pixel(linkage->linkage, shaders[0]->metadata))
            return E_INVALIDARG;
    auto p = std::make_shared<Program>(); p->device = object.device;
    if (shaders[5]) {
        if (linked) return E_INVALIDARG;
        if (desc.NumExports != 1) return E_INVALIDARG;
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = root; d.NodeMask = object.node_mask;
        d.CS = {shaders[5]->container.bytes.data(), shaders[5]->container.bytes.size()};
        HRESULT hr = object.device->device->CreateComputePipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&p->pipeline));
        if (FAILED(hr)) return hr;
    } else {
        if (!partial_pixel && ((!shaders[1] && !shaders[13]) || (shaders[1] && shaders[13]))) return E_INVALIDARG;
        SubobjectStream stream;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
        const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE stages[] = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS};
        for (UINT i = 0; i != shaders.size(); ++i) if (shaders[i]) {
            // Representative MS contributes linkage metadata, never executable
            // shader state or root bindings to a pixel-only library.
            if (partial_pixel && i == 13) continue;
            const auto type = i < 5 ? stages[i] : i == 13 ? D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS : D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
            const D3D12_SHADER_BYTECODE code{shaders[i]->container.bytes.data(), shaders[i]->container.bytes.size()}; stream.put(type, code);
        }
        const auto* blend = find(D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND);
        const auto* raster = find(D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER);
        const auto* depth = find(D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL);
        const auto* formats = find(D3D12DDI_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS);
        const auto* samples = find(D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_DESC);
        auto blend_desc = blend ? blend->blend : default_blend();
        auto raster_desc = raster ? raster->raster : default_raster();
        if (partial_pixel_state) if (const auto* state = find(static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(41))) {
            const auto& f = state->pixel_fields;
            // ABI 1.4 libraries bake raster state. Never send a late-raster
            // contract to an older engine that would silently ignore it.
            if (f.late_rasterizer && (object.device->funcs.AbiVersion & 0xffffu) < 5)
                return E_NOINTERFACE;
            if (f.late_sample_desc && (object.device->funcs.AbiVersion & 0xffffu) < 6)
                return E_NOINTERFACE;
            if ((f.late_depth_format || f.late_depth_stencil) &&
                    (object.device->funcs.AbiVersion & 0xffffu) < 7) return E_NOINTERFACE;
            if (f.dual_source_blend && (object.device->funcs.AbiVersion & 0xffffu) < 9)
                return E_NOINTERFACE;
            if (uses_dual_source(blend_desc) && !f.dual_source_blend) return E_INVALIDARG;
            if ((raster && (static_cast<UINT>(raster_desc.LineRasterizationMode) != static_cast<UINT>(f.line_mode) ||
                    raster_desc.ForcedSampleCount != f.forced_samples)) ||
                    (blend && !!blend_desc.AlphaToCoverageEnable != !!f.alpha_to_coverage)) return E_INVALIDARG;
            raster_desc.LineRasterizationMode = static_cast<D3D12_LINE_RASTERIZATION_MODE>(f.line_mode);
            raster_desc.ForcedSampleCount = f.forced_samples;
            blend_desc.AlphaToCoverageEnable = f.alpha_to_coverage;
        }
        // ABI 1.9 carries the fixed PS line mode through independent pre-raster libraries.
        if (partial_pixel_state && raster_desc.LineRasterizationMode != D3D12_LINE_RASTERIZATION_MODE_ALIASED &&
                (object.device->funcs.AbiVersion & 0xffffu) < 9) return E_NOINTERFACE;
        if (partial_pixel_state && raster_desc.ForcedSampleCount &&
                (object.device->funcs.AbiVersion & 0xffffu) < 9) return E_NOINTERFACE;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend_desc);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, raster_desc);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, depth ? depth->depth : default_depth());
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, value(D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT_MAX));
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats ? formats->formats : D3D12_RT_FORMAT_ARRAY{});
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, static_cast<DXGI_FORMAT>(value(D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT_UNKNOWN)));
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, samples ? samples->samples : DXGI_SAMPLE_DESC{1, 0});
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, object.node_mask);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, static_cast<D3D12_PIPELINE_STATE_FLAGS>(value(D3D12DDI_STATE_SUBOBJECT_TYPE_FLAGS, 0)));
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(value(D3D12DDI_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, 0)));
        const UINT topology = value(D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
                partial_pixel ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : 0);
        if (shaders[1] && !topology) return E_INVALIDARG;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(topology));
        std::vector<D3D12_INPUT_ELEMENT_DESC> inputs;
        if (const auto* state = find(D3D12DDI_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT)) {
            if (!shaders[1] && !state->input.empty()) return E_INVALIDARG;
            for (const auto& a : state->input) {
                const sc::dxil::Element* semantic = nullptr;
                for (const auto& e : shaders[1]->metadata.input) if (e.startRow >= 0 && a.InputRegister >= UINT(e.startRow) && a.InputRegister - UINT(e.startRow) < e.rows && !e.kind) { semantic = &e; break; }
                if (!semantic) return E_INVALIDARG;
                inputs.push_back({semantic->name.c_str(), semantic->indices[a.InputRegister - UINT(semantic->startRow)], a.Format,
                    a.InputSlot, a.AlignedByteOffset, static_cast<D3D12_INPUT_CLASSIFICATION>(a.InputSlotClass), a.InstanceDataStepRate});
            }
        }
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, D3D12_INPUT_LAYOUT_DESC{inputs.data(), static_cast<UINT>(inputs.size())});
        if (const auto* state = find(D3D12DDI_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING)) {
            const D3D12_VIEW_INSTANCING_DESC d{static_cast<UINT>(state->views.size()), state->views.data(), static_cast<D3D12_VIEW_INSTANCING_FLAGS>(state->value)};
            stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, d);
        }
        std::vector<D3D12_SO_DECLARATION_ENTRY> so;
        std::vector<sc::StreamOutputElement> semantics;
        if (const auto* state = find(D3D12DDI_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT)) {
            const Shader* last = shaders[2] ? shaders[2] : shaders[4] ? shaders[4] : shaders[1];
            if (!last && !state->so.empty()) return E_INVALIDARG;
            semantics.resize(state->so.size());
            for (size_t i = 0; i != state->so.size(); ++i) {
                // A mesh-only pipeline has no stream-output stage. Check the invariant at its use as well,
                // independently of the vector's empty/size relationship used above.
                if (!last) return E_INVALIDARG;
                const auto r = sc::StreamOutputSemantic(last->container, state->so[i], &semantics[i]); if (!r) return r.hresult();
                const auto& e = semantics[i]; const auto& a = state->so[i];
                so.push_back({a.Stream, e.gap ? nullptr : e.semantic.name.c_str(), e.semantic.index, e.startComponent, e.componentCount, static_cast<BYTE>(a.OutputSlot)});
            }
            const D3D12_STREAM_OUTPUT_DESC d{so.data(), static_cast<UINT>(so.size()), state->strides.data(), static_cast<UINT>(state->strides.size()), state->value};
            stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, d);
        }
        if (partial_preraster || partial_pixel) {
            const auto* fields = find(static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(40));
            BC250_VKD3D_PARTIAL_GRAPHICS_DESC library{};
            auto d = stream.desc();
            library.Size = sizeof(library); library.Stream = &d;
            library.Subsets = partial_pixel ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
                VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT :
                VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
            if (partial_combined) library.Subsets |= VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
                VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            // Alpha-to-coverage remains part of the PS compilation contract.
            // Only the output interface is deferred when blend is late linked.
            if (partial_pixel_state) if (const auto* pixel = find(static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(41)))
            {
                if (pixel->pixel_fields.late_blend || pixel->pixel_fields.late_rt_formats ||
                        pixel->pixel_fields.late_sample_mask || pixel->pixel_fields.late_sample_desc ||
                        pixel->pixel_fields.late_depth_format)
                    library.Subsets &= ~VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
                if (pixel->pixel_fields.late_sample_desc)
                    library.Subsets |= BC250_VKD3D_PARTIAL_LATE_SAMPLE_DESC;
                if (pixel->pixel_fields.dual_source_blend)
                    library.Subsets |= BC250_VKD3D_PARTIAL_DUAL_SOURCE_BLEND;
            }
            if (partial_preraster && !shaders[13] && (!fields || !fields->preraster_fields.late_input_layout))
                library.Subsets |= VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
            library.ExcludePixelShader = fields && fields->preraster_fields.exclude_ps;
            if (partial_combined && library.ExcludePixelShader) return E_INVALIDARG;
            if (partial_pixel && shaders[13]) {
                library.RepresentativeMesh = shaders[13]->container.bytes.data();
                library.RepresentativeMeshSize = shaders[13]->container.bytes.size();
                if (!library.RepresentativeMeshSize) return E_INVALIDARG;
            }
            const HRESULT hr = p->library.create(&object.device->funcs,
                    object.device->device, library);
            if (FAILED(hr)) return hr;
            result = std::move(p);
            return S_OK; // A partial library is not an executable program identifier.
        }
        auto d = stream.desc();
        HRESULT hr;
        if (linked) {
            const auto& funcs = object.device->funcs;
            if (funcs.Size < sizeof(funcs) || (funcs.AbiVersion >> 16) != BC250_VKD3D_ENGINE_ABI_MAJOR ||
                    (funcs.AbiVersion & 0xffffu) < 4 || !funcs.CreateLinkedGraphicsPipeline) return E_NOINTERFACE;
            void* libraries[4]{};
            for (size_t i = 0; i < linked->size(); ++i) {
                const auto& part = (*linked)[i];
                if (!part || part->device != object.device || !part->library.get()) return E_INVALIDARG;
                libraries[i] = part->library.get();
            }
            // The engine retains every library used by the returned PSO. Shared
            // Program owners above keep handles valid throughout the synchronous call.
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) && AMDGPU_WDDM_LAB_FLAVOUR
            fprintf(stderr,"PGP compilation link optimize=%u policy=%u\n",object.compilation == CompilationPolicy::Full,static_cast<UINT>(object.compilation));
#endif
            hr = funcs.CreateLinkedGraphicsPipeline(object.device->device, &d, libraries,
                    static_cast<UINT>(linked->size()), object.compilation == CompilationPolicy::Full, __uuidof(ID3D12PipelineState),
                    reinterpret_cast<void**>(&p->pipeline));
        } else {
            ID3D12Device2* device2 = nullptr;
            hr = object.device->device->QueryInterface(__uuidof(ID3D12Device2), reinterpret_cast<void**>(&device2));
            if (FAILED(hr)) return hr;
            hr = device2->CreatePipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&p->pipeline));
            device2->Release();
        }
        if (FAILED(hr)) return hr;
    }
    if (!p->pipeline) return E_UNEXPECTED;
    if (linked && object.compilation == CompilationPolicy::Background) {
        if ((object.device->funcs.AbiVersion & 0xffffu) < 8) return E_NOINTERFACE;
        const HRESULT hr=schedule_specialization(object,desc,*linked,*p);
        if (FAILED(hr)) return hr;
    }
    p->id = next_id.fetch_add(1); if (!p->id) return E_UNEXPECTED;
    { std::lock_guard lock(registry_lock); registry.emplace(p->id, p); }
    result = std::move(p); return S_OK;
}

HRESULT schedule_specialization(GenericStateObject& object,
        const D3D12DDI_GENERIC_PROGRAM_DESC_0108& desc,
        const std::vector<std::shared_ptr<Program>>& libraries, Program& target) {
    // Copy owning metadata, never the caller's arrays, strings or executable
    // Program map. Imported library owners keep every shader library alive.
    auto snapshot=std::make_shared<GenericStateObject>();
    snapshot->device=object.device; snapshot->node_mask=object.node_mask;
    snapshot->compilation=CompilationPolicy::Full;
    snapshot->states=object.states; snapshot->exports=object.exports;
    std::wstring name=desc.ProgramName;
    std::vector<std::wstring> exports;
    for (UINT i=0;i<desc.NumExports;++i) exports.emplace_back(desc.pExports[i]);
    std::vector<const D3D12DDI_STATE_SUBOBJECT_0054*> identities;
    for (UINT i=0;i<desc.NumSubobjects;++i) identities.push_back(desc.pSubobjects[i]);
    target.background=BackgroundPipeline<ID3D12PipelineState>::start(
        [snapshot,libraries,name=std::move(name),exports=std::move(exports),identities=std::move(identities)]
        (ID3D12PipelineState** out) mutable -> HRESULT {
            *out=nullptr;
            std::vector<LPCWSTR> pointers;
            for (const auto& entry:exports) pointers.push_back(entry.c_str());
            const D3D12DDI_GENERIC_PROGRAM_DESC_0108 owned{name.c_str(),
                static_cast<UINT>(pointers.size()),pointers.data(),
                static_cast<UINT>(identities.size()),identities.data()};
            std::shared_ptr<Program> optimized;
            const HRESULT hr=compile(*snapshot,owned,optimized,partial::ProgramType::None,&libraries);
            if (FAILED(hr)) return hr;
            if (!optimized || !optimized->pipeline) return E_UNEXPECTED;
            optimized->pipeline->AddRef(); *out=optimized->pipeline;
            return S_OK;
        });
    return target.background ? S_OK : E_OUTOFMEMORY;
}

HRESULT compile_linked_exports(GenericStateObject& object, const D3D12DDI_GENERIC_PROGRAM_DESC_0108& desc,
                               std::shared_ptr<Program>& result) {
    // Raw stages in a mixed program are new compilation work, unlike imported
    // partials. Compile only the missing stage family, then use the same strict
    // partial-link path for ownership, duplicate stages and final signatures.
    std::vector<LPCWSTR> raw_names;
    bool raw_pixel = false, raw_preraster = false;
    for (UINT i = 0; i < desc.NumExports; ++i) {
        if (!name_ok(desc.pExports[i])) return E_INVALIDARG;
        if (object.partial_programs.contains(desc.pExports[i])) continue;
        const auto it = object.exports.find(desc.pExports[i]);
        if (it == object.exports.end() || !it->second.shader) return E_INVALIDARG;
        const UINT stage = it->second.shader->container.programType;
        if (stage == 0) raw_pixel = true;
        else if ((stage >= 1 && stage <= 4) || stage == 13 || stage == 14) raw_preraster = true;
        else return E_INVALIDARG;
        raw_names.push_back(desc.pExports[i]);
    }
    if (!raw_names.empty()) {
        if (raw_pixel == raw_preraster) return E_INVALIDARG;
        for (UINT i = 0; i < desc.NumExports; ++i) {
            const auto it = object.partial_programs.find(desc.pExports[i]);
            if (it == object.partial_programs.end()) continue;
            const auto& part = it->second;
            if (!part) return E_INVALIDARG;
            if (raw_preraster && part->description.type == partial::ProgramType::Prerasterization)
                return E_INVALIDARG;
            if (raw_pixel) for (const auto& e : part->exports)
                if (e.second.shader && e.second.shader->container.programType == 0) return E_INVALIDARG;
        }
        GenericStateObject normalized = object;
        auto raw = std::make_shared<PartialProgram>();
        raw->description.type = raw_pixel ? partial::ProgramType::Pixel : partial::ProgramType::Prerasterization;
        std::vector<const D3D12DDI_STATE_SUBOBJECT_0054*> consumed, remaining;
        for (UINT i = 0; i < desc.NumSubobjects; ++i) {
            const auto it = object.states.find(desc.pSubobjects[i]);
            if (it == object.states.end() || !it->second) return E_INVALIDARG;
            bool owns = false;
            switch (it->second->type) {
            case D3D12DDI_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE: owns = raw_preraster; break;
            case D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            case D3D12DDI_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: owns = raw_pixel; break;
            default: break;
            }
            if (owns) { consumed.push_back(desc.pSubobjects[i]); raw->states.push_back(it->second); }
            else remaining.push_back(desc.pSubobjects[i]);
        }
        for (const auto* name : raw_names) raw->exports.emplace(name, object.exports.at(name));
        GenericStateObject raw_object = object;
        std::wstring representative_name = L"__partial_raw_mesh_metadata";
        if (raw_pixel) {
            std::shared_ptr<Shader> representative;
            for (UINT i = 0; i < desc.NumExports; ++i) {
                const auto part = object.partial_programs.find(desc.pExports[i]);
                if (part == object.partial_programs.end()) continue;
                for (const auto& [name, value] : part->second->exports) {
                    (void)name;
                    if (value.shader && value.shader->container.programType == 13) {
                        if (representative && representative != value.shader) return E_INVALIDARG;
                        representative = value.shader;
                    }
                }
            }
            if (representative) {
                while (raw_object.exports.contains(representative_name)) representative_name += L"_";
                Export metadata{representative, {}, {}};
                raw_object.exports.emplace(representative_name, metadata);
                raw->exports.emplace(representative_name, metadata);
                raw_names.push_back(representative_name.c_str());
            }
        }
        D3D12DDI_STATE_SUBOBJECT_0054 inferred_output_id{};
        const Shader* raw_mesh = nullptr;
        for (const auto& name : raw_names)
            if (raw_preraster && raw_object.exports.at(name).shader->container.programType == 13)
                raw_mesh = raw_object.exports.at(name).shader.get();
        if (raw_mesh) {
            // A raw MS in a generic executable is compiled here, not reused as
            // an independently declared partial. Infer its internal interface
            // from the imported PS's inputs and validate the real producer.
            const Shader* pixel = nullptr;
            for (UINT i = 0; i < desc.NumExports; ++i) {
                const auto part = object.partial_programs.find(desc.pExports[i]);
                if (part == object.partial_programs.end()) continue;
                for (const auto& [name, value] : part->second->exports) {
                    (void)name;
                    if (value.shader && value.shader->container.programType == 0) {
                        if (pixel && pixel != value.shader.get()) return E_INVALIDARG;
                        pixel = value.shader.get();
                    }
                }
            }
            if (!pixel) return E_INVALIDARG;
            partial::SemanticMasks inputs;
            if (!partial::metadata_masks(pixel->metadata.input, inputs)) return E_INVALIDARG;
            auto linkage = std::make_shared<State>();
            linkage->type = static_cast<D3D12DDI_STATE_SUBOBJECT_TYPE>(38);
            for (const auto& [semantic, mask] : inputs) {
                for (UINT bit = 0; bit < 4;) {
                    if (!(mask & (1u << bit))) { ++bit; continue; }
                    const UINT start = bit++;
                    while (bit < 4 && (mask & (1u << bit))) ++bit;
                    linkage->linkage.output.push_back({semantic.first, semantic.second,
                            static_cast<BYTE>(start), static_cast<BYTE>(bit - start)});
                }
            }
            if (!partial::output_linkage_valid(linkage->linkage, raw_mesh->metadata, true))
                return E_INVALIDARG;
            raw_object.states.emplace(&inferred_output_id, linkage);
            consumed.push_back(&inferred_output_id);
            raw->states.push_back(linkage);
        }
        const D3D12DDI_GENERIC_PROGRAM_DESC_0108 raw_desc{desc.ProgramName,
            static_cast<UINT>(raw_names.size()), raw_names.data(), static_cast<UINT>(consumed.size()), consumed.data()};
        HRESULT hr = compile(raw_object, raw_desc, raw->compiled, raw->description.type);
        if (FAILED(hr)) return hr;
        std::wstring synthetic = L"__partial_raw";
        while (normalized.exports.contains(synthetic) || normalized.partial_programs.contains(synthetic)) synthetic += L"_";
        normalized.partial_programs.emplace(synthetic, raw);
        std::vector<LPCWSTR> names{synthetic.c_str()};
        for (UINT i = 0; i < desc.NumExports; ++i)
            if (object.partial_programs.contains(desc.pExports[i])) names.push_back(desc.pExports[i]);
        const D3D12DDI_GENERIC_PROGRAM_DESC_0108 normalized_desc{desc.ProgramName,
            static_cast<UINT>(names.size()), names.data(), static_cast<UINT>(remaining.size()), remaining.data()};
        return compile_linked_exports(normalized, normalized_desc, result);
    }
    GenericStateObject resolved;
    resolved.device = object.device; resolved.node_mask = object.node_mask;
    resolved.compilation = object.compilation;
    std::vector<std::shared_ptr<Program>> libraries;
    std::map<D3D12DDI_STATE_SUBOBJECT_TYPE, std::shared_ptr<State>> states;
    std::set<const PartialProgram*> seen;
    bool has_preraster = false, late_input_allowed = true, excludes_pixel = false;
    bool has_pixel = false, late_blend_allowed = true;
    bool late_rt_formats_allowed = true;
    bool late_sample_mask_allowed = true;
    bool late_rasterizer_allowed = true;
    bool late_sample_desc_allowed = true;
    bool late_depth_format_allowed = true, late_depth_stencil_allowed = true;
    const partial::Linkage* explicit_output = nullptr;
    const Shader* linked_pixel = nullptr;
    const Shader* linked_mesh = nullptr;
    const Shader* representative_mesh = nullptr;
    UINT pixel_line_mode = 0, pixel_forced_samples = 0;
    bool pixel_alpha_to_coverage = false, pixel_dual_source = false;
    for (UINT i = 0; i < desc.NumExports; ++i) {
        if (!name_ok(desc.pExports[i])) return E_INVALIDARG;
        const auto found = object.partial_programs.find(desc.pExports[i]);
        if (found == object.partial_programs.end()) return E_INVALIDARG;
        const auto& part = found->second;
        if (!part || !part->compiled || !seen.insert(part.get()).second || libraries.size() == 4) return E_INVALIDARG;
        libraries.push_back(part->compiled);
        bool part_late_input = false;
        bool part_has_pixel = false, part_late_blend = false;
        bool part_late_rt_formats = false;
        bool part_late_sample_mask = false;
        bool part_late_rasterizer = false;
        bool part_late_sample_desc = false;
        bool part_late_depth_format = false, part_late_depth_stencil = false;
        UINT part_line_mode = 0, part_forced_samples = 0;
        bool part_alpha_to_coverage = false, part_dual_source = false;
        bool has_pixel_fields = false;
        const State* part_blend = nullptr;
        const State* part_raster = nullptr;
        for (const auto& entry : part->exports) {
            if (!entry.second.shader) return E_INVALIDARG;
            part_has_pixel = part_has_pixel || entry.second.shader->container.programType == 0;
            if (entry.second.shader->container.programType == 0) linked_pixel = entry.second.shader.get();
            if (entry.second.shader->container.programType == 13) {
                if (part->description.type == partial::ProgramType::Pixel) {
                    representative_mesh = entry.second.shader.get();
                    continue; // Representative MS is not an executable stage.
                }
                linked_mesh = entry.second.shader.get();
            }
            const auto name = L"stage" + std::to_wstring(resolved.exports.size());
            resolved.exports.emplace(name, entry.second);
        }
        for (const auto& state : part->states) {
            if (!state) return E_INVALIDARG;
            const UINT kind = static_cast<UINT>(state->type);
            if (kind == 38) {
                if (explicit_output) return E_INVALIDARG;
                explicit_output = &state->linkage;
                continue;
            }
            if (kind == 39) continue; // Validated against the PS at collection creation.
            if (state->type == D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND) part_blend = state.get();
            if (state->type == D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER) part_raster = state.get();
            if (kind == 40) { part_late_input = !!state->preraster_fields.late_input_layout; excludes_pixel |= !!state->preraster_fields.exclude_ps; continue; }
            if (kind == 41) {
                has_pixel_fields = true;
                const auto& f = state->pixel_fields;
                if (f.forced_samples && (object.device->funcs.AbiVersion & 0xffffu) < 9) return E_NOINTERFACE;
                part_dual_source = !!f.dual_source_blend;
                part_late_sample_desc = !!f.late_sample_desc;
                part_late_depth_format = !!f.late_depth_format;
                part_late_depth_stencil = !!f.late_depth_stencil;
                part_late_blend = !!f.late_blend;
                part_late_rt_formats = !!f.late_rt_formats;
                part_late_sample_mask = !!f.late_sample_mask;
                part_late_rasterizer = !!f.late_rasterizer;
                part_line_mode = static_cast<UINT>(f.line_mode);
                part_forced_samples = f.forced_samples;
                part_alpha_to_coverage = !!f.alpha_to_coverage;
                continue;
            }
            const auto [it, inserted] = states.emplace(state->type, state);
            if (!inserted && it->second != state) {
                // Topology is commonly declared by both independently compiled parts.
                if (state->type != D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY) return E_NOTIMPL;
                if (it->second->value != state->value) return E_INVALIDARG;
            }
        }
        if (part->description.type == partial::ProgramType::Prerasterization) {
            has_preraster = true;
            late_input_allowed = late_input_allowed && part_late_input;
        }
        if (part_has_pixel) {
            if (part_raster) {
                if (has_pixel_fields && (part_line_mode != static_cast<UINT>(part_raster->raster.LineRasterizationMode) ||
                        part_forced_samples != part_raster->raster.ForcedSampleCount)) return E_INVALIDARG;
                part_line_mode = static_cast<UINT>(part_raster->raster.LineRasterizationMode);
                part_forced_samples = part_raster->raster.ForcedSampleCount;
            }
            if (has_pixel && (pixel_line_mode != part_line_mode || pixel_forced_samples != part_forced_samples))
                return E_INVALIDARG;
            pixel_line_mode = part_line_mode;
            pixel_forced_samples = part_forced_samples;
            late_rasterizer_allowed = late_rasterizer_allowed && part_late_rasterizer;
            late_sample_desc_allowed = late_sample_desc_allowed && part_late_sample_desc;
            // The compile-time field can come from the full blend descriptor
            // when no partial-fields object was supplied.
            if (part_blend) {
                if (has_pixel_fields && part_alpha_to_coverage != !!part_blend->blend.AlphaToCoverageEnable)
                    return E_INVALIDARG;
                part_alpha_to_coverage = !!part_blend->blend.AlphaToCoverageEnable;
                if (uses_dual_source(part_blend->blend)) {
                    if (has_pixel_fields && !part_dual_source) return E_INVALIDARG;
                    part_dual_source = true;
                }
            }
            if (has_pixel && pixel_alpha_to_coverage != part_alpha_to_coverage) return E_INVALIDARG;
            has_pixel = true;
            pixel_alpha_to_coverage = part_alpha_to_coverage;
            pixel_dual_source = part_dual_source;
            late_blend_allowed = late_blend_allowed && part_late_blend;
            late_rt_formats_allowed = late_rt_formats_allowed && part_late_rt_formats;
            late_sample_mask_allowed = late_sample_mask_allowed && part_late_sample_mask;
            late_depth_format_allowed = late_depth_format_allowed && part_late_depth_format;
            late_depth_stencil_allowed = late_depth_stencil_allowed && part_late_depth_stencil;
        }
    }
    if (excludes_pixel && has_pixel) return E_INVALIDARG;
    // With no PS partial there are no PS compile-time defaults to override.
    // Final depth/stencil and output state still apply to depth-only rendering.
    const bool depth_only = has_preraster && !has_pixel;
    if (explicit_output && (!linked_pixel ||
            !partial::output_linkage_matches_pixel(*explicit_output, linked_pixel->metadata)))
        return E_INVALIDARG;
    if (representative_mesh && (!linked_mesh ||
            !partial::representative_outputs_match(representative_mesh->metadata, linked_mesh->metadata)))
        return E_INVALIDARG;
    for (UINT i = 0; i < desc.NumSubobjects; ++i) {
        const auto found = object.states.find(desc.pSubobjects[i]);
        if (found == object.states.end() || !found->second) return E_INVALIDARG;
        const auto& state = found->second;
        switch (state->type) {
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
            if ((!depth_only && (!has_pixel || !late_depth_format_allowed))) return E_INVALIDARG;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
            if ((!depth_only && (!has_pixel || !late_depth_stencil_allowed))) return E_INVALIDARG;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
            if ((!depth_only && (!has_pixel || !late_sample_desc_allowed)) || !state->samples.Count ||
                    state->samples.Count > 64 || (state->samples.Count & (state->samples.Count - 1)))
                return E_INVALIDARG;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER:
            if (!depth_only && (!has_pixel || !late_rasterizer_allowed ||
                    static_cast<UINT>(state->raster.LineRasterizationMode) != pixel_line_mode ||
                    state->raster.ForcedSampleCount != pixel_forced_samples)) return E_INVALIDARG;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
            if ((!depth_only && (!has_pixel || !late_sample_mask_allowed))) return E_INVALIDARG;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
            if ((!depth_only && (!has_pixel || !late_rt_formats_allowed)) || state->formats.NumRenderTargets > 8)
                return E_INVALIDARG;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND:
            if (!depth_only && (!has_pixel || !late_blend_allowed ||
                    !!state->blend.AlphaToCoverageEnable != pixel_alpha_to_coverage)) return E_INVALIDARG;
            // SRC1 factors require a PS compiled with dual-source exports.
            for (const auto& rt : state->blend.RenderTarget) if (rt.BlendEnable) {
                const auto dual = [](D3D12_BLEND factor) {
                    return factor == D3D12_BLEND_SRC1_COLOR || factor == D3D12_BLEND_INV_SRC1_COLOR ||
                        factor == D3D12_BLEND_SRC1_ALPHA || factor == D3D12_BLEND_INV_SRC1_ALPHA;
                };
                if (!pixel_dual_source && (dual(rt.SrcBlend) || dual(rt.DestBlend) || dual(rt.SrcBlendAlpha) || dual(rt.DestBlendAlpha)))
                    return E_INVALIDARG;
            }
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
            if (!has_preraster || !late_input_allowed) return E_INVALIDARG;
            break;
        case D3D12DDI_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
            // This value may be supplied at final linking if absent in the partial.
            break;
        default: return E_NOTIMPL; // Other late-state contracts still need validation.
        }
        if (!states.emplace(state->type, state).second) return E_INVALIDARG;
    }
    if (has_pixel) {
        const auto blend = states.find(D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND);
        if (blend != states.end() && !!blend->second->blend.AlphaToCoverageEnable != pixel_alpha_to_coverage)
            return E_INVALIDARG;
    }
    // Fixed PS fields survive linking even when no complete rasterizer object
    // was supplied. Late rasterization may vary other fields, never these two.
    if (has_pixel && (pixel_line_mode || pixel_forced_samples) &&
            !states.count(D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER)) {
        auto raster = std::make_shared<State>();
        raster->type = D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER;
        raster->raster = default_raster();
        raster->raster.LineRasterizationMode = static_cast<D3D12_LINE_RASTERIZATION_MODE>(pixel_line_mode);
        raster->raster.ForcedSampleCount = pixel_forced_samples;
        states.emplace(raster->type, std::move(raster));
    }
    // A missing final blend object still needs the PS's compile-time alpha state.
    if (has_pixel && pixel_alpha_to_coverage && !states.count(D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND)) {
        auto blend = std::make_shared<State>();
        blend->type = D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND;
        blend->blend = default_blend(); blend->blend.AlphaToCoverageEnable = TRUE;
        states.emplace(blend->type, std::move(blend));
    }
    std::vector<LPCWSTR> exports;
    for (const auto& entry : resolved.exports) exports.push_back(entry.first.c_str());
    std::vector<const D3D12DDI_STATE_SUBOBJECT_0054*> identities;
    std::vector<D3D12DDI_STATE_SUBOBJECT_0054> storage(states.size());
    size_t at = 0;
    for (const auto& entry : states) {
        storage[at].Type = entry.first;
        resolved.states.emplace(&storage[at], entry.second);
        identities.push_back(&storage[at++]);
    }
    const D3D12DDI_GENERIC_PROGRAM_DESC_0108 final_desc{desc.ProgramName,
        static_cast<UINT>(exports.size()), exports.data(), static_cast<UINT>(identities.size()), identities.data()};
    return compile(resolved, final_desc, result, partial::ProgramType::None, &libraries);
}
}

bool generic_state_description(const D3D12DDIARG_CREATE_STATE_OBJECT_0054& args) noexcept {
    // An EXECUTABLE with a WORK_GRAPH subobject is a work graph (state-objects.cpp, work-graphs.cpp).
    if (args.Type == D3D12DDI_STATE_OBJECT_TYPE_EXECUTABLE) {
        if (!gp::valid_count(args.NumSubobjects, args.pSubobjects)) return true;
        for (UINT i = 0; i != args.NumSubobjects; ++i)
            if (args.pSubobjects[i].Type == D3D12DDI_STATE_SUBOBJECT_TYPE_WORK_GRAPH) return false;
        return true;
    }
    if (args.Type != D3D12DDI_STATE_OBJECT_TYPE_COLLECTION || !gp::valid_count(args.NumSubobjects, args.pSubobjects)) return false;
    try {
        for (UINT i = 0; i != args.NumSubobjects; ++i) {
            const auto& s = args.pSubobjects[i]; if (!s.pDesc) continue;
            if (static_cast<UINT>(s.Type) == 37) return true;
            if (s.Type == D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION) {
                const auto* a = static_cast<const D3D12DDI_EXISTING_COLLECTION_DESC_0054*>(s.pDesc);
                const auto* record = record_of<StateObjectRecord>(a->hExistingCollection.pDrvPrivate, Tag::StateObject);
                if (record && record->generic) return true;
            }
            if (s.Type == D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY) {
                const auto* a = static_cast<const D3D12DDI_DXIL_LIBRARY_DESC_0054*>(s.pDesc);
                if (!a->pDXILLibrary) continue;
                const size_t words = shader_container::LibraryPayloadDwords(a->pDXILLibrary);
                if (!words || words > (16u << 20)) continue;
                gp::Shader shader;
                if (shader_container::BuildGenericContainer(a->pDXILLibrary, words, &shader.container, &shader.metadata)) return true;
            }
        }
    } catch (...) { return false; }
    return false;
}

HRESULT create_generic_state(DeviceContext* c, const D3D12DDIARG_CREATE_STATE_OBJECT_0054& args,
                             const GenericStateObject* parent, GenericStateObject** out) noexcept {
    *out = nullptr;
    const bool executable = args.Type == D3D12DDI_STATE_OBJECT_TYPE_EXECUTABLE;
    if (!gp::valid_count(args.NumSubobjects, args.pSubobjects) || (!executable && args.Type != D3D12DDI_STATE_OBJECT_TYPE_COLLECTION) ||
        (parent && (!executable || !parent->executable || parent->device != c || !parent->additions))) return E_INVALIDARG;
    try {
        auto object = std::make_unique<GenericStateObject>();
        object->device = c; object->executable = executable;
        // Graphics subobjects of every new generic program must be local to this description. Only shader
        // definitions, their owned root associations and already compiled programs are inherited by additions.
        if (parent) { object->exports = parent->exports; object->programs = parent->programs;
            object->partial_programs = parent->partial_programs; object->node_mask = parent->node_mask; }
        std::shared_ptr<gp::State> default_root;
        bool ambiguous_root = false;
        std::vector<const D3D12DDI_GENERIC_PROGRAM_DESC_0108*> programs;
        std::vector<partial::OwnedDescription> partial_descriptions;
        std::vector<const D3D12DDI_FUNCTION_SUMMARY_0054*> summaries;
        std::set<const D3D12DDI_STATE_SUBOBJECT_0054*> validated_configs;
        for (UINT i = 0; i != args.NumSubobjects; ++i) {
            const auto& s = args.pSubobjects[i]; if (!s.pDesc) return E_INVALIDARG;
            if (static_cast<UINT>(s.Type) == 37) {
                if (c->ddi_version < 122) return E_INVALIDARG;
                partial::OwnedDescription owned;
                const HRESULT hr = owned.capture(*static_cast<const partial::Description*>(s.pDesc),
                        args.pSubobjects, args.NumSubobjects);
                if (FAILED(hr)) return hr;
                partial_descriptions.push_back(std::move(owned));
                continue;
            }
            switch (s.Type) {
            case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
                const auto flags = static_cast<const D3D12DDI_STATE_OBJECT_CONFIG_0054*>(s.pDesc)->Flags;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) && AMDGPU_WDDM_LAB_FLAVOUR
                fprintf(stderr,"PGP compilation ddiflags=%08x executable=%u\n",static_cast<UINT>(flags),executable);
                fprintf(stderr,"PGP config scope object=%p subobjects=%u index=%u flags=%08x executable=%u\n",static_cast<void*>(object.get()),args.NumSubobjects,i,static_cast<UINT>(flags),executable);
#endif
                constexpr UINT allowed = D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS |
                    D3D12DDI_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS |
                    D3D12DDI_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS | gp::compilation_mask;
                if (flags & ~allowed) return E_NOTIMPL;
                const HRESULT policy_hr=gp::compilation_admission(static_cast<UINT>(flags),executable,object->compilation);
                if (FAILED(policy_hr)) return policy_hr;
                validated_configs.insert(&s);
                object->additions = !!(flags & D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS); break;
            }
            case D3D12DDI_STATE_SUBOBJECT_TYPE_NODE_MASK: object->node_mask = static_cast<const D3D12DDI_NODE_MASK_0054*>(s.pDesc)->NodeMask; break;
            case D3D12DDI_STATE_SUBOBJECT_TYPE_GENERIC_PROGRAM:
                if (!executable) return E_INVALIDARG;
                programs.push_back(static_cast<const D3D12DDI_GENERIC_PROGRAM_DESC_0108*>(s.pDesc)); break;
            case D3D12DDI_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION: {
                const auto& a = *static_cast<const D3D12DDI_EXISTING_COLLECTION_DESC_0054*>(s.pDesc);
                const auto* record = record_of<StateObjectRecord>(a.hExistingCollection.pDrvPrivate, Tag::StateObject, c);
                if (!record || !record->generic || record->generic->executable || !gp::valid_count(a.NumExports, a.pExports)) return E_INVALIDARG;
                const auto& collection = *record->generic;
                if (!a.NumExports) {
                    for (const auto& entry : collection.exports)
                        if (!partial::import_named(object->exports, object->partial_programs,
                                collection.exports, collection.partial_programs, entry.first, entry.first)) return E_INVALIDARG;
                    for (const auto& entry : collection.partial_programs)
                        if (!partial::import_named(object->exports, object->partial_programs,
                                collection.exports, collection.partial_programs, entry.first, entry.first)) return E_INVALIDARG;
                } else for (UINT j = 0; j != a.NumExports; ++j) {
                    const auto& e = a.pExports[j];
                    if (!gp::name_ok(e.Name) || e.Flags || (e.ExportToRename && !gp::name_ok(e.ExportToRename))) return E_INVALIDARG;
                    if (!partial::import_named(object->exports, object->partial_programs,
                            collection.exports, collection.partial_programs,
                            e.ExportToRename ? e.ExportToRename : e.Name, e.Name)) return E_INVALIDARG;
                }
                break;
            }
            case D3D12DDI_STATE_SUBOBJECT_TYPE_SHADER_EXPORT_SUMMARY: summaries.push_back(static_cast<const D3D12DDI_FUNCTION_SUMMARY_0054*>(s.pDesc)); break;
            case D3D12DDI_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY: {
                const auto& a = *static_cast<const D3D12DDI_DXIL_LIBRARY_DESC_0054*>(s.pDesc);
                if (!a.pDXILLibrary || !gp::valid_count(a.NumExports, a.pExports)) return E_INVALIDARG;
                const size_t words = shader_container::LibraryPayloadDwords(a.pDXILLibrary);
                if (!words || words > (16u << 20)) return E_INVALIDARG;
                auto shader = std::make_shared<gp::Shader>();
                auto result = shader_container::BuildGenericContainer(a.pDXILLibrary, words, &shader->container, &shader->metadata);
                if (!result) { log_refusal("GenericProgram: DXIL intake: %s", result.detail.c_str()); return result.hresult(); }
                const std::wstring original = gp::wide(shader->metadata.entryName);
                const UINT count = a.NumExports ? a.NumExports : 1;
                for (UINT j = 0; j != count; ++j) {
                    const LPCWSTR exported = a.NumExports ? a.pExports[j].Name : original.c_str();
                    if (!gp::name_ok(exported) || (a.NumExports && a.pExports[j].Flags)) return E_INVALIDARG;
                    // WorkGraphs: DDI Name is authoritative for non-library shaders; ExportToRename is ignored.
                    if (object->partial_programs.contains(exported) ||
                            !object->exports.emplace(exported, gp::Export{shader, {}, {}}).second) return E_INVALIDARG;
                }
                break;
            }
            default: {
                std::shared_ptr<gp::State> state;
                HRESULT hr = gp::snapshot(c, s, state); if (FAILED(hr)) return hr;
                object->states[&s] = state;
                if (state->root) {
                    if (default_root && default_root->root != state->root) ambiguous_root = true;
                    default_root = state;
                }
                break;
            }
            }
        }
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) && AMDGPU_WDDM_LAB_FLAVOUR
        fprintf(stderr,"PGP config resolved object=%p configs=%zu programs=%zu partials=%zu policy=%u additions=%u\n",static_cast<void*>(object.get()),validated_configs.size(),programs.size(),partial_descriptions.size(),static_cast<UINT>(object->compilation),object->additions);
#endif
        if (parent && !object->additions) return E_INVALIDARG;
        if (!ambiguous_root && default_root) for (auto& entry : object->exports) if (!entry.second.root) entry.second.root = default_root;
        for (const auto* summary : summaries) {
            // Collections may defer resource bindings and associations until imported into an executable.
            // No PSO is compiled for a collection; executable summaries must be fully resolved by the runtime.
            constexpr UINT unresolved = D3D12DDI_EXPORT_SUMMARY_FLAG_UNRESOLVED_RESOURCE_BINDINGS |
                D3D12DDI_EXPORT_SUMMARY_FLAG_UNRESOLVED_FUNCTIONS | D3D12DDI_EXPORT_SUMMARY_FLAG_UNRESOLVED_ASSOCIATIONS;
            const UINT allowed_flags = executable ? 0 : unresolved;
            if (!gp::valid_count(summary->NumExportedFunctions, summary->pSummaries) || (summary->OverallFlags & ~allowed_flags)) return E_INVALIDARG;
            for (UINT i = 0; i != summary->NumExportedFunctions; ++i) {
                const auto& a = summary->pSummaries[i];
                if (!gp::valid_count(a.NumAssociatedSubobjects, a.ppAssociatedSubobjects) || (a.Flags & ~allowed_flags)) return E_INVALIDARG;
                auto e = a.ExportNameUnmangled ? object->exports.find(a.ExportNameUnmangled) : object->exports.end();
                if (e == object->exports.end() && a.ExportNameMangled) e = object->exports.find(a.ExportNameMangled);
                if (e == object->exports.end()) return E_INVALIDARG;
                std::shared_ptr<gp::State> root;
                for (UINT j = 0; j != a.NumAssociatedSubobjects; ++j) {
                    // The runtime includes STATE_OBJECT_CONFIG in collection export associations.
                    // Its flags were validated above and apply to this object, not to root bindings.
                    // Match local identities before dereferencing; unknown associations still fail.
                    if (validated_configs.contains(a.ppAssociatedSubobjects[j])) {
                        e->second.configs.insert(a.ppAssociatedSubobjects[j]);
                        continue;
                    }
                    if (e->second.configs.contains(a.ppAssociatedSubobjects[j])) continue;
                    auto state = object->states.find(a.ppAssociatedSubobjects[j]);
                    if (state != object->states.end() && state->second->root) {
                        if (!root) root = state->second;
                    } else if (e->second.root && e->second.root->identity == a.ppAssociatedSubobjects[j]) {
                        if (!root) root = e->second.root;
                    } else return E_INVALIDARG;
                }
                e->second.root = root;
            }
        }
        for (auto& desc : partial_descriptions) {
            if (object->exports.contains(desc.name) || object->partial_programs.contains(desc.name) ||
                    object->programs.contains(desc.name)) return E_INVALIDARG;
            auto partial_program = std::make_shared<gp::PartialProgram>();
            std::vector<LPCWSTR> names;
            std::vector<const D3D12DDI_STATE_SUBOBJECT_0054*> states;
            for (const auto& name : desc.exports) {
                const auto found = object->exports.find(name);
                if (found == object->exports.end()) return E_INVALIDARG;
                partial_program->exports.emplace(name, found->second);
                names.push_back(name.c_str());
            }
            for (const UINT index : desc.subobjects) {
                const auto* identity = &args.pSubobjects[index];
                const auto found = object->states.find(identity);
                if (found == object->states.end()) return E_INVALIDARG;
                partial_program->states.push_back(found->second);
                states.push_back(identity);
            }
            const D3D12DDI_GENERIC_PROGRAM_DESC_0108 adapted{desc.name.c_str(),
                static_cast<UINT>(names.size()), names.data(), static_cast<UINT>(states.size()), states.data()};
            const HRESULT hr = gp::compile(*object, adapted, partial_program->compiled, desc.type);
            if (FAILED(hr)) return hr;
            partial_program->description = std::move(desc);
            object->partial_programs.emplace(partial_program->description.name, std::move(partial_program));
        }
        for (const auto* d : programs) {
            if (!gp::name_ok(d->ProgramName) || object->programs.contains(d->ProgramName) ||
                    object->partial_programs.contains(d->ProgramName)) return E_INVALIDARG;
            std::shared_ptr<gp::Program> program;
            HRESULT hr = gp::compile(*object, *d, program); if (FAILED(hr)) return hr;
            object->programs.emplace(d->ProgramName, std::move(program));
        }
        if (executable && object->programs.empty()) return E_INVALIDARG;
        *out = object.release(); return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
      catch (...) { return E_INVALIDARG; }
}
void destroy_generic_state(GenericStateObject* object) noexcept { delete object; }

namespace {
constexpr UINT64 generic_magic = 0x4750524f4752414dull;
D3D12DDI_PROGRAM_IDENTIFIER_0108 APIENTRY generic_identifier(D3D12DDI_HSTATEOBJECT_0054 handle, LPCWSTR name) {
    D3D12DDI_PROGRAM_IDENTIFIER_0108 id{};
    auto* record = record_of<StateObjectRecord>(handle.pDrvPrivate, Tag::StateObject);
    if (record && record->work_graph) return work_graph_identifier(*record, name);
    if (!record || !record->generic || !gp::name_ok(name)) return id;
    const auto it = record->generic->programs.find(name);
    if (it != record->generic->programs.end()) { id.OpaqueData[0] = it->second->id; id.OpaqueData[1] = generic_magic; }
    return id;
}
void APIENTRY generic_set_program(D3D12DDI_HCOMMANDLIST handle, const D3D12DDI_SET_PROGRAM_DESC_0108* desc) {
    auto* list = list_of(handle, "SetProgram"); if (!list) return;
    if (desc && desc->Type == D3D12DDI_PROGRAM_TYPE_WORK_GRAPH_0108) return work_graph_set_program(list, desc->WorkGraph);
    HRESULT hr = E_INVALIDARG;
    std::shared_ptr<gp::Program> program;
    if (desc && desc->Type == D3D12DDI_PROGRAM_TYPE_GENERIC_PIPELINE_0108) {
        const auto& id = desc->GenericPipeline.ProgramIdentifier;
        if (id.OpaqueData[0] && id.OpaqueData[1] == generic_magic && !id.OpaqueData[2] && !id.OpaqueData[3]) {
            std::lock_guard lock(gp::registry_lock);
            auto it = gp::registry.find(id.OpaqueData[0]);
            if (it != gp::registry.end()) program = it->second.lock();
        }
        if (program && program->device == list->h.device) hr = S_OK;
    } else if (desc) hr = E_NOTIMPL;
    if (FAILED(hr)) { list->h.device->report_list(list->rt, hr); return; }
    ID3D12PipelineState* pipeline = program->background ? program->background->ready() : nullptr;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR) && AMDGPU_WDDM_LAB_FLAVOUR
    fprintf(stderr,"PGP compilation select id=%llu specialized=%u\n",static_cast<unsigned long long>(program->id),pipeline != nullptr);
#endif
    if (!pipeline) pipeline=program->pipeline;
    // Neither version is released at publication. Program owns both until
    // destruction; replay owns the selected COM ref and the engine allocator
    // retains partial-linked PSOs until its normal post-fence reset.
    // The pending replay owns a COM reference independently of registry/parent/child record lifetimes.
    pipeline->AddRef();
    record(list, [pipeline](ID3D12GraphicsCommandList* engine) { engine->SetPipelineState(pipeline); pipeline->Release(); });
}
}
void fill_modern_state_object_entries(native12::ModernCoreEntries* core, native12::ModernListEntries* lists) noexcept {
    if (core) core->pfnGetProgramIdentifier = generic_identifier;
    if (lists) lists->pfnSetProgram = generic_set_program;
    fill_work_graph_entries(core, lists);
}
}
#endif
