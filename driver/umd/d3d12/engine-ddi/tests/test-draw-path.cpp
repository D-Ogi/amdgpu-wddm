// SPDX-License-Identifier: MIT
// Round trips and costs of the per-draw path a game drives hardest (Witcher 3, lab session 291: indexed draws, each
// after vertex and index buffer sets, a root constant buffer view and a descriptor table, and the descriptor copies
// that fill the tables).
//
// Ranged descriptor copies: CopyDescriptors with several destination and source ranges of different sizes, and with
// null size arrays, fills a shader-visible heap with a permutation of eight buffer UAVs; a dispatch through each
// table slot writes its own seed, and every buffer must come back with the seed of the slot its descriptor went to.
//
// Root tables: dispatches through descriptor tables set and set again over two root signatures that order the same
// three tables differently; each table's slot must take the value of the last dispatch that named it.
//
// Buffer rebinds: sixteen 16x16 cells of one target, one draw each under its own scissor, between which the vertex
// and index buffers are set again unchanged, or changed in exactly one of buffer, offset, size, stride or index
// format. The draw's tag comes from the vertex the bound buffers select (the fixture shaders of test_graphics), so a
// change the engine failed to bind leaves the previous cell's tag, and a format change it failed to bind draws a
// triangle where none belongs.
//
// Query reuse: occlusion queries over three lists, the second after a reset of the first's pool, the third on a new
// pool. The engine's command allocator borrows Vulkan query pools from the device and resets a pool lent again over
// the queries it handed out last; each query's count must be the one its own draws give.
//
// Read-only transitions: a texture moved between read-only states between draws into another target, which the
// engine may hold until the render pass ends, over three lists (test_read_barriers).
//
// Cost: the recording thread's time per draw for a list of such draws, with the buffer sets repeated unchanged and
// with the vertex buffer alternating, and per call of ranged and simple descriptor copies. The figures are printed
// ("measure" lines), not checked; the drawn image is.
#include "harness.h"
#include "fixture-cs.h"
#include "fixture-gfx.h"
#include "fixture-tables.h"
#include <cstdio>
#include <cstring>

namespace harness {

namespace {
constexpr UINT kSize = 64;
constexpr UINT kPitch = 256;
constexpr UINT kSeed = 0x5eed2b0bu;
constexpr UINT kTags[5] = {0xA0000000u, 0xB0000000u, 0xC0000000u, 0xD0000000u, 0xE0000000u};     // A B C D E
enum { A, B, C, D, E };

struct Vertex {
    float x, y;
    UINT tag;
};

uint64_t qpc() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}
double ns_per(uint64_t ticks, uint64_t count) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return count ? static_cast<double>(ticks) * 1e9 / static_cast<double>(f.QuadPart) / static_cast<double>(count) : 0.0;
}

void* create_root_signature(Env& env, Device& device, UINT count, const D3D12DDI_ROOT_PARAMETER_0013* params) {
    const D3D12DDI_ROOT_SIGNATURE_0013 rs{count, params, 0, nullptr,
                                          D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 args{};
    args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1;
    args.pRootSignature_1_1 = &rs;
    void* storage = env.storage.alloc(env.core.pfnCalcPrivateRootSignatureSize(device.h(), &args));
    if (storage && env.core.pfnCreateRootSignature(device.h(), &args, D3D12DDI_HROOTSIGNATURE{storage}) != S_OK)
        storage = nullptr;
    return storage;
}

// The fixture's vertex and pixel programs over a root signature whose parameter 0 supplies b0, with test_graphics'
// element layout, states and R32_UINT target.
struct Pipeline {
    void* rs = nullptr;
    void* vs = nullptr;
    void* ps = nullptr;
    void* layout = nullptr;
    void* blend = nullptr;
    void* raster = nullptr;
    void* depth = nullptr;
    void* pso = nullptr;
    int pso_rt = 0;
    D3D12DDI_HROOTSIGNATURE hrs() const { return D3D12DDI_HROOTSIGNATURE{rs}; }
    D3D12DDI_HPIPELINESTATE hpso() const { return D3D12DDI_HPIPELINESTATE{pso}; }

    bool create(Env& env, Device& device, UINT count, const D3D12DDI_ROOT_PARAMETER_0013* params) {
        rs = create_root_signature(env, device, count, params);
        DdiShader v, p;
        if (!rs || !ddi_form(g_fixture_vs, sizeof(g_fixture_vs), v) || !ddi_form(g_fixture_ps, sizeof(g_fixture_ps), p))
            return false;
        vs = create_shader(env, device, env.core.pfnCreateVertexShader, v, hrs());
        ps = create_shader(env, device, env.core.pfnCreatePixelShader, p, hrs());
        const D3D12DDIARG_INPUT_ELEMENT_DESC elements[] = {
            {0, 0, DXGI_FORMAT_R32G32_FLOAT, D3D12DDI_INPUT_CLASSIFICIATION_PER_VERTEX_DATA, 0,
             v.input_register("POSITION", 0)},
            {0, 8, DXGI_FORMAT_R32_UINT, D3D12DDI_INPUT_CLASSIFICIATION_PER_VERTEX_DATA, 0, v.input_register("TAG", 0)},
        };
        D3D12DDIARG_CREATEELEMENTLAYOUT_0010 layout_args{};
        layout_args.pVertexElements = elements;
        layout_args.NumElements = 2;
        layout = env.storage.alloc(env.core.pfnCalcPrivateElementLayoutSize(device.h(), &layout_args));
        if (layout) env.core.pfnCreateElementLayout(device.h(), &layout_args, D3D12DDI_HELEMENTLAYOUT{layout});

        D3D12DDI_BLEND_DESC_0010 blend_desc{};
        for (D3D12DDI_RENDER_TARGET_BLEND_DESC& rt : blend_desc.RenderTarget)
            rt = {FALSE, FALSE, D3D12DDI_BLEND_ONE, D3D12DDI_BLEND_ZERO, D3D12DDI_BLEND_OP_ADD, D3D12DDI_BLEND_ONE,
                  D3D12DDI_BLEND_ZERO, D3D12DDI_BLEND_OP_ADD, D3D12DDI_LOGIC_OP_NOOP,
                  static_cast<UINT8>(D3D12DDI_COLOR_WRITE_ENABLE_ALL)};
        blend = env.storage.alloc(env.core.pfnCalcPrivateBlendStateSize(device.h(), &blend_desc));
        if (blend) env.core.pfnCreateBlendState(device.h(), &blend_desc, D3D12DDI_HBLENDSTATE{blend});
        D3D12DDI_RASTERIZER_DESC_0010 raster_desc{};
        raster_desc.FillMode = D3D12DDI_FILL_MODE_SOLID;
        raster_desc.CullMode = D3D12DDI_CULL_MODE_NONE;
        raster_desc.DepthClipEnable = TRUE;
        raster_desc.ScissorEnable = TRUE;
        raster_desc.ConservativeRasterizationMode = D3D12DDI_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        raster = env.storage.alloc(env.core.pfnCalcPrivateRasterizerStateSize(device.h(), &raster_desc));
        if (raster) env.core.pfnCreateRasterizerState(device.h(), &raster_desc, D3D12DDI_HRASTERIZERSTATE{raster});
        D3D12DDI_DEPTH_STENCIL_DESC_0025 depth_desc{};
        depth_desc.DepthWriteMask = D3D12DDI_DEPTH_WRITE_MASK_ZERO;
        depth_desc.DepthFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS;
        depth_desc.StencilReadMask = 0xff;
        depth_desc.StencilWriteMask = 0xff;
        const D3D12DDI_DEPTH_STENCILOP_DESC keep{D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_STENCIL_OP_KEEP,
                                                D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_COMPARISON_FUNC_ALWAYS};
        depth_desc.FrontFace = keep;
        depth_desc.BackFace = keep;
        depth = env.storage.alloc(env.core.pfnCalcPrivateDepthStencilStateSize(device.h(), &depth_desc));
        if (depth) env.core.pfnCreateDepthStencilState(device.h(), &depth_desc, D3D12DDI_HDEPTHSTENCILSTATE{depth});
        if (!vs || !ps || !layout || !blend || !raster || !depth) return false;

        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 args{};
        args.hVertexShader = D3D12DDI_HSHADER{vs};
        args.hPixelShader = D3D12DDI_HSHADER{ps};
        args.hRootSignature = hrs();
        args.hBlendState = D3D12DDI_HBLENDSTATE{blend};
        args.SampleMask = UINT_MAX;
        args.hRasterizerState = D3D12DDI_HRASTERIZERSTATE{raster};
        args.hDepthStencilState = D3D12DDI_HDEPTHSTENCILSTATE{depth};
        args.hElementLayout = D3D12DDI_HELEMENTLAYOUT{layout};
        args.IBStripCutValue = D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        args.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        args.NumRenderTargets = 1;
        args.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
        args.DSVFormat = DXGI_FORMAT_UNKNOWN;
        args.SampleDesc = {1, 0};
        pso = env.storage.alloc(env.core.pfnCalcPrivatePipelineStateSize(device.h(), &args));
        if (pso && env.core.pfnCreatePipelineState(device.h(), &args, hpso(), D3D12DDI_HRTPIPELINESTATE{&pso_rt}) != S_OK)
            pso = nullptr;
        return pso != nullptr;
    }
    void destroy(Env& env, Device& device) {
        if (pso) env.core.pfnDestroyPipelineState(device.h(), hpso());
        if (depth) env.core.pfnDestroyDepthStencilState(device.h(), D3D12DDI_HDEPTHSTENCILSTATE{depth});
        if (raster) env.core.pfnDestroyRasterizerState(device.h(), D3D12DDI_HRASTERIZERSTATE{raster});
        if (blend) env.core.pfnDestroyBlendState(device.h(), D3D12DDI_HBLENDSTATE{blend});
        if (layout) env.core.pfnDestroyElementLayout(device.h(), D3D12DDI_HELEMENTLAYOUT{layout});
        if (ps) env.core.pfnDestroyShader(device.h(), D3D12DDI_HSHADER{ps});
        if (vs) env.core.pfnDestroyShader(device.h(), D3D12DDI_HSHADER{vs});
        if (rs) env.core.pfnDestroyRootSignature(device.h(), hrs());
        *this = Pipeline{};
    }
};

// A committed 64x64 R32_UINT render target in a DEFAULT heap, created in COMMON, and its view in an RTV heap of one.
struct Target {
    Buffer texture;
    void* heap = nullptr;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE rtv{};

    bool create(Env& env, Device& device) {
        D3D12DDIARG_CREATERESOURCE_0088 res{};
        res.ResourceType = D3D12DDI_RT_TEXTURE2D;
        res.Width = kSize;
        res.Height = kSize;
        res.DepthOrArraySize = 1;
        res.MipLevels = 1;
        res.Format = DXGI_FORMAT_R32_UINT;
        res.SampleDesc = {1, 0};
        res.Layout = D3D12DDI_TL_UNDEFINED;
        res.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
        res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
        D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
        env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
        if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return false;
        const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
        D3D12DDIARG_CREATEHEAP_0001 h{};
        h.ByteSize = info.ResourceDataSize;
        h.Alignment = info.ResourceDataAlignment;
        h.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
        h.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
        h.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
        h.CreationNodeMask = 1;
        h.VisibleNodeMask = 1;
        const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
            env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), &h, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
        texture.heap = env.storage.alloc(sizes.Heap);
        texture.resource = env.storage.alloc(sizes.Resource);
        if (!texture.heap || !texture.resource ||
            env.core.pfnCreateHeapAndResource(device.h(), &h, texture.hheap(), D3D12DDI_HRTRESOURCE{&texture.rt}, &res,
                                              nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{},
                                              texture.hres()) != S_OK) {
            texture = Buffer{};
            return false;
        }
        D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                          D3D12DDI_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        heap = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &heap_args));
        if (!heap || env.core.pfnCreateDescriptorHeap(device.h(), &heap_args, D3D12DDI_HDESCRIPTORHEAP{heap}) != S_OK) {
            heap = nullptr;
            return false;
        }
        rtv = env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), D3D12DDI_HDESCRIPTORHEAP{heap});
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view{};
        view.hDrvResource = texture.hres();
        view.Format = DXGI_FORMAT_R32_UINT;
        view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        view.Tex2D = {0, 0, 1, 0};
        env.core.pfnCreateRenderTargetView(device.h(), &view, rtv);
        return rtv.ptr != 0;
    }
    void destroy(Env& env, Device& device) {
        if (heap) env.core.pfnDestroyDescriptorHeap(device.h(), D3D12DDI_HDESCRIPTORHEAP{heap});
        destroy_buffer(env, device, texture);
        heap = nullptr;
        rtv = {};
    }
};

void* create_view_heap(Env& env, Device& device, UINT count, bool visible, D3D12DDI_CPU_DESCRIPTOR_HANDLE& cpu,
                       D3D12DDI_GPU_DESCRIPTOR_HANDLE* gpu) {
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, count,
                                                 visible ? D3D12DDI_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                                                         : D3D12DDI_DESCRIPTOR_HEAP_FLAG_NONE,
                                                 0};
    void* storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &args));
    if (!storage || env.core.pfnCreateDescriptorHeap(device.h(), &args, D3D12DDI_HDESCRIPTORHEAP{storage}) != S_OK)
        return nullptr;
    cpu = env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), D3D12DDI_HDESCRIPTORHEAP{storage});
    if (gpu) *gpu = env.core.pfnGetGPUDescriptorHandleForHeapStart(device.h(), D3D12DDI_HDESCRIPTORHEAP{storage});
    return storage;
}

D3D12DDI_GPU_VIRTUAL_ADDRESS write_buffer(Env& env, Device& device, const Buffer& buffer, const void* data,
                                          size_t bytes) {
    void* cpu = nullptr;
    if (env.core.pfnMapHeap(device.h(), buffer.hheap(), &cpu) != S_OK || !cpu) return 0;
    std::memcpy(cpu, data, bytes);
    env.core.pfnUnmapHeap(device.h(), buffer.hheap());
    return env.core.pfnCheckResourceVirtualAddress(device.h(), buffer.hres());
}

UINT expected_texel(UINT tag, UINT x, UINT y) { return kSeed ^ tag ^ ((y << 16) | x) ^ (3u << 8) ^ 0x51u; }

// ---- Ranged descriptor copies ---------------------------------------------------------------------------------------
void test_descriptor_ranges(Env& env, Device& device) {
    constexpr UINT kViews = 8, kWords = 64;
    constexpr UINT64 kBytes = UINT64{kWords} * 4;
    const uint32_t errors_before = device.shell.device_errors;

    // The root signature and shader of test_compute: DescriptorTable(UAV(u0)), RootConstants(b0); output[i] =
    // i * 2654435761 + seed.
    const D3D12DDI_DESCRIPTOR_RANGE_0013 range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
                                               D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE,
                                               D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    D3D12DDI_ROOT_PARAMETER_0013 params[2]{};
    params[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {1, &range};
    params[0].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 1};
    params[1].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    void* rs = create_root_signature(env, device, 2, params);
    const D3D12DDI_HROOTSIGNATURE hrs{rs};
    DdiShader cs;
    void* cs_storage = rs && ddi_form(g_engine_test_cs, sizeof(g_engine_test_cs), cs)
                           ? create_shader(env, device, env.core.pfnCreateComputeShader, cs, hrs)
                           : nullptr;
    D3D12DDIARG_CREATE_PIPELINE_STATE_0075 pso_args{};
    pso_args.hComputeShader = D3D12DDI_HSHADER{cs_storage};
    pso_args.hRootSignature = hrs;
    void* pso = cs_storage ? env.storage.alloc(env.core.pfnCalcPrivatePipelineStateSize(device.h(), &pso_args)) : nullptr;
    int pso_rt = 0;
    if (pso && env.core.pfnCreatePipelineState(device.h(), &pso_args, D3D12DDI_HPIPELINESTATE{pso},
                                               D3D12DDI_HRTPIPELINESTATE{&pso_rt}) != S_OK)
        pso = nullptr;

    Buffer out[kViews], readback;
    bool made = pso != nullptr;
    for (Buffer& b : out) made = made && create_buffer(env, device, HeapKind::Default, kBytes, true, b) == S_OK;
    made = made && create_buffer(env, device, HeapKind::Readback, kBytes * kViews, false, readback) == S_OK;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE plain{}, visible{};
    D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu{};
    void* plain_heap = create_view_heap(env, device, kViews, false, plain, nullptr);
    void* visible_heap = create_view_heap(env, device, kViews, true, visible, &gpu);
    const UINT inc = env.core.pfnGetDescriptorSizeInBytes(device.h(), D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    made = made && plain_heap && visible_heap && plain.ptr && visible.ptr && gpu.ptr && inc;
    checkf(made && device.shell.device_errors == errors_before,
           "descriptor ranges: compute pipeline, eight DEFAULT UAV buffers, a READBACK buffer, a plain and a "
           "shader-visible heap of eight");

    // Plain slot j holds buffer j's UAV. Three copies put a permutation into the visible heap:
    //   destination ranges {0, 2}, {4, 3}, {7, 1} from source ranges {5, 1}, {0, 4}, {4, 1}: slots 0 1 4 5 6 7
    //   take buffers 5 0 1 2 3 4; null size arrays (every range one descriptor) put buffers 6 and 7 into slots 3
    //   and 2 from one source range of two (sizes given only on the source side); one range of size zero on each
    //   side copies nothing.
    constexpr UINT kSlotOf[kViews] = {1, 4, 5, 6, 7, 0, 3, 2};     // visible slot of buffer j
    if (made) {
        for (UINT j = 0; j < kViews; ++j) {
            D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 uav{};
            uav.hDrvResource = out[j].hres();
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ResourceDimension = D3D12DDI_RD_BUFFER;
            uav.Buffer.NumElements = kWords;
            uav.Buffer.Flags = D3D12DDI_BUFFER_UAV_FLAG_RAW;
            env.core.pfnCreateUnorderedAccessView(device.h(), &uav, {plain.ptr + SIZE_T{j} * inc});
        }
        const auto at = [&](D3D12DDI_CPU_DESCRIPTOR_HANDLE base, UINT slot) {
            return D3D12DDI_CPU_DESCRIPTOR_HANDLE{base.ptr + SIZE_T{slot} * inc};
        };
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE dst1[] = {at(visible, 0), at(visible, 4), at(visible, 7)};
        const UINT dst1_sizes[] = {2, 3, 1};
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE src1[] = {at(plain, 5), at(plain, 0), at(plain, 4)};
        const UINT src1_sizes[] = {1, 4, 1};
        env.core.pfnCopyDescriptors(device.h(), 3, dst1, dst1_sizes, 3, src1, src1_sizes,
                                    D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE dst2[] = {at(visible, 3), at(visible, 2)};
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE src2[] = {at(plain, 6)};
        const UINT src2_sizes[] = {2};
        env.core.pfnCopyDescriptors(device.h(), 2, dst2, nullptr, 1, src2, src2_sizes,
                                    D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE dst3[] = {at(visible, 0)};
        const D3D12DDI_CPU_DESCRIPTOR_HANDLE src3[] = {at(plain, 1)};
        const UINT none[] = {0};
        env.core.pfnCopyDescriptors(device.h(), 1, dst3, none, 1, src3, none, D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        made = device.shell.device_errors == errors_before;
        checkf(made, "descriptor ranges: eight UAVs and three ranged copies report no error");
    }

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    Recording rec;
    if (made) {
        made = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue) == S_OK && queue &&
               open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, rec) == S_OK && rec.table == 0;
        checkf(made, "descriptor ranges: COMPUTE queue and list");
    }
    if (made) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[0];
        D3D12DDI_HDESCRIPTORHEAP heaps[] = {D3D12DDI_HDESCRIPTORHEAP{visible_heap}};
        t.pfnSetDescriptorHeaps(rec.hlist(), 1, heaps);
        t.pfnSetComputeRootSignature(rec.hlist(), hrs);
        t.pfnSetPipelineState(rec.hlist(), D3D12DDI_HPIPELINESTATE{pso});
        D3D12DDIARG_RESOURCE_BARRIER_0022 barriers[kViews];
        for (UINT j = 0; j < kViews; ++j)
            barriers[j] = transition(out[j], D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
        t.pfnResourceBarrier(rec.hlist(), kViews, barriers);
        for (UINT s = 0; s < kViews; ++s) {
            t.pfnSetComputeRootDescriptorTable(rec.hlist(), 0, {gpu.ptr + UINT64{s} * inc});
            t.pfnSetComputeRoot32BitConstant(rec.hlist(), 1, 0x5107000u + s, 0);
            t.pfnDispatch(rec.hlist(), 1, 1, 1);
        }
        for (UINT j = 0; j < kViews; ++j)
            barriers[j] = transition(out[j], D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), kViews, barriers);
        for (UINT j = 0; j < kViews; ++j) {
            D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
            dst.BaseAddress.UMD = {readback.hres(), UINT64{j} * kBytes};
            src.BaseAddress.UMD = {out[j].hres(), 0};
            t.pfnCopyBufferRegion(rec.hlist(), dst, src, kBytes);
        }
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        const HRESULT hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors,
               "descriptor ranges: one dispatch through each of the eight table slots, copies, execute (hr %08lx)",
               static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "descriptor ranges");
        void* cpu = nullptr;
        UINT bad = 0, first = kViews;
        if (env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) == S_OK && cpu) {
            const auto* words = static_cast<const UINT32*>(cpu);
            for (UINT j = 0; j < kViews; ++j) {
                for (UINT i = 0; i < kWords; ++i) {
                    if (words[j * kWords + i] != i * 2654435761u + 0x5107000u + kSlotOf[j]) {
                        if (!bad) first = j;
                        ++bad;
                    }
                }
            }
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            bad = ~0u;
        }
        checkf(!bad, "descriptor ranges: every buffer holds the seed of the slot its descriptor was copied to (%u words "
                     "differ, first in buffer %u)",
               bad, first);
    }
    destroy_recording(env, device, rec);
    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "descriptor ranges: destroy_engine_queue reports Retired");
    if (visible_heap) env.core.pfnDestroyDescriptorHeap(device.h(), D3D12DDI_HDESCRIPTORHEAP{visible_heap});
    if (plain_heap) env.core.pfnDestroyDescriptorHeap(device.h(), D3D12DDI_HDESCRIPTORHEAP{plain_heap});
    for (Buffer& b : out) destroy_buffer(env, device, b);
    destroy_buffer(env, device, readback);
    if (pso) env.core.pfnDestroyPipelineState(device.h(), D3D12DDI_HPIPELINESTATE{pso});
    if (cs_storage) env.core.pfnDestroyShader(device.h(), D3D12DDI_HSHADER{cs_storage});
    if (rs) env.core.pfnDestroyRootSignature(device.h(), hrs);
}

// ---- Root tables ------------------------------------------------------------------------------------------------------
// Two root signatures over the fixture-tables program, its three UAV tables at different parameter indices around a
// root constant: A = {Table(u2), Constants(b0), Table(u0), Table(u1)}, B = {Table(u1), Table(u0), Constants(b0),
// Table(u2)}. Six dispatches on one list, each followed by a UAV barrier: all tables set on A, one changed, a switch to
// B with all set, one changed, back to A with all set, one changed. A dispatch stores seed ^ tag into the slot each
// table names (twelve 16-byte slots of one buffer, a raw UAV each), and every slot must hold the value of the last
// dispatch that named it: a table a set left out, or a table order taken from the other signature, shows up as another
// seed or tag.
void test_root_tables(Env& env, Device& device) {
    constexpr UINT kSlots = 12, kSlotBytes = 16, kParams = 4, kSeedBase = 0x7AB1E000u;
    constexpr UINT kTagOf[3] = {0x10000000u, 0x20000000u, 0x30000000u};     // fixture-tables: tags of u0, u1, u2
    struct Layout {
        UINT constant;      // parameter of b0
        int reg[kParams];   // UAV register of each table parameter, -1 for the constant
    };
    const Layout layouts[2] = {{1, {2, -1, 0, 1}}, {2, {1, 0, -1, 2}}};
    struct Step {
        int layout;   // root signature set before the tables, -1: the bound one kept
        UINT count;
        UINT param[3];
        UINT slot[3];
    };
    const Step steps[] = {
        {0, 3, {0, 2, 3}, {0, 1, 2}}, {-1, 1, {3}, {3}}, {1, 3, {0, 1, 3}, {4, 5, 6}},
        {-1, 1, {1}, {7}},            {0, 3, {0, 2, 3}, {8, 9, 10}}, {-1, 1, {0}, {11}},
    };
    constexpr UINT kSteps = sizeof(steps) / sizeof(steps[0]);
    const uint32_t errors_before = device.shell.device_errors;

    D3D12DDI_DESCRIPTOR_RANGE_0013 ranges[3];
    for (UINT r = 0; r < 3; ++r)
        ranges[r] = {D3D12DDI_DESCRIPTOR_RANGE_TYPE_UAV, 1, r, 0, D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE,
                     D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND};
    void* rs[2] = {};
    void* cs_storage[2] = {};
    void* pso[2] = {};
    int pso_rt[2] = {};
    DdiShader cs;
    bool made = ddi_form(g_fixture_tables, sizeof(g_fixture_tables), cs);
    for (UINT l = 0; l < 2 && made; ++l) {
        D3D12DDI_ROOT_PARAMETER_0013 params[kParams]{};
        for (UINT p = 0; p < kParams; ++p) {
            params[p].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
            if (layouts[l].reg[p] < 0) {
                params[p].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
                params[p].Constants = {0, 0, 1};
            } else {
                params[p].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                params[p].DescriptorTable = {1, &ranges[layouts[l].reg[p]]};
            }
        }
        rs[l] = create_root_signature(env, device, kParams, params);
        const D3D12DDI_HROOTSIGNATURE hrs{rs[l]};
        cs_storage[l] = rs[l] ? create_shader(env, device, env.core.pfnCreateComputeShader, cs, hrs) : nullptr;
        D3D12DDIARG_CREATE_PIPELINE_STATE_0075 pso_args{};
        pso_args.hComputeShader = D3D12DDI_HSHADER{cs_storage[l]};
        pso_args.hRootSignature = hrs;
        pso[l] = cs_storage[l] ? env.storage.alloc(env.core.pfnCalcPrivatePipelineStateSize(device.h(), &pso_args))
                               : nullptr;
        if (pso[l] && env.core.pfnCreatePipelineState(device.h(), &pso_args, D3D12DDI_HPIPELINESTATE{pso[l]},
                                                      D3D12DDI_HRTPIPELINESTATE{&pso_rt[l]}) != S_OK)
            pso[l] = nullptr;
        made = pso[l] != nullptr;
    }

    Buffer out, readback;
    made = made && create_buffer(env, device, HeapKind::Default, kSlots * kSlotBytes, true, out) == S_OK &&
           create_buffer(env, device, HeapKind::Readback, kSlots * kSlotBytes, false, readback) == S_OK;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu{};
    void* heap = create_view_heap(env, device, kSlots, true, cpu, &gpu);
    const UINT inc = env.core.pfnGetDescriptorSizeInBytes(device.h(), D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    made = made && heap && cpu.ptr && gpu.ptr && inc;
    if (made) {
        for (UINT k = 0; k < kSlots; ++k) {
            D3D12DDIARG_CREATE_UNORDERED_ACCESS_VIEW_0002 uav{};
            uav.hDrvResource = out.hres();
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ResourceDimension = D3D12DDI_RD_BUFFER;
            uav.Buffer.FirstElement = UINT64{k} * kSlotBytes / 4;
            uav.Buffer.NumElements = kSlotBytes / 4;
            uav.Buffer.Flags = D3D12DDI_BUFFER_UAV_FLAG_RAW;
            env.core.pfnCreateUnorderedAccessView(device.h(), &uav, {cpu.ptr + SIZE_T{k} * inc});
        }
        made = device.shell.device_errors == errors_before;
    }
    checkf(made, "root tables: two root signatures with three UAV tables in different orders, a compute pipeline on "
                 "each, a DEFAULT and a READBACK buffer, twelve raw UAVs in a shader-visible heap");

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    Recording rec;
    if (made) {
        made = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue) == S_OK && queue &&
               open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, rec) == S_OK && rec.table == 0;
        checkf(made, "root tables: COMPUTE queue and list");
    }
    if (made) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[0];
        D3D12DDI_HDESCRIPTORHEAP heaps[] = {D3D12DDI_HDESCRIPTORHEAP{heap}};
        t.pfnSetDescriptorHeaps(rec.hlist(), 1, heaps);
        D3D12DDIARG_RESOURCE_BARRIER_0022 barrier =
            transition(out, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS);
        t.pfnResourceBarrier(rec.hlist(), 1, &barrier);
        D3D12DDIARG_RESOURCE_BARRIER_0022 uav{};
        uav.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
        UINT expected[kSlots] = {};
        UINT table_slot[kParams] = {};
        int bound = -1;
        for (UINT d = 0; d < kSteps; ++d) {
            const Step& s = steps[d];
            if (s.layout >= 0) {
                bound = s.layout;
                t.pfnSetComputeRootSignature(rec.hlist(), D3D12DDI_HROOTSIGNATURE{rs[bound]});
                t.pfnSetPipelineState(rec.hlist(), D3D12DDI_HPIPELINESTATE{pso[bound]});
            }
            for (UINT k = 0; k < s.count; ++k) {
                t.pfnSetComputeRootDescriptorTable(rec.hlist(), s.param[k], {gpu.ptr + UINT64{s.slot[k]} * inc});
                table_slot[s.param[k]] = s.slot[k];
            }
            t.pfnSetComputeRoot32BitConstant(rec.hlist(), layouts[bound].constant, kSeedBase + d, 0);
            t.pfnDispatch(rec.hlist(), 1, 1, 1);
            t.pfnResourceBarrier(rec.hlist(), 1, &uav);
            for (UINT p = 0; p < kParams; ++p) {
                if (layouts[bound].reg[p] >= 0)
                    expected[table_slot[p]] = (kSeedBase + d) ^ kTagOf[layouts[bound].reg[p]];
            }
        }
        barrier = transition(out, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), 1, &barrier);
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD = {readback.hres(), 0};
        src.BaseAddress.UMD = {out.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), dst, src, kSlots * kSlotBytes);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        const HRESULT hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors,
               "root tables: six dispatches over two root signatures with some tables set again, copy, execute "
               "(hr %08lx)",
               static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "root tables");
        void* mapped = nullptr;
        UINT bad = 0, first = kSlots, got = 0;
        if (env.core.pfnMapHeap(device.h(), readback.hheap(), &mapped) == S_OK && mapped) {
            const auto* words = static_cast<const UINT32*>(mapped);
            for (UINT k = 0; k < kSlots; ++k) {
                if (words[k * kSlotBytes / 4] != expected[k]) {
                    if (!bad) {
                        first = k;
                        got = words[k * kSlotBytes / 4];
                    }
                    ++bad;
                }
            }
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            bad = ~0u;
        }
        checkf(!bad, "root tables: every slot holds seed ^ tag of the last dispatch whose table named it (%u slots "
                     "differ, first %u: %08x, expected %08x)",
               bad, first, got, first < kSlots ? expected[first] : 0u);
    }
    destroy_recording(env, device, rec);
    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "root tables: destroy_engine_queue reports Retired");
    if (heap) env.core.pfnDestroyDescriptorHeap(device.h(), D3D12DDI_HDESCRIPTORHEAP{heap});
    destroy_buffer(env, device, out);
    destroy_buffer(env, device, readback);
    for (UINT l = 0; l < 2; ++l) {
        if (pso[l]) env.core.pfnDestroyPipelineState(device.h(), D3D12DDI_HPIPELINESTATE{pso[l]});
        if (cs_storage[l]) env.core.pfnDestroyShader(device.h(), D3D12DDI_HSHADER{cs_storage[l]});
        if (rs[l]) env.core.pfnDestroyRootSignature(device.h(), D3D12DDI_HROOTSIGNATURE{rs[l]});
    }
}

// ---- Buffer rebinds ---------------------------------------------------------------------------------------------------
// Vertex buffer a: five full-target triangles, tagged A, B, C, E and A (vertices 0-2, 3-5, 6-8, 9-11, 12-14); vertex
// buffer d: one, tagged D. The index buffer holds u16 0..11 at byte 0 and u32 3, 4, 5 at byte 64; read as u16, the
// latter is 3, 0, 4: a triangle on the target's left edge, which covers no texel.
constexpr UINT kTriangleTags[5] = {A, B, C, E, A};
constexpr UINT kIndex32Offset = 64;

struct Cell {
    UINT tag;           // the expected tag, or ~0u for a cell nothing is drawn into
};

void test_buffer_rebinds(Env& env, Device& device, const Pipeline& pipeline, Target& target, const Buffer& readback,
                         D3D12DDI_GPU_VIRTUAL_ADDRESS a, D3D12DDI_GPU_VIRTUAL_ADDRESS d, D3D12DDI_GPU_VIRTUAL_ADDRESS ib,
                         const Buffer& ib_buffer, const Buffer& scratch) {
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    Recording rec;
    const bool made = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue) == S_OK && queue &&
                      open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) == S_OK && rec.table == 1;
    checkf(made, "buffer rebinds: DIRECT queue and list");
    if (made) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[1];
        const D3D12DDI_HCOMMANDLIST l = rec.hlist();
        const D3D12DDI_VERTEX_BUFFER_VIEW va12{a, 15 * sizeof(Vertex), sizeof(Vertex)};
        const D3D12DDI_VERTEX_BUFFER_VIEW va12_36{a + 3 * sizeof(Vertex), 12 * sizeof(Vertex), sizeof(Vertex)};
        const D3D12DDI_VERTEX_BUFFER_VIEW va12_small{a, 6 * sizeof(Vertex), sizeof(Vertex)};
        const D3D12DDI_VERTEX_BUFFER_VIEW va24{a, 15 * sizeof(Vertex), 2 * sizeof(Vertex)};
        const D3D12DDI_VERTEX_BUFFER_VIEW vd{d, 3 * sizeof(Vertex), sizeof(Vertex)};
        const D3D12DDI_INDEX_BUFFER_VIEW i16{ib, 24, DXGI_FORMAT_R16_UINT};
        const D3D12DDI_INDEX_BUFFER_VIEW i16_6{ib + 6, 18, DXGI_FORMAT_R16_UINT};
        const D3D12DDI_INDEX_BUFFER_VIEW i32_64{ib + kIndex32Offset, 12, DXGI_FORMAT_R32_UINT};
        const D3D12DDI_INDEX_BUFFER_VIEW i16_64{ib + kIndex32Offset, 12, DXGI_FORMAT_R16_UINT};

        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_target =
            transition(target.texture, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_RENDER_TARGET);
        t.pfnResourceBarrier(l, 1, &to_target);
        const FLOAT zero[4] = {};
        t.pfnClearRenderTargetView(l, target.rtv, zero, 0, nullptr);
        t.pfnOMSetRenderTargets(l, 1, &target.rtv, TRUE, nullptr);
        const D3D12DDI_VIEWPORT viewport{0.0f, 0.0f, static_cast<FLOAT>(kSize), static_cast<FLOAT>(kSize), 0.0f, 1.0f};
        t.pfnRsSetViewports(l, 1, &viewport);
        t.pfnSetGraphicsRootSignature(l, pipeline.hrs());
        t.pfnSetPipelineState(l, pipeline.hpso());
        t.pfnSetGraphicsRoot32BitConstant(l, 0, kSeed, 0);
        t.pfnIaSetTopology(l, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        UINT cell = 0;
        const auto draw = [&](UINT start) {
            const LONG x = static_cast<LONG>(cell % 4 * 16), y = static_cast<LONG>(cell / 4 * 16);
            const D3D12DDI_RECT scissor{x, y, x + 16, y + 16};
            t.pfnRsSetScissorRects(l, 1, &scissor);
            t.pfnDrawIndexedInstanced(l, 3, 1, start, 0, 0);
            ++cell;
        };
        const auto vb = [&](const D3D12DDI_VERTEX_BUFFER_VIEW& view) { t.pfnIASetVertexBuffers(l, 0, 1, &view); };
        const auto ibv = [&](const D3D12DDI_INDEX_BUFFER_VIEW* view) { t.pfnIASetIndexBuffer(l, view); };

        vb(va12), ibv(&i16), draw(0);                   // 0: A
        vb(va12), ibv(&i16), draw(3);                   // 1: both set again unchanged; B by the start index
        ibv(&i16_6), draw(0);                           // 2: index offset 6: indices 3, 4, 5: B
        ibv(&i16), draw(0);                             // 3: back: A
        ibv(&i32_64), draw(0);                          // 4: offset and format: u32 3, 4, 5: B
        ibv(&i16_64), draw(0);                          // 5: format alone: u16 3, 0, 4 covers nothing
        ibv(&i16), vb(vd), draw(0);                     // 6: vertex buffer: D
        vb(va12), draw(0);                              // 7: back: A
        vb(va12_36), draw(0);                           // 8: vertex offset 36: vertices 3-5: B
        vb(va12_36), draw(3);                           // 9: set again unchanged: vertices 6-8: C
        vb(va24), draw(3);                              // 10: stride 24: vertices 6, 8, 10, provoking 6: C
        vb(va12), ibv(nullptr), ibv(&i16), draw(9);     // 11: stride back, index buffer null and back: E
        vb(va12_small), vb(va12_small), draw(0);        // 12: size alone, then unchanged: A
        t.pfnSetGraphicsRootSignature(l, pipeline.hrs());
        t.pfnSetPipelineState(l, pipeline.hpso());
        t.pfnSetGraphicsRoot32BitConstant(l, 0, kSeed, 0);
        vb(va12_small), ibv(&i16), draw(3);             // 13: root signature and pipeline set again: B
        D3D12DDIARG_BUFFER_PLACEMENT copy_dst{}, copy_src{};
        copy_dst.BaseAddress.UMD = {scratch.hres(), 0};
        copy_src.BaseAddress.UMD = {ib_buffer.hres(), 0};
        t.pfnCopyBufferRegion(l, copy_dst, copy_src, 16);
        vb(va12), ibv(&i16), draw(6);                   // 14: after a copy ends the render pass: C
        const D3D12DDI_VERTEX_BUFFER_VIEW two[2] = {vd, va12};
        t.pfnIASetVertexBuffers(l, 0, 2, two);          // slot 0 changes, slot 1 is not in the layout
        t.pfnIASetVertexBuffers(l, 1, 1, &vd);          // slot 1 alone
        draw(0);                                        // 15: D

        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(target.texture, D3D12DDI_RESOURCE_STATE_RENDER_TARGET, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(l, 1, &to_source);
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{DXGI_FORMAT_R32_UINT, kSize, kSize, 1, kPitch,
                                                                        kPitch * kSize};
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD = {readback.hres(), 0};
        src.BaseAddress.UMD = {target.texture.hres(), 0};
        t.pfnCopyTextureRegion(l, &dst, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint}, 0, 0, 0, &src,
                               {D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr}, nullptr);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_common =
            transition(target.texture, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_COMMON);
        t.pfnResourceBarrier(l, 1, &to_common);         // where the cost lists start
        t.pfnCloseCommandList(l);
        const D3D12DDI_HCOMMANDLIST lists[] = {l};
        const HRESULT hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors && cell == 16,
               "buffer rebinds: sixteen indexed draws between vertex and index buffer sets, execute (hr %08lx)",
               static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "buffer rebinds");

        constexpr UINT kNone = ~0u;
        constexpr UINT kCells[16] = {A, B, B, A, B, kNone, D, A, B, C, C, E, A, B, C, D};
        UINT bad[16] = {}, total = 0;
        void* cpu = nullptr;
        if (env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) == S_OK && cpu) {
            const auto* bytes = static_cast<const BYTE*>(cpu);
            for (UINT y = 0; y < kSize; ++y) {
                for (UINT x = 0; x < kSize; ++x) {
                    UINT v;
                    std::memcpy(&v, bytes + SIZE_T{y} * kPitch + SIZE_T{x} * 4, 4);
                    const UINT c = y / 16 * 4 + x / 16;
                    const UINT want = kCells[c] == kNone ? 0u : expected_texel(kTags[kCells[c]], x, y);
                    if (v != want) {
                        ++bad[c];
                        ++total;
                    }
                }
            }
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            total = ~0u;
        }
        checkf(!total,
               "buffer rebinds: each cell shows the tag its bound buffers select (texels differ per cell: %u %u %u %u "
               "%u %u %u %u %u %u %u %u %u %u %u %u)",
               bad[0], bad[1], bad[2], bad[3], bad[4], bad[5], bad[6], bad[7], bad[8], bad[9], bad[10], bad[11], bad[12],
               bad[13], bad[14], bad[15]);
    }
    destroy_recording(env, device, rec);
    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "buffer rebinds: destroy_engine_queue reports Retired");
}

// ---- Query reuse ------------------------------------------------------------------------------------------------------
// Round r records kCounts[r] occlusion queries of one heap; query i counts a full-target triangle (every one of the
// target's 4096 samples) when i + r is even and holds no draw otherwise. Round 1 reuses round 0's pool after
// ResetCommandPool and hands out two queries round 0 never did; round 2 records on a new pool after round 0's is
// destroyed. A query lent again without its reset keeps the previous round's count or fails validation.
void test_query_reuse(Env& env, Device& device, const Pipeline& pipeline, Target& target,
                      D3D12DDI_GPU_VIRTUAL_ADDRESS a) {
    constexpr UINT kQueries = 8, kRounds = 3;
    constexpr UINT kCounts[kRounds] = {6, 8, 3};
    constexpr UINT64 kFull = UINT64{kSize} * kSize;
    const uint32_t errors_before = device.shell.device_errors;
    D3D12DDIARG_CREATE_QUERY_HEAP_0001 heap_args{D3D12DDI_QUERY_HEAP_TYPE_OCCLUSION, kQueries, 0};
    void* heap = env.storage.alloc(env.core.pfnCalcPrivateQueryHeapSize(device.h(), &heap_args));
    if (heap && env.core.pfnCreateQueryHeap(device.h(), &heap_args, D3D12DDI_HQUERYHEAP{heap}) != S_OK) heap = nullptr;
    const D3D12DDI_HQUERYHEAP hheap{heap};
    Buffer results;
    bool made = heap && create_buffer(env, device, HeapKind::Readback, UINT64{kRounds} * kQueries * 8, false,
                                      results) == S_OK;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    made = made && engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue) == S_OK && queue;
    checkf(made && device.shell.device_errors == errors_before,
           "query reuse: occlusion query heap of %u, READBACK buffer, DIRECT queue", kQueries);

    Recording rec;
    for (UINT r = 0; made && r < kRounds; ++r) {
        if (r == 1) {
            // The pool of round 0 again: its allocator returns the query pools it borrowed and borrows them back.
            env.core.pfnResetCommandPool(device.h(), D3D12DDI_HCOMMANDPOOL_0040{rec.pool});
            D3D12DDIARG_RESETCOMMANDLIST_0040 reset{D3D12DDI_HCOMMANDRECORDER_0040{rec.recorder}, 1,
                                                   D3D12DDI_COMMAND_LIST_FLAG_NONE};
            env.lists[rec.table].pfnResetCommandList(rec.hlist(), &reset);
        } else {
            destroy_recording(env, device, rec);
            if (open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) != S_OK || rec.table != 1) {
                checkf(false, "query reuse: DIRECT list for round %u", r);
                break;
            }
        }
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[1];
        const D3D12DDI_HCOMMANDLIST l = rec.hlist();
        const D3D12DDI_VERTEX_BUFFER_VIEW view{a, 3 * sizeof(Vertex), sizeof(Vertex)};
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_target =
            transition(target.texture, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_RENDER_TARGET);
        t.pfnResourceBarrier(l, 1, &to_target);
        t.pfnOMSetRenderTargets(l, 1, &target.rtv, TRUE, nullptr);
        const D3D12DDI_VIEWPORT viewport{0.0f, 0.0f, static_cast<FLOAT>(kSize), static_cast<FLOAT>(kSize), 0.0f, 1.0f};
        t.pfnRsSetViewports(l, 1, &viewport);
        const D3D12DDI_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
        t.pfnRsSetScissorRects(l, 1, &scissor);
        t.pfnSetGraphicsRootSignature(l, pipeline.hrs());
        t.pfnSetPipelineState(l, pipeline.hpso());
        t.pfnSetGraphicsRoot32BitConstant(l, 0, kSeed, 0);
        t.pfnIaSetTopology(l, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.pfnIASetVertexBuffers(l, 0, 1, &view);
        for (UINT i = 0; i < kCounts[r]; ++i) {
            t.pfnBeginQuery(l, hheap, D3D12DDI_QUERY_TYPE_OCCLUSION, i);
            if ((i + r) % 2 == 0) t.pfnDrawInstanced(l, 3, 1, 0, 0);
            t.pfnEndQuery(l, hheap, D3D12DDI_QUERY_TYPE_OCCLUSION, i);
        }
        t.pfnResolveQueryData(l, hheap, D3D12DDI_QUERY_TYPE_OCCLUSION, 0, kCounts[r], results.hres(),
                              UINT64{r} * kQueries * 8);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_common =
            transition(target.texture, D3D12DDI_RESOURCE_STATE_RENDER_TARGET, D3D12DDI_RESOURCE_STATE_COMMON);
        t.pfnResourceBarrier(l, 1, &to_common);
        t.pfnCloseCommandList(l);
        const D3D12DDI_HCOMMANDLIST lists[] = {l};
        const HRESULT hr = engine_ddi::execute_command_lists(queue, 1, lists);
        made = hr == S_OK && !device.shell.list_errors && wait_queue_idle(env, queue, "query reuse");
        checkf(made, "query reuse: round %u, %u occlusion queries, resolve, execute (hr %08lx)", r, kCounts[r],
               static_cast<unsigned long>(hr));
    }
    destroy_recording(env, device, rec);

    if (made) {
        UINT bad = 0, first = 0;
        UINT64 first_value = 0;
        void* cpu = nullptr;
        if (env.core.pfnMapHeap(device.h(), results.hheap(), &cpu) == S_OK && cpu) {
            const auto* values = static_cast<const UINT64*>(cpu);
            for (UINT r = 0; r < kRounds; ++r) {
                for (UINT i = 0; i < kCounts[r]; ++i) {
                    const UINT64 v = values[r * kQueries + i];
                    if (v != ((i + r) % 2 == 0 ? kFull : 0)) {
                        if (!bad) first = r * kQueries + i, first_value = v;
                        ++bad;
                    }
                }
            }
            env.core.pfnUnmapHeap(device.h(), results.hheap());
        } else {
            bad = ~0u;
        }
        checkf(!bad, "query reuse: each query counts its own draws, %llu samples or none (%u differ, first round %u "
                     "query %u: %llu)",
               static_cast<unsigned long long>(kFull), bad, first / kQueries, first % kQueries,
               static_cast<unsigned long long>(first_value));
    }
    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "query reuse: destroy_engine_queue reports Retired");
    destroy_buffer(env, device, results);
    if (heap) env.core.pfnDestroyQueryHeap(device.h(), hheap);
}

// ---- Read-only transitions in a render pass ---------------------------------------------------------------------------
// Three lists on one queue, around a second R32_UINT target "tex". List 1 draws tex (tag A) and copies it out, then
// moves it between read-only states (COPY_SOURCE, PIXEL_SHADER_RESOURCE, both shader-resource states) between draws
// into cells of the target, transitions it to RENDER_TARGET, draws it again (D) and copies it out. List 2 moves tex
// from COPY_SOURCE between two more cells and closes with no further barrier; list 3 draws tex (C) and copies tex and
// the target out. The engine may hold a read-only transition until the render pass ends, and must then emit it before
// the next barrier and at Close: every draw into tex comes after a copy that read it, and the only chain from that
// copy to the draw runs through the transition out of COPY_SOURCE, so a lost or misplaced one shows as a
// write-after-read hazard under synchronization validation. The copies and the target's cells check the images.
//
// Cost: the recording thread's time per draw in one render pass, plain and with a read-only transition of tex
// (PIXEL_SHADER_RESOURCE to both shader-resource states and back) before each draw, printed as a "measure" line.
void test_read_barriers(Env& env, Device& device, const Pipeline& pipeline, Target& target,
                        D3D12DDI_GPU_VIRTUAL_ADDRESS a, D3D12DDI_GPU_VIRTUAL_ADDRESS d) {
    constexpr UINT64 kImage = UINT64{kPitch} * kSize;
    constexpr UINT kDraws = 2048;
    const uint32_t errors_before = device.shell.device_errors;
    Target tex;
    Buffer readback;
    bool made = tex.create(env, device) &&
                create_buffer(env, device, HeapKind::Readback, 4 * kImage, false, readback) == S_OK;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    made = made && engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue) == S_OK && queue;
    checkf(made && device.shell.device_errors == errors_before,
           "read-only transitions: second 64x64 R32_UINT target, READBACK buffer of four images, DIRECT queue");

    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[1];
    const D3D12DDI_VERTEX_BUFFER_VIEW va{a, 15 * sizeof(Vertex), sizeof(Vertex)};
    const D3D12DDI_VERTEX_BUFFER_VIEW vd{d, 3 * sizeof(Vertex), sizeof(Vertex)};
    const D3D12DDI_VIEWPORT viewport{0.0f, 0.0f, static_cast<FLOAT>(kSize), static_cast<FLOAT>(kSize), 0.0f, 1.0f};
    const D3D12DDI_RECT full{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
    const D3D12DDI_RESOURCE_STATES common = D3D12DDI_RESOURCE_STATE_COMMON;
    const D3D12DDI_RESOURCE_STATES rt = D3D12DDI_RESOURCE_STATE_RENDER_TARGET;
    const D3D12DDI_RESOURCE_STATES source = D3D12DDI_RESOURCE_STATE_COPY_SOURCE;
    const D3D12DDI_RESOURCE_STATES pixel = D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12DDI_RESOURCE_STATES shader =
        D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12DDI_HCOMMANDLIST l{};
    const auto barrier = [&](const Target& of, D3D12DDI_RESOURCE_STATES before, D3D12DDI_RESOURCE_STATES after) {
        const D3D12DDIARG_RESOURCE_BARRIER_0022 b = transition(of.texture, before, after);
        t.pfnResourceBarrier(l, 1, &b);
    };
    const auto begin = [&](const Recording& rec) {
        l = rec.hlist();
        t.pfnRsSetViewports(l, 1, &viewport);
        t.pfnSetGraphicsRootSignature(l, pipeline.hrs());
        t.pfnSetPipelineState(l, pipeline.hpso());
        t.pfnSetGraphicsRoot32BitConstant(l, 0, kSeed, 0);
        t.pfnIaSetTopology(l, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    };
    const auto copy_out = [&](const Target& of, UINT image) {
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{DXGI_FORMAT_R32_UINT, kSize, kSize, 1, kPitch,
                                                                        kPitch * kSize};
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD = {readback.hres(), image * kImage};
        src.BaseAddress.UMD = {of.texture.hres(), 0};
        t.pfnCopyTextureRegion(l, &dst, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint}, 0, 0, 0, &src,
                               {D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr}, nullptr);
    };
    // A full-target triangle into tex: triangle k of va (tags A B C E A), or vd's (D) for k = 5.
    const auto draw_tex = [&](UINT k) {
        t.pfnOMSetRenderTargets(l, 1, &tex.rtv, TRUE, nullptr);
        t.pfnRsSetScissorRects(l, 1, &full);
        t.pfnIASetVertexBuffers(l, 0, 1, k < 5 ? &va : &vd);
        t.pfnDrawInstanced(l, 3, 1, k < 5 ? 3 * k : 0, 0);
    };
    // Triangle k of va into the next 16x16 cell of the target, which must be bound.
    UINT cell = 0;
    const auto draw_cell = [&](UINT k) {
        const LONG x = static_cast<LONG>(cell % 4 * 16), y = static_cast<LONG>(cell / 4 * 16);
        const D3D12DDI_RECT scissor{x, y, x + 16, y + 16};
        t.pfnRsSetScissorRects(l, 1, &scissor);
        t.pfnDrawInstanced(l, 3, 1, 3 * k, 0);
        ++cell;
    };

    Recording recs[3];
    for (Recording& rec : recs)
        made = made && open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) == S_OK && rec.table == 1;
    checkf(made, "read-only transitions: three DIRECT lists");
    if (made) {
        const FLOAT zero[4] = {};
        // The target's transition out of COMMON waits for all earlier work: it comes before the first copy of tex.
        begin(recs[0]);
        barrier(target, common, rt);
        t.pfnClearRenderTargetView(l, target.rtv, zero, 0, nullptr);
        barrier(tex, common, rt);
        draw_tex(0);                                    // tex: A
        barrier(tex, rt, source);
        copy_out(tex, 0);
        t.pfnOMSetRenderTargets(l, 1, &target.rtv, TRUE, nullptr);
        t.pfnIASetVertexBuffers(l, 0, 1, &va);
        draw_cell(1);                                   // 0: B
        barrier(tex, source, pixel);
        draw_cell(2);                                   // 1: C
        barrier(tex, pixel, shader);
        draw_cell(3);                                   // 2: E
        barrier(tex, shader, rt);
        draw_tex(5);                                    // tex: D, after the copy of A read it
        barrier(tex, rt, source);
        copy_out(tex, 1);
        barrier(target, rt, pixel);
        t.pfnCloseCommandList(l);

        // A barrier first, as a frame's list has: the engine holds read-only transitions only once an action command
        // has settled that the list does not resume the previous list's render pass.
        begin(recs[1]);
        barrier(target, pixel, rt);
        t.pfnOMSetRenderTargets(l, 1, &target.rtv, TRUE, nullptr);
        t.pfnIASetVertexBuffers(l, 0, 1, &va);
        draw_cell(0);                                   // 3: A
        barrier(tex, source, shader);                   // emitted at Close at the latest
        draw_cell(1);                                   // 4: B
        t.pfnCloseCommandList(l);

        begin(recs[2]);
        barrier(tex, shader, rt);
        draw_tex(2);                                    // tex: C, after the copy of D read it
        barrier(tex, rt, source);
        copy_out(tex, 2);
        barrier(target, rt, source);
        copy_out(target, 3);
        D3D12DDIARG_RESOURCE_BARRIER_0022 to_common[2] = {transition(tex.texture, source, common),
                                                          transition(target.texture, source, common)};
        t.pfnResourceBarrier(l, 2, to_common);
        t.pfnCloseCommandList(l);

        const D3D12DDI_HCOMMANDLIST lists[] = {recs[0].hlist(), recs[1].hlist(), recs[2].hlist()};
        const HRESULT hr = engine_ddi::execute_command_lists(queue, 3, lists);
        made = hr == S_OK && !device.shell.list_errors && wait_queue_idle(env, queue, "read-only transitions");
        checkf(made, "read-only transitions: three lists with read-only transitions of tex between draws into the "
                     "target, execute (hr %08lx)",
               static_cast<unsigned long>(hr));
    }
    for (Recording& rec : recs) destroy_recording(env, device, rec);

    if (made) {
        constexpr UINT kNone = ~0u;
        constexpr UINT kImages[3] = {A, D, C};
        constexpr UINT kCells[16] = {B, C, E, A, B, kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone, kNone,
                                     kNone};
        UINT bad[4] = {};
        void* cpu = nullptr;
        if (env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) == S_OK && cpu) {
            const auto* bytes = static_cast<const BYTE*>(cpu);
            for (UINT image = 0; image < 4; ++image) {
                for (UINT y = 0; y < kSize; ++y) {
                    for (UINT x = 0; x < kSize; ++x) {
                        UINT v;
                        std::memcpy(&v, bytes + image * kImage + SIZE_T{y} * kPitch + SIZE_T{x} * 4, 4);
                        const UINT tag = image < 3 ? kImages[image] : kCells[y / 16 * 4 + x / 16];
                        bad[image] += v != (tag == kNone ? 0u : expected_texel(kTags[tag], x, y));
                    }
                }
            }
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            bad[0] = ~0u;
        }
        checkf(!bad[0] && !bad[1] && !bad[2],
               "read-only transitions: tex shows A, D and C in turn (texels differ: %u %u %u)", bad[0], bad[1], bad[2]);
        checkf(!bad[3], "read-only transitions: the target's five cells show B C E A B, the rest zero (%u differ)",
               bad[3]);
    }

    // Cost: each list is recorded twice and the second figure kept, as in measure_draw_path.
    double per_draw[2]{};
    for (int run = 0; made && run < 4; ++run) {
        const int variant = run & 1;
        Recording rec;
        if (open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) != S_OK || rec.table != 1) {
            checkf(false, "read-only transitions: DIRECT list for the cost");
            destroy_recording(env, device, rec);
            break;
        }
        begin(rec);
        barrier(tex, common, pixel);
        barrier(target, common, rt);
        t.pfnOMSetRenderTargets(l, 1, &target.rtv, TRUE, nullptr);
        t.pfnRsSetScissorRects(l, 1, &full);
        t.pfnIASetVertexBuffers(l, 0, 1, &va);
        const uint64_t t0 = qpc();
        for (UINT k = 0; k < kDraws; ++k) {
            if (variant) barrier(tex, k & 1 ? shader : pixel, k & 1 ? pixel : shader);
            t.pfnDrawInstanced(l, 3, 1, 0, 0);
        }
        const uint64_t t1 = qpc();
        if (run >= 2) per_draw[variant] = ns_per(t1 - t0, kDraws);
        D3D12DDIARG_RESOURCE_BARRIER_0022 to_common[2] = {transition(tex.texture, pixel, common),
                                                          transition(target.texture, rt, common)};
        t.pfnResourceBarrier(l, 2, to_common);
        t.pfnCloseCommandList(l);
        const D3D12DDI_HCOMMANDLIST lists[] = {l};
        const HRESULT hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors && wait_queue_idle(env, queue, "read-only transitions"),
               "read-only transitions: cost list %s, execute (hr %08lx)", variant ? "with transitions" : "plain",
               static_cast<unsigned long>(hr));
        destroy_recording(env, device, rec);
    }
    if (made) {
        std::printf("measure  per draw in one render pass on the recording thread: %.1f ns plain, %.1f ns with a "
                    "read-only transition of another texture before each (%u draws each)\n",
                    per_draw[0], per_draw[1], kDraws);
    }

    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "read-only transitions: destroy_engine_queue reports Retired");
    destroy_buffer(env, device, readback);
    tex.destroy(env, device);
}

// ---- Cost -------------------------------------------------------------------------------------------------------------
void measure_draw_path(Env& env, Device& device, Target& target, const Buffer& readback,
                       D3D12DDI_GPU_VIRTUAL_ADDRESS a, D3D12DDI_GPU_VIRTUAL_ADDRESS ib) {
    constexpr UINT kDraws = 2048, kTables = 64, kTableSize = 8, kCopies = 8192;
    const uint32_t errors_before = device.shell.device_errors;

    // Root signature: CBV(b0), DescriptorTable(SRV(t0, numDescriptors = 8), visibility = pixel). The shaders read b0
    // and no texture; the table is set and flushed per draw as a game's is.
    const D3D12DDI_DESCRIPTOR_RANGE_0013 range{D3D12DDI_DESCRIPTOR_RANGE_TYPE_SRV, kTableSize, 0, 0,
                                               D3D12DDI_DESCRIPTOR_RANGE_FLAG_0013_NONE, 0};
    D3D12DDI_ROOT_PARAMETER_0013 params[2]{};
    params[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    params[0].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {1, &range};
    params[1].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_PIXEL;
    Pipeline pipeline;
    bool made = pipeline.create(env, device, 2, params);

    Buffer cb, source;
    const UINT seed_words[64] = {kSeed};
    made = made && create_buffer(env, device, HeapKind::Upload, sizeof(seed_words), false, cb) == S_OK;
    made = made && create_buffer(env, device, HeapKind::Upload, 4096, false, source) == S_OK;
    const D3D12DDI_GPU_VIRTUAL_ADDRESS cb_va = made ? write_buffer(env, device, cb, seed_words, sizeof(seed_words)) : 0;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE plain{}, visible{};
    D3D12DDI_GPU_DESCRIPTOR_HANDLE gpu{};
    void* plain_heap = create_view_heap(env, device, kTableSize, false, plain, nullptr);
    void* visible_heap = create_view_heap(env, device, kTables * kTableSize, true, visible, &gpu);
    const UINT inc = env.core.pfnGetDescriptorSizeInBytes(device.h(), D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    made = made && cb_va && plain_heap && visible_heap && inc;
    if (made) {
        for (UINT j = 0; j < kTableSize; ++j) {
            D3D12DDIARG_CREATE_SHADER_RESOURCE_VIEW_0002 srv{};
            srv.hDrvResource = source.hres();
            srv.Format = DXGI_FORMAT_R32_UINT;
            srv.ResourceDimension = D3D12DDI_RD_BUFFER;
            srv.Shader4ComponentMapping = D3D12DDI_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer = {UINT64{j} * 64, 64, 0, D3D12DDI_BUFFER_SRV_FLAG_NONE};
            env.core.pfnCreateShaderResourceView(device.h(), &srv, {plain.ptr + SIZE_T{j} * inc});
        }
        made = device.shell.device_errors == errors_before;
    }
    checkf(made, "cost: pipeline over CBV(b0) and a table of eight SRVs, constant buffer, buffer SRVs, a plain heap of "
                 "eight and a shader-visible heap of %u",
           kTables * kTableSize);

    // Descriptor copies, as a game fills a table: eight one-descriptor ranges on each side (null size arrays), one
    // range of eight, and CopyDescriptorsSimple of eight.
    double ranged = 0, single = 0, simple = 0;
    if (made) {
        D3D12DDI_CPU_DESCRIPTOR_HANDLE src[kTableSize], dst[kTableSize];
        for (UINT j = 0; j < kTableSize; ++j) src[j] = {plain.ptr + SIZE_T{j} * inc};
        const UINT eight = kTableSize;
        for (int pass = 0; pass < 2; ++pass) {          // the first pass warms the caches
            uint64_t t0 = qpc();
            for (UINT k = 0; k < kCopies; ++k) {
                const SIZE_T base = visible.ptr + SIZE_T{k % kTables} * kTableSize * inc;
                for (UINT j = 0; j < kTableSize; ++j) dst[j] = {base + SIZE_T{j} * inc};
                env.core.pfnCopyDescriptors(device.h(), kTableSize, dst, nullptr, kTableSize, src, nullptr,
                                            D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            uint64_t t1 = qpc();
            ranged = ns_per(t1 - t0, kCopies);
            t0 = qpc();
            for (UINT k = 0; k < kCopies; ++k) {
                const D3D12DDI_CPU_DESCRIPTOR_HANDLE to{visible.ptr + SIZE_T{k % kTables} * kTableSize * inc};
                env.core.pfnCopyDescriptors(device.h(), 1, &to, &eight, 1, &plain, &eight,
                                            D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            t1 = qpc();
            single = ns_per(t1 - t0, kCopies);
            t0 = qpc();
            for (UINT k = 0; k < kCopies; ++k) {
                const D3D12DDI_CPU_DESCRIPTOR_HANDLE to{visible.ptr + SIZE_T{k % kTables} * kTableSize * inc};
                env.core.pfnCopyDescriptorsSimple(device.h(), kTableSize, to, plain,
                                                  D3D12DDI_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            t1 = qpc();
            simple = ns_per(t1 - t0, kCopies);
        }
        std::printf("measure  descriptor copies of %u CBV_SRV_UAV descriptors: CopyDescriptors of %u one-descriptor "
                    "ranges %.1f ns, of one range %.1f ns, CopyDescriptorsSimple %.1f ns per call (%u calls each)\n",
                    kTableSize, kTableSize, ranged, single, simple, kCopies);
        check(device.shell.device_errors == errors_before, "cost: descriptor copies report no error");
    }

    // Draws: per draw IASetVertexBuffers, IASetIndexBuffer, SetGraphicsRootConstantBufferView,
    // SetGraphicsRootDescriptorTable and DrawIndexedInstanced(3), in one render pass. "same" sets the same buffers
    // every time; "alternating" switches the vertex buffer between offsets 0 and 36 (tags A and B) every draw. Each
    // list is recorded twice and the second figure kept: the first draws of a pipeline also pay for its variant.
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    if (made) {
        made = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue) == S_OK && queue;
        checkf(made, "cost: DIRECT queue");
    }
    const char* names[2] = {"same buffers", "alternating vertex buffer"};
    double per_draw[2]{}, with_close[2]{};
    for (int run = 0; made && run < 4; ++run) {
        const int variant = run & 1;
        Recording rec;
        if (open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) != S_OK || rec.table != 1) {
            checkf(false, "cost: DIRECT list for %s", names[variant]);
            destroy_recording(env, device, rec);
            break;
        }
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[1];
        const D3D12DDI_HCOMMANDLIST l = rec.hlist();
        const D3D12DDI_VERTEX_BUFFER_VIEW views[2] = {{a, 15 * sizeof(Vertex), sizeof(Vertex)},
                                                      {a + 3 * sizeof(Vertex), 12 * sizeof(Vertex), sizeof(Vertex)}};
        const D3D12DDI_INDEX_BUFFER_VIEW index{ib, 24, DXGI_FORMAT_R16_UINT};
        D3D12DDI_HDESCRIPTORHEAP heaps[] = {D3D12DDI_HDESCRIPTORHEAP{visible_heap}};
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_target =
            transition(target.texture, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_RENDER_TARGET);
        t.pfnResourceBarrier(l, 1, &to_target);
        t.pfnOMSetRenderTargets(l, 1, &target.rtv, TRUE, nullptr);
        const D3D12DDI_VIEWPORT viewport{0.0f, 0.0f, static_cast<FLOAT>(kSize), static_cast<FLOAT>(kSize), 0.0f, 1.0f};
        t.pfnRsSetViewports(l, 1, &viewport);
        const D3D12DDI_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
        t.pfnRsSetScissorRects(l, 1, &scissor);
        t.pfnSetDescriptorHeaps(l, 1, heaps);
        t.pfnSetGraphicsRootSignature(l, pipeline.hrs());
        t.pfnSetPipelineState(l, pipeline.hpso());
        t.pfnIaSetTopology(l, D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const uint64_t t0 = qpc();
        for (UINT k = 0; k < kDraws; ++k) {
            t.pfnIASetVertexBuffers(l, 0, 1, &views[variant ? k & 1 : 0]);
            t.pfnIASetIndexBuffer(l, &index);
            t.pfnSetGraphicsRootConstantBufferView(l, 0, cb_va);
            t.pfnSetGraphicsRootDescriptorTable(l, 1, {gpu.ptr + UINT64{k % kTables} * kTableSize * inc});
            t.pfnDrawIndexedInstanced(l, 3, 1, 0, 0, 0);
        }
        const uint64_t t1 = qpc();
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(target.texture, D3D12DDI_RESOURCE_STATE_RENDER_TARGET, D3D12DDI_RESOURCE_STATE_COMMON);
        t.pfnResourceBarrier(l, 1, &to_source);
        t.pfnCloseCommandList(l);
        const uint64_t t2 = qpc();
        if (run >= 2) {
            per_draw[variant] = ns_per(t1 - t0, kDraws);
            with_close[variant] = ns_per(t2 - t0, kDraws);
        }
        const D3D12DDI_HCOMMANDLIST lists[] = {l};
        const HRESULT hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors, "cost: %u draws with %s, execute (hr %08lx)", kDraws,
               names[variant], static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "cost");
        destroy_recording(env, device, rec);
    }
    if (made) {
        std::printf("measure  per draw on the recording thread (vertex and index buffer, root CBV, table, "
                    "DrawIndexedInstanced): %s %.1f ns, %.1f ns with Close; %s %.1f ns, %.1f ns with Close "
                    "(%u draws each)\n",
                    names[0], per_draw[0], with_close[0], names[1], per_draw[1], with_close[1], kDraws);
    }

    // The last draw of the alternating list (odd) used offset 36: every texel shows tag B.
    if (made) {
        Recording rec;
        bool copied = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) == S_OK && rec.table == 1;
        if (copied) {
            const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[1];
            const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
                transition(target.texture, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
            const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{DXGI_FORMAT_R32_UINT, kSize, kSize, 1,
                                                                            kPitch, kPitch * kSize};
            D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
            dst.BaseAddress.UMD = {readback.hres(), 0};
            src.BaseAddress.UMD = {target.texture.hres(), 0};
            t.pfnCopyTextureRegion(rec.hlist(), &dst, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint}, 0,
                                   0, 0, &src, {D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr}, nullptr);
            const D3D12DDIARG_RESOURCE_BARRIER_0022 back =
                transition(target.texture, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_COMMON);
            t.pfnResourceBarrier(rec.hlist(), 1, &back);
            t.pfnCloseCommandList(rec.hlist());
            const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
            copied = engine_ddi::execute_command_lists(queue, 1, lists) == S_OK && wait_queue_idle(env, queue, "cost");
        }
        destroy_recording(env, device, rec);
        UINT bad = 0;
        void* cpu = nullptr;
        if (copied && env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) == S_OK && cpu) {
            const auto* bytes = static_cast<const BYTE*>(cpu);
            for (UINT y = 0; y < kSize; ++y) {
                for (UINT x = 0; x < kSize; ++x) {
                    UINT v;
                    std::memcpy(&v, bytes + SIZE_T{y} * kPitch + SIZE_T{x} * 4, 4);
                    bad += v != expected_texel(kTags[B], x, y);
                }
            }
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            bad = ~0u;
        }
        checkf(!bad, "cost: the last draw of the alternating list (vertex offset 36, root CBV seed) wrote every texel "
                     "(%u differ)",
               bad);
    }
    if (queue)
        check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
              "cost: destroy_engine_queue reports Retired");
    if (visible_heap) env.core.pfnDestroyDescriptorHeap(device.h(), D3D12DDI_HDESCRIPTORHEAP{visible_heap});
    if (plain_heap) env.core.pfnDestroyDescriptorHeap(device.h(), D3D12DDI_HDESCRIPTORHEAP{plain_heap});
    destroy_buffer(env, device, cb);
    destroy_buffer(env, device, source);
    pipeline.destroy(env, device);
}
} // namespace

void test_draw_path(Env& env, Device& device) {
    test_descriptor_ranges(env, device);
    test_root_tables(env, device);

    D3D12DDI_ROOT_PARAMETER_0013 constant{};
    constant.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constant.Constants = {0, 0, 1};
    constant.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    Pipeline pipeline;
    Target target;
    Buffer vb_a, vb_d, ib, readback, scratch;
    bool made = pipeline.create(env, device, 1, &constant) && target.create(env, device);
    made = made && create_buffer(env, device, HeapKind::Upload, 256, false, vb_a) == S_OK &&
           create_buffer(env, device, HeapKind::Upload, 256, false, vb_d) == S_OK &&
           create_buffer(env, device, HeapKind::Upload, 256, false, ib) == S_OK &&
           create_buffer(env, device, HeapKind::Readback, UINT64{kPitch} * kSize, false, readback) == S_OK &&
           create_buffer(env, device, HeapKind::Default, 256, false, scratch) == S_OK;
    D3D12DDI_GPU_VIRTUAL_ADDRESS a = 0, d = 0, ib_va = 0;
    if (made) {
        Vertex vertices[15];
        for (UINT k = 0; k < 5; ++k) {
            const UINT tag = kTags[kTriangleTags[k]];
            vertices[3 * k] = {-1.0f, -1.0f, tag};
            vertices[3 * k + 1] = {-1.0f, 3.0f, tag};
            vertices[3 * k + 2] = {3.0f, -1.0f, tag};
        }
        const Vertex others[3] = {{-1.0f, -1.0f, kTags[D]}, {-1.0f, 3.0f, kTags[D]}, {3.0f, -1.0f, kTags[D]}};
        BYTE indices[kIndex32Offset + 12] = {};
        for (uint16_t k = 0; k < 12; ++k) std::memcpy(indices + 2 * k, &k, 2);
        const UINT32 wide[3] = {3, 4, 5};
        std::memcpy(indices + kIndex32Offset, wide, sizeof(wide));
        a = write_buffer(env, device, vb_a, vertices, sizeof(vertices));
        d = write_buffer(env, device, vb_d, others, sizeof(others));
        ib_va = write_buffer(env, device, ib, indices, sizeof(indices));
        made = a && d && ib_va && device.shell.device_errors == 0;
    }
    checkf(made, "draw path: fixture pipeline over a root constant, 64x64 R32_UINT target, vertex, index, READBACK "
                 "and scratch buffers");
    if (made) {
        test_buffer_rebinds(env, device, pipeline, target, readback, a, d, ib_va, ib, scratch);
        test_query_reuse(env, device, pipeline, target, a);
        test_read_barriers(env, device, pipeline, target, a, d);
        measure_draw_path(env, device, target, readback, a, ib_va);
    }
    destroy_buffer(env, device, vb_a);
    destroy_buffer(env, device, vb_d);
    destroy_buffer(env, device, ib);
    destroy_buffer(env, device, readback);
    destroy_buffer(env, device, scratch);
    target.destroy(env, device);
    pipeline.destroy(env, device);
    checkf(!engine_ddi::harness_live_objects(device.context) && !engine_ddi::harness_pending_releases(device.context),
           "draw path: no live object and no pending release left (%u live)",
           engine_ddi::harness_live_objects(device.context));
}

} // namespace harness
