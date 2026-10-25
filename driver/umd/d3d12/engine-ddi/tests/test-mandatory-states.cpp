// SPDX-License-Identifier: MIT
// Modern state creation and commands cross the DDI. Engine API resource setup
// isolates state semantics; all commands share the normal replay recording path.
#include "harness.h"
#include "modern-core-harness.h"
#include "replay.h"
#include "pso-async.h"
#include "fixture-mandatory-states.h"
#include <array>
#include <cstring>
#include <cmath>

namespace harness {
#if defined(D3D12DDI_BUILD_VERSION_0119)
namespace {
constexpr UINT width = 128, height = 16, cell = 16;
using Core = D3D12DDI_DEVICE_FUNCS_CORE_0119;
using Commands = D3D12DDI_COMMAND_LIST_FUNCS_3D_0099;
void APIENTRY list_error(D3D12DDI_HCOMMANDLIST h, HRESULT hr) {
    if (auto* l = engine_ddi::record_of<engine_ddi::CommandListRecord>(h.pDrvPrivate, engine_ddi::Tag::CommandList))
        l->h.device->report_list(l->rt, hr);
}
struct Fixture {
    Env& env;
    Device device;
    Core core{};
    Commands modern{};
    engine_ddi::EngineQueue* queue = nullptr;
    std::vector<IUnknown*> objects;
    std::vector<void*> psos, rasters, depths;
    void *root = nullptr, *vs = nullptr, *ps = nullptr;
    ID3D12Resource *target = nullptr, *depth = nullptr, *stencil = nullptr, *readback = nullptr,
        *depth_readback = nullptr, *indices = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{}, stencil_dsv{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT color_copy{}, depth_copy{};
    std::array<UINT, width * height> pixels{};
    std::array<float, width * height> z{};
    int pso_rt = 0;
    explicit Fixture(Env& e) : env(e) {}
    template<class T> T* hold(T* object) { if (object) objects.push_back(object); return object; }
    ~Fixture() {
        if (queue) check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
                         "mandatory states: queue retired");
        if (device.context) {
            for (void* p : psos) core.pfnDestroyPipelineState(device.h(), {p});
            for (void* p : depths) core.pfnDestroyDepthStencilState(device.h(), {p});
            for (void* p : rasters) core.pfnDestroyRasterizerState(device.h(), {p});
            if (ps) core.pfnDestroyShader(device.h(), {ps});
            if (vs) core.pfnDestroyShader(device.h(), {vs});
            if (root) core.pfnDestroyRootSignature(device.h(), {root});
            UINT live = ~0u;
            const HRESULT hr = engine_ddi::destroy_device_context(device.context, &live);
            checkf(hr == S_OK && !live && !device.shell.device_errors && !device.shell.list_errors,
                   "mandatory states: cleanup without live objects or DDI errors (hr %08lx, live %u, errors %u/%u)",
                   static_cast<unsigned long>(hr), live, device.shell.device_errors, device.shell.list_errors);
        }
        for (auto i = objects.rbegin(); i != objects.rend(); ++i) (*i)->Release();
    }
    ID3D12Resource* resource(DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                            D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT, UINT64 bytes = 0) {
        D3D12_HEAP_PROPERTIES properties{}; properties.Type = heap;
        properties.CreationNodeMask = properties.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = bytes ? D3D12_RESOURCE_DIMENSION_BUFFER : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = bytes ? bytes : width; desc.Height = bytes ? 1 : height;
        desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Format = format; desc.Flags = flags;
        desc.Layout = bytes ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ID3D12Resource* result = nullptr;
        if (FAILED(env.engine->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                __uuidof(ID3D12Resource), reinterpret_cast<void**>(&result)))) return nullptr;
        return hold(result);
    }
    bool init() {
        if (open_device(env, device) != S_OK || !modern_core_table(env, core)) return false;
        D3D12DDI_COMMAND_LIST_FUNCS_3D_0094 base{};
        if (!engine_ddi::make_list_0094(env.lists[1], &base) ||
            !native12::upgrade_list(base, &modern, 1, list_error, engine_ddi::modern_list_entries())) return false;
        D3D12DDI_ROOT_PARAMETER_0013 parameter{};
        parameter.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants = {0, 0, 4}; parameter.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
        D3D12DDI_ROOT_SIGNATURE_0100 desc{1, &parameter, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
        D3D12DDIARG_CREATE_ROOT_SIGNATURE_0100 args{};
        args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_2; args.pRootSignature_1_2 = &desc;
        root = env.storage.alloc(core.pfnCalcPrivateRootSignatureSize(device.h(), &args));
        if (!root || core.pfnCreateRootSignature(device.h(), &args, {root}) != S_OK) { root = nullptr; return false; }
        DdiShader v, p;
        if (!ddi_form(g_mandatory_vs, sizeof(g_mandatory_vs), v) || !ddi_form(g_mandatory_ps, sizeof(g_mandatory_ps), p)) return false;
        vs = create_shader(env, device, env.core.pfnCreateVertexShader, v, {root});
        ps = create_shader(env, device, env.core.pfnCreatePixelShader, p, {root});
        if (!vs || !ps) return false;
        target = resource(DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        depth = resource(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        stencil = resource(DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (!target || !depth || !stencil) return false;
        const auto color_desc = target->GetDesc(), depth_desc = depth->GetDesc();
        UINT64 color_bytes = 0, depth_bytes = 0;
        env.engine->GetCopyableFootprints(&color_desc, 0, 1, 0, &color_copy, nullptr, nullptr, &color_bytes);
        env.engine->GetCopyableFootprints(&depth_desc, 0, 1, 0, &depth_copy, nullptr, nullptr, &depth_bytes);
        readback = resource(DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK, color_bytes);
        depth_readback = resource(DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_HEAP_TYPE_READBACK, depth_bytes);
        indices = resource(DXGI_FORMAT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_HEAP_TYPE_UPLOAD, 256);
        if (!readback || !depth_readback || !indices) return false;
        void* mapped = nullptr;
        if (FAILED(indices->Map(0, nullptr, &mapped))) return false;
        const UINT16 strip[] = {0, 1, 2, 0xffff, 3, 4, 5};
        std::memcpy(mapped, strip, sizeof(strip)); indices->Unmap(0, nullptr);
        ID3D12DescriptorHeap *rtvs = nullptr, *dsvs = nullptr;
        const D3D12_DESCRIPTOR_HEAP_DESC rdesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC ddesc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        HRESULT hr = env.engine->CreateDescriptorHeap(&rdesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtvs));
        hold(rtvs);
        if (SUCCEEDED(hr)) hr = env.engine->CreateDescriptorHeap(&ddesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&dsvs));
        hold(dsvs);
        if (FAILED(hr)) return false;
        rtv = rtvs->GetCPUDescriptorHandleForHeapStart(); dsv = dsvs->GetCPUDescriptorHandleForHeapStart();
        stencil_dsv = {dsv.ptr + env.engine->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV)};
        env.engine->CreateRenderTargetView(target, nullptr, rtv);
        env.engine->CreateDepthStencilView(depth, nullptr, dsv);
        env.engine->CreateDepthStencilView(stencil, nullptr, stencil_dsv);
        BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
        return engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue) == S_OK;
    }
    void* pipeline(D3D12DDI_DEPTH_STENCIL_DESC_0095 depth_desc, FLOAT bias, D3D12DDI_LINE_RASTERIZATION_MODE line,
                   D3D12DDI_PIPELINE_STATE_FLAGS flags, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE cut,
                   DXGI_FORMAT depth_format, bool lines = false) {
        D3D12DDI_RASTERIZER_DESC_0102 raster{};
        raster.FillMode = D3D12DDI_FILL_MODE_SOLID; raster.CullMode = D3D12DDI_CULL_MODE_NONE;
        raster.DepthClipEnable = raster.ScissorEnable = TRUE; raster.DepthBias = bias; raster.LineRasterizationMode = line;
        void* r = env.storage.alloc(core.pfnCalcPrivateRasterizerStateSize(device.h(), &raster));
        void* d = env.storage.alloc(core.pfnCalcPrivateDepthStencilStateSize(device.h(), &depth_desc));
        if (!r || !d) return nullptr;
        core.pfnCreateRasterizerState(device.h(), &raster, {r}); rasters.push_back(r);
        core.pfnCreateDepthStencilState(device.h(), &depth_desc, {d}); depths.push_back(d);
        D3D12DDIARG_CREATE_PIPELINE_STATE_0119 args{};
        args.hVertexShader = {vs}; args.hPixelShader = {ps}; args.hRootSignature = {root};
        args.hRasterizerState = {r}; args.hDepthStencilState = {d}; args.SampleMask = UINT_MAX;
        args.NumRenderTargets = 1; args.RTVFormats[0] = DXGI_FORMAT_R32_UINT; args.DSVFormat = depth_format;
        args.SampleDesc.Count = 1; args.Flags = flags; args.IBStripCutValue = cut;
        args.PrimitiveTopologyType = lines ? D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        void* pso = env.storage.alloc(core.pfnCalcPrivatePipelineStateSize(device.h(), &args));
        if (!pso || core.pfnCreatePipelineState(device.h(), &args, {pso}, {&pso_rt}) != S_OK) return nullptr;
        psos.push_back(pso); return pso;
    }
    template<class F> void command(Recording& rec, F function) {
        if (auto* l = engine_ddi::list_of(rec.hlist(), "mandatory test setup")) engine_ddi::record(l, function);
    }
    ID3D12PipelineState* bias_control(FLOAT bias) {
        engine_ddi::SubobjectStream stream;
        auto* signature = static_cast<ID3D12RootSignature*>(engine_ddi::harness_engine_object(root));
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, signature);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, D3D12_SHADER_BYTECODE{g_mandatory_vs, sizeof(g_mandatory_vs)});
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, D3D12_SHADER_BYTECODE{g_mandatory_ps, sizeof(g_mandatory_ps)});
        D3D12_RASTERIZER_DESC2 raster{}; raster.FillMode = D3D12_FILL_MODE_SOLID;
        raster.CullMode = D3D12_CULL_MODE_NONE; raster.DepthClipEnable = TRUE; raster.DepthBias = bias;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER2, raster);
        D3D12_DEPTH_STENCIL_DESC1 state{}; state.DepthEnable = TRUE;
        state.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; state.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, state);
        D3D12_BLEND_DESC blend{}; blend.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, blend);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, UINT_MAX);
        D3D12_RT_FORMAT_ARRAY formats{}; formats.NumRenderTargets = 1; formats.RTFormats[0] = DXGI_FORMAT_R32_UINT;
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, formats);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, DXGI_FORMAT_D32_FLOAT);
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, DXGI_SAMPLE_DESC{1, 0});
        stream.put(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        ID3D12Device2* device2 = nullptr;
        if (FAILED(env.engine->QueryInterface(__uuidof(ID3D12Device2), reinterpret_cast<void**>(&device2)))) return nullptr;
        ID3D12PipelineState* result = nullptr; const auto desc = stream.desc();
        const HRESULT hr = device2->CreatePipelineState(&desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&result));
        device2->Release(); return SUCCEEDED(hr) ? hold(result) : nullptr;
    }
    bool begin(Recording& rec, bool use_stencil = false, bool use_depth = false) {
        if (open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) != S_OK) return false;
        const auto render = rtv, ds = use_stencil ? stencil_dsv : dsv;
        command(rec, [=](ID3D12GraphicsCommandList* e) {
            const FLOAT clear[4]{}; e->ClearRenderTargetView(render, clear, 0, nullptr);
            e->ClearDepthStencilView(ds, use_stencil ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH,
                                     0.0f, 0, 0, nullptr);
            e->OMSetRenderTargets(1, &render, TRUE, use_depth || use_stencil ? &ds : nullptr);
        });
        env.lists[1].pfnSetGraphicsRootSignature(rec.hlist(), {root});
        return true;
    }
    void draw(Recording& rec, UINT column, UINT shape = 0, UINT value = 77, bool indexed = false, bool line = false) {
        const D3D12DDI_VIEWPORT viewport{FLOAT(column * cell), 0, FLOAT(cell), FLOAT(height), 0, 1};
        const D3D12DDI_RECT scissor{LONG(column * cell), 0, LONG((column + 1) * cell), LONG(height)};
        const auto& t = env.lists[1]; const auto h = rec.hlist();
        t.pfnRsSetViewports(h, 1, &viewport); t.pfnRsSetScissorRects(h, 1, &scissor);
        const UINT constants[] = {0x3f000000u, shape, value, 0};
        t.pfnSetGraphicsRoot32BitConstants(h, 0, 4, constants, 0);
        t.pfnIaSetTopology(h, line ? D3D12DDI_PRIMITIVE_TOPOLOGY_LINELIST : indexed ?
            D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (indexed) {
            const D3D12DDI_INDEX_BUFFER_VIEW view{indices->GetGPUVirtualAddress(), 14, DXGI_FORMAT_R16_UINT};
            t.pfnIASetIndexBuffer(h, &view); t.pfnDrawIndexedInstanced(h, 7, 1, 0, 0, 0);
        } else t.pfnDrawInstanced(h, line ? 2 : 3, 1, 0, 0);
    }
    bool finish(Recording& rec, bool copy_depth = false) {
        ID3D12Resource *color = target, *dst = readback, *source_depth = depth, *dst_depth = depth_readback;
        const auto cfoot = color_copy, dfoot = depth_copy;
        command(rec, [=](ID3D12GraphicsCommandList* e) {
            const auto copy = [=](ID3D12Resource* source, ID3D12Resource* destination,
                                   D3D12_RESOURCE_STATES state, D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint) {
                D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition = {source, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
                e->ResourceBarrier(1, &b);
                D3D12_TEXTURE_COPY_LOCATION from{}, to{};
                from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.pResource = destination; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
                e->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = state;
                e->ResourceBarrier(1, &b);
            };
            copy(color, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, cfoot);
            if (copy_depth) copy(source_depth, dst_depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, dfoot);
        });
        const auto h = rec.hlist(); env.lists[1].pfnCloseCommandList(h);
        bool done = engine_ddi::execute_command_lists(queue, 1, &h) == S_OK && wait_queue_idle(env, queue, "mandatory states");
        void* data = nullptr;
        if (done && SUCCEEDED(readback->Map(0, nullptr, &data))) {
            for (UINT y = 0; y < height; ++y) std::memcpy(pixels.data() + width * y,
                static_cast<BYTE*>(data) + color_copy.Offset + color_copy.Footprint.RowPitch * y, width * sizeof(UINT));
            readback->Unmap(0, nullptr);
        } else done = false;
        if (copy_depth && done && SUCCEEDED(depth_readback->Map(0, nullptr, &data))) {
            for (UINT y = 0; y < height; ++y) std::memcpy(z.data() + width * y,
                static_cast<BYTE*>(data) + depth_copy.Offset + depth_copy.Footprint.RowPitch * y, width * sizeof(float));
            depth_readback->Unmap(0, nullptr);
        } else if (copy_depth) done = false;
        destroy_recording(env, device, rec); return done;
    }
};

D3D12DDI_DEPTH_STENCIL_DESC_0095 depth_state(bool enabled = false) {
    D3D12DDI_DEPTH_STENCIL_DESC_0095 d{};
    d.DepthEnable = enabled; d.DepthWriteMask = D3D12DDI_DEPTH_WRITE_MASK_ALL; d.DepthFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS;
    return d;
}
void test_bias(Fixture& f) {
    // Negative1.5 is exactly distinguishable from the legacy integer-1 on D32
    // near0.5; positive1.5 can quantize to the same result as1 on real hardware.
    void* pso = f.pipeline(depth_state(true), -1.5f, D3D12DDI_LINE_RASTERIZATION_MODE_ALIASED,
        D3D12DDI_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, DXGI_FORMAT_D32_FLOAT);
    Recording rec, bundle;
    auto* control = f.bias_control(-1.5f); auto* integer_control = f.bias_control(-1.0f);
    bool ok = pso && control && integer_control && f.begin(rec, false, true) &&
        open_recording(f.env, f.device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, bundle, D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) == S_OK;
    if (!ok) { check(false, "mandatory bias: setup"); return; }
    const auto h = rec.hlist(); const auto& t = f.env.lists[1];
    t.pfnSetPipelineState(h, {pso}); f.draw(rec, 0);
    f.modern.pfnRSSetDepthBias(h, -2.0f, 0, 0); f.draw(rec, 1);
    t.pfnSetPipelineState(h, {pso}); f.draw(rec, 2);
    f.modern.pfnRSSetDepthBias(bundle.hlist(), -2.0f, 0, 0);
    t.pfnCloseCommandList(bundle.hlist()); t.pfnExecuteBundle(h, bundle.hlist()); f.draw(rec, 3);
    t.pfnSetPipelineState(h, {pso}); f.draw(rec, 4);
    f.command(rec, [=](ID3D12GraphicsCommandList* e) { e->SetPipelineState(control); }); f.draw(rec, 5);
    f.command(rec, [=](ID3D12GraphicsCommandList* e) { e->SetPipelineState(integer_control); }); f.draw(rec, 6);
    ok = f.finish(rec, true);
    if (ok) {
        save_readback_artifact("bias-color", f.pixels.data(), width, height, width * 4, DXGI_FORMAT_R32_UINT);
        save_readback_artifact("bias-depth", f.z.data(), width, height, width * 4, DXGI_FORMAT_R32_FLOAT);
    }
    const float fractional = 0.5f - 1.5f * std::ldexp(1.0f, -24), negative = 0.5f - 2.0f * std::ldexp(1.0f, -24);
    UINT bad = 0, control_bad = 0, truncation_difference = 0;
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < 5 * cell; ++x) {
        const UINT c = x / cell; const float expected = c == 1 || c == 3 ? negative : fractional;
        bad += f.pixels[y * width + x] != 77 || f.z[y * width + x] != expected;
    }
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < cell; ++x) {
        control_bad += f.z[y * width + x] != f.z[y * width + 5 * cell + x];
        truncation_difference += f.z[y * width + x] != f.z[y * width + 6 * cell + x];
    }
    checkf(ok && !bad && !control_bad && truncation_difference == cell * height,
           "mandatory bias: FLOAT -1.5, dynamic -2, PSO reset and bundle match direct API; integer -1 differs (%u errors, %u control errors, %u distinct pixels; %.9g %.9g %.9g)",
           bad, control_bad, truncation_difference, f.z[0], f.z[cell], f.z[6 * cell]);
    destroy_recording(f.env, f.device, bundle);
}
void test_stencil(Fixture& f) {
    auto write = depth_state(); write.StencilEnable = write.FrontEnable = write.BackEnable = TRUE;
    write.FrontFace = write.BackFace = {D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_STENCIL_OP_KEEP,
        D3D12DDI_STENCIL_OP_REPLACE, D3D12DDI_COMPARISON_FUNC_ALWAYS};
    write.FrontFaceStencilReadMask = write.BackFaceStencilReadMask = 0xff;
    write.FrontFaceStencilWriteMask = 0x0f; write.BackFaceStencilWriteMask = 0xf0;
    auto read = write; read.FrontFaceStencilReadMask = 0x0f; read.BackFaceStencilReadMask = 0xf0;
    read.FrontFaceStencilWriteMask = read.BackFaceStencilWriteMask = 0;
    read.FrontFace.StencilFunc = read.BackFace.StencilFunc = D3D12DDI_COMPARISON_FUNC_EQUAL;
    void* writer = f.pipeline(write, 0, D3D12DDI_LINE_RASTERIZATION_MODE_ALIASED, D3D12DDI_PIPELINE_STATE_FLAG_NONE,
        D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, DXGI_FORMAT_D24_UNORM_S8_UINT);
    void* reader = f.pipeline(read, 0, D3D12DDI_LINE_RASTERIZATION_MODE_ALIASED, D3D12DDI_PIPELINE_STATE_FLAG_NONE,
        D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, DXGI_FORMAT_D24_UNORM_S8_UINT);
    Recording rec, bundle;
    bool ok = writer && reader && f.begin(rec, true) &&
        open_recording(f.env, f.device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, bundle, D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) == S_OK;
    if (!ok) { check(false, "mandatory stencil: setup"); return; }
    const auto h = rec.hlist(); const auto& t = f.env.lists[1];
    t.pfnSetPipelineState(h, {writer}); f.modern.pfnOmSetFrontAndBackStencilRef(h, 0xab, 0xcd);
    for (UINT i = 0; i < 4; ++i) f.draw(rec, i, i & 1, 11);
    t.pfnSetPipelineState(h, {reader}); f.modern.pfnOmSetFrontAndBackStencilRef(h, 0xdb, 0xc1);
    f.draw(rec, 0, 0); f.draw(rec, 1, 1);
    f.modern.pfnOmSetFrontAndBackStencilRef(h, 0, 0);
    f.modern.pfnOmSetFrontAndBackStencilRef(bundle.hlist(), 0xdb, 0xc1);
    t.pfnCloseCommandList(bundle.hlist()); t.pfnExecuteBundle(h, bundle.hlist());
    f.draw(rec, 2, 0); f.draw(rec, 3, 1);
    ok = f.finish(rec); UINT bad = 0;
    if (ok) save_readback_artifact("stencil", f.pixels.data(), width, height, width * 4, DXGI_FORMAT_R32_UINT);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < 4 * cell; ++x) bad += f.pixels[y * width + x] != 77;
    checkf(ok && !bad, "mandatory stencil: independent front/back refs, read/write masks and bundle inheritance (%u differ)", bad);
    destroy_recording(f.env, f.device, bundle);
}
void test_strip_and_line(Fixture& f) {
    void* control = f.pipeline(depth_state(), 0, D3D12DDI_LINE_RASTERIZATION_MODE_ALIASED, D3D12DDI_PIPELINE_STATE_FLAG_NONE,
        D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF, DXGI_FORMAT_UNKNOWN);
    void* dynamic = f.pipeline(depth_state(), 0, D3D12DDI_LINE_RASTERIZATION_MODE_ALIASED,
        D3D12DDI_PIPELINE_STATE_FLAG_DYNAMIC_INDEX_BUFFER_STRIP_CUT, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, DXGI_FORMAT_UNKNOWN);
    void* disabled = f.pipeline(depth_state(), 0, D3D12DDI_LINE_RASTERIZATION_MODE_ALIASED,
        D3D12DDI_PIPELINE_STATE_FLAG_NONE, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, DXGI_FORMAT_UNKNOWN);
    void* narrow = f.pipeline(depth_state(), 0, D3D12DDI_LINE_RASTERIZATION_MODE_QUADRILATERAL_NARROW,
        D3D12DDI_PIPELINE_STATE_FLAG_NONE, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED, DXGI_FORMAT_UNKNOWN, true);
    Recording rec, bundle;
    bool ok = control && dynamic && disabled && narrow && f.begin(rec) &&
        open_recording(f.env, f.device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, bundle, D3D12DDI_COMMAND_LIST_TYPE_BUNDLE) == S_OK;
    if (!ok) { check(false, "mandatory strip/line: setup"); return; }
    const auto h = rec.hlist(); const auto& t = f.env.lists[1];
    t.pfnSetPipelineState(h, {control}); f.draw(rec, 0, 2, 77, true);
    t.pfnSetPipelineState(h, {dynamic}); f.modern.pfnIASetIndexBufferStripCutValue(h, D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    f.draw(rec, 1, 2, 77, true);
    t.pfnSetPipelineState(h, {dynamic});
    f.modern.pfnIASetIndexBufferStripCutValue(bundle.hlist(), D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF);
    t.pfnCloseCommandList(bundle.hlist()); t.pfnExecuteBundle(h, bundle.hlist()); f.draw(rec, 2, 2, 77, true);
    t.pfnSetPipelineState(h, {dynamic}); f.draw(rec, 3, 2, 77, true);
    t.pfnSetPipelineState(h, {disabled}); f.draw(rec, 4, 2, 77, true);
    t.pfnSetPipelineState(h, {narrow}); f.draw(rec, 5, 3, 77, false, true);
    ok = f.finish(rec); UINT bad = 0, filled = 0, line_pixels = 0, reset_bad = 0, reset_difference = 0;
    if (ok) save_readback_artifact("strip-and-line", f.pixels.data(), width, height, width * 4, DXGI_FORMAT_R32_UINT);
    for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < cell; ++x) {
        const UINT expected = f.pixels[y * width + x]; filled += expected == 77;
        bad += f.pixels[y * width + cell + x] != expected || f.pixels[y * width + 2 * cell + x] != expected;
        reset_bad += f.pixels[y * width + 3 * cell + x] != f.pixels[y * width + 4 * cell + x];
        reset_difference += f.pixels[y * width + 4 * cell + x] != expected;
        line_pixels += f.pixels[y * width + 5 * cell + x] == 77;
    }
    checkf(ok && !bad && filled > 0 && filled < cell * height,
        "mandatory strip cut: dynamic and bundle0xffff match static control (%u differ, %u control pixels)", bad, filled);
    checkf(ok && !reset_bad && reset_difference,
        "mandatory strip cut: PSO reset matches disabled control and differs from cut-enabled rendering (%u errors, %u different pixels)",
        reset_bad, reset_difference);
    checkf(ok && line_pixels == 12, "mandatory narrow lines: one-pixel-wide12-pixel segment (%u pixels)", line_pixels);
    destroy_recording(f.env, f.device, bundle);
}
} // namespace
#endif
void test_mandatory_states(Env& env) {
#if defined(D3D12DDI_BUILD_VERSION_0119)
    Fixture f(env);
    const bool ready = f.init(); check(ready, "mandatory states: modern DDI fixture");
    if (ready) { test_bias(f); test_stencil(f); test_strip_and_line(f); }
#else
    (void)env; // The legacy header build has no modern DDI entry points.
#endif
}
} // namespace harness
