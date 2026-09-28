// SPDX-License-Identifier: MIT
// engine-ddi: shader intake (D15-D25, D118-D120), pipeline states (D33-D35) and the compute state of a command
// list (L4, L29, L31, L33, L35, L37, L39, L41, L43).
//
// Native intake (engine-ddi.h, "Shaders"). The payload is read within bounds only:
//   1. pShaderCode is checked for null;
//   2. only then is DWORD 1, the length in DWORDs, read;
//   3. the length must be at least 2 (the version and length tokens) and len * 4 must fit in 32 bits;
//   4. exactly len * 4 bytes are copied into the record's private storage, which CalcPrivateShaderSize sized from
//      the same DWORD 1, and the snapshot holds the first min(len * 4, 32) of them.
// A two-DWORD program is therefore read as eight bytes and no more. Nothing past the declared length is read, and
// the declared length is trusted as the extent of the buffer (INFERENCE for 0026, engine-ddi.h). DXIL libraries of
// state objects are bounded separately by their own size field when state objects are implemented.
#include "internal.h"
#include <algorithm>
#include <cstring>

namespace engine_ddi {

namespace {
constexpr uint32_t kDxbcMagic = 0x43425844u;           // "DXBC"

enum class Stage { Standard, Tessellation, Mesh };

struct Payload {
    uint32_t intake;                                    // ShaderIntake
    uint32_t bytes;
};

// Classifies a payload with bounded reads only; nothing beyond DWORD 1 (native) or the container's own size
// (harness) is read here.
Payload classify(const UINT* code) noexcept {
    if (!code) return {kIntakeInvalid, 0};
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    if (code[0] == kDxbcMagic) {
        const uint32_t total = code[6];                 // container size, harness path only
        if (total < 32 || total % 4) return {kIntakeInvalid, 0};
        return {kIntakeHarnessContainer, total};
    }
#endif
    const uint32_t len = code[1];
    if (len < 2 || len > UINT32_MAX / 4) return {kIntakeInvalid, 0};
    return {kIntakeNative, len * 4};
}

SIZE_T shader_size(const D3D12DDIARG_CREATE_SHADER_0026* args) noexcept {
    const Payload p = args ? classify(args->pShaderCode) : Payload{kIntakeInvalid, 0};
    return sizeof(ShaderRecord) + p.bytes;
}

SIZE_T APIENTRY calc_shader(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SHADER_0026* args) { return shader_size(args); }
SIZE_T APIENTRY calc_gs_so(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_GEOMETRY_SHADER_WITH_STREAM_OUTPUT_0026* args) {
    return shader_size(args ? &args->CreateShader : nullptr);
}

void intake(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_SHADER_0026* args, D3D12DDI_HSHADER h, Stage stage,
            const char* slot) noexcept {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!args || !h.pDrvPrivate) {
        c->report(E_INVALIDARG);
        return;
    }
    // An inert record first, so that DestroyShader is valid whatever happens below.
    auto* r = new (h.pDrvPrivate) ShaderRecord{{Tag::Shader, 0, nullptr, c}, kIntakeInvalid, 0, {}, 0, 0, 0, 0};
    const Payload p = classify(args->pShaderCode);
    if (p.intake == kIntakeInvalid) {
        log_line("%s: %s", slot, args->pShaderCode ? "declared length below 2 DWORDs or too large" : "null pShaderCode");
        c->report(E_INVALIDARG);
        return;
    }
    std::memcpy(r + 1, args->pShaderCode, p.bytes);
    std::memcpy(r->snapshot, args->pShaderCode, std::min<uint32_t>(p.bytes, sizeof(r->snapshot)));
    r->bytes = p.bytes;
    r->intake = p.intake;
    if (p.intake == kIntakeHarnessContainer) return;

    if (stage == Stage::Tessellation && args->IOSignatures.Tessellation) {
        r->inputs = args->IOSignatures.Tessellation->NumInputSignatureEntries;
        r->outputs = args->IOSignatures.Tessellation->NumOutputSignatureEntries;
        r->patch_constants = args->IOSignatures.Tessellation->NumPatchConstantSignatureEntries;
    } else if (stage == Stage::Mesh && args->IOSignatures.Mesh) {
        r->outputs = args->IOSignatures.Mesh->NumVertexOutputSignatureEntries;
        r->patch_constants = args->IOSignatures.Mesh->NumPrimitiveOutputSignatureEntries;
    } else if (stage == Stage::Standard && args->IOSignatures.Standard) {
        r->inputs = args->IOSignatures.Standard->NumInputSignatureEntries;
        r->outputs = args->IOSignatures.Standard->NumOutputSignatureEntries;
    }
    const uint32_t shown = std::min<uint32_t>(p.bytes / 4, 4);
    log_line("%s: native payload of %u DWORDs, first %u: %08x %08x %08x %08x, signature entries %u/%u/%u; "
             "no translation yet",
             slot, p.bytes / 4, shown, shown > 0 ? r->snapshot[0] : 0, shown > 1 ? r->snapshot[1] : 0,
             shown > 2 ? r->snapshot[2] : 0, shown > 3 ? r->snapshot[3] : 0, r->inputs, r->outputs,
             r->patch_constants);
    c->report(E_NOTIMPL);
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
    intake(d, a ? &a->CreateShader : nullptr, h, Stage::Standard, "CreateGeometryShaderWithStreamOutput");
}
void APIENTRY create_hs(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Tessellation, "CreateHullShader");
}
void APIENTRY create_ds(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Tessellation, "CreateDomainShader");
}
void APIENTRY create_as(D3D12DDI_HDEVICE d, const D3D12DDIARG_CREATE_SHADER_0026* a, D3D12DDI_HSHADER h) {
    intake(d, a, h, Stage::Standard, "CreateAmplificationShader");
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
    poison(r->h);
}

// ---- Pipeline states -----------------------------------------------------------------------------------------------
SIZE_T APIENTRY calc_pipeline(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075*) {
    return sizeof(PipelineRecord);
}

HRESULT APIENTRY create_pipeline(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* args,
                                 D3D12DDI_HPIPELINESTATE h, D3D12DDI_HRTPIPELINESTATE rt) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate || args->NodeMask > 1) return E_INVALIDARG;
    if (args->LibraryReference.hLibrary.pDrvPrivate) return E_NOTIMPL;      // pipeline libraries (P4)
    const bool graphics = args->hVertexShader.pDrvPrivate || args->hPixelShader.pDrvPrivate ||
                          args->hDomainShader.pDrvPrivate || args->hHullShader.pDrvPrivate ||
                          args->hGeometryShader.pDrvPrivate || args->hMeshShader.pDrvPrivate ||
                          args->hAmplificationShader.pDrvPrivate;
    if (!args->hComputeShader.pDrvPrivate) return graphics ? E_NOTIMPL : E_INVALIDARG;   // graphics PSOs: P1
    if (graphics) return E_INVALIDARG;
    auto* cs = record_of<ShaderRecord>(args->hComputeShader.pDrvPrivate, Tag::Shader, c);
    auto* rs = record_of<RootSignatureRecord>(args->hRootSignature.pDrvPrivate, Tag::RootSignature, c);
    if (!cs || !rs) return E_INVALIDARG;
    if (cs->intake == kIntakeNative) return E_NOTIMPL;                     // no translation yet
    if (cs->intake != kIntakeHarnessContainer) return E_INVALIDARG;
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = static_cast<ID3D12RootSignature*>(rs->h.engine);
    desc.CS = {cs->payload(), cs->bytes};
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = c->device->CreateComputePipelineState(&desc, __uuidof(ID3D12PipelineState),
                                                       reinterpret_cast<void**>(&pso));
    if (FAILED(hr)) return hr;
    new (h.pDrvPrivate) PipelineRecord{{Tag::PipelineState, 0, pso, c}, rt, true};
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
    release_engine(r->h);
    poison(r->h);
    c->live.fetch_sub(1);
}

// ---- Command-list slots -----------------------------------------------------------------------------------------
void APIENTRY set_pipeline_state(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HPIPELINESTATE h) {
    CommandListRecord* l = list_of(hlist, "SetPipelineState");
    if (!l) return;
    auto* p = record_of<PipelineRecord>(h.pDrvPrivate, Tag::PipelineState, l->h.device);
    if (!p) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->SetPipelineState(static_cast<ID3D12PipelineState*>(p->h.engine));
}

void APIENTRY set_compute_root_signature(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HROOTSIGNATURE h) {
    CommandListRecord* l = list_of(hlist, "SetComputeRootSignature");
    if (!l) return;
    auto* r = record_of<RootSignatureRecord>(h.pDrvPrivate, Tag::RootSignature, l->h.device);
    if (h.pDrvPrivate && !r) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->SetComputeRootSignature(r ? static_cast<ID3D12RootSignature*>(r->h.engine) : nullptr);
}

void APIENTRY set_compute_root_table(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_DESCRIPTOR_HANDLE base) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootDescriptorTable"))
        l->list()->SetComputeRootDescriptorTable(index, D3D12_GPU_DESCRIPTOR_HANDLE{base.ptr});
}

void APIENTRY set_compute_root_constant(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT data, UINT offset) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRoot32BitConstant"))
        l->list()->SetComputeRoot32BitConstant(index, data, offset);
}

void APIENTRY set_compute_root_constants(D3D12DDI_HCOMMANDLIST hlist, UINT index, UINT count, const void* data,
                                         UINT offset) {
    CommandListRecord* l = list_of(hlist, "SetComputeRoot32BitConstants");
    if (!l) return;
    if (count && !data) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->SetComputeRoot32BitConstants(index, count, data, offset);
}

void APIENTRY set_compute_root_cbv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootConstantBufferView"))
        l->list()->SetComputeRootConstantBufferView(index, va);
}

void APIENTRY set_compute_root_srv(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootShaderResourceView"))
        l->list()->SetComputeRootShaderResourceView(index, va);
}

void APIENTRY set_compute_root_uav(D3D12DDI_HCOMMANDLIST hlist, UINT index, D3D12DDI_GPU_VIRTUAL_ADDRESS va) {
    if (CommandListRecord* l = list_of(hlist, "SetComputeRootUnorderedAccessView"))
        l->list()->SetComputeRootUnorderedAccessView(index, va);
}

void APIENTRY dispatch(D3D12DDI_HCOMMANDLIST hlist, UINT x, UINT y, UINT z) {
    if (CommandListRecord* l = list_of(hlist, "Dispatch")) l->list()->Dispatch(x, y, z);
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
