// SPDX-License-Identifier: MIT
// Round trip 8: acceleration structures through the DDI (D108 prebuild info, L60 build, L61 postbuild info, L62
// copy) and an inline ray query in a compute program created through CreateComputeShader (fixture-rayquery.h,
// cs_6_5). RuntimeBacked on the stub shell, on a COMPUTE engine queue. The engine places an acceleration structure
// from its address to the end of the VkBuffer behind it (va_map.c); EnginePrivateTest suballocates small heaps from
// one shared buffer, so there every structure would span its neighbours for the validation layer. RuntimeBacked
// heaps are borrowed memory, which the engine never suballocates (memory.c), as in the native driver.
//   1. Prebuild info of a bottom level of one triangle and a top level of one instance, compared with the engine's own
//      ID3D12Device5 answer; its sizes size the structure and scratch buffers.
//   2. One list: the bottom level (with a CURRENT_SIZE postbuild description), a UAV barrier with no resource, a clone
//      of it, CURRENT_SIZE emitted, a top level over each (instance descriptions in an UPLOAD buffer, holding the
//      structures' CheckResourceVirtualAddress answers), then the ray query over each top level, the top level as a
//      root SRV by address: 8x8 orthographic rays toward the triangle, 1 for a hit and 2 for a miss.
//   3. Both 64-word results equal the pattern computed here from the triangle, whose edges keep a margin from every
//      ray; the two CURRENT_SIZE answers are equal, nonzero and within the prebuild maximum.
// Then a few refusals, each reported once on the list or device that was called. The slots are the positive path's
// subject; the raytracing tier engine-ddi reports stays NOT_SUPPORTED (INTEGRATION.md). If the engine reports no
// raytracing tier 1.1 on this GPU, the round trip says so in one SKIP line.
#include "harness.h"
#include "fixture-rayquery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace harness {

namespace {
constexpr UINT kGrid = 8;
constexpr UINT kWords = kGrid * kGrid;
constexpr UINT kHit = 1, kMiss = 2;
constexpr UINT64 kOutBytes = 2 * 256;               // one 64-word result per top level, 256 bytes apart
constexpr UINT64 kInstanceOffset = 256;             // in the UPLOAD buffer, after the vertices
// The triangle at z = 0.5, in the rays' xy plane; the shader's ray (x, y) starts at ((x + 0.5) / 4 - 1, (y + 0.5) / 4 - 1).
constexpr float kTriangle[3][3] = {{-0.8f, -0.7f, 0.5f}, {0.1f, -0.7f, 0.5f}, {-0.7f, 0.9f, 0.5f}};
constexpr double kMargin = 0.05;                    // least distance of a ray from an edge line

// The expected word of each ray from the triangle's edge functions, and the least distance of any ray from an edge.
double expected_words(UINT32* words) {
    double least = 1e9;
    for (UINT y = 0; y < kGrid; ++y)
        for (UINT x = 0; x < kGrid; ++x) {
            const double px = (x + 0.5) / 4.0 - 1.0, py = (y + 0.5) / 4.0 - 1.0;
            int positive = 0;
            for (int e = 0; e < 3; ++e) {
                const float* a = kTriangle[e];
                const float* b = kTriangle[(e + 1) % 3];
                const double dx = double{b[0]} - a[0], dy = double{b[1]} - a[1];
                const double side = dx * (py - a[1]) - dy * (px - a[0]);
                least = std::min(least, std::fabs(side) / std::hypot(dx, dy));
                positive += side > 0 ? 1 : 0;
            }
            words[y * kGrid + x] = (positive == 0 || positive == 3) ? kHit : kMiss;
        }
    return least;
}

// A DEFAULT buffer for acceleration structures: unordered access and the acceleration structure flag, as
// CreateCommittedResource of the API makes it (the runtime's exact heap shape is the INFERENCE of harness.cpp).
HRESULT create_structure_buffer(Env& env, Device& device, UINT64 size, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_BUFFER;
    res.Width = size;
    res.Height = 1;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = DXGI_FORMAT_UNKNOWN;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_ROW_MAJOR;
    res.Flags = D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS | D3D12DDI_RESOURCE_FLAG_0088_RAYTRACING_ACCELERATION_STRUCTURE;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_UNDEFINED;
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    if (!info.ResourceDataSize) return E_FAIL;
    const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = info.ResourceDataSize;
    heap.Alignment = info.ResourceDataAlignment;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = D3D12DDI_HEAP_FLAG_BUFFERS;
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

D3D12DDIARG_RESOURCE_BARRIER_0022 uav_barrier() {
    D3D12DDIARG_RESOURCE_BARRIER_0022 b{};
    b.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_UAV;
    return b;
}

// 64 result words against the expected ones: how many differ, and the first that does.
UINT mismatches(const UINT32* words, const UINT32* expected, UINT* first) {
    UINT bad = 0;
    *first = kWords;
    for (UINT i = 0; i < kWords; ++i) {
        if (words[i] == expected[i]) continue;
        if (!bad) *first = i;
        ++bad;
    }
    return bad;
}
} // namespace

void test_raytracing(Env& env) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    const HRESULT hr_options = env.engine->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (hr_options != S_OK || options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
        std::printf("SKIP  raytracing: the engine reports raytracing tier %d on this GPU (hr %08lx); the round trip "
                    "needs 1.1 for inline ray queries\n",
                    static_cast<int>(options5.RaytracingTier), static_cast<unsigned long>(hr_options));
        return;
    }
    UINT32 expected[kWords];
    const double margin = expected_words(expected);
    UINT hits = 0;
    for (UINT32 w : expected) hits += w == kHit ? 1u : 0u;
    checkf(margin >= kMargin && hits > 0 && hits < kWords,
           "raytracing: the engine reports tier %d; the triangle covers %u of %u rays, no ray within %.3f of an edge",
           static_cast<int>(options5.RaytracingTier), hits, kWords, margin);

    StubMemory m;
    check(load_stub(env, m), "raytracing: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free);
    checkf(hr == S_OK && device.context, "raytracing: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[0];

    // 1. Prebuild info. The instance and vertex addresses are not read for it.
    Buffer upload;
    const HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, 1024, false, upload);
    const D3D12DDI_GPU_VIRTUAL_ADDRESS upload_va =
        hr_u == S_OK ? env.core.pfnCheckResourceVirtualAddress(device.h(), upload.hres()) : 0;
    D3D12DDI_RAYTRACING_GEOMETRY_DESC_0054 geometry{};
    geometry.Type = D3D12DDI_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags = D3D12DDI_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
    geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geometry.Triangles.VertexCount = 3;
    geometry.Triangles.VertexBuffer = {upload_va, sizeof(kTriangle[0])};
    D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054 bottom{};
    bottom.Type = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bottom.NumDescs = 1;
    bottom.DescsLayout = D3D12DDI_ELEMENTS_LAYOUT_ARRAY;
    bottom.pGeometryDescs = &geometry;
    D3D12DDI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_0054 top{};
    top.Type = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags = D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    top.NumDescs = 1;
    top.DescsLayout = D3D12DDI_ELEMENTS_LAYOUT_ARRAY;
    top.InstanceDescs = upload_va + kInstanceOffset;
    D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_0054 blas_info{}, tlas_info{};
    env.core.pfnGetRaytracingAccelerationStructurePrebuildInfo(device.h(), &bottom, &blas_info);
    env.core.pfnGetRaytracingAccelerationStructurePrebuildInfo(device.h(), &top, &tlas_info);
    // The engine's own answer to the same inputs in the API's structures.
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO engine_blas{}, engine_tlas{};
    ID3D12Device5* device5 = nullptr;
    if (SUCCEEDED(env.engine->QueryInterface(__uuidof(ID3D12Device5), reinterpret_cast<void**>(&device5)))) {
        D3D12_RAYTRACING_GEOMETRY_DESC api_geometry{};
        api_geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        api_geometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        api_geometry.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        api_geometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        api_geometry.Triangles.VertexCount = 3;
        api_geometry.Triangles.VertexBuffer = {upload_va, sizeof(kTriangle[0])};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS api{};
        api.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        api.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        api.NumDescs = 1;
        api.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        api.pGeometryDescs = &api_geometry;
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&api, &engine_blas);
        api.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        api.InstanceDescs = upload_va + kInstanceOffset;
        device5->GetRaytracingAccelerationStructurePrebuildInfo(&api, &engine_tlas);
        device5->Release();
    }
    const bool prebuild = hr_u == S_OK && upload_va && blas_info.ResultDataMaxSizeInBytes &&
                          blas_info.ScratchDataSizeInBytes && tlas_info.ResultDataMaxSizeInBytes &&
                          tlas_info.ScratchDataSizeInBytes &&
                          !std::memcmp(&blas_info, &engine_blas, sizeof(blas_info)) &&
                          !std::memcmp(&tlas_info, &engine_tlas, sizeof(tlas_info)) && !device.shell.device_errors;
    checkf(prebuild,
           "raytracing: GetRaytracingAccelerationStructurePrebuildInfo answers as the engine's ID3D12Device5: bottom "
           "level %llu bytes (scratch %llu), top level %llu bytes (scratch %llu)",
           static_cast<unsigned long long>(blas_info.ResultDataMaxSizeInBytes),
           static_cast<unsigned long long>(blas_info.ScratchDataSizeInBytes),
           static_cast<unsigned long long>(tlas_info.ResultDataMaxSizeInBytes),
           static_cast<unsigned long long>(tlas_info.ScratchDataSizeInBytes));

    // Buffers sized by the prebuild answer; the scratch serves every build in turn.
    const UINT64 scratch_bytes = std::max(blas_info.ScratchDataSizeInBytes, tlas_info.ScratchDataSizeInBytes);
    Buffer blas, clone, tlas, tlas2, scratch, out, post, readback;
    const HRESULT hr_buffers[] = {
        create_structure_buffer(env, device, blas_info.ResultDataMaxSizeInBytes, blas),
        create_structure_buffer(env, device, blas_info.ResultDataMaxSizeInBytes, clone),
        create_structure_buffer(env, device, tlas_info.ResultDataMaxSizeInBytes, tlas),
        create_structure_buffer(env, device, tlas_info.ResultDataMaxSizeInBytes, tlas2),
        create_buffer(env, device, HeapKind::Default, scratch_bytes, true, scratch),
        create_buffer(env, device, HeapKind::Default, kOutBytes, true, out),
        create_buffer(env, device, HeapKind::Default, 2 * sizeof(UINT64), true, post),
        create_buffer(env, device, HeapKind::Readback, kOutBytes + 2 * sizeof(UINT64), false, readback),
    };
    bool created = prebuild;
    for (HRESULT h : hr_buffers) created = created && h == S_OK;
    D3D12DDI_GPU_VIRTUAL_ADDRESS va[6]{};
    const Buffer* named[6] = {&blas, &clone, &tlas, &tlas2, &scratch, &out};
    for (int i = 0; created && i < 6; ++i) {
        va[i] = env.core.pfnCheckResourceVirtualAddress(device.h(), named[i]->hres());
        created = va[i] != 0 && va[i] % D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT == 0;
    }
    const D3D12DDI_GPU_VIRTUAL_ADDRESS post_va =
        created ? env.core.pfnCheckResourceVirtualAddress(device.h(), post.hres()) : 0;
    checkf(created && post_va,
           "raytracing: four structure buffers (UAV, acceleration structure flag), scratch, output, postbuild and "
           "READBACK buffers, every address 256-byte aligned");
    if (!created || !post_va) return;
    const D3D12DDI_GPU_VIRTUAL_ADDRESS blas_va = va[0], clone_va = va[1], tlas_va = va[2], tlas2_va = va[3],
                                       scratch_va = va[4], out_va = va[5];

    // Vertices at 0, the two instances at kInstanceOffset: the first names the bottom level, the second its clone.
    void* cpu = nullptr;
    hr = env.core.pfnMapHeap(device.h(), upload.hheap(), &cpu);
    checkf(hr == S_OK && cpu, "raytracing: MapHeap of the UPLOAD heap (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK || !cpu) return;
    std::memcpy(cpu, kTriangle, sizeof(kTriangle));
    D3D12_RAYTRACING_INSTANCE_DESC instances[2]{};
    for (D3D12_RAYTRACING_INSTANCE_DESC& i : instances) {
        i.Transform[0][0] = i.Transform[1][1] = i.Transform[2][2] = 1.0f;
        i.InstanceMask = 0xFF;
    }
    instances[0].AccelerationStructure = blas_va;
    instances[1].AccelerationStructure = clone_va;
    std::memcpy(static_cast<BYTE*>(cpu) + kInstanceOffset, instances, sizeof(instances));
    env.core.pfnUnmapHeap(device.h(), upload.hheap());

    // The program: dxc's DXIL part alone; root signature SRV(t0), UAV(u0) as the DDI's parsed description.
    D3D12DDI_ROOT_PARAMETER_0013 params[2]{};
    params[0].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    params[0].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12DDI_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = {0, 0, D3D12DDI_ROOT_DESCRIPTOR_FLAG_0013_NONE};
    params[1].ShaderVisibility = D3D12DDI_SHADER_VISIBILITY_ALL;
    const D3D12DDI_ROOT_SIGNATURE_0013 rs{2, params, 0, nullptr, D3D12DDI_ROOT_SIGNATURE_FLAG_NONE};
    D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013 rs_args{};
    rs_args.Version = D3D12DDI_ROOT_SIGNATURE_VERSION_1_1;
    rs_args.pRootSignature_1_1 = &rs;
    void* rs_storage = env.storage.alloc(env.core.pfnCalcPrivateRootSignatureSize(device.h(), &rs_args));
    const D3D12DDI_HROOTSIGNATURE hrs{rs_storage};
    const HRESULT hr_rs = rs_storage ? env.core.pfnCreateRootSignature(device.h(), &rs_args, hrs) : E_OUTOFMEMORY;
    DdiShader cs;
    const bool stripped = ddi_form(g_fixture_rayquery, sizeof(g_fixture_rayquery), cs);
    void* cs_storage = stripped && hr_rs == S_OK ? create_shader(env, device, env.core.pfnCreateComputeShader, cs, hrs)
                                                 : nullptr;
    const D3D12DDI_HSHADER hcs{cs_storage};
    D3D12DDIARG_CREATE_PIPELINE_STATE_0075 pso_args{};
    pso_args.hComputeShader = hcs;
    pso_args.hRootSignature = hrs;
    void* pso_storage = cs_storage ? env.storage.alloc(env.core.pfnCalcPrivatePipelineStateSize(device.h(), &pso_args))
                                   : nullptr;
    int pso_rt = 0;
    const D3D12DDI_HPIPELINESTATE hpso{pso_storage};
    const HRESULT hr_pso =
        pso_storage ? env.core.pfnCreatePipelineState(device.h(), &pso_args, hpso, D3D12DDI_HRTPIPELINESTATE{&pso_rt})
                    : E_OUTOFMEMORY;
    checkf(hr_rs == S_OK && stripped && cs_storage && hr_pso == S_OK && !device.shell.device_errors,
           "raytracing: root signature SRV(t0) UAV(u0), the dxc cs_6_5 ray query program (%zu DWORDs) through "
           "CreateComputeShader, CreatePipelineState (hr %08lx %08lx)",
           cs.code.size(), static_cast<unsigned long>(hr_rs), static_cast<unsigned long>(hr_pso));
    if (hr_rs != S_OK || !cs_storage || hr_pso != S_OK) return;

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_COMPUTE, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    Recording rec;
    if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE, rec);
    checkf(hr == S_OK && queue && rec.table == 0,
           "raytracing: COMPUTE engine queue and a COMPUTE list on the compute table (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK || rec.table != 0) return;

    // 2. The list.
    const D3D12DDIARG_RESOURCE_BARRIER_0022 begin[] = {
        transition(scratch, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(out, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS),
        transition(post, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    t.pfnResourceBarrier(rec.hlist(), 3, begin);
    const D3D12DDIARG_RESOURCE_BARRIER_0022 uav = uav_barrier();
    const D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC_0054 built_size{
        post_va, D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE};
    D3D12DDIARG_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_0054 build{};
    build.DestAccelerationStructureData = blas_va;
    build.Inputs = bottom;
    build.ScratchAccelerationStructureData = scratch_va;
    build.NumPostbuildInfoDescs = 1;
    build.pPostbuildInfoDescs = &built_size;
    t.pfnBuildRaytracingAccelerationStructure(rec.hlist(), &build);
    t.pfnResourceBarrier(rec.hlist(), 1, &uav);
    const D3D12DDIARG_COPY_RAYTRACING_ACCELERATION_STRUCTURE_0054 copy{
        clone_va, blas_va, D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE};
    t.pfnCopyRaytracingAccelerationStructure(rec.hlist(), &copy);
    const D3D12DDIARG_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_0054 emit{
        {post_va + sizeof(UINT64), D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE}, 1, &blas_va};
    t.pfnEmitRaytracingAccelerationStructurePostbuildInfo(rec.hlist(), &emit);
    build = {};
    build.DestAccelerationStructureData = tlas_va;
    build.Inputs = top;
    build.ScratchAccelerationStructureData = scratch_va;
    t.pfnBuildRaytracingAccelerationStructure(rec.hlist(), &build);
    t.pfnResourceBarrier(rec.hlist(), 1, &uav);         // the scratch again, and the clone's copy before its reader
    build.DestAccelerationStructureData = tlas2_va;
    build.Inputs.InstanceDescs = upload_va + kInstanceOffset + sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
    t.pfnBuildRaytracingAccelerationStructure(rec.hlist(), &build);
    t.pfnResourceBarrier(rec.hlist(), 1, &uav);
    t.pfnSetComputeRootSignature(rec.hlist(), hrs);
    t.pfnSetPipelineState(rec.hlist(), hpso);
    t.pfnSetComputeRootShaderResourceView(rec.hlist(), 0, tlas_va);
    t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1, out_va);
    t.pfnDispatch(rec.hlist(), 1, 1, 1);
    t.pfnSetComputeRootShaderResourceView(rec.hlist(), 0, tlas2_va);
    t.pfnSetComputeRootUnorderedAccessView(rec.hlist(), 1, out_va + kOutBytes / 2);
    t.pfnDispatch(rec.hlist(), 1, 1, 1);
    const D3D12DDIARG_RESOURCE_BARRIER_0022 end[] = {
        transition(out, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE),
        transition(post, D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS, D3D12DDI_RESOURCE_STATE_COPY_SOURCE),
    };
    t.pfnResourceBarrier(rec.hlist(), 2, end);
    D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
    dst.BaseAddress.UMD = {readback.hres(), 0};
    src.BaseAddress.UMD = {out.hres(), 0};
    t.pfnCopyBufferRegion(rec.hlist(), dst, src, kOutBytes);
    dst.BaseAddress.UMD = {readback.hres(), kOutBytes};
    src.BaseAddress.UMD = {post.hres(), 0};
    t.pfnCopyBufferRegion(rec.hlist(), dst, src, 2 * sizeof(UINT64));
    t.pfnCloseCommandList(rec.hlist());
    const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
    hr = engine_ddi::execute_command_lists(queue, 1, lists);
    checkf(hr == S_OK && !device.shell.list_errors && !device.shell.device_errors,
           "raytracing: bottom level with postbuild info, clone, emitted postbuild info, two top levels, two ray "
           "query dispatches, executed (hr %08lx, %u list errors, %u device errors)",
           static_cast<unsigned long>(hr), device.shell.list_errors, device.shell.device_errors);

    // 3. The results.
    if (hr == S_OK && wait_queue_idle(env, queue, "raytracing")) {
        void* mapped = nullptr;
        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &mapped);
        checkf(hr == S_OK && mapped, "raytracing: MapHeap of the READBACK heap (hr %08lx)", static_cast<unsigned long>(hr));
        if (hr == S_OK && mapped) {
            const auto* words = static_cast<const UINT32*>(mapped);
            UINT first = 0, first2 = 0;
            const UINT bad = mismatches(words, expected, &first);
            const UINT bad2 = mismatches(words + kOutBytes / 2 / sizeof(UINT32), expected, &first2);
            checkf(bad == 0, "raytracing: the ray query over the top level writes the 64 expected words, %u hits (%u "
                             "differ, first at %u)",
                   hits, bad, first);
            checkf(bad2 == 0,
                   "raytracing: the ray query over the top level of the cloned bottom level writes the same 64 words "
                   "(%u differ, first at %u)",
                   bad2, first2);
            UINT64 sizes[2];
            std::memcpy(sizes, static_cast<const BYTE*>(mapped) + kOutBytes, sizeof(sizes));
            checkf(sizes[0] && sizes[0] <= blas_info.ResultDataMaxSizeInBytes && sizes[1] == sizes[0],
                   "raytracing: CURRENT_SIZE of the bottom level, at its build and emitted after it: %llu and %llu "
                   "bytes, nonzero and within the prebuild maximum %llu",
                   static_cast<unsigned long long>(sizes[0]), static_cast<unsigned long long>(sizes[1]),
                   static_cast<unsigned long long>(blas_info.ResultDataMaxSizeInBytes));
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
    }

    // Refusals: each is reported once, with its HRESULT, on the list or device that was called, and nothing reaches
    // the engine. The count is then put back, so that the closing "no error" keeps its meaning.
    const auto refused = [&](const char* what, HRESULT expected_hr, auto call) {
        const uint32_t before = device.shell.list_errors;
        device.shell.last_list_error = S_OK;
        device.shell.last_list = nullptr;
        call();
        checkf(device.shell.list_errors == before + 1 && device.shell.last_list_error == expected_hr &&
                   device.shell.last_list == rec.rtlist().handle,
               "raytracing: %s: one %08lx on the calling list (%u errors, last %08lx)", what,
               static_cast<unsigned long>(expected_hr), device.shell.list_errors - before,
               static_cast<unsigned long>(device.shell.last_list_error));
        device.shell.list_errors = before;
    };
    build = {};
    build.DestAccelerationStructureData = blas_va;
    build.Inputs = bottom;
    build.ScratchAccelerationStructureData = scratch_va;
    refused("a build on a closed list", E_INVALIDARG,
            [&] { t.pfnBuildRaytracingAccelerationStructure(rec.hlist(), &build); });
    const D3D12DDIARG_RESETCOMMANDLIST_0040 reset{D3D12DDI_HCOMMANDRECORDER_0040{rec.recorder}, 1,
                                                 D3D12DDI_COMMAND_LIST_FLAG_NONE};
    t.pfnResetCommandList(rec.hlist(), &reset);
    refused("postbuild info with no argument", E_INVALIDARG,
            [&] { t.pfnEmitRaytracingAccelerationStructurePostbuildInfo(rec.hlist(), nullptr); });
    const D3D12DDIARG_COPY_RAYTRACING_ACCELERATION_STRUCTURE_0054 serialize{
        clone_va, blas_va, D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE};
    refused("a serializing copy (engine-ddi serializes nothing)", E_NOTIMPL,
            [&] { t.pfnCopyRaytracingAccelerationStructure(rec.hlist(), &serialize); });
    t.pfnCloseCommandList(rec.hlist());
    {
        const uint32_t before = device.shell.device_errors;
        device.shell.last_device_error = S_OK;
        D3D12DDI_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_0054 info{1, 1, 1};
        env.core.pfnGetRaytracingAccelerationStructurePrebuildInfo(device.h(), nullptr, &info);
        checkf(device.shell.device_errors == before + 1 && device.shell.last_device_error == E_INVALIDARG &&
                   !info.ResultDataMaxSizeInBytes && !info.ScratchDataSizeInBytes && !info.UpdateScratchDataSizeInBytes,
               "raytracing: prebuild info with no inputs: one E_INVALIDARG on the device, the answer zeroed (%u errors)",
               device.shell.device_errors - before);
        device.shell.device_errors = before;
    }
    {
        // A recording bundle. The engine's bundle answers for the list interface and drops these calls with a log
        // line, so the list type is refused here.
        Recording bundle;
        const HRESULT hr_b =
            open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, bundle, D3D12DDI_COMMAND_LIST_TYPE_BUNDLE);
        const uint32_t before = device.shell.list_errors;
        device.shell.last_list_error = S_OK;
        device.shell.last_list = nullptr;
        if (hr_b == S_OK) env.lists[bundle.table].pfnBuildRaytracingAccelerationStructure(bundle.hlist(), &build);
        checkf(hr_b == S_OK && device.shell.list_errors == before + 1 && device.shell.last_list_error == E_INVALIDARG &&
                   device.shell.last_list == bundle.rtlist().handle,
               "raytracing: a build on a recording bundle: one E_INVALIDARG on the bundle (hr %08lx, %u errors, last "
               "%08lx)",
               static_cast<unsigned long>(hr_b), device.shell.list_errors - before,
               static_cast<unsigned long>(device.shell.last_list_error));
        device.shell.list_errors = before;
        if (hr_b == S_OK) env.lists[bundle.table].pfnCloseCommandList(bundle.hlist());
        destroy_recording(env, device, bundle);
    }

    destroy_recording(env, device, rec);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "raytracing: destroy_engine_queue reports Retired");
    for (Buffer* b : {&blas, &clone, &tlas, &tlas2, &scratch, &out, &post, &readback, &upload})
        destroy_buffer(env, device, *b);
    env.core.pfnDestroyPipelineState(device.h(), hpso);
    env.core.pfnDestroyShader(device.h(), hcs);
    env.core.pfnDestroyRootSignature(device.h(), hrs);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && m.frees == m.allocations && !device.shell.device_errors &&
               !device.shell.list_errors,
           "raytracing: destroy_device_context S_OK with no live object, %u of %u allocations freed, no error but the "
           "refusals' (hr %08lx, %u live, %u device, %u list errors)",
           m.frees, m.allocations, static_cast<unsigned long>(hr), live, device.shell.device_errors,
           device.shell.list_errors);
}

} // namespace harness
