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
//
// Pipeline libraries (D85-D90). The engine object of a library is its ID3D12PipelineLibrary1, from the engine's own
// ID3D12Device1::CreatePipelineLibrary. Two things have to be decided here, because the DDI and the engine's
// interface do not describe a library the same way; both are written out at "Pipeline libraries" below. A create
// whose LibraryReference names a library is a load out of that library, not a new pipeline.
//
// Pipeline states. CreatePipelineState makes three kinds of pipeline: a compute pipeline, a vertex-side graphics
// pipeline, and, when the mesh handle is set, a mesh pipeline. The two rasterizing kinds are both created through
// the engine's ID3D12Device2::CreatePipelineState with a subobject stream (SubobjectStream below), not through a
// D3D12_GRAPHICS_PIPELINE_STATE_DESC: that structure carries neither the AS and MS stages of a mesh pipeline nor
// DepthBoundsTestEnable (L52) nor the view instancing description (L57), and the engine's stream entry carries all
// three. A mesh pipeline's stage linkage is by semantic name, so the mesh and amplification containers are named
// from their own DXIL metadata and the pixel container is rebuilt that way for the pipeline (shader-container.h,
// Slot). 1006 reports a mesh shader tier only when the engine does (caps.cpp).
//
// Asynchronous compilation (BD-158, set_pso_policy in engine-ddi.h, pso-async.h). With the compile pool on, a
// create validates the payload and builds its plan on the calling thread exactly as the synchronous path does, and
// then hands the plan to the pool instead of calling the engine: a game that creates its pipelines from one thread
// compiles them on every core. Two kinds of create stay on the calling thread whatever the policy says: a load out
// of a pipeline library, whose failure the application asked for and expects here, and any create the pool could
// not take. The consumers of a pipeline (pfnSetPipelineState, pfnAddPipelineStateToLibrary and the destroy) go
// through pipeline_engine below, which waits for the job or runs it on its own thread.
#include "shader-container/shader-container.h"      // first: it selects the D3D12 tokenized program format header
#include "internal.h"
#include "pso-async.h"
#include "replay.h"
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
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
// View instancing (L57). The DDI location (H:6256-6260) and the API location are the same two UINT in the same
// order, and the two flag sets are equal (H:6262-6266), so a location is copied field by field below and the flags
// pass through by value.
static_assert(sizeof(D3D12DDI_VIEW_INSTANCE_LOCATION) == sizeof(D3D12_VIEW_INSTANCE_LOCATION) &&
                  offsetof(D3D12DDI_VIEW_INSTANCE_LOCATION, ViewportArrayIndex) ==
                      offsetof(D3D12_VIEW_INSTANCE_LOCATION, ViewportArrayIndex) &&
                  offsetof(D3D12DDI_VIEW_INSTANCE_LOCATION, RenderTargetArrayIndex) ==
                      offsetof(D3D12_VIEW_INSTANCE_LOCATION, RenderTargetArrayIndex),
              "view instance location");
static_assert(D3D12DDI_VIEW_INSTANCING_FLAG_NONE == static_cast<int>(D3D12_VIEW_INSTANCING_FLAG_NONE) &&
                  D3D12DDI_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING ==
                      static_cast<int>(D3D12_VIEW_INSTANCING_FLAG_ENABLE_VIEW_INSTANCE_MASKING),
              "view instancing flags");

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
enum class Stage { Standard, Tessellation, Mesh, Amplification };

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
    desc.slot = stage == Stage::Mesh ? sc::Slot::Mesh
                : stage == Stage::Amplification ? sc::Slot::Amplification
                                                : sc::Slot::Standard;
    if (stage == Stage::Tessellation) {
        if (const D3D12DDIARG_TESSELLATION_IO_SIGNATURES* t = args->IOSignatures.Tessellation) {
            desc.input = {t->pInputSignature, t->NumInputSignatureEntries};
            desc.output = {t->pOutputSignature, t->NumOutputSignatureEntries};
            desc.patchConstant = {t->pPatchConstantSignature, t->NumPatchConstantSignatureEntries};
        }
    } else if (stage == Stage::Mesh) {
        // The mesh union member: the vertex output signature goes to OSG1, the primitive output signature to
        // PSG1 (shader-container.h, Slot).
        if (const D3D12DDIARG_MESH_IO_SIGNATURES* t = args->IOSignatures.Mesh) {
            desc.output = {t->pVertexOutputSignature, t->NumVertexOutputSignatureEntries};
            desc.patchConstant = {t->pPrimitiveOutputSignature, t->NumPrimitiveOutputSignatureEntries};
        }
    } else if (stage == Stage::Amplification) {
        // An amplification program has no signature: its one output is the task payload, which the program
        // declares itself, and the engine takes it from the DXIL metadata. Which union member the runtime fills
        // for this slot is not measured, so none is read; the container gets empty signature chunks, which is
        // what dxc writes for an amplification program.
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
    intake(d, a, h, Stage::Amplification, "CreateAmplificationShader");
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
    // Field by field, for the reason create_blend_state (graphics.cpp) states: the three padding bytes after
    // RenderTargetWriteMask stay zero, so one payload always ships the same bytes.
    for (D3D12_RENDER_TARGET_BLEND_DESC& rt : d.RenderTarget) {
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = static_cast<UINT8>(D3D12_COLOR_WRITE_ENABLE_ALL);
    }
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

// The engine's name for the pipeline at one library index: eight upper-case hexadecimal digits and a terminator,
// written as L"%08X" with swprintf_s at each of the two places that need one. The literal format stays at the call
// site on purpose: a format out of a variable is C4774 under /W4, and swprintf_s's own _Post_z_ is what proves the
// name terminated where it is used. See "Pipeline libraries" below for why a library index is named at all.
inline constexpr size_t kLibraryNameChars = 9;                      // eight hexadecimal digits and the terminator

// Where a pipeline create puts its result: the engine device, or one of its pipeline libraries, under the name of
// the index the payload's LibraryReference named (H:3192-3196, H:9917). A library target means the runtime is
// loading a pipeline the application stored, not creating a new one. The engine then answers E_INVALIDARG both for
// a name the library does not hold and for a description that does not match the one stored under that name
// (vkd3d-proton cache.c, d3d12_pipeline_library_load_pipeline, which compares the description's compatibility
// hash with the stored pipeline's), which is what D3D12 shows the application for either mistake.
struct LibraryTarget {
    ID3D12PipelineLibrary1* library = nullptr;  // null: an ordinary create on the engine device
    const WCHAR* name = nullptr;                // the index's name; valid for the length of the create
};

// The bytecode of one stage, copied into the plan (pso-async.h, the plan rule).
D3D12_SHADER_BYTECODE plan_code(PsoPlan& plan, uint32_t stage, const ShaderObject* s) {
    if (!s || !s->has_program()) return {};
    const D3D12_SHADER_BYTECODE b = s->bytecode();
    return plan.take_code(stage, b.pShaderBytecode, b.BytecodeLength);
}

HRESULT compute_plan(DeviceContext* c, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075& a, ID3D12RootSignature* root,
                     PsoPlan& plan) noexcept try {
    HRESULT hr = S_OK;
    const ShaderObject* cs = stage_shader(c, a.hComputeShader, D3D11_SB_COMPUTE_SHADER, hr);
    if (!cs) return E_INVALIDARG;
    plan.compute = true;
    plan.root = root;
    plan.compute_desc.pRootSignature = root;
    plan.compute_desc.CS = plan_code(plan, kPsoStageCS, cs);
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

// ---- The subobject stream ---------------------------------------------------------------------------------------
// Every rasterizing pipeline reaches the engine through ID3D12Device2::CreatePipelineState and a subobject stream
// (vkd3d-proton state.c, vkd3d_pipeline_state_desc_from_d3d12_stream_desc: the AS, MS, DEPTH_STENCIL1 and
// VIEW_INSTANCING subobjects exist there and nowhere else), so engine-ddi writes that stream. SubobjectStream and
// its layout rules live in pso-async.h, next to the plan that holds one, because a plan outlives the create call.

// A load out of a pipeline library: the application stored this pipeline and asks the runtime for it again, so the
// engine's answer (E_INVALIDARG for a name the library does not hold, and for a description whose compatibility
// hash does not match the stored one) is what the application expects from its own call. This never goes to the
// compile pool.
HRESULT load_from_library(PsoPlan& plan, const LibraryTarget& target, ID3D12PipelineState** out) noexcept {
    if (plan.compute)
        return target.library->LoadComputePipeline(target.name, &plan.compute_desc, __uuidof(ID3D12PipelineState),
                                                   reinterpret_cast<void**>(out));
    const D3D12_PIPELINE_STATE_STREAM_DESC desc = plan.stream.desc();
    return target.library->LoadPipeline(target.name, &desc, __uuidof(ID3D12PipelineState),
                                        reinterpret_cast<void**>(out));
}

// The depth-stencil state as DEPTH_STENCIL1, which is DEPTH_STENCIL plus DepthBoundsTestEnable (L52). The engine
// promotes both subobjects into the same internal state (vkd3d-proton state.c, d3d12_promote_depth_stencil_desc
// and d3d12_promote_depth_stencil_desc1), so writing the longer one costs nothing and carries the bound test.
// graphics.cpp captures DepthBoundsTestEnable from D3D12DDI_DEPTH_STENCIL_DESC_0025 into the state record; the
// command-list side of the feature is pfnOMSetDepthBounds (L52, commands.cpp), and 1006 reports
// DepthBoundsTestSupported from the engine's own OPTIONS2 answer, which is its Vulkan device's depthBounds
// feature (caps.cpp). On a device without it the engine refuses the pipeline with E_INVALIDARG
// (vkd3d-proton state.c, d3d12_pipeline_state_init_graphics_create_info), so nothing here can promise the test
// where it does not work.
D3D12_DEPTH_STENCIL_DESC1 depth_stencil1(const DepthStencilStateRecord* depth) noexcept {
    const D3D12_DEPTH_STENCIL_DESC d = depth ? depth->desc : default_depth_stencil();
    D3D12_DEPTH_STENCIL_DESC1 out{};
    out.DepthEnable = d.DepthEnable;
    out.DepthWriteMask = d.DepthWriteMask;
    out.DepthFunc = d.DepthFunc;
    out.StencilEnable = d.StencilEnable;
    out.StencilReadMask = d.StencilReadMask;
    out.StencilWriteMask = d.StencilWriteMask;
    out.FrontFace = d.FrontFace;
    out.BackFace = d.BackFace;
    out.DepthBoundsTestEnable = depth ? depth->depth_bounds : FALSE;
    return out;
}

// The subobjects a graphics and a mesh pipeline share, in one place so that the two paths cannot drift apart: the
// root signature, the pixel program when there is one, the three states, the sample mask, the render target and
// depth-stencil formats, the sample description and the node mask. The caller has already refused a
// NumRenderTargets above D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT.
void put_common_state(SubobjectStream& stream, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075& a,
                      ID3D12RootSignature* root, const D3D12_SHADER_BYTECODE& ps, const BlendStateRecord* blend,
                      const RasterizerStateRecord* raster, const DepthStencilStateRecord* depth) {
    stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, root);
    if (ps.pShaderBytecode) stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, ps);
    stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend ? blend->desc : default_blend());
    stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, a.SampleMask);
    if (raster && raster->use_modern) stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, raster->modern);
    else stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, raster ? raster->desc : default_rasterizer());
    if (depth && depth->use_modern) stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL2, depth->modern);
    else stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, depth_stencil1(depth));
    D3D12_RT_FORMAT_ARRAY formats{};
    formats.NumRenderTargets = a.NumRenderTargets;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) formats.RTFormats[i] = a.RTVFormats[i];
    stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
    stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, a.DSVFormat);
    stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, a.SampleDesc);
    stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK, a.NodeMask);
}

// The VIEW_INSTANCING subobject (L57), or nothing at all when the pipeline has no view instances: a zero
// ViewInstanceCount is what the engine's default already holds, and an empty description would only use up the
// subobject type. The engine reads pViewInstanceLocations inside its create (vkd3d-proton state.c,
// d3d12_pipeline_state_validate_view_instancing, which folds the locations into the pipeline's multiview state),
// so the copy lives in the caller's frame for the length of the call and no longer. D3D12_MAX_VIEW_INSTANCE_COUNT
// is 4 and the engine refuses more, so a larger count is a broken DDI payload and is refused here; so is a count
// without an array. 1006 reports ViewInstancingTier from the engine's own OPTIONS3 answer (caps.cpp), and the
// command-list side is pfnSetViewInstanceMask (L57, commands.cpp).
HRESULT put_view_instancing(PsoPlan& plan, const D3D12DDI_VIEW_INSTANCING_DESC& in) {
    if (!in.ViewInstanceCount) return S_OK;
    if (in.ViewInstanceCount > D3D12_MAX_VIEW_INSTANCE_COUNT || !in.pViewInstanceLocations) {
        log_line("CreatePipelineState: %u view instances with %s locations, the contract allows at most %u with an "
                 "array", in.ViewInstanceCount, in.pViewInstanceLocations ? "an array of" : "no",
                 static_cast<unsigned>(D3D12_MAX_VIEW_INSTANCE_COUNT));
        return E_INVALIDARG;
    }
    plan.locations.resize(in.ViewInstanceCount);                // sized once, then written in place
    for (UINT i = 0; i < in.ViewInstanceCount; ++i)
        plan.locations[i] = {in.pViewInstanceLocations[i].ViewportArrayIndex,
                             in.pViewInstanceLocations[i].RenderTargetArrayIndex};
    D3D12_VIEW_INSTANCING_DESC out{};
    out.ViewInstanceCount = in.ViewInstanceCount;
    out.pViewInstanceLocations = plan.locations.data();
    out.Flags = static_cast<D3D12_VIEW_INSTANCING_FLAGS>(in.Flags);
    plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING, out);
    return S_OK;
}

// The pixel program of a mesh pipeline, rebuilt so that its ISG1 carries the semantic names of its own DXIL
// metadata. The engine checks a mesh pipeline's linkage by semantic name and index
// (vkd3d-proton state.c, vkd3d_validate_mesh_shader_io_signatures), and a mesh shader's primitive outputs have a
// register space of their own, so the synthetic names of the vertex path cannot link them
// (shader-container.h, Slot). A pixel program with no input entry needs no rebuild: there is nothing to link.
// Not noexcept: sc::BuildMeshLinkContainer allocates (the chunk vectors, the string of a refused program), and a
// std::bad_alloc from it belongs to the function-try-block of mesh_pipeline, which turns it into E_OUTOFMEMORY. A
// noexcept here would call std::terminate instead and take the application down.
HRESULT mesh_pixel_bytecode(PsoPlan& plan, const ShaderObject* ps, D3D12_SHADER_BYTECODE* out) {
    if (!ps || !ps->has_program()) return S_OK;
    if (ps->input.empty()) {
        *out = plan_code(plan, kPsoStagePS, ps);
        return S_OK;
    }
    sc::Container rebuilt;
    const sc::SignatureView input{ps->input.data(), static_cast<UINT>(ps->input.size())};
    const sc::SignatureView output{ps->container.output.data(), static_cast<UINT>(ps->container.output.size())};
    const sc::Result r = sc::BuildMeshLinkContainer(ps->container, input, output, &rebuilt);
    if (!r) {
        log_line("CreatePipelineState: mesh pipeline pixel program: %s", r.detail.c_str());
        return r.hresult();
    }
    *out = plan.take_code(kPsoStagePS, rebuilt.bytes.data(), rebuilt.bytes.size());
    return S_OK;
}

HRESULT mesh_plan(DeviceContext* c, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075& a, ID3D12RootSignature* root,
                  PsoPlan& plan) noexcept try {
    if (!c->device4) return E_NOTIMPL;                                      // no ID3D12Device2 on the engine
    if (a.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) return E_INVALIDARG;
    // A mesh pipeline has no vertex-side stage at all: the runtime must pass none of them (DirectX-Specs
    // MeshShader.md, "the mesh shader pipeline"), and the engine refuses the combination anyway.
    if (a.hVertexShader.pDrvPrivate || a.hHullShader.pDrvPrivate || a.hDomainShader.pDrvPrivate ||
        a.hGeometryShader.pDrvPrivate || a.hComputeShader.pDrvPrivate)
        return E_INVALIDARG;
    if (a.hElementLayout.pDrvPrivate) return E_INVALIDARG;                  // no input layout on a mesh pipeline

    HRESULT hr = S_OK;
    const ShaderObject* ms = stage_shader(c, a.hMeshShader, sc::kDxilProgramKindMesh, hr);
    const ShaderObject* as = stage_shader(c, a.hAmplificationShader, sc::kDxilProgramKindAmplification, hr);
    const ShaderObject* ps = stage_shader(c, a.hPixelShader, D3D10_SB_PIXEL_SHADER, hr);
    const auto* blend = state_of<BlendStateRecord>(c, a.hBlendState.pDrvPrivate, Tag::StateBlend, hr);
    const auto* raster = state_of<RasterizerStateRecord>(c, a.hRasterizerState.pDrvPrivate, Tag::StateRaster, hr);
    const auto* depth = state_of<DepthStencilStateRecord>(c, a.hDepthStencilState.pDrvPrivate, Tag::StateDepth, hr);
    if (FAILED(hr) || !ms) return E_INVALIDARG;

    plan.root = root;
    D3D12_SHADER_BYTECODE ps_bytes{};
    if (HRESULT r = mesh_pixel_bytecode(plan, ps, &ps_bytes); FAILED(r)) return r;

    if (as) plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS, plan_code(plan, kPsoStageAS, as));
    plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS, plan_code(plan, kPsoStageMS, ms));
    put_common_state(plan.stream, a, root, ps_bytes, blend, raster, depth);
    if (HRESULT r = put_view_instancing(plan, a.ViewInstancingDesc); FAILED(r)) return r;
    // No INPUT_LAYOUT, IB_STRIP_CUT_VALUE or PRIMITIVE_TOPOLOGY subobject: a mesh pipeline has no vertex input
    // and names its topology in the program ([outputtopology]).
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

HRESULT graphics_plan(DeviceContext* c, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075& a, ID3D12RootSignature* root,
                      PsoPlan& plan) noexcept try {
    if (!c->device4) return E_NOTIMPL;                                      // no ID3D12Device2 on the engine
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

    plan.root = root;
    plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, plan_code(plan, kPsoStageVS, vs));
    if (hs) plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS, plan_code(plan, kPsoStageHS, hs));
    if (ds) plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS, plan_code(plan, kPsoStageDS, ds));
    if (gs && gs->is(D3D10_SB_GEOMETRY_SHADER))
        plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS, plan_code(plan, kPsoStageGS, gs));

    // Stream output, named against the last stage before rasterization. The entries, their semantic names and the
    // strides go into the plan, because the engine reads them inside its create and a job outlives this call.
    std::vector<sc::StreamOutputElement> so_elements;
    if (gs && gs->stream_output) {
        const sc::Container& last = gs->is(D3D10_SB_GEOMETRY_SHADER) ? gs->container : ds ? ds->container : vs->container;
        so_elements.resize(gs->so_entries.size());
        plan.so.resize(gs->so_entries.size());              // sized once, then written in place (the plan rule)
        plan.so_names.resize(gs->so_entries.size());
        plan.so_strides.assign(gs->so_strides.begin(), gs->so_strides.end());
        for (size_t i = 0; i < plan.so.size(); ++i) {
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
            if (!el.gap && el.semantic.name.empty()) {
                log_line("CreatePipelineState: stream-output entry %zu has no semantic name and is not a gap", i);
                return E_INVALIDARG;                        // an empty name is how the plan spells a gap
            }
            if (!el.gap) plan.so_names[i] = el.semantic.name;
            plan.so[i] = {e.Stream, el.gap ? nullptr : plan.so_names[i].c_str(), el.semantic.index, el.startComponent,
                          el.componentCount, static_cast<BYTE>(e.OutputSlot)};
        }
        const D3D12_STREAM_OUTPUT_DESC out_desc{plan.so.data(), static_cast<UINT>(plan.so.size()),
                                                plan.so_strides.empty() ? nullptr : plan.so_strides.data(),
                                                static_cast<UINT>(plan.so_strides.size()), gs->rasterized_stream};
        plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, out_desc);
    }

    put_common_state(plan.stream, a, root, ps ? plan_code(plan, kPsoStagePS, ps) : D3D12_SHADER_BYTECODE{}, blend,
                     raster, depth);
    if (HRESULT r = put_view_instancing(plan, a.ViewInstancingDesc); FAILED(r)) return r;

    // Input elements: the semantic of the vertex program's rebuilt input signature entry on each element's register.
    if (layout && layout->count) {
        plan.elements.resize(layout->count);                // sized once, then written in place (the plan rule)
        plan.element_names.resize(layout->count);
        const sc::SignatureView input{vs->input.data(), static_cast<UINT>(vs->input.size())};
        for (UINT i = 0; i < layout->count; ++i) {
            const D3D12DDIARG_INPUT_ELEMENT_DESC& e = layout->elements()[i];
            sc::Semantic name;
            const sc::Result r = sc::InputLayoutSemantic(input, e.InputRegister, &name);
            if (!r) {
                log_line("CreatePipelineState: input element %u: %s", i, r.detail.c_str());
                return r.hresult();
            }
            plan.element_names[i] = name.name;
            plan.elements[i] = {plan.element_names[i].c_str(), name.index, e.Format, e.InputSlot, e.AlignedByteOffset,
                                static_cast<D3D12_INPUT_CLASSIFICATION>(e.InputSlotClass), e.InstanceDataStepRate};
        }
        const D3D12_INPUT_LAYOUT_DESC input_layout{plan.elements.data(), layout->count};
        plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, input_layout);
    }

    plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE,
                    static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(a.IBStripCutValue));
    plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY,
                    static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(a.PrimitiveTopologyType));
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

HRESULT create_pipeline_impl(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* args,
                             D3D12DDI_HPIPELINESTATE h, D3D12DDI_HRTPIPELINESTATE rt, UINT flags) try {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate || args->NodeMask > 1) return E_INVALIDARG;
    constexpr UINT dynamic_flags = D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS |
                                   D3D12_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT;
    if (flags & ~dynamic_flags) return E_INVALIDARG;
    // A LibraryReference that names a library turns this create into a load out of it, under the name of its
    // PipelineIndex (D85-D90, "Pipeline libraries"). A handle that is not a live library of this device is the
    // same mistake as any other bad handle in this payload.
    WCHAR library_pipeline[kLibraryNameChars] = {};
    LibraryTarget target;
    if (void* storage = args->LibraryReference.hLibrary.pDrvPrivate) {
        const auto* l = record_of<PipelineLibraryRecord>(storage, Tag::PipelineLibrary, c);
        if (!l || !l->h.engine || (l->h.flags & kRecordInvalid)) return E_INVALIDARG;
        swprintf_s(library_pipeline, L"%08X", args->LibraryReference.PipelineIndex);
        target = {static_cast<ID3D12PipelineLibrary1*>(l->h.engine), library_pipeline};
    }
    // A mesh pipeline is named by its mesh program; an amplification program without one is not a pipeline.
    const bool mesh = args->hMeshShader.pDrvPrivate != nullptr;
    if (!mesh && args->hAmplificationShader.pDrvPrivate) return E_INVALIDARG;
    const bool graphics = mesh || args->hVertexShader.pDrvPrivate || args->hPixelShader.pDrvPrivate ||
                          args->hDomainShader.pDrvPrivate || args->hHullShader.pDrvPrivate ||
                          args->hGeometryShader.pDrvPrivate;
    if (graphics == (args->hComputeShader.pDrvPrivate != nullptr)) return E_INVALIDARG;    // exactly one kind
    if (!graphics && flags) return E_INVALIDARG;
    auto* rs = record_of<RootSignatureRecord>(args->hRootSignature.pDrvPrivate, Tag::RootSignature, c);
    if (!rs) return E_INVALIDARG;
    auto* root = static_cast<ID3D12RootSignature*>(rs->h.engine);
    // Everything above and the plan below are this thread's work whatever the policy says: every refusal an
    // application can get from its own create comes out of this call.
    PsoPlan plan;
    const HRESULT hr = mesh      ? mesh_plan(c, *args, root, plan)
                       : graphics ? graphics_plan(c, *args, root, plan)
                                  : compute_plan(c, *args, root, plan);
    if (FAILED(hr)) return hr;
    if (flags) plan.stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS, static_cast<D3D12_PIPELINE_STATE_FLAGS>(flags));
    ID3D12PipelineState* pso = nullptr;
    PsoJob* job = nullptr;
    if (!target.library && c->pso) job = pso_submit(c, std::move(plan));
    if (!job) {
        if (c->pso) c->pso->synchronous.fetch_add(1, std::memory_order_relaxed);
        const HRESULT create = target.library ? load_from_library(plan, target, &pso) : pso_create_now(c, plan, &pso);
        if (FAILED(create)) return create;
    }
    new (h.pDrvPrivate) PipelineRecord{{Tag::PipelineState, 0, pso, c}, rt, !graphics, job};
    c->live.fetch_add(1);
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

HRESULT APIENTRY create_pipeline(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* args,
                                 D3D12DDI_HPIPELINESTATE h, D3D12DDI_HRTPIPELINESTATE rt) {
    return create_pipeline_impl(device, args, h, rt, 0);
}

#if defined(D3D12DDI_BUILD_VERSION_0119)
HRESULT APIENTRY create_pipeline_modern(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* args,
                                        D3D12DDI_HPIPELINESTATE h, D3D12DDI_HRTPIPELINESTATE rt, UINT flags) {
    return create_pipeline_impl(device, args, h, rt, flags);
}
#endif

// The engine pipeline of a record, on a DDI thread. With a job it waits for the compile, or makes it here when no
// pool thread has started it; hr is the engine create's result and is S_OK whenever a pipeline comes back.
ID3D12PipelineState* pipeline_engine(PipelineRecord* r, HRESULT& hr) noexcept {
    if (!r->job) {
        hr = S_OK;
        return static_cast<ID3D12PipelineState*>(r->h.engine);
    }
    return pso_resolve(r->job, &hr);
}

// One line for the first consumer that finds a failed compile, then the code to the runtime at every use.
void refuse_uncreated(PipelineRecord* r, const char* slot, HRESULT hr) noexcept {
    if (r->job && !r->job->reported.exchange(1, std::memory_order_relaxed))
        log_refusal("%s: the engine did not create the pipeline of job %llu (hr %08lx); nothing of it runs", slot,
                    static_cast<unsigned long long>(r->job->id), static_cast<unsigned long>(hr));
}

void flush_pending_of(PipelineRecord* p) noexcept;   // the library store this pipeline still owes (BD-158, below)

void APIENTRY destroy_pipeline(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINESTATE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<PipelineRecord>(h.pDrvPrivate, Tag::PipelineState, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    drain_all(c, Drain::Destroy);
    // A library this pipeline was stored into keeps what it was given, so the store that still waits for the
    // compile is made here, while the pipeline exists.
    flush_pending_of(r);
    // A library collector can already own this pipeline's detached store. Its compile/store must finish
    // before the runtime can reclaim the pipeline's private storage or we can destroy its job.
    while (r->pending_users.load(std::memory_order_acquire)) Sleep(1);
    if (r->job) {
        // Ends the job (a queued one without a create at all) and releases the pipeline it holds, inside this
        // destroy, exactly where release_engine releases a synchronously created one.
        pso_destroy(r->job);
        r->job = nullptr;
    } else {
        release_engine(r->h);
    }
    poison(r->h);
    c->live.fetch_sub(1);
}

// ---- Pipeline libraries (D85-D90) ------------------------------------------------------------------------------
// Two decisions, because the DDI and the engine's ID3D12PipelineLibrary do not describe a library the same way.
//
// PSO identity. The DDI names a pipeline inside a library by a UINT index: pfnAddPipelineStateToLibrary takes one
// (H:3337-3341) and a create payload names one through D3D12DDI_LIBRARY_REFERENCE_0010 (H:3192-3196). The engine's
// interface names a pipeline by a string. This module turns the index into eight upper-case hexadecimal digits,
// which is defined for every UINT, gives a different name to every index, and is the same mapping on both sides of
// a serialize and a reload. Nothing but this module ever names a pipeline in a library of ours, so no second
// naming scheme can collide with it. That the index itself survives a reload is an INFERENCE: the DDI gives the
// runtime no other way to find a pipeline it added, and it must find one after the application reloads the blob.
//
// The blob's shape. pfnSerializeLibrary writes what the engine's Serialize writes, and pfnCreatePipelineLibrary
// hands pInitialData to the engine unchanged, so the shape is the engine's own, with its table of contents and its
// pipeline-cache UUID check (vkd3d-proton cache.c, d3d12_pipeline_library_init and
// d3d12_pipeline_library_serialize). Symmetry is what the DDI needs: whatever this driver wrote, this driver
// reads. Whether the runtime wraps those bytes in a container of its own is the runtime's business and changes
// nothing here.
//
// No capability gates these six slots: the 0092 capability types carry no pipeline library field at all
// (H:11078-11112). DriverManagedShaderCachePresent is a different thing: it reports the engine's per-user disk
// cache, which D121 pfnImplicitShaderCacheControl controls, while an application pipeline library is the
// application's cache, which that slot never touches (caps.cpp, context.cpp).
SIZE_T APIENTRY calc_pipeline_library(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010*) {
    return sizeof(PipelineLibraryRecord);
}

PipelineLibraryRecord* library_of(DeviceContext* c, D3D12DDI_HPIPELINELIBRARY h) noexcept {
    return record_of<PipelineLibraryRecord>(h.pDrvPrivate, Tag::PipelineLibrary, c);
}
ID3D12PipelineLibrary1* engine_library(const PipelineLibraryRecord* l) noexcept {
    return static_cast<ID3D12PipelineLibrary1*>(l->h.engine);
}

// ---- The serialized size, per thread (D89 and D90) --------------------------------------------------------------
// pfnSerializeLibrary carries no size of its own (H:3346-3348), so the only bound of the runtime's buffer is what
// pfnCalcSerializedLibrarySize last answered. ID3D12PipelineLibrary is free-threaded, and both DDI calls happen
// inside one ID3D12PipelineLibrary::Serialize of the calling thread, so the size is remembered per thread and not
// per library: with one value per library, a store and a second query on another thread between one thread's pair
// would raise the remembered size, and the engine would write that many bytes into the first thread's smaller
// buffer - its own Serialize refuses only a size below the one it needs (vkd3d-proton cache.c,
// d3d12_pipeline_library_serialize). A thread with no slot of its own is refused instead of guessed, which is the
// same answer as a serialize with no query behind it.
SerializedSize* size_slot_of(PipelineLibraryRecord* l, uint32_t thread) noexcept {
    for (SerializedSize& s : l->sizes)
        if (s.thread.load(std::memory_order_acquire) == thread) return &s;
    return nullptr;
}

void remember_serialized_size(PipelineLibraryRecord* l, uint64_t bytes) noexcept {
    const uint32_t self = GetCurrentThreadId();
    SerializedSize* slot = size_slot_of(l, self);
    if (!slot)
        for (SerializedSize& s : l->sizes) {
            uint32_t free_slot = 0;
            if (s.thread.compare_exchange_strong(free_slot, self, std::memory_order_acq_rel)) {
                slot = &s;
                break;
            }
        }
    if (!slot) {
        // Every slot is held, which takes a thread that asked for a size and never serialized it. The slot at
        // this thread's own index is taken over instead of refusing: its last owner's serialize is then refused
        // (take_serialized_size below checks the owner after it reads the size), which is the safe answer, while
        // a refusal here would cost this thread its own serialize for another thread's leak.
        slot = &l->sizes[self % kSerializedSizeSlots];
        slot->thread.store(self, std::memory_order_release);
        log_refusal("CalcSerializedLibrarySize: all %u size slots of this library were held, so slot %u was taken "
                    "over from its last thread", static_cast<unsigned>(kSerializedSizeSlots),
                    static_cast<unsigned>(self % kSerializedSizeSlots));
    }
    slot->bytes.store(bytes, std::memory_order_release);
}

// What this thread was last told, and the slot is free again: one size answers one serialize. 0 for a thread that
// asked for no size, whose size a store invalidated, or whose slot another thread has taken over since - the slot
// is given back only while it is still this thread's, and the size counts only then, so a size this thread did
// not ask for can never become its capacity.
uint64_t take_serialized_size(PipelineLibraryRecord* l) noexcept {
    const uint32_t self = GetCurrentThreadId();
    SerializedSize* slot = size_slot_of(l, self);
    if (!slot) return 0;
    const uint64_t bytes = slot->bytes.exchange(0, std::memory_order_acq_rel);
    uint32_t owner = self;
    if (!slot->thread.compare_exchange_strong(owner, 0, std::memory_order_acq_rel)) return 0;
    return bytes;
}

// Every remembered size, forgotten, on every thread: the blob is longer than any of them describes.
void forget_serialized_sizes(PipelineLibraryRecord* l) noexcept {
    for (SerializedSize& s : l->sizes) s.bytes.store(0, std::memory_order_release);
}

// ---- Stores that wait for their compile (BD-158) -----------------------------------------------------------------
// The engine's StorePipeline needs the pipeline object, but what it writes is read only when the blob is collected:
// at pfnCalcSerializedLibrarySize, at pfnSerializeLibrary, or when the pipeline is destroyed, whichever comes
// first. So a store whose compile is still running is remembered in the library and made at one of those three
// points. That is what keeps a game which stores every pipeline right after creating it off the serial chain the
// asynchronous compile exists to break. StorePipeline can also fail for allocation, not only a duplicate name
// (Microsoft's ID3D12PipelineLibrary::StorePipeline contract). Once this deferred Add has answered S_OK, however,
// an engine cache-store refusal has no caller left to answer: omit that entry and log once per library (BD-219).
// Serialize still writes the engine's valid remaining cache; a missing entry is a subsequent cache miss, not a
// reason to discard all successful entries. Actual pipeline compile/use failures remain failures of their consumers.
// One pending store per pipeline: a second Add first settles its older store, then makes the new one. This also
// prevents a later size query from re-storing an entry a second Add has already put into the engine.
// Every list below is under the device's pending_stores lock (internal.h), which says why it is not a lock of the
// library record: a pipeline's destroy finds its library through a pointer that the library's own destroy clears,
// and a lock inside a record being destroyed cannot guard the thread that is still reading it.
bool link_pending(PipelineLibraryRecord* l, PipelineRecord* p, UINT index) noexcept {
    SRWLOCK& lock = p->h.device->pending_stores;
    AcquireSRWLockExclusive(&lock);
    // The test and insertion are one operation. Two Adds must never put the same intrusive node in
    // a list twice, or overwrite its next pointer while it still belongs to another library (BD-220).
    if (p->pending_library.load(std::memory_order_relaxed)) {
        ReleaseSRWLockExclusive(&lock);
        return false;
    }
    p->pending_index = index;
    p->pending_next = l->pending_head;
    l->pending_head = p;
    p->pending_library.store(l, std::memory_order_release);
    ReleaseSRWLockExclusive(&lock);
    return true;
}

// Takes one pending store out of its library's list, so that exactly one thread makes it: the taker owns it. The
// library pointer is read again under the lock, because that is where a destroy of that library clears it; the
// read before the lock is a fast path for the pipelines that owe nothing, which is nearly all of them.
bool take_pending(PipelineRecord* p, PipelineLibraryRecord** library, UINT* index) noexcept {
    if (!p->pending_library.load(std::memory_order_acquire)) return false;
    SRWLOCK& lock = p->h.device->pending_stores;
    bool taken = false;
    AcquireSRWLockExclusive(&lock);
    PipelineLibraryRecord* l = p->pending_library.load(std::memory_order_relaxed);
    if (l)
        for (PipelineRecord** at = &l->pending_head; *at; at = &(*at)->pending_next) {
            if (*at != p) continue;
            *at = p->pending_next;
            p->pending_next = nullptr;
            p->pending_library.store(nullptr, std::memory_order_release);
            p->pending_users.fetch_add(1, std::memory_order_relaxed);
            l->pending_users.fetch_add(1, std::memory_order_relaxed);
            taken = true;
            break;
        }
    const UINT at_index = p->pending_index;
    ReleaseSRWLockExclusive(&lock);
    *library = l;
    *index = at_index;
    return taken;
}

PipelineRecord* take_pending_head(PipelineLibraryRecord* l, UINT* index) noexcept {
    SRWLOCK& lock = l->h.device->pending_stores;
    AcquireSRWLockExclusive(&lock);
    PipelineRecord* p = l->pending_head;
    if (p) {
        l->pending_head = p->pending_next;
        p->pending_next = nullptr;
        p->pending_library.store(nullptr, std::memory_order_release);
        *index = p->pending_index;
        p->pending_users.fetch_add(1, std::memory_order_relaxed);
        l->pending_users.fetch_add(1, std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&lock);
    return p;
}

void finish_pending(PipelineLibraryRecord* l, PipelineRecord* p) noexcept {
    // Do not touch either record after releasing its last collector reference.
    p->pending_users.fetch_sub(1, std::memory_order_release);
    l->pending_users.fetch_sub(1, std::memory_order_release);
}

// The refusal of a pipeline library create. Every code is clamped the way a creation slot must clamp it
// (engine-ddi.h, admitted_create_failure: an unadmitted code costs the device, BD-075), the two the public
// contract reserves for a blob this driver cannot use included. D3D12_ERROR_DRIVER_VERSION_MISMATCH and
// D3D12_ERROR_ADAPTER_NOT_FOUND are the documented answers of ID3D12Device1::CreatePipelineLibrary for a stale or
// foreign blob, and nothing but a driver can decide either of them, so carrying them out of this slot would be the
// better diagnosis. They are clamped anyway, because that they are admitted out of a creation DDI is an INFERENCE
// and nothing more: the reference documents no error category for PFND3D12DDI_CREATE_PIPELINE_LIBRARY_0010 at all
// (ref\ddi-display, a declaration and one line of description), the runtime takes a code a function does not admit
// as critical ("Handling Errors", AllowOutOfMemory), and a stale blob is the ordinary case after every driver
// update - so the inference would be paid in lost devices on a common path, on the one slot of this lane whose
// refusals are otherwise hardened. Nothing is lost at the API: an application answers any failure here by creating
// a fresh empty library. The pass-through goes back when a lab trial measures the runtime's reaction to either code
// out of this slot (LAB-PLAN.md, step C3, the pipeline_library group), and not before.

HRESULT APIENTRY create_pipeline_library(D3D12DDI_HDEVICE device,
                                         const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010* args,
                                         D3D12DDI_HPIPELINELIBRARY h) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate || (args->InitialDataSize && !args->pInitialData))
        return admitted_create_failure(E_INVALIDARG);
    if (!c->device4) return admitted_create_failure(E_NOTIMPL);     // no ID3D12Device1 on the engine
    ID3D12PipelineLibrary1* library = nullptr;
    const HRESULT hr =
        c->device4->CreatePipelineLibrary(args->pInitialData, args->InitialDataSize,
                                          __uuidof(ID3D12PipelineLibrary1), reinterpret_cast<void**>(&library));
    if (FAILED(hr) || !library) {
        log_refusal("CreatePipelineLibrary: the engine refused a %zu byte blob (hr %08lx)",
                    static_cast<size_t>(args->InitialDataSize), static_cast<unsigned long>(hr));
        return admitted_create_failure(SUCCEEDED(hr) ? E_UNEXPECTED : hr);
    }
    auto* r = new (h.pDrvPrivate) PipelineLibraryRecord{};
    r->h = RecordHeader{Tag::PipelineLibrary, 0, library, c};
    c->live.fetch_add(1);
    return S_OK;
}

// D87. A library holds engine references and nothing else: no command list can name one, and no memory of the
// shell's sits behind it, so there is nothing to drain before the release. A pipeline state is different, which is
// why destroy_pipeline above drains first.
void APIENTRY destroy_pipeline_library(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    PipelineLibraryRecord* r = library_of(c, h);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    // A store this library still waits for has nowhere to go: the library it would be written into is what is
    // being destroyed, and no blob of it will ever be read. The pipelines are let go of without their compile
    // being waited for, which is also what makes this destroy cheap for an application that gives up on a cache.
    uint32_t dropped = 0;
    AcquireSRWLockExclusive(&c->pending_stores);
    while (PipelineRecord* p = r->pending_head) {
        r->pending_head = p->pending_next;
        p->pending_next = nullptr;
        p->pending_library.store(nullptr, std::memory_order_release);
        ++dropped;
    }
    ReleaseSRWLockExclusive(&c->pending_stores);
    // A pipeline's own destroy can already have taken its store. Dropping the remaining list does
    // not end that collector's use of this record. Wait without holding the list lock.
    while (r->pending_users.load(std::memory_order_acquire)) Sleep(1);
    if (dropped)
        log_line("pipeline library: destroyed with %u store(s) that no serialize asked for", dropped);
    release_engine(r->h);
    poison(r->h);
    c->live.fetch_sub(1);
}

// One store into the engine's library, on the thread that owns it: the slot's own thread for a pipeline that is
// already compiled, or the thread that collects the blob for one that was not. The pipeline is waited for here,
// which for the deferred store is the point of the whole exercise: the wait is paid once, by the thread that needs
// the blob, and not by the thread that creates the pipelines.
void note_store_failure(PipelineLibraryRecord* l, UINT index, const char* slot, HRESULT hr, bool deferred) noexcept {
    if (deferred) {
        long first = S_OK;
        if (!l->first_deferred_failure.compare_exchange_strong(first, hr, std::memory_order_relaxed)) return;
        log_refusal("%s: deferred cache entry at index %u omitted (hr %08lx); serializing the remaining library; "
                    "further deferred cache refusals of this library are not logged", slot, index,
                    static_cast<unsigned long>(hr));
    } else {
        log_refusal("%s: the engine refused the pipeline at index %u (hr %08lx)", slot, index,
                    static_cast<unsigned long>(hr));
    }
}

HRESULT store_pipeline(PipelineLibraryRecord* l, PipelineRecord* p, UINT index, const char* slot,
                       bool deferred = false) noexcept {
    HRESULT ready = S_OK;
    ID3D12PipelineState* pso = pipeline_engine(p, ready);
    if (FAILED(ready) || !pso) {
        refuse_uncreated(p, slot, ready);
        const HRESULT hr = FAILED(ready) ? ready : E_INVALIDARG;
        note_store_failure(l, index, slot, hr, deferred);
        return hr;
    }
    WCHAR name[kLibraryNameChars] = {};
    swprintf_s(name, L"%08X", index);
    const HRESULT hr = engine_library(l)->StorePipeline(name, pso);
    if (FAILED(hr)) {
        note_store_failure(l, index, slot, hr, deferred);
        return hr;
    }
    // The blob is longer now, so no size the runtime asked for before describes it any more. Forgetting them all
    // makes a serialize before the next size query refuse instead of writing a longer blob into a shorter buffer.
    forget_serialized_sizes(l);
    const uint32_t stored = l->stored.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!(stored & (stored - 1)))                                   // at powers of two: one line per doubling
        log_line("pipeline library: %u pipelines stored (last index %u)", stored, index);
    return S_OK;
}

// Every store this library still owes, taken exactly once. A failed deferred cache entry
// does not poison serialization of the entries the engine did accept.
void flush_pending(PipelineLibraryRecord* l, const char* slot) noexcept {
    UINT index = 0;
    while (PipelineRecord* p = take_pending_head(l, &index)) {
        (void)store_pipeline(l, p, index, slot, true);
        finish_pending(l, p);
    }
}

// The store a pipeline owes its library, made before the pipeline is destroyed, so that a library keeps what was
// stored into it even when the application destroys the pipeline before it saves the blob.
void flush_pending_of(PipelineRecord* p) noexcept {
    PipelineLibraryRecord* l = nullptr;
    UINT index = 0;
    if (!take_pending(p, &l, &index)) return;
    (void)store_pipeline(l, p, index, "FlushPendingPipelineState", true);
    finish_pending(l, p);
}

// D88, a creation slot, so its refusals are clamped. The engine answers E_INVALIDARG for a name it already holds
// (vkd3d-proton cache.c, d3d12_pipeline_library_StorePipeline), which is what D3D12 shows an application that
// stores two pipelines under one name - but the runtime owns the names and the DDI only has indices, so the
// duplicate is the runtime's to detect and this slot never carries that code. An immediate engine failure is
// returned here, clamped to the DDI contract. A deferred cache-store failure is only logged when its compile
// finishes (see "Stores that wait for their compile"); the valid remaining cache can still be serialized.
HRESULT APIENTRY add_pipeline_to_library(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY hl,
                                         D3D12DDI_HPIPELINESTATE hp, UINT index) {
    DeviceContext* c = resolve(device);
    if (!c) return admitted_create_failure(E_INVALIDARG);
    PipelineLibraryRecord* l = library_of(c, hl);
    auto* p = record_of<PipelineRecord>(hp.pDrvPrivate, Tag::PipelineState, c);
    if (!l || !p) return admitted_create_failure(E_INVALIDARG);
    // Settle the earlier Add even if the compiler has finished since it was queued.
    // The new Add retains its synchronous error channel, including duplicate-name refusal.
    flush_pending_of(p);
    if (pso_pending(p->job) && link_pending(l, p, index)) {
        forget_serialized_sizes(l);                                 // the blob will be longer than any size held
        return S_OK;
    }
    return admitted_create_failure(store_pipeline(l, p, index, "AddPipelineStateToLibrary"));
}

// D89, a size query with no error channel of its own (H:3343-3345): an unknown library answers 0 and writes one
// line, and reports nothing, because a status out of a query slot is the BD-075 signature. 0 is also the engine's
// own answer for a library it does not serialize as one blob (a stream archive, vkd3d-proton cache.c,
// d3d12_pipeline_library_get_serialized_size), and a serialize is refused after either.
SIZE_T APIENTRY calc_serialized_library_size(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY h) {
    DeviceContext* c = resolve(device);
    if (!c) return 0;
    PipelineLibraryRecord* l = library_of(c, h);
    if (!l) {
        log_refusal("CalcSerializedLibrarySize: not a live pipeline library of this device");
        return 0;
    }
    flush_pending(l, "CalcSerializedLibrarySize");                   // the size covers every store, deferred or not
    const SIZE_T size = engine_library(l)->GetSerializedSize();
    remember_serialized_size(l, size);
    return size;
}

// D90, a creation slot (SLOTS.md). The runtime sized the buffer from the answer of pfnCalcSerializedLibrarySize and
// this slot carries no size of its own (H:3346-3348), so the engine is given exactly the number of bytes that
// query answered to this thread and can write no more. A library that grew in between has no remembered size any
// more (pfnAddPipelineStateToLibrary forgets them all), a serialize whose thread asked for no size is refused, and
// a library whose blob the engine has made longer for a reason of its own makes the engine itself refuse, because
// its Serialize compares the size it needs with the size it is given. What holds here is therefore: the engine is
// never given a byte count this thread's own size query did not answer for the library's current contents.
HRESULT APIENTRY serialize_library(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINELIBRARY h, void* blob) {
    DeviceContext* c = resolve(device);
    if (!c) return admitted_create_failure(E_INVALIDARG);
    PipelineLibraryRecord* l = library_of(c, h);
    if (!l || !blob) return admitted_create_failure(E_INVALIDARG);
    flush_pending(l, "SerializeLibrary");           // normally nothing: the size query of this thread flushed
    const uint64_t capacity = take_serialized_size(l);
    if (!capacity || capacity > SIZE_MAX) {
        log_refusal("SerializeLibrary: no size query of this thread answered %llu bytes for this library since it "
                    "last changed", static_cast<unsigned long long>(capacity));
        return admitted_create_failure(E_INVALIDARG);
    }
    const HRESULT hr = engine_library(l)->Serialize(blob, static_cast<SIZE_T>(capacity));
    if (FAILED(hr))
        log_refusal("SerializeLibrary: the engine refused to write %llu bytes (hr %08lx)",
                    static_cast<unsigned long long>(capacity), static_cast<unsigned long>(hr));
    return admitted_create_failure(hr);
}

// D83 (H:2726-2727). engine-ddi keeps no driver comment on a pipeline and the engine exposes none, so the answer
// is the empty string: one character, the terminator alone. The slot's contract is read off its annotations,
// because the reference documents no behaviour for it (ref\ddi-display, PFND3D12DDI_RETRIEVE_SHADER_COMMENT_0003:
// declaration and parameter names only). The count is _Inout_, so it arrives as the buffer's capacity in
// characters and leaves as the characters the answer needs including the terminator; pBuffer is _Out_writes_z_,
// so a non-null buffer is written with a terminated string and never past that capacity. Two shapes the
// annotations do not describe are answered anyway (INFERENCE): a null buffer is the size query an _Inout_ count
// invites, and a non-null buffer with no room for even a terminator is refused with the needed count left in
// place. This is an HRESULT query slot and not a creation function, so E_INVALIDARG stays E_INVALIDARG.
HRESULT APIENTRY retrieve_shader_comment(D3D12DDI_HDEVICE device, D3D12DDI_HPIPELINESTATE h, WCHAR* buffer,
                                         SIZE_T* characters) {
    DeviceContext* c = resolve(device);
    if (!c || !characters) return E_INVALIDARG;
    if (!record_of<PipelineRecord>(h.pDrvPrivate, Tag::PipelineState, c)) return E_INVALIDARG;
    const SIZE_T capacity = *characters;
    *characters = 1;                                                // the empty comment: the terminator alone
    if (!buffer) return S_OK;                                       // the size query
    if (!capacity) return E_INVALIDARG;
    buffer[0] = L'\0';
    return S_OK;
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
    // The bind needs the engine pipeline, so this is where a compile is waited for. On this thread and not in the
    // deferred replay: a ring is in order, so a worker held inside a compile would hold back every later call of
    // its thread (engine-ddi.h, set_pso_policy).
    ID3D12PipelineState* pso = nullptr;
    if (p) {
        HRESULT ready = S_OK;
        pso = pipeline_engine(p, ready);
        if (FAILED(ready)) {
            // No draw may run with a pipeline the engine refused, so the bind is not recorded at all.
            refuse_uncreated(p, "SetPipelineState", ready);
            l->h.device->report_list(l->rt, ready);
            return;
        }
    }
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
    t->pfnCalcPrivatePipelineLibrarySize = calc_pipeline_library;
    t->pfnCreatePipelineLibrary = create_pipeline_library;
    t->pfnDestroyPipelineLibrary = destroy_pipeline_library;
    t->pfnAddPipelineStateToLibrary = add_pipeline_to_library;
    t->pfnCalcSerializedLibrarySize = calc_serialized_library_size;
    t->pfnSerializeLibrary = serialize_library;
    t->pfnRetrieveShaderComment = retrieve_shader_comment;
}

#if defined(D3D12DDI_BUILD_VERSION_0119)
void fill_modern_pipelines(native12::ModernCoreEntries* entries) noexcept {
    entries->pipeline_create = create_pipeline_modern;
}
#endif

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
