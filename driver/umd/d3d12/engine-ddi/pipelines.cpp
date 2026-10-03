// SPDX-License-Identifier: MIT
// engine-ddi: shader intake (D15-D25, D118-D120), pipeline states (D33-D35) and the compute state of a command
// list (L4, L29, L31, L33, L35, L37, L39, L41, L43).
//
// Native intake (engine-ddi.h, "Shaders"). Each create-shader slot rebuilds the DXBC container the engine consumes
// from the DDI payload with shader-container's BuildContainer (shader-container/README.md): pShaderCode is the bare
// program (DXBC tokens, or the DXIL part) with its length in DWORDs in DWORD 1, and the signatures are register-only
// entries. The DDI passes no code size, so DWORD 1 is also the extent BuildContainer may read (INFERENCE from
// _In_reads_(pShaderCode[1]); the runtime's payload has not been measured). The container stays on the shader, in
// engine-ddi's own allocation, until DestroyShader. CreatePipelineState hands its bytes to the engine as
// D3D12_SHADER_BYTECODE, names input elements with InputLayoutSemantic and stream-output entries with
// StreamOutputSemantic.
#include "shader-container/shader-container.h"      // first: it selects the D3D12 tokenized program format header
#include "internal.h"
#include "replay.h"
#include <new>
#include <string>
#include <vector>

namespace engine_ddi {

namespace sc = shader_container;

static_assert(D3D12DDI_INPUT_CLASSIFICIATION_PER_VERTEX_DATA == static_cast<int>(D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA) &&
                  D3D12DDI_INPUT_CLASSIFICIATION_PER_INSTANCE_DATA ==
                      static_cast<int>(D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA),
              "input classification");
static_assert(D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF ==
                  static_cast<int>(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF), "strip cut values");
static_assert(D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_PATCH == static_cast<int>(D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH),
              "primitive topology types");

// Whether the pinned engine takes a stream-output gap, the NULL SemanticName of a hole. Engine 4FFA7493 (fork
// 7bfcd7f0, r3) crashed on one; the pin since r4 (d31d6133) carries the fix (0869138a, c5d9d85f) and takes it. Set to
// false for an engine without the fix: a declaration with a gap is then refused with E_NOTIMPL before it reaches
// the engine.
constexpr bool kEngineTakesStreamOutputGaps = true;

// What a shader keeps: the rebuilt container; its input signature entries, because the DDI arrays live only during
// the create and a vertex program's input layout is named at CreatePipelineState; and for
// CreateGeometryShaderWithStreamOutput its declaration. A stream-output shader without a program of its own
// (pShaderCode NULL) has an empty container.
struct ShaderObject {
    sc::Container container;
    std::vector<D3D12DDIARG_SIGNATURE_ENTRY_0012> input;
    bool stream_output = false;
    std::vector<D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY> so_entries;
    std::vector<UINT> so_strides;
    UINT rasterized_stream = 0;

    bool has_program() const noexcept { return !container.bytes.empty(); }
    bool is(uint32_t type) const noexcept { return has_program() && container.programType == type; }
    D3D12_SHADER_BYTECODE bytecode() const noexcept { return {container.bytes.data(), container.bytes.size()}; }
};

namespace {
enum class Stage { Standard, Tessellation, Mesh };

void refuse(DeviceContext* c, const char* slot, HRESULT hr, const char* why) noexcept {
    log_line("%s: %s (hr %08lx)", slot, why, static_cast<unsigned long>(hr));
    c->report(hr);
}

// The shader object of one create, or null after the failure has been logged and reported. program_optional is set
// for the stream-output slot, whose program may be absent (INFERENCE: the D3D11 DDI convention for stream output
// without a geometry program; how the D3D12 runtime passes stream output from a vertex or domain program is not
// measured).
ShaderObject* build_shader(DeviceContext* c, const D3D12DDIARG_CREATE_SHADER_0026* args, Stage stage,
                           bool program_optional, const char* slot) noexcept {
    if (!args) {
        refuse(c, slot, E_INVALIDARG, "no arguments");
        return nullptr;
    }
    if (stage == Stage::Mesh) {
        refuse(c, slot, E_NOTIMPL, "mesh and amplification programs are not translated");
        return nullptr;
    }
    if (!args->pShaderCode && !program_optional) {
        refuse(c, slot, E_INVALIDARG, "null pShaderCode");
        return nullptr;
    }
    ShaderObject* s = make_new<ShaderObject>();
    if (!s) {
        refuse(c, slot, E_OUTOFMEMORY, "no memory for the shader");
        return nullptr;
    }
    if (!args->pShaderCode) return s;

    sc::ProgramDesc desc;
    desc.code = args->pShaderCode;
    desc.codeCapacity = args->pShaderCode[1];
    if (stage == Stage::Tessellation) {
        if (const D3D12DDIARG_TESSELLATION_IO_SIGNATURES* t = args->IOSignatures.Tessellation) {
            desc.input = {t->pInputSignature, t->NumInputSignatureEntries};
            desc.output = {t->pOutputSignature, t->NumOutputSignatureEntries};
            desc.patchConstant = {t->pPatchConstantSignature, t->NumPatchConstantSignatureEntries};
        }
    } else if (const D3D12DDIARG_STAGE_IO_SIGNATURES* t = args->IOSignatures.Standard) {
        desc.input = {t->pInputSignature, t->NumInputSignatureEntries};
        desc.output = {t->pOutputSignature, t->NumOutputSignatureEntries};
    }
    HRESULT hr = S_OK;
    try {
        const sc::Result r = sc::BuildContainer(desc, &s->container);
        if (r) {
            s->input.assign(desc.input.entries, desc.input.entries + desc.input.count);
        } else {
            log_line("%s: %s", slot, r.detail.c_str());
            hr = r.hresult();
        }
    } catch (const std::bad_alloc&) {
        hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        delete s;
        refuse(c, slot, hr, "no container");
        return nullptr;
    }
    return s;
}

void intake(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_SHADER_0026* args, D3D12DDI_HSHADER h, Stage stage,
            const char* slot, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026* so = nullptr) noexcept {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!h.pDrvPrivate) {
        c->report(E_INVALIDARG);
        return;
    }
    // An inert record first, so that DestroyShader is valid whatever happens below.
    auto* r = new (h.pDrvPrivate) ShaderRecord{{Tag::Shader, 0, nullptr, c}, nullptr};
    ShaderObject* s = build_shader(c, args, stage, so != nullptr, slot);
    if (!s) return;
    if (so) {
        if ((so->NumEntries && !so->pOutputStreamDecl) || (so->NumStrides && !so->BufferStridesInBytes) ||
            so->NumEntries > D3D12_SO_STREAM_COUNT * D3D12_SO_OUTPUT_COMPONENT_COUNT ||
            so->NumStrides > D3D12_SO_BUFFER_SLOT_COUNT) {
            delete s;
            refuse(c, slot, E_INVALIDARG, "stream-output declaration or strides out of range");
            return;
        }
        try {
            s->so_entries.assign(so->pOutputStreamDecl, so->pOutputStreamDecl + so->NumEntries);
            s->so_strides.assign(so->BufferStridesInBytes, so->BufferStridesInBytes + so->NumStrides);
        } catch (const std::bad_alloc&) {
            delete s;
            refuse(c, slot, E_OUTOFMEMORY, "no memory for the stream-output declaration");
            return;
        }
        s->stream_output = true;
        s->rasterized_stream = so->RasterizedStream;
    }
    r->object = s;
}

SIZE_T APIENTRY calc_shader(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SHADER_0026*) { return sizeof(ShaderRecord); }
SIZE_T APIENTRY calc_gs_so(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026*) {
    return sizeof(ShaderRecord);
}

void APIENTRY create_vs(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Standard, "CreateVertexShader");
}
void APIENTRY create_ps(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Standard, "CreatePixelShader");
}
void APIENTRY create_gs(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Standard, "CreateGeometryShader");
}
void APIENTRY create_cs(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Standard, "CreateComputeShader");
}
void APIENTRY create_gs_so(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026* a,
                           D3D12DDI_HSHADER h) {
    intake(d, a ? &a->CreateShader : nullptr, h, Stage::Standard, "CreateGeometryShaderWithStreamOutput", a);
}
void APIENTRY create_hs(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Tessellation, "CreateHullShader");
}
void APIENTRY create_ds(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Tessellation, "CreateDomainShader");
}
void APIENTRY create_as(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Mesh, "CreateAmplificationShader");
}
void APIENTRY create_ms(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Mesh, "CreateMeshShader");
}

void APIENTRY destroy_shader(D3D12DDI_HDEVICE device, D3D12DDI_HSHADER h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<ShaderRecord>(h.pDrvPrivate, Tag::Shader, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    delete r->object;
    r->object = nullptr;
    poison(r->h);
}

// ---- Pipeline states -----------------------------------------------------------------------------------------------
SIZE_T APIENTRY calc_pipeline(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075*) {
    return sizeof(PipelineRecord);
}

// The shader behind a pipeline's handle: null for a null handle. A handle that is not a live shader of this device
// with a program of the given type sets hr to E_INVALIDARG.
const ShaderObject* stage_shader(DeviceContext* c, D3D12DDI_HSHADER h, uint32_t type, HRESULT& hr) noexcept {
    if (!h.pDrvPrivate) return nullptr;
    const auto* r = record_of<ShaderRecord>(h.pDrvPrivate, Tag::Shader, c);
    if (!r || !r->object || !r->object->is(type)) {
        hr = E_INVALIDARG;
        return nullptr;
    }
    return r->object;
}

// A state object behind a pipeline's handle: null for a null handle; one that is not a live, valid state of this
// device sets hr to E_INVALIDARG.
template <class R> const R* state_of(DeviceContext* c, void* storage, Tag tag, HRESULT& hr) noexcept {
    if (!storage) return nullptr;
    const R* r = record_of<R>(storage, tag, c);
    if (!r || (r->h.flags & kRecordInvalid)) {
        hr = E_INVALIDARG;
        return nullptr;
    }
    return r;
}

// The D3D12 API defaults, for a state handle that is null (INFERENCE: the runtime is taken to pass all three states
// with every graphics pipeline; nothing has been measured).
D3D12_BLEND_DESC default_blend() noexcept {
    D3D12_BLEND_DESC d{};
    for (D3D12_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget)
        rt = {FALSE, FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO,
              D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL)};
    return d;
}
D3D12_RASTERIZER_DESC default_rasterizer() noexcept {
    return {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_BACK, FALSE, D3D12_DEFAULT_DEPTH_BIAS, D3D12_DEFAULT_DEPTH_BIAS_CLAMP,
            D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS, TRUE, FALSE, FALSE, 0, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF};
}
D3D12_DEPTH_STENCIL_DESC default_depth_stencil() noexcept {
    const D3D12_DEPTH_STENCILOP_DESC keep{D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                          D3D12_COMPARISON_FUNC_ALWAYS};
    return {TRUE, D3D12_DEPTH_WRITE_MASK_ALL, D3D12_COMPARISON_FUNC_LESS, FALSE, D3D12_DEFAULT_STENCIL_READ_MASK,
            D3D12_DEFAULT_STENCIL_WRITE_MASK, keep, keep};
}

HRESULT compute_pipeline(DeviceContext* c, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075& a, ID3D12RootSignature* root,
                         ID3D12PipelineState** out) noexcept {
    HRESULT hr = S_OK;
    const ShaderObject* cs = stage_shader(c, a.hComputeShader, D3D11_SB_COMPUTE_SHADER, hr);
    if (!cs) return E_INVALIDARG;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS = cs->bytecode();
    return c->device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
}

HRESULT graphics_pipeline(DeviceContext* c, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075& a, ID3D12RootSignature* root,
                          ID3D12PipelineState** out) noexcept try {
    if (a.ViewInstancingDesc.ViewInstanceCount) return E_NOTIMPL;           // ViewInstancingTier NOT_SUPPORTED
    if (a.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) return E_INVALIDARG;
    HRESULT hr = S_OK;
    const ShaderObject* vs = stage_shader(c, a.hVertexShader, D3D10_SB_VERTEX_SHADER, hr);
    const ShaderObject* ps = stage_shader(c, a.hPixelShader, D3D10_SB_PIXEL_SHADER, hr);
    const ShaderObject* hs = stage_shader(c, a.hHullShader, D3D11_SB_HULL_SHADER, hr);
    const ShaderObject* ds = stage_shader(c, a.hDomainShader, D3D11_SB_DOMAIN_SHADER, hr);
    // The geometry slot holds a geometry program, or a stream-output shader whose program, if any, is the vertex or
    // domain program the pipeline binds in its own slot.
    const ShaderObject* gs = nullptr;
    if (a.hGeometryShader.pDrvPrivate) {
        const auto* r = record_of<ShaderRecord>(a.hGeometryShader.pDrvPrivate, Tag::Shader, c);
        gs = r ? r->object : nullptr;
        const bool usable = gs && (gs->is(D3D10_SB_GEOMETRY_SHADER) ||
                                   (gs->stream_output && (!gs->has_program() || gs->is(D3D10_SB_VERTEX_SHADER) ||
                                                          gs->is(D3D11_SB_DOMAIN_SHADER))));
        if (!usable) hr = E_INVALIDARG;
    }
    const auto* layout = state_of<ElementLayoutRecord>(c, a.hElementLayout.pDrvPrivate, Tag::ElementLayout, hr);
    const auto* blend = state_of<BlendStateRecord>(c, a.hBlendState.pDrvPrivate, Tag::StateBlend, hr);
    const auto* raster = state_of<RasterizerStateRecord>(c, a.hRasterizerState.pDrvPrivate, Tag::StateRaster, hr);
    const auto* depth = state_of<DepthStencilStateRecord>(c, a.hDepthStencilState.pDrvPrivate, Tag::StateDepth, hr);
    if (FAILED(hr) || !vs) return E_INVALIDARG;
    if (depth && depth->depth_bounds) return E_NOTIMPL;                     // DepthBoundsTestSupported FALSE

    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root;
    d.VS = vs->bytecode();
    if (ps) d.PS = ps->bytecode();
    if (hs) d.HS = hs->bytecode();
    if (ds) d.DS = ds->bytecode();
    if (gs && gs->is(D3D10_SB_GEOMETRY_SHADER)) d.GS = gs->bytecode();

    // Stream output, named against the last stage before rasterization.
    std::vector<sc::StreamOutputElement> so_elements;
    std::vector<D3D12_SO_DECLARATION_ENTRY> so;
    if (gs && gs->stream_output) {
        const sc::Container& last = gs->is(D3D10_SB_GEOMETRY_SHADER) ? gs->container : ds ? ds->container : vs->container;
        so_elements.resize(gs->so_entries.size());
        so.resize(gs->so_entries.size());
        for (size_t i = 0; i < so.size(); ++i) {
            const D3D12DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY& e = gs->so_entries[i];
            if (e.OutputSlot >= D3D12_SO_BUFFER_SLOT_COUNT) return E_INVALIDARG;
            const sc::Result r = sc::StreamOutputSemantic(last, e, &so_elements[i]);
            if (!r) {
                log_line("CreatePipelineState: stream-output entry %zu: %s", i, r.detail.c_str());
                return r.hresult();
            }
            const sc::StreamOutputElement& el = so_elements[i];
            if constexpr (!kEngineTakesStreamOutputGaps) {
                if (el.gap) {
                    log_line("CreatePipelineState: stream-output entry %zu is a gap, which the pinned engine does not take",
                             i);
                    return E_NOTIMPL;
                }
            }
            so[i] = {e.Stream, el.gap ? nullptr : el.semantic.name.c_str(), el.semantic.index, el.startComponent,
                     el.componentCount, static_cast<BYTE>(e.OutputSlot)};
        }
        d.StreamOutput = {so.data(), static_cast<UINT>(so.size()), gs->so_strides.data(),
                          static_cast<UINT>(gs->so_strides.size()), gs->rasterized_stream};
    }

    d.BlendState = blend ? blend->desc : default_blend();
    d.SampleMask = a.SampleMask;
    d.RasterizerState = raster ? raster->desc : default_rasterizer();
    d.DepthStencilState = depth ? depth->desc : default_depth_stencil();

    // Input elements: the semantic of the vertex program's rebuilt input signature entry on each element's register.
    std::vector<sc::Semantic> names;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    if (layout && layout->count) {
        names.resize(layout->count);
        elements.resize(layout->count);
        const sc::SignatureView input{vs->input.data(), static_cast<UINT>(vs->input.size())};
        for (UINT i = 0; i < layout->count; ++i) {
            const D3D12DDIARG_INPUT_ELEMENT_DESC& e = layout->elements()[i];
            const sc::Result r = sc::InputLayoutSemantic(input, e.InputRegister, &names[i]);
            if (!r) {
                log_line("CreatePipelineState: input element %u: %s", i, r.detail.c_str());
                return r.hresult();
            }
            elements[i] = {names[i].name.c_str(), names[i].index, e.Format, e.InputSlot, e.AlignedByteOffset,
                           static_cast<D3D12_INPUT_CLASSIFICATION>(e.InputSlotClass), e.InstanceDataStepRate};
        }
        d.InputLayout = {elements.data(), layout->count};
    }

    d.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(a.IBStripCutValue);
    d.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(a.PrimitiveTopologyType);
    d.NumRenderTargets = a.NumRenderTargets;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) d.RTVFormats[i] = a.RTVFormats[i];
    d.DSVFormat = a.DSVFormat;
    d.SampleDesc = a.SampleDesc;
    return c->device->CreateGraphicsPipelineState(&d, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(out));
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

HRESULT APIENTRY create_pipeline(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* args,
                                 D3D12DDI_HPIPELINESTATE h, D3D12DDI_HRTPIPELINESTATE rt) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate || args->NodeMask > 1) return E_INVALIDARG;
    if (args->LibraryReference.hLibrary.pDrvPrivate) return E_NOTIMPL;      // pipeline libraries (P4)
    if (args->hMeshShader.pDrvPrivate || args->hAmplificationShader.pDrvPrivate) return E_NOTIMPL;  // mesh (P4)
    const bool graphics = args->hVertexShader.pDrvPrivate || args->hPixelShader.pDrvPrivate ||
                          args->hDomainShader.pDrvPrivate || args->hHullShader.pDrvPrivate ||
                          args->hGeometryShader.pDrvPrivate;
    if (graphics == (args->hComputeShader.pDrvPrivate != nullptr)) return E_INVALIDARG;    // exactly one kind
    auto* rs = record_of<RootSignatureRecord>(args->hRootSignature.pDrvPrivate, Tag::RootSignature, c);
    if (!rs) return E_INVALIDARG;
    auto* root = static_cast<ID3D12RootSignature*>(rs->h.engine);
    ID3D12PipelineState* pso = nullptr;
    const HRESULT hr = graphics ? graphics_pipeline(c, *args, root, &pso) : compute_pipeline(c, *args, root, &pso);
    if (FAILED(hr)) return hr;
    new (h.pDrvPrivate) PipelineRecord{{Tag::PipelineState, 0, pso, c}, rt, !graphics};
    c->live.fetch_add(1);
    return S_OK;
}

void APIENTRY destroy_pipeline(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINESTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<PipelineRecord>(h.pDrvPrivate, Tag::PipelineState, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    drain_all(c, Drain::Destroy);
    release_engine(r->h);
    poison(r->h);
    c->live.fetch_sub(1);
}

// ---- Command-list slots -----------------------------------------------------------------------------------------
using List = ID3D12GraphicsCommandList;

void APIENTRY set_pipeline_state(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HPIPELINESTATE h) {
    CommandListRecord* l = list_of(hlist, "SetPipelineState");
    if (!l) return;
    // No pipeline is a state of its own: a reset list starts with it, and the runtime writes it as default.
    auto* p = record_of<PipelineRecord>(h.pDrvPrivate, Tag::PipelineState, l->h.device);
    if (h.pDrvPrivate && !p) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    ID3D12PipelineState* pso = p ? static_cast<ID3D12PipelineState*>(p->h.engine) : nullptr;
    record(l, [=](List* e) { e->SetPipelineState(pso); });
}

void APIENTRY set_compute_root_signature(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HROOTSIGNATURE h) {
    CommandListRecord* l = list_of(hlist, "SetComputeRootSignature");
    if (!l) return;
    auto* r = record_of<RootSignatureRecord>(h.pDrvPrivate, Tag::RootSignature, l->h.device);
    if (h.pDrvPrivate && !r) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    ID3D12RootSignature* signature = r ? static_cast<ID3D12RootSignature*>(r->h.engine) : nullptr;
    l->root_signatures[0] = r ? h.pDrvPrivate : nullptr;
    record(l, [=](List* e) { e->SetComputeRootSignature(signature); });
}

void APIENTRY set_compute_root_table(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootDescriptorTable")) {
        const D3D12_GPU_DESCRIPTOR_HANDLE handle{base.ptr};
        record(l, [=](List* e) { e->SetComputeRootDescriptorTable(index, handle); });
    }
}

void APIENTRY set_compute_root_constant(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT data, UINT offset) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRoot32BitConstant"))
        record(l, [=](List* e) { e->SetComputeRoot32BitConstant(index, data, offset); });
}

void APIENTRY set_compute_root_constants(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT count, const void* data,
                                         UINT offset) {
    CommandListRecord* l = list_of(hlist, "SetComputeRoot32BitConstants");
    if (!l) return;
    if (count && !data) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    record(l, [=](List* e, const UINT* d) { e->SetComputeRoot32BitConstants(index, count, d, offset); },
           in(static_cast<const UINT*>(data), count));
}

void APIENTRY set_compute_root_cbv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootConstantBufferView"))
        record(l, [=](List* e) { e->SetComputeRootConstantBufferView(index, va); });
}

void APIENTRY set_compute_root_srv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootShaderResourceView"))
        record(l, [=](List* e) { e->SetComputeRootShaderResourceView(index, va); });
}

void APIENTRY set_compute_root_uav(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootUnorderedAccessView"))
        record(l, [=](List* e) { e->SetComputeRootUnorderedAccessView(index, va); });
}

void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST hlist, UINT x, UINT y, UINT z) {
    if (CommandListRecord* l = list_of(hlist, "Dispatch")) record(l, [=](List* e) { e->Dispatch(x, y, z); });
}
} // namespace

void fill_core_pipelines(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCalcPrivateShaderSize = calc_shader;
    t->pfnCreateVertexShader = create_vs;
    t->pfnCreatePixelShader = create_ps;
    t->pfnCreateGeometryShader = create_gs;
    t->pfnCreateComputeShader = create_cs;
    t->pfnCalcPrivateGeometryShaderWithStreamOutput = calc_gs_so;
    t->pfnCreateGeometryShaderWithStreamOutput = create_gs_so;
    t->pfnCalcPrivateTessellationShaderSize = calc_shader;
    t->pfnCreateHullShader = create_hs;
    t->pfnCreateDomainShader = create_ds;
    t->pfnDestroyShader = destroy_shader;
    t->pfnCreateAmplificationShader = create_as;
    t->pfnCreateMeshShader = create_ms;
    t->pfnCalcPrivateMeshShaderSize = calc_shader;
    t->pfnCalcPrivatePipelineStateSize = calc_pipeline;
    t->pfnCreatePipelineState = create_pipeline;
    t->pfnDestroyPipelineState = destroy_pipeline;
}

void fill_list_pipelines(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t) noexcept {
    t->pfnDispatch = dispatch;
    t->pfnSetPipelineState = set_pipeline_state;
    t->pfnSetComputeRootSignature = set_compute_root_signature;
    t->pfnSetComputeRootDescriptorTable = set_compute_root_table;
    t->pfnSetComputeRoot32BitConstant = set_compute_root_constant;
    t->pfnSetComputeRoot32BitConstants = set_compute_root_constants;
    t->pfnSetComputeRootConstantBufferView = set_compute_root_cbv;
    t->pfnSetComputeRootShaderResourceView = set_compute_root_srv;
    t->pfnSetComputeRootUnorderedAccessView = set_compute_root_uav;
}

} // namespace engine_ddi
