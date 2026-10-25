// SPDX-License-Identifier: MIT
// Generic programs are named engine PSOs. The identifiers are registry keys, never caller-provided pointers.
#include "shader-container/shader-container.h"
#include "shader-container/dxil-metadata.h"
#include "generic-programs.h"
#if defined(D3D12DDI_BUILD_VERSION_0119)
#include "ddi-modern-core.h"
#include "ddi-modern-list.h"
#include "pso-async.h"
#include "replay.h"
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
    ~State() { if (root) root->Release(); }
};
struct Shader { sc::Container container; sc::dxil::Signatures metadata; };
struct Export { std::shared_ptr<Shader> shader; std::shared_ptr<State> root; };
struct Program;
std::mutex registry_lock;
std::map<UINT64, std::weak_ptr<Program>> registry;
std::atomic<UINT64> next_id{1};
struct Program {
    DeviceContext* device = nullptr;
    ID3D12PipelineState* pipeline = nullptr;
    UINT64 id = 0;
    ~Program() {
        if (id) { std::lock_guard lock(registry_lock); registry.erase(id); }
        if (pipeline) pipeline->Release();
    }
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
    std::map<const void*, std::shared_ptr<gp::State>> states;
    std::map<std::wstring, gp::Export> exports;
    std::map<std::wstring, std::shared_ptr<gp::Program>> programs;
};

namespace gp {
HRESULT compile(GenericStateObject& object, const D3D12DDI_GENERIC_PROGRAM_DESC_0108& desc,
                std::shared_ptr<Program>& result) {
    if (!name_ok(desc.ProgramName) || !valid_count(desc.NumExports, desc.pExports) || !desc.NumExports ||
        !valid_count(desc.NumSubobjects, desc.pSubobjects)) return E_INVALIDARG;
    std::array<const Shader*, 15> shaders{};
    ID3D12RootSignature* root = nullptr;
    for (UINT i = 0; i != desc.NumExports; ++i) {
        if (!name_ok(desc.pExports[i])) return E_INVALIDARG;
        auto e = object.exports.find(desc.pExports[i]); if (e == object.exports.end()) return E_INVALIDARG;
        const auto kind = e->second.shader->container.programType;
        if (kind >= shaders.size() || shaders[kind]) return E_INVALIDARG;
        shaders[kind] = e->second.shader.get();
        if (e->second.root) {
            // The runtime resolves and validates matching root definitions. Different COM objects can
            // represent identical roots (notably two serialized-root subobjects), so identity is not equality.
            if (!root) root = e->second.root->root;
        }
    }
    std::map<D3D12DDI_STATE_SUBOBJECT_TYPE, const State*> selected;
    for (UINT i = 0; i != desc.NumSubobjects; ++i) {
        const auto found = object.states.find(desc.pSubobjects[i]);
        if (found == object.states.end() || !selected.emplace(found->second->type, found->second.get()).second) return E_INVALIDARG;
    }
    auto find = [&](D3D12DDI_STATE_SUBOBJECT_TYPE type) -> const State* { auto it = selected.find(type); return it == selected.end() ? nullptr : it->second; };
    auto value = [&](D3D12DDI_STATE_SUBOBJECT_TYPE type, UINT fallback) { const auto* s = find(type); return s ? s->value : fallback; };
    auto p = std::make_shared<Program>(); p->device = object.device;
    if (shaders[5]) {
        if (desc.NumExports != 1) return E_INVALIDARG;
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{}; d.pRootSignature = root; d.NodeMask = object.node_mask;
        d.CS = {shaders[5]->container.bytes.data(), shaders[5]->container.bytes.size()};
        HRESULT hr = object.device->device->CreateComputePipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&p->pipeline));
        if (FAILED(hr)) return hr;
    } else {
        if ((!shaders[1] && !shaders[13]) || (shaders[1] && shaders[13])) return E_INVALIDARG;
        SubobjectStream stream;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
        const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE stages[] = {D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS,
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS};
        for (UINT i = 0; i != shaders.size(); ++i) if (shaders[i]) {
            const auto type = i < 5 ? stages[i] : i == 13 ? D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS : D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
            const D3D12_SHADER_BYTECODE code{shaders[i]->container.bytes.data(), shaders[i]->container.bytes.size()}; stream.put(type, code);
        }
        const auto* blend = find(D3D12DDI_STATE_SUBOBJECT_TYPE_BLEND);
        const auto* raster = find(D3D12DDI_STATE_SUBOBJECT_TYPE_RASTERIZER);
        const auto* depth = find(D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL);
        const auto* formats = find(D3D12DDI_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS);
        const auto* samples = find(D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_DESC);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend ? blend->blend : default_blend());
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, raster ? raster->raster : default_raster());
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, depth ? depth->depth : default_depth());
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, value(D3D12DDI_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT_MAX));
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats ? formats->formats : D3D12_RT_FORMAT_ARRAY{});
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, static_cast<DXGI_FORMAT>(value(D3D12DDI_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT_UNKNOWN)));
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, samples ? samples->samples : DXGI_SAMPLE_DESC{1, 0});
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, object.node_mask);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, static_cast<D3D12_PIPELINE_STATE_FLAGS>(value(D3D12DDI_STATE_SUBOBJECT_TYPE_FLAGS, 0)));
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(value(D3D12DDI_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE, 0)));
        const UINT topology = value(D3D12DDI_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, 0);
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
        ID3D12Device2* device2 = nullptr;
        HRESULT hr = object.device->device->QueryInterface(__uuidof(ID3D12Device2), reinterpret_cast<void**>(&device2));
        if (FAILED(hr)) return hr;
        auto d = stream.desc(); hr = device2->CreatePipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&p->pipeline));
        device2->Release(); if (FAILED(hr)) return hr;
    }
    if (!p->pipeline) return E_UNEXPECTED;
    p->id = next_id.fetch_add(1); if (!p->id) return E_UNEXPECTED;
    { std::lock_guard lock(registry_lock); registry.emplace(p->id, p); }
    result = std::move(p); return S_OK;
}
}

bool generic_state_description(const D3D12DDIARG_CREATE_STATE_OBJECT_0054& args) noexcept {
    if (args.Type == D3D12DDI_STATE_OBJECT_TYPE_EXECUTABLE) return true;
    if (args.Type != D3D12DDI_STATE_OBJECT_TYPE_COLLECTION || !gp::valid_count(args.NumSubobjects, args.pSubobjects)) return false;
    try {
        for (UINT i = 0; i != args.NumSubobjects; ++i) {
            const auto& s = args.pSubobjects[i]; if (!s.pDesc) continue;
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
        if (parent) { object->exports = parent->exports; object->programs = parent->programs; object->node_mask = parent->node_mask; }
        std::shared_ptr<gp::State> default_root;
        bool ambiguous_root = false;
        std::vector<const D3D12DDI_GENERIC_PROGRAM_DESC_0108*> programs;
        std::vector<const D3D12DDI_FUNCTION_SUMMARY_0054*> summaries;
        for (UINT i = 0; i != args.NumSubobjects; ++i) {
            const auto& s = args.pSubobjects[i]; if (!s.pDesc) return E_INVALIDARG;
            switch (s.Type) {
            case D3D12DDI_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG: {
                const auto flags = static_cast<const D3D12DDI_STATE_OBJECT_CONFIG_0054*>(s.pDesc)->Flags;
                constexpr UINT allowed = D3D12DDI_STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS |
                    D3D12DDI_STATE_OBJECT_FLAG_ALLOW_LOCAL_DEPENDENCIES_ON_EXTERNAL_DEFINITIONS |
                    D3D12DDI_STATE_OBJECT_FLAG_ALLOW_EXTERNAL_DEPENDENCIES_ON_LOCAL_DEFINITIONS;
                if (flags & ~allowed) return E_NOTIMPL;
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
                        if (!object->exports.emplace(entry).second) return E_INVALIDARG;
                } else for (UINT j = 0; j != a.NumExports; ++j) {
                    const auto& e = a.pExports[j];
                    if (!gp::name_ok(e.Name) || e.Flags || (e.ExportToRename && !gp::name_ok(e.ExportToRename))) return E_INVALIDARG;
                    const auto found = collection.exports.find(e.ExportToRename ? e.ExportToRename : e.Name);
                    if (found == collection.exports.end() || !object->exports.emplace(e.Name, found->second).second) return E_INVALIDARG;
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
                    if (!object->exports.emplace(exported, gp::Export{shader, {}}).second) return E_INVALIDARG;
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
        for (const auto* d : programs) {
            if (!gp::name_ok(d->ProgramName) || object->programs.contains(d->ProgramName)) return E_INVALIDARG;
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
    if (!record || !record->generic || !gp::name_ok(name)) return id;
    const auto it = record->generic->programs.find(name);
    if (it != record->generic->programs.end()) { id.OpaqueData[0] = it->second->id; id.OpaqueData[1] = generic_magic; }
    return id;
}
void APIENTRY generic_set_program(D3D12DDI_HCOMMANDLIST handle, const D3D12DDI_SET_PROGRAM_DESC_0108* desc) {
    auto* list = list_of(handle, "SetProgram"); if (!list) return;
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
    ID3D12PipelineState* pipeline = program->pipeline;
    // The pending replay owns a COM reference independently of registry/parent/child record lifetimes.
    pipeline->AddRef();
    record(list, [pipeline](ID3D12GraphicsCommandList* engine) { engine->SetPipelineState(pipeline); pipeline->Release(); });
}
}
void fill_modern_state_object_entries(native12::ModernCoreEntries* core, native12::ModernListEntries* lists) noexcept {
    if (core) core->pfnGetProgramIdentifier = generic_identifier;
    if (lists) lists->pfnSetProgram = generic_set_program;
}
}
#endif
