// SPDX-License-Identifier: MIT
// Host-only typed CORE upgrade contract. No engine, graphics device or lab needed.
#include <windows.h>
#include "../ddi-modern-core.h"
#include <cassert>
#include <cstdio>

namespace {
HRESULT last_error = S_OK;
unsigned resource_calls = 0;
D3D12DDIARG_CREATERESOURCE_0088 last_resource{};
bool heap_only = false;
UINT allocation_calls = 0;
UINT allocation_floor = 0;
void APIENTRY allocation_info(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATERESOURCE_0088* resource,
    D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS, UINT32 floor, UINT, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) {
    ++allocation_calls; last_resource = *resource; allocation_floor = floor;
    *out = {}; out->ResourceDataSize = resource->Width; out->ResourceDataAlignment = floor;
}
HRESULT resource_result = S_OK;
void APIENTRY error(D3D12DDI_HDEVICE, HRESULT hr) { last_error = hr; }
HRESULT APIENTRY create_resource(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001*, D3D12DDI_HHEAP,
    D3D12DDI_HRTRESOURCE, const D3D12DDIARG_CREATERESOURCE_0088* resource, const D3D12DDI_CLEAR_VALUES*,
    D3D12DDI_HPROTECTEDRESOURCESESSION_0030, D3D12DDI_HRESOURCE) {
    ++resource_calls; heap_only = resource == nullptr;
    if (resource) last_resource = *resource;
    return resource_result;
}
SIZE_T APIENTRY fence_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_FENCE* f) {
    assert(f && f->FenceCount == 1 && f->Fences); return 123;
}
HRESULT APIENTRY fence_create(D3D12DDI_HDEVICE h, D3D12DDI_HFENCE, const D3D12DDIARG_CREATE_FENCE* f) {
    assert(fence_size(h, f) == 123); return S_OK;
}
SIZE_T APIENTRY root_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013* s) {
    assert(s->Version == D3D12DDI_ROOT_SIGNATURE_VERSION_1_1);
    assert(s->pRootSignature_1_1->NumStaticSamplers == 1);
    assert(s->pRootSignature_1_1->pStaticSamplers[0].ShaderRegister == 17);
    return 41;
}
template<class Core> void table(const D3D12DDI_DEVICE_FUNCS_CORE_0088& source, const native12::ModernCoreEntries& entries = {}) {
    Core out{};
    assert(native12::upgrade_core(source, &out, error, entries));
    assert(out.pfnCreateDepthStencilState && out.pfnCalcPrivateDepthStencilStateSize);
    assert(out.pfnDestroyShader == source.pfnDestroyShader);
    assert(native12::upgrade_core(source, &out, error, entries));
    auto different = source;
    different.pfnCreateHeapAndResource = nullptr;
    assert(!native12::upgrade_core(different, &out, error, entries));
    assert(!native12::upgrade_core(source, &out, nullptr, entries));
}
SIZE_T APIENTRY modern_depth_size(D3D12DDI_HDEVICE, const D3D12DDI_DEPTH_STENCIL_DESC_0095* s) {
    assert(s->FrontFaceStencilReadMask == 0x0f && s->BackFaceStencilReadMask == 0xf0); return 95;
}
void APIENTRY modern_depth(D3D12DDI_HDEVICE h, const D3D12DDI_DEPTH_STENCIL_DESC_0095* s, D3D12DDI_HDEPTHSTENCILSTATE) {
    assert(modern_depth_size(h, s) == 95);
}
SIZE_T APIENTRY modern_raster_size(D3D12DDI_HDEVICE, const D3D12DDI_RASTERIZER_DESC_0102* s) {
    assert(s->DepthBias == 1.5f && s->LineRasterizationMode == D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW); return 102;
}
void APIENTRY modern_raster(D3D12DDI_HDEVICE h, const D3D12DDI_RASTERIZER_DESC_0102* s, D3D12DDI_HRASTERIZERSTATE) {
    assert(modern_raster_size(h, s) == 102);
}
void APIENTRY modern_sampler(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_SAMPLER_0096* s, D3D12DDI_CPU_DESCRIPTOR_HANDLE) {
    assert(s->pSamplerDesc->Flags == D3D12DDI_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES);
}
SIZE_T APIENTRY modern_root_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100* s) {
    assert(s->Version == D3D12DDI_ROOT_SIGNATURE_VERSION_1_2);
    assert(s->pRootSignature_1_2->pStaticSamplers[0].Flags == D3D12DDI_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES); return 100;
}
HRESULT APIENTRY modern_root(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100* s, D3D12DDI_HROOTSIGNATURE) {
    assert(modern_root_size(h, s) == 100); return S_OK;
}
HRESULT APIENTRY modern_pipeline(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_PIPELINE_STATE_0075* s,
                                 D3D12DDI_HPIPELINESTATE, D3D12DDI_HRTPIPELINESTATE, UINT flags) {
    assert(s->SampleMask == 0x12345678 && flags == 0x0c); return S_OK;
}
void mandatory_entries(const D3D12DDI_DEVICE_FUNCS_CORE_0088& source) {
    native12::ModernCoreEntries entries{};
    entries.depth_size = modern_depth_size; entries.depth_create = modern_depth;
    entries.raster_size102 = modern_raster_size; entries.raster_create102 = modern_raster;
    entries.sampler = modern_sampler; entries.root_size = modern_root_size; entries.root_create = modern_root;
    entries.pipeline_create = modern_pipeline;
    table<D3D12DDI_DEVICE_FUNCS_CORE_0102>(source, entries);
    D3D12DDI_DEVICE_FUNCS_CORE_0102 core{};
    assert(native12::upgrade_core(source, &core, error, entries));
    D3D12DDI_DEPTH_STENCIL_DESC_0095 depth{};
    depth.StencilEnable = depth.FrontEnable = depth.BackEnable = TRUE;
    depth.FrontFaceStencilReadMask = 0x0f; depth.BackFaceStencilReadMask = 0xf0;
    assert(core.pfnCalcPrivateDepthStencilStateSize({}, &depth) == 95);
    core.pfnCreateDepthStencilState({}, &depth, {});
    D3D12DDI_RASTERIZER_DESC_0102 raster{}; raster.DepthBias = 1.5f;
    raster.LineRasterizationMode = D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW;
    assert(core.pfnCalcPrivateRasterizerStateSize({}, &raster) == 102);
    core.pfnCreateRasterizerState({}, &raster, {});
    D3D12DDI_SAMPLER_DESC_0096 sampler{}; sampler.Flags = D3D12DDI_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES;
    D3D12DDIARG_CREATE_SAMPLER_0096 sampler_args{&sampler}; core.pfnCreateSampler({}, &sampler_args, {});
    D3D12DDI_STATIC_SAMPLER_0100 static_sampler{}; static_sampler.Flags = sampler.Flags;
    D3D12DDI_ROOT_SIGNATURE_0100 root{0, nullptr, 1, &static_sampler, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100 root_args{};
    root_args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_2; root_args.pRootSignature_1_2 = &root;
    assert(core.pfnCalcPrivateRootSignatureSize({}, &root_args) == 100);
    assert(core.pfnCreateRootSignature({}, &root_args, {}) == S_OK);
    D3D12DDIARG_CREATE_PIPELINE_STATE_0099 pipeline{}; pipeline.SampleMask = 0x12345678;
    pipeline.Flags = D3D12DDI_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS | D3D12DDI_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT;
    assert(core.pfnCreatePipelineState({}, &pipeline, {}, {}) == S_OK);
    entries.pipeline_create = nullptr;
    assert(!native12::upgrade_core(source, &core, error, entries));
}
// CORE 0113 application-specific driver state: the three slots go to the entries that the shell gives
// (driver/umd/d3d12/app-state-ddi.h). A table without them keeps the stubs (checked on 0119 in main).
int blob_calls = 0;
UINT APIENTRY shell_blob_size(D3D12DDI_HDEVICE h) { ++blob_calls; return h.pDrvPrivate ? 140u : 0u; }
HRESULT APIENTRY shell_blob(D3D12DDI_HDEVICE, void* data, UINT size) {
    ++blob_calls; return data && size >= 140 ? S_OK : E_INVALIDARG;
}
D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_STATUS APIENTRY shell_blob_status(D3D12DDI_HDEVICE) {
    ++blob_calls; return D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_USED;
}
void blob_entries(const D3D12DDI_DEVICE_FUNCS_CORE_0088& source) {
    native12::ModernCoreEntries entries{};
    entries.blob_size = shell_blob_size; entries.blob = shell_blob; entries.blob_status = shell_blob_status;
    table<D3D12DDI_DEVICE_FUNCS_CORE_0113>(source, entries);
    D3D12DDI_DEVICE_FUNCS_CORE_0113 core{};
    assert(native12::upgrade_core(source, &core, error, entries));
    int device = 0;
    unsigned char bytes[140]{};
    assert(core.pfnGetApplicationSpecificDriverStateBlobSize({&device}) == 140);
    assert(core.pfnGetApplicationSpecificDriverState({&device}, bytes, sizeof(bytes)) == S_OK);
    assert(core.pfnGetApplicationSpecificDriverState({&device}, bytes, 139) == E_INVALIDARG);
    assert(core.pfnGetApplicationSpecificDriverBlobStatus({&device}) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_USED);
    assert(blob_calls == 4);
    // One set of entries per revision: the stubs are not put back over the published shell entries.
    assert(!native12::upgrade_core(source, &core, error));
}
}

int main() {
    using namespace native12::modern_core;
    D3D12DDI_DEVICE_FUNCS_CORE_0088 source{};
    source.pfnCreateHeapAndResource = create_resource;
    source.pfnCheckResourceAllocationInfo = allocation_info;
    source.pfnCalcPrivateFenceSize = fence_size; source.pfnCreateFence = fence_create;
    source.pfnCalcPrivateRootSignatureSize = root_size;
#define CHECK_TABLE(rev) table<D3D12DDI_DEVICE_FUNCS_CORE_##rev>(source)
    CHECK_TABLE(0095); CHECK_TABLE(0096); CHECK_TABLE(0099); CHECK_TABLE(0100);
    mandatory_entries(source); CHECK_TABLE(0108); CHECK_TABLE(0109); CHECK_TABLE(0111);
    CHECK_TABLE(0112); blob_entries(source); CHECK_TABLE(0116); CHECK_TABLE(0119);
#undef CHECK_TABLE

    D3D12DDI_DEPTH_STENCIL_DESC_0095 ds{};
    D3D12DDI_DEPTH_STENCIL_DESC_0025 old_ds{};
    ds.StencilEnable = ds.FrontEnable = ds.BackEnable = TRUE;
    ds.FrontFaceStencilReadMask = ds.BackFaceStencilReadMask = 0xab;
    ds.FrontFaceStencilWriteMask = ds.BackFaceStencilWriteMask = 0xcd;
    assert(depth(ds, old_ds) && old_ds.StencilReadMask == 0xab && old_ds.StencilWriteMask == 0xcd);
    ds.BackFaceStencilWriteMask = 0xef;
    assert(!depth(ds, old_ds));
    ds.FrontEnable = FALSE;
    assert(depth(ds, old_ds) && old_ds.StencilWriteMask == 0xef);

    D3D12DDI_RASTERIZER_DESC_0099 rs{};
    D3D12DDI_RASTERIZER_DESC_0010 old_rs{};
    rs.DepthBias = -123;
    assert(raster(rs, old_rs) && old_rs.DepthBias == -123);
    rs.DepthBias = .5f; assert(!raster(rs, old_rs));
    rs.DepthBias = 2147483648.0f; assert(!raster(rs, old_rs));
    rs.DepthBias = -2147483648.0f; assert(raster(rs, old_rs));
    rs.DepthBias = (std::numeric_limits<float>::quiet_NaN)(); assert(!raster(rs, old_rs));
    rs.DepthBias = (std::numeric_limits<float>::infinity)(); assert(!raster(rs, old_rs));
    D3D12DDI_RASTERIZER_DESC_0102 rs102{};
    rs102.LineRasterizationMode = D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_WIDE;
    assert(raster(rs102, old_rs) && old_rs.MultisampleEnable);
    rs102.LineRasterizationMode = D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW;
    assert(!raster(rs102, old_rs));

    D3D12DDIARG_CREATE_PIPELINE_STATE_0119 ps{};
    D3D12DDIARG_CREATE_PIPELINE_STATE_0075 old_ps{};
    ps.NodeMask = 1; ps.SampleMask = 0x12345678; ps.RTVFormats[7] = DXGI_FORMAT_R8G8B8A8_UNORM;
    assert(pipeline(ps, old_ps) && old_ps.SampleMask == ps.SampleMask && old_ps.RTVFormats[7] == ps.RTVFormats[7]);
    ps.Flags = D3D12DDI_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS;
    assert(!pipeline(ps, old_ps));

    D3D12DDI_DEVICE_FUNCS_CORE_0119 core{};
    assert(native12::upgrade_core(source, &core, error));
    D3D12DDIARG_CREATERESOURCE_0111 resource_desc{};
    resource_desc.Width = 0x123456789ull; resource_desc.Height = 321;
    resource_desc.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM};
    resource_desc.NumCastableFormats = 1; resource_desc.pCastableFormats = formats;
    assert(core.pfnCreateHeapAndResource({}, nullptr, {}, {}, &resource_desc, nullptr, {}, {}) == S_OK);
    assert(resource_calls == 1 && last_resource.Width == resource_desc.Width && last_resource.Height == 321);
    assert(last_resource.pCastableFormats == formats && last_resource.NumCastableFormats == 1);
    resource_desc.CreateAtVirtualAddress = 4096;
    assert(core.pfnCreateHeapAndResource({}, nullptr, {}, {}, &resource_desc, nullptr, {}, {}) == E_OUTOFMEMORY);
    assert(resource_calls == 1);
    resource_desc.CreateAtVirtualAddress = 0; resource_desc.Layout = D3D12DDI_TL_GUID;
    assert(core.pfnCreateHeapAndResource({}, nullptr, {}, {}, &resource_desc, nullptr, {}, {}) == E_OUTOFMEMORY);
    resource_desc.Layout = D3D12DDI_TL_ROW_MAJOR; resource_desc.Flags = D3D12DDI_RESOURCE_FLAG_0111_USE_TIGHT_ALIGNMENT;
    assert(core.pfnCreateHeapAndResource({}, nullptr, {}, {}, &resource_desc, nullptr, {}, {}) == S_OK);
    assert(resource_calls == 2 && last_resource.Flags == resource_desc.Flags);
    assert(core.pfnCreateHeapAndResource({}, nullptr, {}, {}, nullptr, nullptr, {}, {}) == S_OK && heap_only);
    last_error = S_OK;
    resource_result = E_INVALIDARG;
    assert(core.pfnCreateHeapAndResource({}, nullptr, {}, {}, nullptr, nullptr, {}, {}) == E_OUTOFMEMORY);
    resource_result = DXGI_ERROR_DEVICE_REMOVED;
    assert(core.pfnCreateHeapAndResource({}, nullptr, {}, {}, nullptr, nullptr, {}, {}) == native12::kDdiDriverDeviceRemoved);
    assert(last_error == S_OK); // no second device error callback from a creation refusal
    resource_result = S_OK;
    assert(core.pfnCreatePipelineState({}, nullptr, {}, {}) == E_OUTOFMEMORY);
    assert(core.pfnCreateRootSignature({}, nullptr, {}) == E_OUTOFMEMORY);
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    last_error = S_OK;
    core.pfnCheckResourceAllocationInfo({}, &resource_desc, {}, 4096, 0, &info);
    assert(last_error == S_OK && info.ResourceDataSize == resource_desc.Width);
    assert(allocation_calls == 1 && allocation_floor == 4096 && last_resource.Flags == resource_desc.Flags);
    resource_desc.Flags = {}; resource_desc.Layout = D3D12DDI_TL_GUID; resource_desc.SampleDesc.Count = 4;
    core.pfnCheckResourceAllocationInfo({}, &resource_desc, {}, 0, 0, &info);
    assert(last_error == S_OK && info.ResourceDataSize == UINT64_MAX);
    assert(info.ResourceDataAlignment == D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT);
    assert(info.AdditionalDataAlignment == info.ResourceDataAlignment && info.AdditionalDataHeaderAlignment == info.ResourceDataAlignment);
    resource_desc.pCastableFormats = nullptr;
    core.pfnCheckResourceAllocationInfo({}, &resource_desc, {}, 0, 0, &info);
    assert(last_error == E_INVALIDARG && info.ResourceDataSize == 0);

    D3D12DDIARG_CREATE_FENCE_0116 fence{};
    assert(core.pfnCalcPrivateFenceSize({}, &fence) == 123);
    assert(core.pfnCreateFence({}, {}, {}, &fence) == S_OK);
    fence.FenceType = D3D12DDI_FENCE_TYPE_NATIVE;
    assert(core.pfnCalcPrivateFenceSize({}, &fence) == 0 && last_error == E_NOTIMPL);
    assert(core.pfnCreateFence({}, {}, {}, &fence) == E_OUTOFMEMORY);

    D3D12DDI_STATIC_SAMPLER_0100 static_sampler{}; static_sampler.ShaderRegister = 17;
    D3D12DDI_ROOT_SIGNATURE_0100 root{}; root.NumStaticSamplers = 1; root.pStaticSamplers = &static_sampler;
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100 root_args{};
    root_args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_2; root_args.pRootSignature_1_2 = &root;
    assert(core.pfnCalcPrivateRootSignatureSize({}, &root_args) == 41);
    static_sampler.Flags = D3D12DDI_SAMPLER_FLAG_UINT_BORDER_COLOR;
    assert(core.pfnCalcPrivateRootSignatureSize({}, &root_args) == 0 && last_error == E_NOTIMPL);
    UINT count = 17;
    core.pfnGetTextureLayouts({}, nullptr, {}, {}, &count, nullptr); assert(count == 0);
    assert(core.pfnGetApplicationSpecificDriverStateBlobSize({}) == 0);
    assert(core.pfnGetApplicationSpecificDriverState({}, nullptr, 0) == E_NOTIMPL);
    assert(core.pfnGetApplicationSpecificDriverBlobStatus({}) == D3D12DDI_APPLICATION_SPECIFIC_DRIVER_BLOB_NOT_SPECIFIED);
    D3D12DDI_WORK_GRAPH_MEMORY_REQUIREMENTS_0108 memory{};
    memory.MinSizeInBytes = 99;
    core.pfnGetWorkGraphMemoryRequirements({}, nullptr, &memory); assert(memory.MinSizeInBytes == 0);
    std::puts("modern CORE typed adapters: PASS (12 table revisions)");
}
