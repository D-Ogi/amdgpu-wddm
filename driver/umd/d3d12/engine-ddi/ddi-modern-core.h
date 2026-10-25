// SPDX-License-Identifier: MIT
// Typed adapters for WDK 10.0.28000.0 CORE 0095-0119. No table or argument
// prefix is treated as a different C type. Optional features remain unadvertised.
#pragma once
#include <d3d12umddi.h>
#include "../ddi-entry.h" // the shell's AllowOutOfMemory contract
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <type_traits>
#include <vector>

namespace native12 {
struct ModernCoreEntries {
    PFND3D12DDI_GET_PROGRAM_IDENTIFIER_0108 pfnGetProgramIdentifier{};
    PFND3D12DDI_CALCPRIVATEDEPTHSTENCILSTATESIZE_0095 depth_size{};
    PFND3D12DDI_CREATEDEPTHSTENCILSTATE_0095 depth_create{};
    PFND3D12DDI_CREATE_SAMPLER_0096 sampler{};
    PFND3D12DDI_CALCPRIVATERASTERIZERSTATESIZE_0099 raster_size99{};
    PFND3D12DDI_CREATERASTERIZERSTATE_0099 raster_create99{};
    PFND3D12DDI_CALCPRIVATERASTERIZERSTATESIZE_0102 raster_size102{};
    PFND3D12DDI_CREATERASTERIZERSTATE_0102 raster_create102{};
    PFND3D12DDI_CALC_PRIVATE_ROOT_SIGNATURE_SIZE_0100 root_size{};
    PFND3D12DDI_CREATE_ROOT_SIGNATURE_0100 root_create{};
    HRESULT (APIENTRY* pipeline_create)(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075*,
        D3D12DDI_HPIPELINESTATE, D3D12DDI_HRTPIPELINESTATE, UINT){};
    bool operator==(const ModernCoreEntries&) const = default;
};
namespace modern_core {
using Error = void (APIENTRY*)(D3D12DDI_HDEVICE, HRESULT);

inline HRESULT creation_status(const char* slot, HRESULT raw) noexcept {
    const HRESULT admitted = ddi_admitted_create_failure(raw);
    if (FAILED(raw)) {
        // Preserve the actual refusal for diagnosis without reporting an extra
        // device error: this creation callback has its own admitted HRESULT.
        char line[160];
        sprintf_s(line, "modern CORE %s: hr %08lx, admitted %08lx\n", slot,
                  static_cast<unsigned long>(raw), static_cast<unsigned long>(admitted));
        OutputDebugStringA(line);
    }
    return admitted;
}

inline bool depth(const D3D12DDI_DEPTH_STENCIL_DESC_0095& s, D3D12DDI_DEPTH_STENCIL_DESC_0025& d) {
    if (s.StencilEnable && s.FrontEnable && s.BackEnable &&
        (s.FrontFaceStencilReadMask != s.BackFaceStencilReadMask ||
         s.FrontFaceStencilWriteMask != s.BackFaceStencilWriteMask)) return false;
    d.DepthEnable = s.DepthEnable; d.DepthWriteMask = s.DepthWriteMask; d.DepthFunc = s.DepthFunc;
    d.StencilEnable = s.StencilEnable; d.FrontEnable = s.FrontEnable; d.BackEnable = s.BackEnable;
    d.StencilReadMask = s.FrontEnable ? s.FrontFaceStencilReadMask : s.BackFaceStencilReadMask;
    d.StencilWriteMask = s.FrontEnable ? s.FrontFaceStencilWriteMask : s.BackFaceStencilWriteMask;
    d.FrontFace = s.FrontFace; d.BackFace = s.BackFace; d.LibraryReference = s.LibraryReference;
    d.DepthBoundsTestEnable = s.DepthBoundsTestEnable;
    return true;
}

template<class Raster>
bool raster(const Raster& s, D3D12DDI_RASTERIZER_DESC_0010& d) {
    // The old DDI has signed integral bias. Check before conversion, including
    // INT_MAX rounding up when represented by float, NaN and infinity.
    const double bias = s.DepthBias;
    if (!std::isfinite(bias) || bias < (std::numeric_limits<INT>::min)() ||
        bias > (std::numeric_limits<INT>::max)() || std::trunc(bias) != bias) return false;
    d.FillMode = s.FillMode; d.CullMode = s.CullMode; d.FrontCounterClockwise = s.FrontCounterClockwise;
    d.DepthBias = static_cast<INT>(bias); d.DepthBiasClamp = s.DepthBiasClamp;
    d.SlopeScaledDepthBias = s.SlopeScaledDepthBias; d.DepthClipEnable = s.DepthClipEnable;
    d.ScissorEnable = s.ScissorEnable; d.ForcedSampleCount = s.ForcedSampleCount;
    d.ConservativeRasterizationMode = s.ConservativeRasterizationMode; d.LibraryReference = s.LibraryReference;
    if constexpr (std::is_same_v<Raster, D3D12DDI_RASTERIZER_DESC_0099>) {
        d.MultisampleEnable = s.MultisampleEnable; d.AntialiasedLineEnable = s.AntialiasedLineEnable;
    } else {
        d.MultisampleEnable = FALSE; d.AntialiasedLineEnable = FALSE;
        switch (s.LineRasterizationMode) {
        case D3D12DDI_LINE_RASTERIZATION_MODE_ALIASED: break;
        case D3D12DDI_LINE_RASTERIZATION_MODE_ALPHA_ANTIALIASED: d.AntialiasedLineEnable = TRUE; break;
        case D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE: d.MultisampleEnable = TRUE; break;
        default: return false; // Narrow quadrilaterals have no legacy equivalent.
        }
    }
    return true;
}

template<class Pipeline>
bool pipeline(const Pipeline& s, D3D12DDIARG_CREATE_PIPELINE_STATE_0075& d, bool modern = false) {
    if (!modern && s.Flags != D3D12DDI_PIPELINE_STATE_FLAG_NONE) return false;
    d.hComputeShader = s.hComputeShader; d.hVertexShader = s.hVertexShader; d.hPixelShader = s.hPixelShader;
    d.hDomainShader = s.hDomainShader; d.hHullShader = s.hHullShader; d.hGeometryShader = s.hGeometryShader;
    d.hRootSignature = s.hRootSignature; d.hBlendState = s.hBlendState; d.SampleMask = s.SampleMask;
    d.hRasterizerState = s.hRasterizerState; d.hDepthStencilState = s.hDepthStencilState;
    d.hElementLayout = s.hElementLayout; d.IBStripCutValue = s.IBStripCutValue;
    d.PrimitiveTopologyType = s.PrimitiveTopologyType; d.NumRenderTargets = s.NumRenderTargets;
    for (unsigned i = 0; i != 8; ++i) d.RTVFormats[i] = s.RTVFormats[i];
    d.DSVFormat = s.DSVFormat; d.SampleDesc = s.SampleDesc; d.NodeMask = s.NodeMask;
    d.LibraryReference = s.LibraryReference; d.ViewInstancingDesc = s.ViewInstancingDesc;
    d.hMeshShader = s.hMeshShader; d.hAmplificationShader = s.hAmplificationShader;
    // 0119's transient code pointers are not retained: CreateShader has already
    // rebuilt owned containers, and the asynchronous PSO plan owns its copies.
    return true;
}

struct Root {
    D3D12DDI_ROOT_SIGNATURE_0013 desc{};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 args{};
    std::vector<D3D12DDI_STATIC_SAMPLER> samplers;
    HRESULT convert(const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100* input) {
        if (!input || input->Version != D3D12DDI_ROOT_SIGNATURE_VERSION_1_2 || !input->pRootSignature_1_2)
            return E_INVALIDARG;
        const auto& s = *input->pRootSignature_1_2;
        if ((s.NumParameters && !s.pRootParameters) || (s.NumStaticSamplers && !s.pStaticSamplers)) return E_INVALIDARG;
        try { samplers.resize(s.NumStaticSamplers); } catch (...) { return E_OUTOFMEMORY; }
        for (UINT i = 0; i != s.NumStaticSamplers; ++i) {
            const auto& a = s.pStaticSamplers[i]; auto& b = samplers[i];
            if (a.Flags != D3D12DDI_SAMPLER_FLAG_NONE || a.BorderColor > D3D12DDI_STATIC_BORDER_COLOR_OPAQUE_WHITE)
                return E_NOTIMPL;
            b.Filter = a.Filter; b.AddressU = a.AddressU; b.AddressV = a.AddressV; b.AddressW = a.AddressW;
            b.MipLODBias = a.MipLODBias; b.MaxAnisotropy = a.MaxAnisotropy; b.ComparisonFunc = a.ComparisonFunc;
            b.BorderColor = a.BorderColor; b.MinLOD = a.MinLOD; b.MaxLOD = a.MaxLOD;
            b.ShaderRegister = a.ShaderRegister; b.RegisterSpace = a.RegisterSpace; b.ShaderVisibility = a.ShaderVisibility;
        }
        desc.NumParameters = s.NumParameters; desc.pRootParameters = s.pRootParameters;
        desc.NumStaticSamplers = s.NumStaticSamplers; desc.pStaticSamplers = samplers.data(); desc.Flags = s.Flags;
        args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1; args.pRootSignature_1_1 = &desc; args.NodeMask = input->NodeMask;
        return S_OK;
    }
};

template<class Resource>
bool resource(const Resource& s, D3D12DDIARG_CREATERESOURCE_0088& d) {
    if (s.CreateAtVirtualAddress || s.Layout == D3D12DDI_TL_GUID) return false;
    d.ReuseBufferGPUVA = s.ReuseBufferGPUVA; d.ResourceType = s.ResourceType; d.Width = s.Width;
    d.Height = s.Height; d.DepthOrArraySize = s.DepthOrArraySize; d.MipLevels = s.MipLevels;
    d.Format = s.Format; d.SampleDesc = s.SampleDesc; d.Layout = s.Layout; d.Flags = s.Flags;
    d.InitialBarrierLayout = s.InitialBarrierLayout; d.pRowMajorLayout = s.pRowMajorLayout;
    d.SamplerFeedbackMipRegion = s.SamplerFeedbackMipRegion;
    d.NumCastableFormats = s.NumCastableFormats; d.pCastableFormats = s.pCastableFormats;
    return true;
}

template<class Core> struct Bridge {
    inline static D3D12DDI_DEVICE_FUNCS_CORE_0088 source{};
    inline static ModernCoreEntries modern{};
    inline static Error report = nullptr;
    inline static std::mutex mutex;
    inline static bool ready = false;

    static SIZE_T APIENTRY depth_size(D3D12DDI_HDEVICE h, const D3D12DDI_DEPTH_STENCIL_DESC_0095* s) {
        if (modern.depth_size) return modern.depth_size(h, s);
        D3D12DDI_DEPTH_STENCIL_DESC_0025 d{};
        if (!s || !depth(*s, d)) { report(h, s ? E_NOTIMPL : E_INVALIDARG); return 0; }
        return source.pfnCalcPrivateDepthStencilStateSize(h, &d);
    }
    static void APIENTRY depth_create(D3D12DDI_HDEVICE h, const D3D12DDI_DEPTH_STENCIL_DESC_0095* s, D3D12DDI_HDEPTHSTENCILSTATE o) {
        if (modern.depth_create) { modern.depth_create(h, s, o); return; }
        D3D12DDI_DEPTH_STENCIL_DESC_0025 d{};
        if (!s || !depth(*s, d)) { report(h, s ? E_NOTIMPL : E_INVALIDARG); return; }
        source.pfnCreateDepthStencilState(h, &d, o);
    }
    static void APIENTRY sampler(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_SAMPLER_0096* s, D3D12DDI_CPU_DESCRIPTOR_HANDLE o) {
        if (modern.sampler) { modern.sampler(h, s, o); return; }
        if (!s || !s->pSamplerDesc) { report(h, E_INVALIDARG); return; }
        const auto& a = *s->pSamplerDesc;
        if (a.Flags != D3D12DDI_SAMPLER_FLAG_NONE) { report(h, E_NOTIMPL); return; }
        D3D12DDI_SAMPLER_DESC b{};
        b.Filter = a.Filter; b.AddressU = a.AddressU; b.AddressV = a.AddressV; b.AddressW = a.AddressW;
        b.MipLODBias = a.MipLODBias; b.MaxAnisotropy = a.MaxAnisotropy; b.ComparisonFunc = a.ComparisonFunc;
        for (unsigned i = 0; i != 4; ++i) b.BorderColor[i] = a.FloatBorderColor[i];
        b.MinLOD = a.MinLOD; b.MaxLOD = a.MaxLOD;
        D3D12DDIARG_CREATE_SAMPLER args{&b}; source.pfnCreateSampler(h, &args, o);
    }
    template<class Raster> static SIZE_T APIENTRY raster_size(D3D12DDI_HDEVICE h, const Raster* s) {
        if constexpr (std::is_same_v<Raster, D3D12DDI_RASTERIZER_DESC_0099>) {
            if (modern.raster_size99) return modern.raster_size99(h, s);
        } else if (modern.raster_size102) return modern.raster_size102(h, s);
        D3D12DDI_RASTERIZER_DESC_0010 d{};
        if (!s || !raster(*s, d)) { report(h, s ? E_NOTIMPL : E_INVALIDARG); return 0; }
        return source.pfnCalcPrivateRasterizerStateSize(h, &d);
    }
    template<class Raster> static void APIENTRY raster_create(D3D12DDI_HDEVICE h, const Raster* s, D3D12DDI_HRASTERIZERSTATE o) {
        if constexpr (std::is_same_v<Raster, D3D12DDI_RASTERIZER_DESC_0099>) {
            if (modern.raster_create99) { modern.raster_create99(h, s, o); return; }
        } else if (modern.raster_create102) { modern.raster_create102(h, s, o); return; }
        D3D12DDI_RASTERIZER_DESC_0010 d{};
        if (!s || !raster(*s, d)) { report(h, s ? E_NOTIMPL : E_INVALIDARG); return; }
        source.pfnCreateRasterizerState(h, &d, o);
    }
    template<class Pipeline> static SIZE_T APIENTRY pipeline_size(D3D12DDI_HDEVICE h, const Pipeline* s) {
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 d{};
        if (!s || !pipeline(*s, d, modern.pipeline_create != nullptr)) { report(h, s ? E_NOTIMPL : E_INVALIDARG); return 0; }
        return source.pfnCalcPrivatePipelineStateSize(h, &d);
    }
    template<class Pipeline> static HRESULT APIENTRY pipeline_create(D3D12DDI_HDEVICE h, const Pipeline* s, D3D12DDI_HPIPELINESTATE o, D3D12DDI_HRTPIPELINESTATE rt) {
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 d{};
        if (!s || !pipeline(*s, d, modern.pipeline_create != nullptr)) return creation_status("CreatePipelineState", s ? E_NOTIMPL : E_INVALIDARG);
        if (modern.pipeline_create) return creation_status("CreatePipelineState", modern.pipeline_create(h, &d, o, rt, s->Flags));
        return creation_status("CreatePipelineState", source.pfnCreatePipelineState(h, &d, o, rt));
    }
    static SIZE_T APIENTRY root_size(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100* s) {
        if (modern.root_size) return modern.root_size(h, s);
        Root d; const HRESULT hr = d.convert(s);
        if (FAILED(hr)) { report(h, hr); return 0; }
        return source.pfnCalcPrivateRootSignatureSize(h, &d.args);
    }
    static HRESULT APIENTRY root_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100* s, D3D12DDI_HROOTSIGNATURE o) {
        if (modern.root_create) return creation_status("CreateRootSignature", modern.root_create(h, s, o));
        Root d; const HRESULT hr = d.convert(s);
        return creation_status("CreateRootSignature", FAILED(hr) ? hr : source.pfnCreateRootSignature(h, &d.args, o));
    }
    template<class Resource> static D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY resource_size(
        D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001* heap, const Resource* s, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session) {
        D3D12DDIARG_CREATERESOURCE_0088 d{};
        if (s && !resource(*s, d)) { report(h, E_NOTIMPL); return {}; }
        return source.pfnCalcPrivateHeapAndResourceSizes(h, heap, s ? &d : nullptr, session);
    }
    template<class Resource> static HRESULT APIENTRY resource_create(
        D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATEHEAP_0001* heap, D3D12DDI_HHEAP hh, D3D12DDI_HRTRESOURCE rt,
        const Resource* s, const D3D12DDI_CLEAR_VALUES* clear, D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE o) {
        D3D12DDIARG_CREATERESOURCE_0088 d{};
        if (s && !resource(*s, d)) return creation_status("CreateHeapAndResource", E_NOTIMPL);
        // Forward through the composed shell callback, including its owner scope.
        return creation_status("CreateHeapAndResource", source.pfnCreateHeapAndResource(h, heap, hh, rt, s ? &d : nullptr, clear, session, o));
    }
    template<class Resource> static void APIENTRY resource_info(D3D12DDI_HDEVICE h, const Resource* s,
        D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS flags, UINT32 alignment, UINT nodes, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* o) {
        if (!o) { report(h, E_INVALIDARG); return; }
        *o = {};
        if (!s || (s->NumCastableFormats && !s->pCastableFormats)) { report(h, E_INVALIDARG); return; }
        D3D12DDIARG_CREATERESOURCE_0088 d{};
        if (!resource(*s, d)) {
            // Allocation questions about a valid but unsupported description do
            // not permit SetErrorCb. Match resources.cpp's unsized answer.
            o->ResourceDataSize = UINT64_MAX;
            o->ResourceDataAlignment = s->SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                              : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            o->Layout = s->ResourceType == D3D12DDI_RT_BUFFER ? D3D12DDI_TL_ROW_MAJOR : s->Layout;
            o->AdditionalDataHeaderAlignment = o->ResourceDataAlignment;
            o->AdditionalDataAlignment = o->ResourceDataAlignment;
            return;
        }
        source.pfnCheckResourceAllocationInfo(h, &d, flags, alignment, nodes, o);
    }
    template<class Fence> static SIZE_T APIENTRY fence_size(D3D12DDI_HDEVICE h, const Fence* s) {
        if (!s || s->FenceType != D3D12DDI_FENCE_TYPE_MONITORED) { report(h, s ? E_NOTIMPL : E_INVALIDARG); return 0; }
        D3D12DDI_FENCE f = s->MonitoredFenceArgs; D3D12DDIARG_CREATE_FENCE d{1, &f};
        return source.pfnCalcPrivateFenceSize(h, &d);
    }
    template<class Fence> static HRESULT APIENTRY fence_create(D3D12DDI_HDEVICE h, D3D12DDI_HFENCE o, D3D12DDI_HRTFENCE, const Fence* s) {
        if (!s || s->FenceType != D3D12DDI_FENCE_TYPE_MONITORED) return creation_status("CreateFence", s ? E_NOTIMPL : E_INVALIDARG);
        D3D12DDI_FENCE f = s->MonitoredFenceArgs; D3D12DDIARG_CREATE_FENCE d{1, &f};
        return creation_status("CreateFence", source.pfnCreateFence(h, o, &d));
    }
    static D3D12DDI_PROGRAM_IDENTIFIER_0108 APIENTRY program(D3D12DDI_HSTATEOBJECT_0054 h, LPCWSTR name) {
        return modern.pfnGetProgramIdentifier ? modern.pfnGetProgramIdentifier(h, name) : D3D12DDI_PROGRAM_IDENTIFIER_0108{};
    }
    static void APIENTRY memory(D3D12DDI_HSTATEOBJECT_0054, LPCWSTR, D3D12DDI_WORK_GRAPH_MEMORY_REQUIREMENTS_0108* o) {
        if (o) *o = {};
    }
    static void APIENTRY layouts(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATERESOURCE_0111*, D3D12DDI_BARRIER_ACCESS,
        D3D12DDI_BARRIER_ACCESS, UINT* count, GUID*) {
        if (count) *count = 0; else report(h, E_INVALIDARG);
    }
    static UINT APIENTRY blob_size(D3D12DDI_HDEVICE) { return 0; }
    static HRESULT APIENTRY blob(D3D12DDI_HDEVICE, void*, UINT) { return E_NOTIMPL; }
    static D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_STATUS APIENTRY blob_status(D3D12DDI_HDEVICE) {
        return D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED;
    }
};

template<class Core> constexpr unsigned revision = 0;
#define BC250_CORE_REV(rev, number) template<> inline constexpr unsigned revision<D3D12DDI_DEVICE_FUNCS_CORE_##rev> = number;
BC250_CORE_REV(0095, 95) BC250_CORE_REV(0096, 96) BC250_CORE_REV(0099, 99) BC250_CORE_REV(0100, 100)
BC250_CORE_REV(0102, 102) BC250_CORE_REV(0108, 108) BC250_CORE_REV(0109, 109) BC250_CORE_REV(0111, 111)
BC250_CORE_REV(0112, 112) BC250_CORE_REV(0113, 113) BC250_CORE_REV(0116, 116) BC250_CORE_REV(0119, 119)
#undef BC250_CORE_REV

} // namespace modern_core

// Initialization and callback identity checks are serialized. Once published, a
// revision's source is immutable. Refuse a different backend instead of silently
// redirecting already published device tables to it.
template<class Core> bool upgrade_core(const D3D12DDI_DEVICE_FUNCS_CORE_0088& source, Core* out, modern_core::Error report,
                                      const ModernCoreEntries& modern = {}) {
    static_assert(modern_core::revision<Core> != 0, "unsupported modern CORE type");
    using B = modern_core::Bridge<Core>;
    constexpr unsigned rev = modern_core::revision<Core>;
    if (!out || !report) return false;
    std::lock_guard<std::mutex> lock(B::mutex);
    if (B::ready && B::modern != modern) return false;
    if (!B::ready) B::modern = modern;
    // Named source comparisons and assignments below are generated from the WDK
    // declarations. The compiler verifies each function-pointer assignment.
    if (B::ready && B::source.pfnCheckFormatSupport != source.pfnCheckFormatSupport) return false;
    if (B::ready && B::source.pfnCheckMultisampleQualityLevels != source.pfnCheckMultisampleQualityLevels) return false;
    if (B::ready && B::source.pfnGetMipPacking != source.pfnGetMipPacking) return false;
    if (B::ready && B::source.pfnCalcPrivateElementLayoutSize != source.pfnCalcPrivateElementLayoutSize) return false;
    if (B::ready && B::source.pfnCreateElementLayout != source.pfnCreateElementLayout) return false;
    if (B::ready && B::source.pfnDestroyElementLayout != source.pfnDestroyElementLayout) return false;
    if (B::ready && B::source.pfnCalcPrivateBlendStateSize != source.pfnCalcPrivateBlendStateSize) return false;
    if (B::ready && B::source.pfnCreateBlendState != source.pfnCreateBlendState) return false;
    if (B::ready && B::source.pfnDestroyBlendState != source.pfnDestroyBlendState) return false;
    if (B::ready && B::source.pfnCalcPrivateDepthStencilStateSize != source.pfnCalcPrivateDepthStencilStateSize) return false;
    if (B::ready && B::source.pfnCreateDepthStencilState != source.pfnCreateDepthStencilState) return false;
    if (B::ready && B::source.pfnDestroyDepthStencilState != source.pfnDestroyDepthStencilState) return false;
    if (B::ready && B::source.pfnCalcPrivateRasterizerStateSize != source.pfnCalcPrivateRasterizerStateSize) return false;
    if (B::ready && B::source.pfnCreateRasterizerState != source.pfnCreateRasterizerState) return false;
    if (B::ready && B::source.pfnDestroyRasterizerState != source.pfnDestroyRasterizerState) return false;
    if (B::ready && B::source.pfnCalcPrivateShaderSize != source.pfnCalcPrivateShaderSize) return false;
    if (B::ready && B::source.pfnCreateVertexShader != source.pfnCreateVertexShader) return false;
    if (B::ready && B::source.pfnCreatePixelShader != source.pfnCreatePixelShader) return false;
    if (B::ready && B::source.pfnCreateGeometryShader != source.pfnCreateGeometryShader) return false;
    if (B::ready && B::source.pfnCreateComputeShader != source.pfnCreateComputeShader) return false;
    if (B::ready && B::source.pfnCalcPrivateGeometryShaderWithStreamOutput != source.pfnCalcPrivateGeometryShaderWithStreamOutput) return false;
    if (B::ready && B::source.pfnCreateGeometryShaderWithStreamOutput != source.pfnCreateGeometryShaderWithStreamOutput) return false;
    if (B::ready && B::source.pfnCalcPrivateTessellationShaderSize != source.pfnCalcPrivateTessellationShaderSize) return false;
    if (B::ready && B::source.pfnCreateHullShader != source.pfnCreateHullShader) return false;
    if (B::ready && B::source.pfnCreateDomainShader != source.pfnCreateDomainShader) return false;
    if (B::ready && B::source.pfnDestroyShader != source.pfnDestroyShader) return false;
    if (B::ready && B::source.pfnCalcPrivateCommandQueueSize != source.pfnCalcPrivateCommandQueueSize) return false;
    if (B::ready && B::source.pfnCreateCommandQueue != source.pfnCreateCommandQueue) return false;
    if (B::ready && B::source.pfnDestroyCommandQueue != source.pfnDestroyCommandQueue) return false;
    if (B::ready && B::source.pfnCalcPrivateCommandPoolSize != source.pfnCalcPrivateCommandPoolSize) return false;
    if (B::ready && B::source.pfnCreateCommandPool != source.pfnCreateCommandPool) return false;
    if (B::ready && B::source.pfnDestroyCommandPool != source.pfnDestroyCommandPool) return false;
    if (B::ready && B::source.pfnResetCommandPool != source.pfnResetCommandPool) return false;
    if (B::ready && B::source.pfnCalcPrivatePipelineStateSize != source.pfnCalcPrivatePipelineStateSize) return false;
    if (B::ready && B::source.pfnCreatePipelineState != source.pfnCreatePipelineState) return false;
    if (B::ready && B::source.pfnDestroyPipelineState != source.pfnDestroyPipelineState) return false;
    if (B::ready && B::source.pfnCalcPrivateCommandListSize != source.pfnCalcPrivateCommandListSize) return false;
    if (B::ready && B::source.pfnCreateCommandList != source.pfnCreateCommandList) return false;
    if (B::ready && B::source.pfnDestroyCommandList != source.pfnDestroyCommandList) return false;
    if (B::ready && B::source.pfnCalcPrivateFenceSize != source.pfnCalcPrivateFenceSize) return false;
    if (B::ready && B::source.pfnCreateFence != source.pfnCreateFence) return false;
    if (B::ready && B::source.pfnDestroyFence != source.pfnDestroyFence) return false;
    if (B::ready && B::source.pfnCalcPrivateDescriptorHeapSize != source.pfnCalcPrivateDescriptorHeapSize) return false;
    if (B::ready && B::source.pfnCreateDescriptorHeap != source.pfnCreateDescriptorHeap) return false;
    if (B::ready && B::source.pfnDestroyDescriptorHeap != source.pfnDestroyDescriptorHeap) return false;
    if (B::ready && B::source.pfnGetDescriptorSizeInBytes != source.pfnGetDescriptorSizeInBytes) return false;
    if (B::ready && B::source.pfnGetCPUDescriptorHandleForHeapStart != source.pfnGetCPUDescriptorHandleForHeapStart) return false;
    if (B::ready && B::source.pfnGetGPUDescriptorHandleForHeapStart != source.pfnGetGPUDescriptorHandleForHeapStart) return false;
    if (B::ready && B::source.pfnCreateShaderResourceView != source.pfnCreateShaderResourceView) return false;
    if (B::ready && B::source.pfnCreateConstantBufferView != source.pfnCreateConstantBufferView) return false;
    if (B::ready && B::source.pfnCreateSampler != source.pfnCreateSampler) return false;
    if (B::ready && B::source.pfnCreateUnorderedAccessView != source.pfnCreateUnorderedAccessView) return false;
    if (B::ready && B::source.pfnCreateRenderTargetView != source.pfnCreateRenderTargetView) return false;
    if (B::ready && B::source.pfnCreateDepthStencilView != source.pfnCreateDepthStencilView) return false;
    if (B::ready && B::source.pfnCalcPrivateRootSignatureSize != source.pfnCalcPrivateRootSignatureSize) return false;
    if (B::ready && B::source.pfnCreateRootSignature != source.pfnCreateRootSignature) return false;
    if (B::ready && B::source.pfnDestroyRootSignature != source.pfnDestroyRootSignature) return false;
    if (B::ready && B::source.pfnMapHeap != source.pfnMapHeap) return false;
    if (B::ready && B::source.pfnUnmapHeap != source.pfnUnmapHeap) return false;
    if (B::ready && B::source.pfnCalcPrivateHeapAndResourceSizes != source.pfnCalcPrivateHeapAndResourceSizes) return false;
    if (B::ready && B::source.pfnCreateHeapAndResource != source.pfnCreateHeapAndResource) return false;
    if (B::ready && B::source.pfnDestroyHeapAndResource != source.pfnDestroyHeapAndResource) return false;
    if (B::ready && B::source.pfnMakeResident != source.pfnMakeResident) return false;
    if (B::ready && B::source.pfnEvict != source.pfnEvict) return false;
    if (B::ready && B::source.pfnCalcPrivateOpenedHeapAndResourceSizes != source.pfnCalcPrivateOpenedHeapAndResourceSizes) return false;
    if (B::ready && B::source.pfnOpenHeapAndResource != source.pfnOpenHeapAndResource) return false;
    if (B::ready && B::source.pfnCopyDescriptors != source.pfnCopyDescriptors) return false;
    if (B::ready && B::source.pfnCopyDescriptorsSimple != source.pfnCopyDescriptorsSimple) return false;
    if (B::ready && B::source.pfnCalcPrivateQueryHeapSize != source.pfnCalcPrivateQueryHeapSize) return false;
    if (B::ready && B::source.pfnCreateQueryHeap != source.pfnCreateQueryHeap) return false;
    if (B::ready && B::source.pfnDestroyQueryHeap != source.pfnDestroyQueryHeap) return false;
    if (B::ready && B::source.pfnCalcPrivateCommandSignatureSize != source.pfnCalcPrivateCommandSignatureSize) return false;
    if (B::ready && B::source.pfnCreateCommandSignature != source.pfnCreateCommandSignature) return false;
    if (B::ready && B::source.pfnDestroyCommandSignature != source.pfnDestroyCommandSignature) return false;
    if (B::ready && B::source.pfnCheckResourceVirtualAddress != source.pfnCheckResourceVirtualAddress) return false;
    if (B::ready && B::source.pfnCheckResourceAllocationInfo != source.pfnCheckResourceAllocationInfo) return false;
    if (B::ready && B::source.pfnCheckSubresourceInfo != source.pfnCheckSubresourceInfo) return false;
    if (B::ready && B::source.pfnCheckExistingResourceAllocationInfo != source.pfnCheckExistingResourceAllocationInfo) return false;
    if (B::ready && B::source.pfnOfferResources != source.pfnOfferResources) return false;
    if (B::ready && B::source.pfnReclaimResources != source.pfnReclaimResources) return false;
    if (B::ready && B::source.pfnGetImplicitPhysicalAdapterMask != source.pfnGetImplicitPhysicalAdapterMask) return false;
    if (B::ready && B::source.pfnGetPresentPrivateDriverDataSize != source.pfnGetPresentPrivateDriverDataSize) return false;
    if (B::ready && B::source.pfnQueryNodeMap != source.pfnQueryNodeMap) return false;
    if (B::ready && B::source.pfnRetrieveShaderComment != source.pfnRetrieveShaderComment) return false;
    if (B::ready && B::source.pfnCheckResourceAllocationHandle != source.pfnCheckResourceAllocationHandle) return false;
    if (B::ready && B::source.pfnCalcPrivatePipelineLibrarySize != source.pfnCalcPrivatePipelineLibrarySize) return false;
    if (B::ready && B::source.pfnCreatePipelineLibrary != source.pfnCreatePipelineLibrary) return false;
    if (B::ready && B::source.pfnDestroyPipelineLibrary != source.pfnDestroyPipelineLibrary) return false;
    if (B::ready && B::source.pfnAddPipelineStateToLibrary != source.pfnAddPipelineStateToLibrary) return false;
    if (B::ready && B::source.pfnCalcSerializedLibrarySize != source.pfnCalcSerializedLibrarySize) return false;
    if (B::ready && B::source.pfnSerializeLibrary != source.pfnSerializeLibrary) return false;
    if (B::ready && B::source.pfnGetDebugAllocationInfo != source.pfnGetDebugAllocationInfo) return false;
    if (B::ready && B::source.pfnCalcPrivateCommandRecorderSize != source.pfnCalcPrivateCommandRecorderSize) return false;
    if (B::ready && B::source.pfnCreateCommandRecorder != source.pfnCreateCommandRecorder) return false;
    if (B::ready && B::source.pfnDestroyCommandRecorder != source.pfnDestroyCommandRecorder) return false;
    if (B::ready && B::source.pfnCommandRecorderSetCommandPoolAsTarget != source.pfnCommandRecorderSetCommandPoolAsTarget) return false;
    if (B::ready && B::source.pfnCalcPrivateSchedulingGroupSize != source.pfnCalcPrivateSchedulingGroupSize) return false;
    if (B::ready && B::source.pfnCreateSchedulingGroup != source.pfnCreateSchedulingGroup) return false;
    if (B::ready && B::source.pfnDestroySchedulingGroup != source.pfnDestroySchedulingGroup) return false;
    if (B::ready && B::source.pfnEnumerateMetaCommands != source.pfnEnumerateMetaCommands) return false;
    if (B::ready && B::source.pfnEnumerateMetaCommandParameters != source.pfnEnumerateMetaCommandParameters) return false;
    if (B::ready && B::source.pfnCalcPrivateMetaCommandSize != source.pfnCalcPrivateMetaCommandSize) return false;
    if (B::ready && B::source.pfnCreateMetaCommand != source.pfnCreateMetaCommand) return false;
    if (B::ready && B::source.pfnDestroyMetaCommand != source.pfnDestroyMetaCommand) return false;
    if (B::ready && B::source.pfnGetMetaCommandRequiredParameterInfo != source.pfnGetMetaCommandRequiredParameterInfo) return false;
    if (B::ready && B::source.pfnCalcPrivateStateObjectSize != source.pfnCalcPrivateStateObjectSize) return false;
    if (B::ready && B::source.pfnCreateStateObject != source.pfnCreateStateObject) return false;
    if (B::ready && B::source.pfnDestroyStateObject != source.pfnDestroyStateObject) return false;
    if (B::ready && B::source.pfnGetRaytracingAccelerationStructurePrebuildInfo != source.pfnGetRaytracingAccelerationStructurePrebuildInfo) return false;
    if (B::ready && B::source.pfnCheckDriverMatchingIdentifier != source.pfnCheckDriverMatchingIdentifier) return false;
    if (B::ready && B::source.pfnGetShaderIdentifier != source.pfnGetShaderIdentifier) return false;
    if (B::ready && B::source.pfnGetShaderStackSize != source.pfnGetShaderStackSize) return false;
    if (B::ready && B::source.pfnGetPipelineStackSize != source.pfnGetPipelineStackSize) return false;
    if (B::ready && B::source.pfnSetPipelineStackSize != source.pfnSetPipelineStackSize) return false;
    if (B::ready && B::source.pfnSetBackgroundProcessingMode != source.pfnSetBackgroundProcessingMode) return false;
    if (B::ready && B::source.pfnCalcPrivateAddToStateObjectSize != source.pfnCalcPrivateAddToStateObjectSize) return false;
    if (B::ready && B::source.pfnAddToStateObject != source.pfnAddToStateObject) return false;
    if (B::ready && B::source.pfnCreateSamplerFeedbackUnorderedAccessView != source.pfnCreateSamplerFeedbackUnorderedAccessView) return false;
    if (B::ready && B::source.pfnCreateAmplificationShader != source.pfnCreateAmplificationShader) return false;
    if (B::ready && B::source.pfnCreateMeshShader != source.pfnCreateMeshShader) return false;
    if (B::ready && B::source.pfnCalcPrivateMeshShaderSize != source.pfnCalcPrivateMeshShaderSize) return false;
    if (B::ready && B::source.pfnImplicitShaderCacheControl != source.pfnImplicitShaderCacheControl) return false;
    if (B::ready && B::report != report) return false;
    if (!B::ready) { B::source = source; B::report = report; B::ready = true; }
    *out = {};
    out->pfnCheckFormatSupport = source.pfnCheckFormatSupport;
    out->pfnCheckMultisampleQualityLevels = source.pfnCheckMultisampleQualityLevels;
    out->pfnGetMipPacking = source.pfnGetMipPacking;
    out->pfnCalcPrivateElementLayoutSize = source.pfnCalcPrivateElementLayoutSize;
    out->pfnCreateElementLayout = source.pfnCreateElementLayout;
    out->pfnDestroyElementLayout = source.pfnDestroyElementLayout;
    out->pfnCalcPrivateBlendStateSize = source.pfnCalcPrivateBlendStateSize;
    out->pfnCreateBlendState = source.pfnCreateBlendState;
    out->pfnDestroyBlendState = source.pfnDestroyBlendState;
    out->pfnDestroyDepthStencilState = source.pfnDestroyDepthStencilState;
    out->pfnDestroyRasterizerState = source.pfnDestroyRasterizerState;
    out->pfnCalcPrivateShaderSize = source.pfnCalcPrivateShaderSize;
    out->pfnCreateVertexShader = source.pfnCreateVertexShader;
    out->pfnCreatePixelShader = source.pfnCreatePixelShader;
    out->pfnCreateGeometryShader = source.pfnCreateGeometryShader;
    out->pfnCreateComputeShader = source.pfnCreateComputeShader;
    out->pfnCalcPrivateGeometryShaderWithStreamOutput = source.pfnCalcPrivateGeometryShaderWithStreamOutput;
    out->pfnCreateGeometryShaderWithStreamOutput = source.pfnCreateGeometryShaderWithStreamOutput;
    out->pfnCalcPrivateTessellationShaderSize = source.pfnCalcPrivateTessellationShaderSize;
    out->pfnCreateHullShader = source.pfnCreateHullShader;
    out->pfnCreateDomainShader = source.pfnCreateDomainShader;
    out->pfnDestroyShader = source.pfnDestroyShader;
    out->pfnCalcPrivateCommandQueueSize = source.pfnCalcPrivateCommandQueueSize;
    out->pfnCreateCommandQueue = source.pfnCreateCommandQueue;
    out->pfnDestroyCommandQueue = source.pfnDestroyCommandQueue;
    out->pfnCalcPrivateCommandPoolSize = source.pfnCalcPrivateCommandPoolSize;
    out->pfnCreateCommandPool = source.pfnCreateCommandPool;
    out->pfnDestroyCommandPool = source.pfnDestroyCommandPool;
    out->pfnResetCommandPool = source.pfnResetCommandPool;
    out->pfnDestroyPipelineState = source.pfnDestroyPipelineState;
    out->pfnCalcPrivateCommandListSize = source.pfnCalcPrivateCommandListSize;
    out->pfnCreateCommandList = source.pfnCreateCommandList;
    out->pfnDestroyCommandList = source.pfnDestroyCommandList;
    out->pfnDestroyFence = source.pfnDestroyFence;
    out->pfnCalcPrivateDescriptorHeapSize = source.pfnCalcPrivateDescriptorHeapSize;
    out->pfnCreateDescriptorHeap = source.pfnCreateDescriptorHeap;
    out->pfnDestroyDescriptorHeap = source.pfnDestroyDescriptorHeap;
    out->pfnGetDescriptorSizeInBytes = source.pfnGetDescriptorSizeInBytes;
    out->pfnGetCPUDescriptorHandleForHeapStart = source.pfnGetCPUDescriptorHandleForHeapStart;
    out->pfnGetGPUDescriptorHandleForHeapStart = source.pfnGetGPUDescriptorHandleForHeapStart;
    out->pfnCreateShaderResourceView = source.pfnCreateShaderResourceView;
    out->pfnCreateConstantBufferView = source.pfnCreateConstantBufferView;
    out->pfnCreateUnorderedAccessView = source.pfnCreateUnorderedAccessView;
    out->pfnCreateRenderTargetView = source.pfnCreateRenderTargetView;
    out->pfnCreateDepthStencilView = source.pfnCreateDepthStencilView;
    out->pfnDestroyRootSignature = source.pfnDestroyRootSignature;
    out->pfnMapHeap = source.pfnMapHeap;
    out->pfnUnmapHeap = source.pfnUnmapHeap;
    out->pfnDestroyHeapAndResource = source.pfnDestroyHeapAndResource;
    out->pfnMakeResident = source.pfnMakeResident;
    out->pfnEvict = source.pfnEvict;
    out->pfnCalcPrivateOpenedHeapAndResourceSizes = source.pfnCalcPrivateOpenedHeapAndResourceSizes;
    out->pfnOpenHeapAndResource = source.pfnOpenHeapAndResource;
    out->pfnCopyDescriptors = source.pfnCopyDescriptors;
    out->pfnCopyDescriptorsSimple = source.pfnCopyDescriptorsSimple;
    out->pfnCalcPrivateQueryHeapSize = source.pfnCalcPrivateQueryHeapSize;
    out->pfnCreateQueryHeap = source.pfnCreateQueryHeap;
    out->pfnDestroyQueryHeap = source.pfnDestroyQueryHeap;
    out->pfnCalcPrivateCommandSignatureSize = source.pfnCalcPrivateCommandSignatureSize;
    out->pfnCreateCommandSignature = source.pfnCreateCommandSignature;
    out->pfnDestroyCommandSignature = source.pfnDestroyCommandSignature;
    out->pfnCheckResourceVirtualAddress = source.pfnCheckResourceVirtualAddress;
    out->pfnCheckSubresourceInfo = source.pfnCheckSubresourceInfo;
    out->pfnCheckExistingResourceAllocationInfo = source.pfnCheckExistingResourceAllocationInfo;
    out->pfnOfferResources = source.pfnOfferResources;
    out->pfnReclaimResources = source.pfnReclaimResources;
    out->pfnGetImplicitPhysicalAdapterMask = source.pfnGetImplicitPhysicalAdapterMask;
    out->pfnGetPresentPrivateDriverDataSize = source.pfnGetPresentPrivateDriverDataSize;
    out->pfnQueryNodeMap = source.pfnQueryNodeMap;
    out->pfnRetrieveShaderComment = source.pfnRetrieveShaderComment;
    out->pfnCheckResourceAllocationHandle = source.pfnCheckResourceAllocationHandle;
    out->pfnCalcPrivatePipelineLibrarySize = source.pfnCalcPrivatePipelineLibrarySize;
    out->pfnCreatePipelineLibrary = source.pfnCreatePipelineLibrary;
    out->pfnDestroyPipelineLibrary = source.pfnDestroyPipelineLibrary;
    out->pfnAddPipelineStateToLibrary = source.pfnAddPipelineStateToLibrary;
    out->pfnCalcSerializedLibrarySize = source.pfnCalcSerializedLibrarySize;
    out->pfnSerializeLibrary = source.pfnSerializeLibrary;
    out->pfnGetDebugAllocationInfo = source.pfnGetDebugAllocationInfo;
    out->pfnCalcPrivateCommandRecorderSize = source.pfnCalcPrivateCommandRecorderSize;
    out->pfnCreateCommandRecorder = source.pfnCreateCommandRecorder;
    out->pfnDestroyCommandRecorder = source.pfnDestroyCommandRecorder;
    out->pfnCommandRecorderSetCommandPoolAsTarget = source.pfnCommandRecorderSetCommandPoolAsTarget;
    out->pfnCalcPrivateSchedulingGroupSize = source.pfnCalcPrivateSchedulingGroupSize;
    out->pfnCreateSchedulingGroup = source.pfnCreateSchedulingGroup;
    out->pfnDestroySchedulingGroup = source.pfnDestroySchedulingGroup;
    out->pfnEnumerateMetaCommands = source.pfnEnumerateMetaCommands;
    out->pfnEnumerateMetaCommandParameters = source.pfnEnumerateMetaCommandParameters;
    out->pfnCalcPrivateMetaCommandSize = source.pfnCalcPrivateMetaCommandSize;
    out->pfnCreateMetaCommand = source.pfnCreateMetaCommand;
    out->pfnDestroyMetaCommand = source.pfnDestroyMetaCommand;
    out->pfnGetMetaCommandRequiredParameterInfo = source.pfnGetMetaCommandRequiredParameterInfo;
    out->pfnCalcPrivateStateObjectSize = source.pfnCalcPrivateStateObjectSize;
    out->pfnCreateStateObject = source.pfnCreateStateObject;
    out->pfnDestroyStateObject = source.pfnDestroyStateObject;
    out->pfnGetRaytracingAccelerationStructurePrebuildInfo = source.pfnGetRaytracingAccelerationStructurePrebuildInfo;
    out->pfnCheckDriverMatchingIdentifier = source.pfnCheckDriverMatchingIdentifier;
    out->pfnGetShaderIdentifier = source.pfnGetShaderIdentifier;
    out->pfnGetShaderStackSize = source.pfnGetShaderStackSize;
    out->pfnGetPipelineStackSize = source.pfnGetPipelineStackSize;
    out->pfnSetPipelineStackSize = source.pfnSetPipelineStackSize;
    out->pfnSetBackgroundProcessingMode = source.pfnSetBackgroundProcessingMode;
    out->pfnCalcPrivateAddToStateObjectSize = source.pfnCalcPrivateAddToStateObjectSize;
    out->pfnAddToStateObject = source.pfnAddToStateObject;
    out->pfnCreateSamplerFeedbackUnorderedAccessView = source.pfnCreateSamplerFeedbackUnorderedAccessView;
    out->pfnCreateAmplificationShader = source.pfnCreateAmplificationShader;
    out->pfnCreateMeshShader = source.pfnCreateMeshShader;
    out->pfnCalcPrivateMeshShaderSize = source.pfnCalcPrivateMeshShaderSize;
    out->pfnImplicitShaderCacheControl = source.pfnImplicitShaderCacheControl;
    out->pfnCalcPrivateDepthStencilStateSize = B::depth_size;
    out->pfnCreateDepthStencilState = B::depth_create;
    if constexpr (rev >= 96) out->pfnCreateSampler = B::sampler;
    else out->pfnCreateSampler = source.pfnCreateSampler;
    if constexpr (rev >= 99) {
        using Raster = std::conditional_t<(rev >= 102), D3D12DDI_RASTERIZER_DESC_0102, D3D12DDI_RASTERIZER_DESC_0099>;
        out->pfnCalcPrivateRasterizerStateSize = B::template raster_size<Raster>;
        out->pfnCreateRasterizerState = B::template raster_create<Raster>;
        using Pipeline = std::conditional_t<(rev >= 119), D3D12DDIARG_CREATE_PIPELINE_STATE_0119, D3D12DDIARG_CREATE_PIPELINE_STATE_0099>;
        out->pfnCalcPrivatePipelineStateSize = B::template pipeline_size<Pipeline>;
        out->pfnCreatePipelineState = B::template pipeline_create<Pipeline>;
    } else {
        out->pfnCalcPrivateRasterizerStateSize = source.pfnCalcPrivateRasterizerStateSize;
        out->pfnCreateRasterizerState = source.pfnCreateRasterizerState;
        out->pfnCalcPrivatePipelineStateSize = source.pfnCalcPrivatePipelineStateSize;
        out->pfnCreatePipelineState = source.pfnCreatePipelineState;
    }
    if constexpr (rev >= 100) {
        out->pfnCalcPrivateRootSignatureSize = B::root_size; out->pfnCreateRootSignature = B::root_create;
    } else {
        out->pfnCalcPrivateRootSignatureSize = source.pfnCalcPrivateRootSignatureSize;
        out->pfnCreateRootSignature = source.pfnCreateRootSignature;
    }
    if constexpr (rev >= 108) {
        out->pfnGetProgramIdentifier = B::program; out->pfnGetWorkGraphMemoryRequirements = B::memory;
    }
    if constexpr (rev >= 109) {
        using Resource = std::conditional_t<(rev >= 111), D3D12DDIARG_CREATERESOURCE_0111, D3D12DDIARG_CREATERESOURCE_0109>;
        out->pfnCalcPrivateHeapAndResourceSizes = B::template resource_size<Resource>;
        out->pfnCreateHeapAndResource = B::template resource_create<Resource>;
        out->pfnCheckResourceAllocationInfo = B::template resource_info<Resource>;
    } else {
        out->pfnCalcPrivateHeapAndResourceSizes = source.pfnCalcPrivateHeapAndResourceSizes;
        out->pfnCreateHeapAndResource = source.pfnCreateHeapAndResource;
        out->pfnCheckResourceAllocationInfo = source.pfnCheckResourceAllocationInfo;
    }
    if constexpr (rev >= 111) out->pfnGetTextureLayouts = B::layouts;
    if constexpr (rev >= 112) {
        using Fence = std::conditional_t<(rev >= 116), D3D12DDIARG_CREATE_FENCE_0116, D3D12DDIARG_CREATE_FENCE_0112>;
        out->pfnCalcPrivateFenceSize = B::template fence_size<Fence>;
        out->pfnCreateFence = B::template fence_create<Fence>;
    } else {
        out->pfnCalcPrivateFenceSize = source.pfnCalcPrivateFenceSize; out->pfnCreateFence = source.pfnCreateFence;
    }
    if constexpr (rev >= 113) {
        out->pfnGetApplicationSpecificDriverState = B::blob;
        out->pfnGetApplicationSpecificDriverBlobStatus = B::blob_status;
        out->pfnGetApplicationSpecificDriverStateBlobSize = B::blob_size;
    }
    return true;
}
} // namespace native12
