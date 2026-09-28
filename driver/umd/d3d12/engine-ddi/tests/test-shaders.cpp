// SPDX-License-Identifier: MIT
// Shaders in the DDI form, and round trip 3: a draw from shaders created through the native shader slots.
//
// fxc's vertex and pixel programs (fixture-gfx.h) are reduced to the DDI form and created with CreateVertexShader
// and CreatePixelShader; engine-ddi rebuilds their containers (shader-container). The element layout names input
// registers only, the blend, rasterizer and depth-stencil states are DDI descriptions, and the pipeline comes from
// CreatePipelineState. One triangle that covers a 64x64 R32_UINT render target, a copy to a READBACK buffer and a
// word-exact compare with what the pixel program writes:
//   seed ^ tag ^ ((y << 16) | x) ^ (pair.y << 8) ^ pair.x, with tag 0xB0250000 from the vertex buffer and
//   pair = (0x51, 3) from the vertex program.
// The tag goes through an input layout register, the pair through a varying register that TAG shares, the seed
// through a root constant.
#include "harness.h"
#include "fixture-gfx.h"
#include <cstdio>
#include <cstring>

namespace harness {

namespace {
uint32_t rd32(const BYTE* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) | static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24;
}

struct Chunk {
    uint32_t tag;
    const BYTE* data;                   // after the chunk header
    uint32_t size;
};

bool parse_container(const BYTE* b, size_t n, std::vector<Chunk>& chunks) {
    chunks.clear();
    if (n < 32 || rd32(b) != fourcc('D', 'X', 'B', 'C') || rd32(b + 24) != n) return false;
    const uint32_t count = rd32(b + 28);
    if (32 + uint64_t{4} * count > n) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t offset = rd32(b + 32 + size_t{4} * i);
        if (offset + uint64_t{8} > n) return false;
        const uint32_t size = rd32(b + offset + 4);
        if (offset + uint64_t{8} + size > n) return false;
        chunks.push_back({rd32(b + offset), b + offset + 8, size});
    }
    return true;
}

const Chunk* find_chunk(const std::vector<Chunk>& chunks, std::initializer_list<uint32_t> tags) {
    for (uint32_t tag : tags)
        for (const Chunk& c : chunks)
            if (c.tag == tag) return &c;
    return nullptr;
}

// D3D_NAME of a container element to D3D10_SB_NAME: tessellation factors split by semantic index; names without a
// tokenized-format equivalent (SV_Target, SV_Depth, SV_Coverage, ...) become undefined.
D3D10_SB_NAME to_sb_name(uint32_t name, uint32_t index) {
    switch (name) {
    case D3D_NAME_FINAL_QUAD_EDGE_TESSFACTOR:
        return static_cast<D3D10_SB_NAME>(D3D11_SB_NAME_FINAL_QUAD_U_EQ_0_EDGE_TESSFACTOR + index);
    case D3D_NAME_FINAL_QUAD_INSIDE_TESSFACTOR:
        return static_cast<D3D10_SB_NAME>(D3D11_SB_NAME_FINAL_QUAD_U_INSIDE_TESSFACTOR + index);
    case D3D_NAME_FINAL_TRI_EDGE_TESSFACTOR:
        return static_cast<D3D10_SB_NAME>(D3D11_SB_NAME_FINAL_TRI_U_EQ_0_EDGE_TESSFACTOR + index);
    case D3D_NAME_FINAL_TRI_INSIDE_TESSFACTOR: return D3D11_SB_NAME_FINAL_TRI_INSIDE_TESSFACTOR;
    case D3D_NAME_FINAL_LINE_DETAIL_TESSFACTOR: return D3D11_SB_NAME_FINAL_LINE_DETAIL_TESSFACTOR;
    case D3D_NAME_FINAL_LINE_DENSITY_TESSFACTOR: return D3D11_SB_NAME_FINAL_LINE_DENSITY_TESSFACTOR;
    default: return name <= D3D_NAME_SAMPLE_INDEX ? static_cast<D3D10_SB_NAME>(name) : D3D10_SB_NAME_UNDEFINED;
    }
}

// One signature chunk: ISGN/OSGN/PCSG (24-byte elements), OSG5 (28, stream first), ISG1/OSG1/PSG1 (32, stream
// first, minimum precision last).
bool read_signature(const Chunk& c, std::vector<D3D12DDIARG_SIGNATURE_ENTRY_0012>& entries,
                    std::vector<DdiShader::Named>* names) {
    const bool v1 = c.tag == fourcc('I', 'S', 'G', '1') || c.tag == fourcc('O', 'S', 'G', '1') ||
                    c.tag == fourcc('P', 'S', 'G', '1');
    const bool stream = v1 || c.tag == fourcc('O', 'S', 'G', '5');
    const uint32_t stride = v1 ? 32 : stream ? 28 : 24;
    if (c.size < 8) return false;
    const uint32_t count = rd32(c.data);
    if (8 + uint64_t{count} * stride > c.size) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const BYTE* p = c.data + 8 + size_t{i} * stride;
        D3D12DDIARG_SIGNATURE_ENTRY_0012 e{};
        if (stream) {
            e.Stream = static_cast<BYTE>(rd32(p));
            p += 4;
        }
        const uint32_t name_offset = rd32(p);
        const uint32_t index = rd32(p + 4);
        e.SystemValue = to_sb_name(rd32(p + 8), index);
        e.RegisterComponentType = static_cast<D3D10_SB_REGISTER_COMPONENT_TYPE>(rd32(p + 12));
        e.Register = rd32(p + 16);
        e.Mask = static_cast<BYTE>(rd32(p + 20) & 0xf);
        e.MinPrecision = static_cast<D3D11_SB_OPERAND_MIN_PRECISION>(v1 ? rd32(p + 24) : 0);
        entries.push_back(e);
        if (names) {
            if (name_offset >= c.size) return false;
            const char* s = reinterpret_cast<const char*>(c.data + name_offset);
            const size_t len = strnlen(s, c.size - name_offset);
            if (name_offset + len >= c.size) return false;
            names->push_back({std::string(s, len), index, e.Register});
        }
    }
    return true;
}
} // namespace

UINT DdiShader::input_register(const char* name, UINT index) const {
    for (const Named& n : input_names)
        if (n.name == name && n.index == index) return n.reg;
    return ~0u;
}

bool ddi_form(const BYTE* container, size_t bytes, DdiShader& out) {
    out = DdiShader{};
    std::vector<Chunk> chunks;
    if (!parse_container(container, bytes, chunks)) return false;
    // DWORD 1 of the program is its length in DWORDs: LenTok, or the DXIL program header's SizeInUint32.
    const Chunk* code = find_chunk(chunks, {fourcc('S', 'H', 'E', 'X'), fourcc('S', 'H', 'D', 'R'), fourcc('D', 'X', 'I', 'L')});
    if (!code || code->size < 8 || code->size % 4 || uint64_t{rd32(code->data + 4)} * 4 != code->size) return false;
    out.code.resize(code->size / 4);
    std::memcpy(out.code.data(), code->data, code->size);
    if (const Chunk* c = find_chunk(chunks, {fourcc('I', 'S', 'G', '1'), fourcc('I', 'S', 'G', 'N')}))
        if (!read_signature(*c, out.input, &out.input_names)) return false;
    if (const Chunk* c = find_chunk(chunks, {fourcc('O', 'S', 'G', '1'), fourcc('O', 'S', 'G', '5'), fourcc('O', 'S', 'G', 'N')}))
        if (!read_signature(*c, out.output, nullptr)) return false;
    return true;
}

void* create_shader(Env& env, Device& device, PFND3D12DDI_CREATE_SHADER_0026 slot, const DdiShader& shader,
                    D3D12DDI_HROOTSIGNATURE root) {
    // The DDI declares the entry pointers without const; the driver only reads them.
    D3D12DDIARG_STAGE_IO_SIGNATURES io{};
    io.pInputSignature = const_cast<D3D12DDIARG_SIGNATURE_ENTRY_0012*>(shader.input.data());
    io.NumInputSignatureEntries = static_cast<UINT>(shader.input.size());
    io.pOutputSignature = const_cast<D3D12DDIARG_SIGNATURE_ENTRY_0012*>(shader.output.data());
    io.NumOutputSignatureEntries = static_cast<UINT>(shader.output.size());
    D3D12DDIARG_CREATE_SHADER_0026 args{};
    args.hRootSignature = root;
    args.pShaderCode = shader.code.data();
    args.IOSignatures.Standard = &io;
    void* storage = env.storage.alloc(env.core.pfnCalcPrivateShaderSize(device.h(), &args));
    if (storage) slot(device.h(), &args, D3D12DDI_HSHADER{storage});
    return storage;
}

namespace {
constexpr UINT kSize = 64;
constexpr UINT kSeed = 0x5eed1234;
constexpr UINT kTag = 0xB0250000;
constexpr UINT kPitch = 256;

// A committed 2D render target of one mip in a DEFAULT heap, created in the COMMON layout.
HRESULT create_render_target(Env& env, Device& device, DXGI_FORMAT format, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = kSize;
    res.Height = kSize;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = format;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    res.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    if (!info.ResourceDataSize) return E_FAIL;
    const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = info.ResourceDataSize;
    heap.Alignment = info.ResourceDataAlignment;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), &heap, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.heap = env.storage.alloc(sizes.Heap);
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.heap || !out.resource) return E_OUTOFMEMORY;
    return env.core.pfnCreateHeapAndResource(device.h(), &heap, out.hheap(), D3D12DDI_HRTRESOURCE{&out.rt}, &res, nullptr,
                                             D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
}

struct Vertex {
    float x, y;
    UINT tag;
};
} // namespace

void test_graphics(Env& env, Device& device) {
    const uint32_t errors_before = device.shell.device_errors;

    // Root signature: one root constant at b0, the input assembler allowed.
    D3D12DDI_ROOT_PARAMETER_0013 param{};
    param.ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants = {0, 0, 1};
    param.ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    const D3D12DDI_ROOT_SIGNATURE_0013 rs{1, &param, 0, nullptr,
                                          D3D12DDI_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 rs_args{};
    rs_args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1;
    rs_args.pRootSignature_1_1 = &rs;
    void* rs_storage = env.storage.alloc(env.core.pfnCalcPrivateRootSignatureSize(device.h(), &rs_args));
    const D3D12DDI_HROOTSIGNATURE hrs{rs_storage};
    HRESULT hr = rs_storage ? env.core.pfnCreateRootSignature(device.h(), &rs_args, hrs) : E_OUTOFMEMORY;
    checkf(hr == S_OK, "graphics: CreateRootSignature, one root constant (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;

    // Shaders in the DDI form.
    DdiShader vs, ps;
    const bool stripped = ddi_form(g_fixture_vs, sizeof(g_fixture_vs), vs) && ddi_form(g_fixture_ps, sizeof(g_fixture_ps), ps);
    const UINT reg_pos = vs.input_register("POSITION", 0);
    const UINT reg_tag = vs.input_register("TAG", 0);
    checkf(stripped && vs.input.size() == 3 && vs.output.size() == 3 && ps.input.size() == 3 && ps.output.size() == 1 &&
               reg_pos != ~0u && reg_tag != ~0u,
           "graphics: fxc vs_5_0 and ps_5_0 reduced to SHEX and nameless entries (vs %zu in %zu out, ps %zu in %zu out; "
           "POSITION v%u, TAG v%u)",
           vs.input.size(), vs.output.size(), ps.input.size(), ps.output.size(), reg_pos, reg_tag);
    void* vs_storage = stripped ? create_shader(env, device, env.core.pfnCreateVertexShader, vs, hrs) : nullptr;
    void* ps_storage = stripped ? create_shader(env, device, env.core.pfnCreatePixelShader, ps, hrs) : nullptr;
    const D3D12DDI_HSHADER hvs{vs_storage}, hps{ps_storage};
    checkf(vs_storage && ps_storage && device.shell.device_errors == errors_before,
           "graphics: CreateVertexShader and CreatePixelShader through the native intake");

    // Element layout: registers only; engine-ddi names them from the vertex program's rebuilt input signature.
    const D3D12DDIARG_INPUT_ELEMENT_DESC elements[] = {
        {0, 0, DXGI_FORMAT_R32G32_FLOAT, D3D12DDI_INPUT_CLASSIFICIATION_PER_VERTEX_DATA, 0, reg_pos},
        {0, 8, DXGI_FORMAT_R32_UINT, D3D12DDI_INPUT_CLASSIFICIATION_PER_VERTEX_DATA, 0, reg_tag},
    };
    D3D12DDIARG_CREATEELEMENTLAYOUT_0010 layout_args{};
    layout_args.pVertexElements = elements;
    layout_args.NumElements = 2;
    void* layout_storage = env.storage.alloc(env.core.pfnCalcPrivateElementLayoutSize(device.h(), &layout_args));
    if (layout_storage) env.core.pfnCreateElementLayout(device.h(), &layout_args, D3D12DDI_HELEMENTLAYOUT{layout_storage});

    D3D12DDI_BLEND_DESC_0010 blend{};
    for (D3D12DDI_RENDER_TARGET_BLEND_DESC& rt : blend.RenderTarget)
        rt = {FALSE, FALSE, D3D12DDI_BLEND_ONE, D3D12DDI_BLEND_ZERO, D3D12DDI_BLEND_OP_ADD, D3D12DDI_BLEND_ONE,
              D3D12DDI_BLEND_ZERO, D3D12DDI_BLEND_OP_ADD, D3D12DDI_LOGIC_OP_NOOP,
              static_cast<UINT8>(D3D12DDI_COLOR_WRITE_ENABLE_ALL)};
    void* blend_storage = env.storage.alloc(env.core.pfnCalcPrivateBlendStateSize(device.h(), &blend));
    if (blend_storage) env.core.pfnCreateBlendState(device.h(), &blend, D3D12DDI_HBLENDSTATE{blend_storage});

    D3D12DDI_RASTERIZER_DESC_0010 raster{};
    raster.FillMode = D3D12DDI_FILL_MODE_SOLID;
    raster.CullMode = D3D12DDI_CULL_MODE_NONE;
    raster.DepthClipEnable = TRUE;
    raster.ScissorEnable = TRUE;
    raster.ConservativeRasterizationMode = D3D12DDI_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    void* raster_storage = env.storage.alloc(env.core.pfnCalcPrivateRasterizerStateSize(device.h(), &raster));
    if (raster_storage) env.core.pfnCreateRasterizerState(device.h(), &raster, D3D12DDI_HRASTERIZERSTATE{raster_storage});

    D3D12DDI_DEPTH_STENCIL_DESC_0025 depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D12DDI_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D12DDI_COMPARISON_FUNC_ALWAYS;
    depth.StencilReadMask = 0xff;
    depth.StencilWriteMask = 0xff;
    const D3D12DDI_DEPTH_STENCILOP_DESC keep{D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_STENCIL_OP_KEEP, D3D12DDI_STENCIL_OP_KEEP,
                                            D3D12DDI_COMPARISON_FUNC_ALWAYS};
    depth.FrontFace = keep;
    depth.BackFace = keep;
    void* depth_storage = env.storage.alloc(env.core.pfnCalcPrivateDepthStencilStateSize(device.h(), &depth));
    if (depth_storage) env.core.pfnCreateDepthStencilState(device.h(), &depth, D3D12DDI_HDEPTHSTENCILSTATE{depth_storage});
    checkf(layout_storage && blend_storage && raster_storage && depth_storage &&
               device.shell.device_errors == errors_before,
           "graphics: element layout by register, blend, rasterizer and depth-stencil states");

    D3D12DDIARG_CREATE_PIPELINE_STATE_0075 pso_args{};
    pso_args.hVertexShader = hvs;
    pso_args.hPixelShader = hps;
    pso_args.hRootSignature = hrs;
    pso_args.hBlendState = D3D12DDI_HBLENDSTATE{blend_storage};
    pso_args.SampleMask = UINT_MAX;
    pso_args.hRasterizerState = D3D12DDI_HRASTERIZERSTATE{raster_storage};
    pso_args.hDepthStencilState = D3D12DDI_HDEPTHSTENCILSTATE{depth_storage};
    pso_args.hElementLayout = D3D12DDI_HELEMENTLAYOUT{layout_storage};
    pso_args.IBStripCutValue = D3D12DDI_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    pso_args.PrimitiveTopologyType = D3D12DDI_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_args.NumRenderTargets = 1;
    pso_args.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    pso_args.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso_args.SampleDesc = {1, 0};
    void* pso_storage = env.storage.alloc(env.core.pfnCalcPrivatePipelineStateSize(device.h(), &pso_args));
    int pso_rt = 0;
    const D3D12DDI_HPIPELINESTATE hpso{pso_storage};
    hr = pso_storage ? env.core.pfnCreatePipelineState(device.h(), &pso_args, hpso, D3D12DDI_HRTPIPELINESTATE{&pso_rt})
                     : E_OUTOFMEMORY;
    checkf(hr == S_OK, "graphics: CreatePipelineState, graphics (hr %08lx)", static_cast<unsigned long>(hr));

    Buffer target, vb, readback;
    const HRESULT hr_t = create_render_target(env, device, DXGI_FORMAT_R32_UINT, target);
    const HRESULT hr_v = create_buffer(env, device, HeapKind::Upload, 256, false, vb);
    const HRESULT hr_r = create_buffer(env, device, HeapKind::Readback, UINT64{kPitch} * kSize, false, readback);
    checkf(hr_t == S_OK && hr_v == S_OK && hr_r == S_OK,
           "graphics: 64x64 R32_UINT render target, UPLOAD vertex buffer, READBACK buffer (hr %08lx %08lx %08lx)",
           static_cast<unsigned long>(hr_t), static_cast<unsigned long>(hr_v), static_cast<unsigned long>(hr_r));

    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12DDI_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* heap_storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &heap_args));
    const D3D12DDI_HDESCRIPTORHEAP hheap{heap_storage};
    const HRESULT hr_h = heap_storage ? env.core.pfnCreateDescriptorHeap(device.h(), &heap_args, hheap) : E_OUTOFMEMORY;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE rtv{};
    if (hr_h == S_OK) rtv = env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), hheap);
    checkf(hr_h == S_OK && rtv.ptr, "graphics: RTV heap of 1 (hr %08lx)", static_cast<unsigned long>(hr_h));

    // Vertices: one triangle over the whole target.
    D3D12DDI_GPU_VIRTUAL_ADDRESS vb_va = 0;
    if (hr_v == S_OK) {
        void* cpu = nullptr;
        const HRESULT hr_m = env.core.pfnMapHeap(device.h(), vb.hheap(), &cpu);
        if (hr_m == S_OK && cpu) {
            const Vertex vertices[3] = {{-1.0f, -1.0f, kTag}, {-1.0f, 3.0f, kTag}, {3.0f, -1.0f, kTag}};
            std::memcpy(cpu, vertices, sizeof(vertices));
            env.core.pfnUnmapHeap(device.h(), vb.hheap());
        }
        vb_va = env.core.pfnCheckResourceVirtualAddress(device.h(), vb.hres());
        checkf(hr_m == S_OK && cpu && vb_va, "graphics: vertex buffer written through MapHeap, VA %llx",
               static_cast<unsigned long long>(vb_va));
    }
    if (hr != S_OK || hr_t != S_OK || hr_v != S_OK || hr_r != S_OK || hr_h != S_OK || !rtv.ptr || !vb_va) return;

    D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 rtv_args{};
    rtv_args.hDrvResource = target.hres();
    rtv_args.Format = DXGI_FORMAT_R32_UINT;
    rtv_args.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
    rtv_args.Tex2D = {0, 0, 1, 0};
    env.core.pfnCreateRenderTargetView(device.h(), &rtv_args, rtv);
    checkf(device.shell.device_errors == errors_before, "graphics: render target view");

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "graphics: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));

    Recording rec;
    if (hr == S_OK) {
        hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
        checkf(hr == S_OK && rec.table == 1, "graphics: DIRECT list bound to the graphics table (hr %08lx, table %u)",
               static_cast<unsigned long>(hr), rec.table);
    }
    if (hr == S_OK && rec.table == 1) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[1];
        t.pfnSetGraphicsRootSignature(rec.hlist(), hrs);
        t.pfnSetPipelineState(rec.hlist(), hpso);
        t.pfnSetGraphicsRoot32BitConstant(rec.hlist(), 0, kSeed, 0);
        t.pfnIaSetTopology(rec.hlist(), D3D12DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12DDI_VERTEX_BUFFER_VIEW vbv{vb_va, 3 * sizeof(Vertex), sizeof(Vertex)};
        t.pfnIASetVertexBuffers(rec.hlist(), 0, 1, &vbv);
        const D3D12DDI_VIEWPORT viewport{0.0f, 0.0f, static_cast<FLOAT>(kSize), static_cast<FLOAT>(kSize), 0.0f, 1.0f};
        t.pfnRsSetViewports(rec.hlist(), 1, &viewport);
        const D3D12DDI_RECT scissor{0, 0, static_cast<LONG>(kSize), static_cast<LONG>(kSize)};
        t.pfnRsSetScissorRects(rec.hlist(), 1, &scissor);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_target =
            transition(target, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_RENDER_TARGET);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_target);
        // A full clear to a value the draw must overwrite. It is also the initialization vkd3d-proton requires of a
        // placed render target (it gives those no initial layout transition), and engine-ddi places its committed
        // resources too (INTEGRATION.md, "Committed render targets"). Without it the draw still matches, but the
        // validation layer reports the image in VK_IMAGE_LAYOUT_UNDEFINED.
        const FLOAT sentinel[4] = {12345.0f, 0.0f, 0.0f, 0.0f};
        t.pfnClearRenderTargetView(rec.hlist(), rtv, sentinel, 0, nullptr);
        t.pfnOMSetRenderTargets(rec.hlist(), 1, &rtv, TRUE, nullptr);
        t.pfnDrawInstanced(rec.hlist(), 3, 1, 0, 0);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(target, D3D12DDI_RESOURCE_STATE_RENDER_TARGET, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_source);

        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{DXGI_FORMAT_R32_UINT, kSize, kSize, 1, kPitch,
                                                                        kPitch * kSize};
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD = {readback.hres(), 0};
        src.BaseAddress.UMD = {target.hres(), 0};
        t.pfnCopyTextureRegion(rec.hlist(), &dst, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint}, 0, 0, 0,
                               &src, {D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr}, nullptr);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors,
               "graphics: root signature, PSO, root constant, vertex buffer, viewport, barriers, clear, "
               "DrawInstanced(3), copy, execute (hr %08lx)",
               static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "graphics");

        void* cpu = nullptr;
        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu);
        if (hr == S_OK && cpu) {
            const auto* bytes = static_cast<const BYTE*>(cpu);
            UINT bad = 0, first_x = 0, first_y = 0, got = 0, want = 0;
            for (UINT y = 0; y < kSize; ++y) {
                for (UINT x = 0; x < kSize; ++x) {
                    UINT v;
                    std::memcpy(&v, bytes + SIZE_T{y} * kPitch + SIZE_T{x} * 4, 4);
                    const UINT expected = kSeed ^ kTag ^ ((y << 16) | x) ^ (3u << 8) ^ 0x51u;
                    if (v != expected) {
                        if (!bad) {
                            first_x = x;
                            first_y = y;
                            got = v;
                            want = expected;
                        }
                        ++bad;
                    }
                }
            }
            checkf(bad == 0,
                   "graphics: every texel is seed ^ tag ^ ((y << 16) | x) ^ 0x351 (%u of %u differ; first (%u,%u) %08x, "
                   "expected %08x)",
                   bad, kSize * kSize, first_x, first_y, got, want);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            checkf(false, "graphics: MapHeap of the READBACK heap (hr %08lx)", static_cast<unsigned long>(hr));
        }
    }
    destroy_recording(env, device, rec);
    if (queue) engine_ddi::destroy_engine_queue(queue);
    env.core.pfnDestroyDescriptorHeap(device.h(), hheap);
    destroy_buffer(env, device, target);
    destroy_buffer(env, device, vb);
    destroy_buffer(env, device, readback);
    env.core.pfnDestroyPipelineState(device.h(), hpso);
    env.core.pfnDestroyDepthStencilState(device.h(), D3D12DDI_HDEPTHSTENCILSTATE{depth_storage});
    env.core.pfnDestroyRasterizerState(device.h(), D3D12DDI_HRASTERIZERSTATE{raster_storage});
    env.core.pfnDestroyBlendState(device.h(), D3D12DDI_HBLENDSTATE{blend_storage});
    env.core.pfnDestroyElementLayout(device.h(), D3D12DDI_HELEMENTLAYOUT{layout_storage});
    env.core.pfnDestroyShader(device.h(), hps);
    env.core.pfnDestroyShader(device.h(), hvs);
    env.core.pfnDestroyRootSignature(device.h(), hrs);
    checkf(!engine_ddi::harness_live_objects(device.context) && !engine_ddi::harness_pending_releases(device.context),
           "graphics: no live object and no pending release left (%u live)",
           engine_ddi::harness_live_objects(device.context));
}

} // namespace harness
